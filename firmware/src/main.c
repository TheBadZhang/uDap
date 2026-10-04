#include "bsp/bsp_delay.h"
#include "bsp/bsp_system.h"

#include <ch32x035.h>

// 调试器功能由 xmake.lua 的选项控制，共有 4 种组合：
//   --dap=y --wchlink=n   CMSIS-DAP（默认）
//   --dap=n --wchlink=y   WCH-Link（RVSWD 目标调试 / Flash 烧录，见 src/wchlink）
//   --dap=y --wchlink=y   双模式：一份固件，运行期用按键切换（见下）
//   --dap=n --wchlink=n   只编 USB 骨架
// 未定义时默认走 CMSIS-DAP，便于 MRS 等不传该宏的构建方式。
//
// mode_overlay.h 依据这两个宏推出 ENABLE_DUAL_MODE，并给出各模式私有
// 代码/数据段的属性宏（单模式下退化成 .highcode，与改动前一致）。
#include "mode_overlay.h"

// 双模式：CMSIS-DAP 与 WCH-Link 两套代码都参与编译，运行期切换。
//
// 两者都要占用 PA2=SWCLK/DCK、PA3=SWDIO/DIO，所以**不可能同时运行**，
// 只能互斥地跑：主循环里默认进 CMSIS-DAP，检测到模式切换按键就退出该循环、
// 反初始化 USB，再进 WCH-Link 循环，反之亦然。
//
// 切换必须重新枚举 USB：两种模式的 USB 身份完全不同
// （CMSIS-DAP 是 0d28:0204 + WinUSB，WCH-Link 是 1a86:8010 + 厂商接口），
// 主机侧本来就当成两台不同设备。所以切换 = usbd_deinitialize + 重新
// usbd_desc_register/add/initialize。usbd_desc_register 内部会重置整个
// g_usbd_core（含 intf/endpoint 计数），反复切换不会累积状态。
//
// 两种模式的私有代码与数据还会共用同一段 RAM（overlay），见 mode_overlay.h。

// CDC（COM 口 <-> USART4，PB0=TX / PB1=RX）在三种调试器构建里行为一致，
// 由 drv_uart_init() 与各自的周期性服务函数推进。纯 USB 骨架（两个都关）
// 不注册 CDC 类，也就没必要初始化 UART。
#if ENABLE_DAP || ENABLE_WCHLINK
#define ENABLE_CDC_BRIDGE 1
#else
#define ENABLE_CDC_BRIDGE 0
#endif

#if ENABLE_CDC_BRIDGE
#include "drv/drv_uart.h"
#endif

#if ENABLE_DUAL_MODE
#include <usbd_core.h>

#include "drv/drv_mode_button.h"

// ---------------------------------------------------------------------------
// 双模式的 RAM overlay（见 src/mode_overlay.h 与
// sdk/Ld/Link_highcode_nv_256B_dual.ld）
// ---------------------------------------------------------------------------
// 两种模式的私有代码与数据被链接到**同一段 RAM 地址**上：它们不同时运行
// （都要占 PA2/PA3，USB 身份也不同），所以可以复用。代价是每次进入某个模式
// 都要把该模式的代码从 Flash 拷进这段 RAM、并把该模式的 BSS 清零 ——
// 别的模式的代码/数据在此之前占据着同一段地址，必须重写。
extern uint32_t __load_start_dap_overlay[];
extern uint32_t __load_start_wchlink_overlay[];

extern uint32_t _dap_overlay_code_start[];
extern uint32_t _dap_overlay_code_end[];
extern uint32_t _dap_overlay_bss_start[];
extern uint32_t _dap_overlay_bss_end[];

extern uint32_t _wchlink_overlay_code_start[];
extern uint32_t _wchlink_overlay_code_end[];
extern uint32_t _wchlink_overlay_bss_start[];
extern uint32_t _wchlink_overlay_bss_end[];

// 把 overlay 的代码段从 Flash 拷到位，并清零它的 BSS
static void overlay_load(const uint32_t *lma, uint32_t *code_start,
                         const uint32_t *code_end, uint32_t *bss_start,
                         const uint32_t *bss_end) {
    for (uint32_t *dst = code_start; dst != code_end; ++dst) {
        *dst = *lma++;
    }
    for (uint32_t *dst = bss_start; dst != bss_end; ++dst) {
        *dst = 0u;
    }
}

static void dap_overlay_init(void) {
    overlay_load(__load_start_dap_overlay, _dap_overlay_code_start,
                 _dap_overlay_code_end, _dap_overlay_bss_start,
                 _dap_overlay_bss_end);
}

static void wchlink_overlay_init(void) {
    overlay_load(__load_start_wchlink_overlay, _wchlink_overlay_code_start,
                 _wchlink_overlay_code_end, _wchlink_overlay_bss_start,
                 _wchlink_overlay_bss_end);
}
#else
// 单模式固件不做 overlay：段名退化成 .highcode（启动时由 startup 统一拷贝），
// 数据留在常规 .bss/.data，所以这里什么都不用做。
static void dap_overlay_init(void) {
}

static void wchlink_overlay_init(void) {
}
#endif

#if ENABLE_DAP
#include "dap_main.h"
#endif

#if ENABLE_WCHLINK
#include "wchlink/transport/rvswd_phy_gpio.h"
#include "wchlink/usb/wchlink_usb.h"
#endif

#if ENABLE_DAP
// 双模式下按键一按下就退出本模式；单模式编译成常量 false，由 --gc-sections 回收
static inline bool cmsisdap_mode_should_exit(void) {
#if ENABLE_DUAL_MODE
    return drv_mode_button_pressed();
#else
    return false;
#endif
}

// 注意：本函数自身就在 overlay 段（.highcode.dap）里，所以**不能**在这里调用
// dap_overlay_init()：那等于先跳进尚未初始化的地址再执行。overlay 的初始化必须由
// 不参与 overlay 的 main() 完成（见下方 main 的循环）。
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) DAP_HIGHCODE_SECTION;
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) {
    chry_dap_init(0u, 0u);

    while (!cmsisdap_mode_should_exit()) {
        chry_dap_handle();
        chry_dap_cdc_bridge();
    }
    // 模式切换时先回到 main（交出 USB 也在那里做），确保 overlay 被覆盖前
    // 已经不在执行 overlay 里的代码。
}
#endif

#if ENABLE_WCHLINK
static inline bool wchlink_mode_should_exit(void) {
#if ENABLE_DUAL_MODE
    return drv_mode_button_pressed();
#else
    return false;
#endif
}

static void wchlink_process(void) {
    // RVSWD 物理层（PA2=SWCLK、PA3=SWDIO）先配置好，再启动 USB。
    // 本板没有 MUX / 按键 / 状态灯 / 目标电源开关，对应 glue 是空实现。
    // wchlink_overlay_init() 同样必须由 main() 调用（rvswd_phy_gpio_* 在 overlay 里）。
    rvswd_phy_gpio_init();
    wchlink_usb_init();

    while (!wchlink_mode_should_exit()) {
        wchlink_usb_process();
    }
}
#endif

int main(void) {
    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    bsp_delay_ms(10u);

    // COM 口 <-> USART4 (PB0=TX, PB1=RX)，默认 115200；
    // 主机改 CDC 行编码时波特率会跟着变
#if ENABLE_CDC_BRIDGE
    drv_uart_init(0u);
#endif

#if ENABLE_DUAL_MODE
    enum firmware_mode {
        FIRMWARE_MODE_CMSIS_DAP = 0,
        FIRMWARE_MODE_WCHLINK,
    };
    enum firmware_mode mode = FIRMWARE_MODE_CMSIS_DAP;

    drv_mode_button_init();

    for (;;) {
        if (mode == FIRMWARE_MODE_CMSIS_DAP) {
            // overlay 的初始化必须在**进入 overlay 代码之前**完成，所以放在这里
            // （main 在 .text，不参与 overlay），不能放进 cherrydap_process() 内部。
            dap_overlay_init();
            cherrydap_process();
            // 交出 USB：下一个模式会用自己的描述符重新注册并初始化。
            // 此时已经退出 overlay 代码，可以安全覆盖那段 RAM。
            usbd_deinitialize(0u);
            mode = FIRMWARE_MODE_WCHLINK;
        } else {
            wchlink_overlay_init();
            wchlink_process();
            usbd_deinitialize(0u);
            mode = FIRMWARE_MODE_CMSIS_DAP;
        }

        // 同一次按住不要立刻触发反向切换。此时 USB 已反初始化，没有需要服务的
        // 端点，短暂自旋是安全的。
        drv_mode_button_wait_release();
    }
#elif ENABLE_DAP
    cherrydap_process();
#elif ENABLE_WCHLINK
    wchlink_process();
#else
    for (;;) {
        bsp_delay_ms(1000u);
    }
#endif
}
