#include "bsp/bsp_delay.h"
#include "bsp/bsp_system.h"

#include <ch32x035.h>

// 本固件固定为**双模式**：CMSIS-DAP 与 WCH-Link 两套调试器代码都编入同一份固件，
// 运行期用模式切换按键（PA4 接地）切换。没有「只编其中一个」的构建。
//
// mode_overlay.h 给出各模式私有代码/数据段的属性宏（overlay 段）。
#include "mode_overlay.h"

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

// CDC（COM 口 <-> USART4，PB0=TX / PB1=RX）在两种模式下行为一致，
// 由 drv_uart_init() 与各自的周期性服务函数推进。
#include "drv/drv_uart.h"

#include <usbd_core.h>

#include "dap_main.h"
#include "drv/drv_mode_button.h"
#include "drv/drv_mode_store.h"
#include "wchlink/transport/rvswd_phy_gpio.h"
#include "wchlink/usb/wchlink_usb.h"

// ---------------------------------------------------------------------------
// RAM overlay（见 src/mode_overlay.h 与
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

// 按键一按下就退出当前模式，回到 main 交出 USB 再切到另一种模式
static inline bool mode_should_exit(void) {
    return drv_mode_button_pressed();
}

// 注意：本函数自身就在 overlay 段（.highcode.dap）里，所以**不能**在这里调用
// dap_overlay_init()：那等于先跳进尚未初始化的地址再执行。overlay 的初始化必须由
// 不参与 overlay 的 main() 完成（见下方 main 的循环）。
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) DAP_HIGHCODE_SECTION;
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) {
    chry_dap_init(0u, 0u);

    while (!mode_should_exit()) {
        chry_dap_handle();
        chry_dap_cdc_bridge();
    }
    // 模式切换时先回到 main（交出 USB 也在那里做），确保 overlay 被覆盖前
    // 已经不在执行 overlay 里的代码。
}

static void wchlink_process(void) {
    // RVSWD 物理层（PA2=SWCLK、PA3=SWDIO）先配置好，再启动 USB。
    // 本板没有 MUX / 按键 / 状态灯 / 目标电源开关，对应 glue 是空实现。
    // wchlink_overlay_init() 同样必须由 main() 调用（rvswd_phy_gpio_* 在 overlay 里）。
    rvswd_phy_gpio_init();
    wchlink_usb_init();

    while (!mode_should_exit()) {
        wchlink_usb_process();
    }
}

// 进入一个模式并一直跑到按键要求退出。本函数在 .text（main 不参与 overlay），
// 所以可以安全地先做该模式的 overlay 初始化、再跳进 overlay 里的 *_process()。
// 新增模式时**只在这里加一个 case**（枚举定义见 drv/drv_mode_store.h）。
static void mode_run(firmware_mode_t mode) {
    switch (mode) {
    case FIRMWARE_MODE_CMSIS_DAP:
        // overlay 的初始化必须在**进入 overlay 代码之前**完成，所以放在这里
        // （main 在 .text，不参与 overlay），不能放进 cherrydap_process() 内部。
        dap_overlay_init();
        cherrydap_process();
        break;

    case FIRMWARE_MODE_WCHLINK:
        wchlink_overlay_init();
        wchlink_process();
        break;

    default:
        break;
    }
}

// 轮转到下一个模式。取模实现：新模式只需在 firmware_mode_t 里追加成员，
// 这里无需改动（FIRMWARE_MODE_COUNT 是哨兵）。
static firmware_mode_t mode_next(firmware_mode_t mode) {
    return (firmware_mode_t)(((unsigned)mode + 1u) % (unsigned)FIRMWARE_MODE_COUNT);
}

int main(void) {
    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    bsp_delay_ms(10u);

    // COM 口 <-> USART4 (PB0=TX, PB1=RX)，默认 115200；
    // 两种模式下行为一致，主机改 CDC 行编码时波特率会跟着变
    drv_uart_init(0u);

    // 模式枚举与持久化的编码共用 firmware_mode_t（见 drv/drv_mode_store.h）：
    // 枚举值就是写进用户选择字 Data0 的值。新增模式 = 枚举末尾追加成员 +
    // 在 mode_run() 里加一个 case，主循环本身不用改。
    firmware_mode_t mode = FIRMWARE_MODE_CMSIS_DAP;

    drv_mode_button_init();

    // 上电时从用户选择字 Data0 恢复上次保存的模式；没有有效记录（全新芯片、
    // 擦除态或校验失败）就保持默认的 CMSIS-DAP。
    drv_mode_store_load(&mode);

    for (;;) {
        // 进入当前模式并跑到按键要求退出（内部会先做该模式的 overlay 初始化）。
        mode_run(mode);

        // 交出 USB：下一个模式会用自己的描述符重新注册并初始化。
        // 此时已经退出 overlay 代码，可以安全覆盖那段 RAM。
        usbd_deinitialize(0u);

        mode = mode_next(mode);

        // 把新选的模式写回用户选择字 Data0，供下次上电恢复。写失败（读保护开启 /
        // 擦写失败）只影响持久化，本次运行仍用刚切换过去的模式。
        drv_mode_store_save(mode);

        // 同一次按住不要立刻触发反向切换。此时 USB 已反初始化，没有需要服务的
        // 端点，短暂自旋是安全的。
        drv_mode_button_wait_release();
    }
}
