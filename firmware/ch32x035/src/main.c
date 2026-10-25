#include "bsp/bsp_delay.h"
#include "bsp/bsp_system.h"

#include <ch32x035.h>

// 调试器模式：
//   CMSIS-DAP               固定编入
//   WCH-Link（RVSWD）       仅 --wchlink=y 时编入（xmake 传 WCHLINK_ENABLE=1）
//
// 调试器模式**不在运行期切换**：两种模式的 USB 身份完全不同（0d28:0204 WinUSB
// vs 1a86:8010 厂商接口），切换必然重新枚举，误触代价高。现在改成「上电时按住
// PB3」进入另一种模式，并把结果写进用户选择字 Data0（见下方 main()）。
//
// 运行期按键只做一件事：**双击**切换桥接串口模式（串口1+DTR/RTS <-> 双串口），
// 见 src/drv/drv_uart.c 的模式小节。
//
// mode_overlay.h 给出各模式私有代码/数据段的属性宏（只有双模式才有 overlay 段）。
#include "mode_overlay.h"

// CDC（COM1 <-> USART4，PB0=TX / PB1=RX；COM2 <-> USART2，PA2/PA3）由
// drv_uart_init() / drv_uart_mode_init() 配置，之后由各自的周期性服务推进。
#include "drv/drv_uart.h"

#include <usbd_core.h>

#include "dap_main.h"
#include "drv/drv_mode_button.h"
#include "drv/drv_mode_store.h"

#if WCHLINK_ENABLE
#include "wchlink/transport/rvswd_phy_gpio.h"
#include "wchlink/usb/wchlink_usb.h"
#endif

// ---------------------------------------------------------------------------
// RAM overlay（仅 --wchlink=y 的双模式固件；见 src/mode_overlay.h 与
// sdk/Ld/Link_highcode_nv_256B_dual.ld）
// ---------------------------------------------------------------------------
// 两种模式的私有**数据**被链接到同一段 RAM 地址：它们不同时运行（USB 身份不同），
// 所以可以复用。这段地址不是 .bss，启动代码不会清零，进模式前必须显式清。
//
// 代码不走这条路：CMSIS-DAP 的热路径并进常驻 .highcode（由 startup .S 在启动时
// 拷一次），WCH-Link 的热路径直接编在 Flash 上执行，所以运行期**没有代码拷贝**。
//
// 单模式构建（默认）的链接脚本里没有 overlay 段，这一整块不编入。
#if WCHLINK_ENABLE
extern uint32_t _dap_overlay_bss_start[];
extern uint32_t _dap_overlay_bss_end[];

extern uint32_t _wchlink_overlay_bss_start[];
extern uint32_t _wchlink_overlay_bss_end[];

// 清零本模式的数据段（没有代码拷贝，见上面的说明）
static void overlay_bss_clear(uint32_t *start, const uint32_t *end) {
    for (uint32_t *dst = start; dst != end; ++dst) {
        *dst = 0u;
    }
}

static void dap_overlay_init(void) {
    overlay_bss_clear(_dap_overlay_bss_start, _dap_overlay_bss_end);
}

static void wchlink_overlay_init(void) {
    overlay_bss_clear(_wchlink_overlay_bss_start, _wchlink_overlay_bss_end);
}
#endif

// 主循环每轮都要推进按键状态机（去抖 + 双击判定）；双击则切换串口模式。
//
// 串口模式决定 USB 描述符里有几个 CDC 接口（单串口 + 流控 = 只有 COM1，
// 双串口 = COM1 + COM2），所以切换必须**重新枚举**：
//   usbd_deinitialize -> 切换引脚/USART2 -> 重新注册描述符、接口、端点
// 代价是主机侧会看到一次插拔（两个串口一起消失再出现），以及擦写选项字期间
// 几十毫秒不服务 USB —— 双击本身就是明确的操作，可以接受。
static void service_button(void) {
    (void)drv_mode_button_pressed();
    if (drv_mode_button_take_double_click()) {
        usbd_deinitialize(0u);
        drv_uart_mode_toggle();
        chry_dap_usb_start();
    }
}

// 注意：本函数自身就在 overlay 段（.highcode.dap）里，所以**不能**在这里调用
// dap_overlay_init()：那等于先跳进尚未初始化的地址再执行。overlay 初始化由
// 不参与 overlay 的 main() 完成。
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) DAP_HIGHCODE_SECTION;
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) {
    chry_dap_init(0u, 0u);

    for (;;) {
        chry_dap_handle();
        chry_dap_cdc_bridge();
        service_button();
    }
}

#if WCHLINK_ENABLE
static void wchlink_process(void) {
    // RVSWD 物理层（PA5=SWCLK、PA6=SWDIO）先配置好，再启动 USB。
    // 本板没有 MUX / 状态灯 / 目标电源开关，对应 glue 是空实现。
    // wchlink_overlay_init() 同样必须由 main() 调用（rvswd_phy_gpio_* 在 overlay 里）。
    rvswd_phy_gpio_init();
    wchlink_usb_init();

    for (;;) {
        wchlink_usb_process();
    }
}
#endif

// 进入一个模式并一直跑到复位：调试器模式不再运行期切换（见 main()）。
// 本函数在 .text（main 不参与 overlay），所以可以安全地先做 overlay 初始化、
// 再跳进 overlay 里的 *_process()。
// 新增模式时**只在这里加一个 case**（枚举定义见 drv/drv_mode_store.h）。
static void mode_run(firmware_mode_t mode) {
    switch (mode) {
    case FIRMWARE_MODE_CMSIS_DAP:
#if WCHLINK_ENABLE
        // overlay 的初始化必须在**进入 overlay 代码之前**完成，所以放在这里
        // （main 在 .text，不参与 overlay），不能放进 cherrydap_process() 内部。
        dap_overlay_init();
#endif
        cherrydap_process();
        break;

#if WCHLINK_ENABLE
    case FIRMWARE_MODE_WCHLINK:
        wchlink_overlay_init();
        wchlink_process();
        break;
#endif

    default:
        // 没编入的模式（例如只编 CMSIS-DAP 时选项字里残留的 WCH-Link）：
        // 退回 CMSIS-DAP
        cherrydap_process();
        break;
    }
}

int main(void) {
    firmware_mode_t mode = FIRMWARE_MODE_CMSIS_DAP;

    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    bsp_delay_ms(10u);

    // COM1 <-> USART4 (PB0=TX, PB1=RX)，默认 115200；
    // 主机改 CDC 行编码时波特率会跟着变
    drv_uart_init(0u);

    // 按键：上拉输入，并采样「上电时是否按住」（用于选调试器模式）
    drv_mode_button_init();

    // PA2/PA3 的用途（DTR/RTS 或 USART2）+ PB12 指示灯 + 从选项字恢复串口模式
    drv_uart_mode_init();

    // 调试器模式：上电默认 CMSIS-DAP，用户选择字 Data0 里有有效记录就按记录走。
    // 模式枚举与持久化的编码共用 firmware_mode_t（见 drv/drv_mode_store.h）：
    // 枚举值就是写进 Data0 的值。新增模式 = 枚举末尾追加成员 + mode_run() 加 case。
    (void)drv_mode_store_load(&mode);

#if WCHLINK_ENABLE
    // 上电时按住按键 = 切到「另一种」调试器模式，并持久化。
    // 用上电按住而不是运行期按键，是因为切模式必然要重新枚举 USB。
    if (drv_mode_button_held_at_boot()) {
        mode = (mode == FIRMWARE_MODE_CMSIS_DAP) ? FIRMWARE_MODE_WCHLINK : FIRMWARE_MODE_CMSIS_DAP;
        (void)drv_mode_store_save(mode);
    }
#else
    // 只编了 CMSIS-DAP：忽略可能是 WCH-Link 的历史记录
    mode = FIRMWARE_MODE_CMSIS_DAP;
#endif

    // 进入选定的调试器模式（不返回）。模式内部的串口模式（DTR/RTS <-> 双串口）
    // 由按键双击在运行期切换，与调试器模式无关。
    mode_run(mode);

    for (;;) {
    }
}
