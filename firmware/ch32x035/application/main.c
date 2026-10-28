#include "bsp/bsp_delay.h"
#include "bsp/bsp_system.h"

#include <ch32x035.h>

// 调试器模式：
//   CMSIS-DAP               固定编入
//   WCH-Link（RVSWD）       仅 --wchlink=y 时编入（xmake 传 WCHLINK_ENABLE=1）
//
// 调试器模式**不能原地切换**：两种模式的 USB 身份完全不同（0d28:0204 WinUSB
// vs 1a86:8010 厂商接口），换模式必然重新枚举，所以切完就软复位重来。
//
// 切换手势：
//   * 上电时长按 PB3 超过 1.2 s -> 进 UF2 bootloader（拖 firmware.uf2 升级），
//     由 bootloader 自己判定（见 bootloader/src/main.c）
//   * 运行期双击 PB3 -> 切串口模式（单模式固件）/ 切调试器模式（双模式固件，
//     见下方 service_button）
//
// mode_overlay.h 给出各模式私有代码/数据段的属性宏（只有双模式才有 overlay 段）。
#include "mode_overlay.h"

#include "boot_info.h"

// CDC（COM1 <-> USART4，PB0=TX / PB1=RX；COM2 <-> USART2，PA2/PA3）由
// drv_uart_init() / drv_uart_mode_init() 配置，之后由各自的周期性服务推进。
#include "drv/drv_uart.h"

#include <usbd_core.h>

#include "dap_main.h"
#include "drv/drv_control.h"
#include "drv/drv_led.h"
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

// 主循环每轮都要推进按键状态机（去抖 + 双击判定）。双击的含义随固件而异：
//
//   单模式（只有 CMSIS-DAP）：切换串口模式。它决定 USB 描述符里有几个 CDC
//     接口（单串口 + 流控 = 只有 COM1，双串口 = COM1 + COM2），所以必须
//     重新枚举：usbd_deinitialize -> 切换引脚/USART2 -> 重新注册描述符与端点。
//
//   双模式（CMSIS-DAP + WCH-Link）：切换调试器模式。那个固件放不下双串口，
//     串口固定单串口，切换串口模式的手势本来就空着；两种调试器的 USB 身份
//     完全不同（0d28:0204 WinUSB vs 1a86:8010 厂商接口），只能复位后重来，
//     所以交给 drv_control 先落选项字再软复位。
//
// 两种都只影响持久化：动作失败也保持已经切过去的运行状态。
static void service_button(void) {
    (void)drv_mode_button_pressed();
    if (!drv_mode_button_take_double_click()) {
        return;
    }

#if WCHLINK_ENABLE
    {
        firmware_mode_t next = (drv_control_runtime_mode() == FIRMWARE_MODE_CMSIS_DAP)
                                   ? FIRMWARE_MODE_WCHLINK
                                   : FIRMWARE_MODE_CMSIS_DAP;

        // 复位动作由 drv_control_service() 在应答/主循环稍后执行
        (void)drv_control_request(DRV_CONTROL_APPLY_DEBUGGER, (uint32_t)next);
    }
#else
    usbd_deinitialize(0u);
    drv_uart_mode_toggle();
    chry_dap_usb_start();
#endif
}

// 注意：本函数自身就在 overlay 段（.highcode.dap）里，所以**不能**在这里调用
// dap_overlay_init()：那等于先跳进尚未初始化的地址再执行。overlay 初始化由
// 不参与 overlay 的 main() 完成。
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) DAP_HIGHCODE_SECTION;
static void __attribute__((noinline, optimize("O2"))) cherrydap_process(void) {
    chry_dap_init(0u, 0u);

    // 走到这里说明 USB 已初始化完成：把路标置成 0xFF（bootloader 下次启动会打印
    // 「上次应用停在 stage 255」，也就是「应用跑起来了」）
    boot_info_stage(0xFFu);

    for (;;) {
        chry_dap_handle();
        chry_dap_cdc_bridge();
        service_button();
        // vendor command 排队的延迟动作（切串口模式后重新枚举 / 进 UF2 / 复位）
        drv_control_service();
        // 状态灯：活动指示超时、LED 模式切换
        drv_led_service();
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
        // 两颗灯是板级资源，WCH-Link 模式也用同一套 LED 模式
        drv_led_service();
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
    drv_led_mode_t led_mode = DRV_LED_MODE_STATUS;

    SystemInit();
    boot_info_stage(1u); /* 走到 main 了（startup 的段拷贝/清 bss 都完成了） */

    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    bsp_delay_ms(10u);
    boot_info_stage(2u);

    // 两颗状态灯（PB11 / PB12）统一由 drv_led 管；必须在 drv_uart_mode_init()
    // 之前初始化 —— 后者会把「是否双串口」告诉 LED 模块。
    drv_led_init();
    if (drv_mode_store_load_led_mode(&led_mode)) {
        drv_led_set_mode(led_mode);
    }
    boot_info_stage(3u);

    // COM1 <-> USART4 (PB0=TX, PB1=RX)，默认 115200；
    // 主机改 CDC 行编码时波特率会跟着变
    drv_uart_init(0u);
    boot_info_stage(4u);

    // 按键：上拉输入 + 复位去抖/双击状态机
    drv_mode_button_init();

    // PA2/PA3 的用途（DTR/RTS 或 USART2）+ 从选项字恢复串口模式
    drv_uart_mode_init();
    boot_info_stage(5u);

    // 调试器模式：上电默认 CMSIS-DAP，用户选择字 Data0 里有有效记录就按记录走。
    // 模式枚举与持久化的编码共用 firmware_mode_t（见 drv/drv_mode_store.h）：
    // 枚举值就是写进 Data0 的值。新增模式 = 枚举末尾追加成员 + mode_run() 加 case。
    (void)drv_mode_store_load(&mode);

#if WCHLINK_ENABLE
    // 双模式固件：两种调试器都在，按选项字记录的走。
    // 切换方式是**运行期双击 PB3**（见 service_button）—— 不再用「上电时按住」，
    // 因为那个手势已经归 bootloader 的「长按进 UF2」了。
#else
    // 只编了 CMSIS-DAP：忽略可能是 WCH-Link 的历史记录
    mode = FIRMWARE_MODE_CMSIS_DAP;
#endif

    // vendor command 需要知道「现在跑的是哪种调试器」（查询/比较用）
    drv_control_init(mode);
    boot_info_stage(6u);

    // 进入选定的调试器模式（不返回）。进入后 cherrydap_process() 会把路标置 0xFF。
    // 串口模式（DTR/RTS <-> 双串口，双模式固件里固定单串口）由按键双击在运行期
    // 切换，与调试器模式无关。
    mode_run(mode);

    for (;;) {
    }
}
