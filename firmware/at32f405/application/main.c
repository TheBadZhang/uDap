/**
 * @file    main.c
 * @brief   uDap（AT32F405KBU7）：USB 调试器 + 串口桥 复合设备
 *
 * 产品形态（与 ch32x035 版一致，见仓库根 readme.md）：
 *   一个 USB 设备里同时提供
 *     - CMSIS-DAP 调试口（WinUSB，VID:PID = 0d28:0204，SWD/JTAG 都支持）
 *     - 一路 CDC ACM 虚拟串口，桥到 USART2（PA2/PA3）
 *   功能由上位机通过 CMSIS-DAP vendor command 配置（串口模式 / LED 模式 /
 *   调试器模式 / 进 bootloader / 读 UID），配置存在用户系统数据区（USD）。
 *
 * 启动顺序（每一步都有依赖，不能随便换）：
 *   1. system_clock_config()  216 MHz（12 MHz HEXT -> PLL）；
 *      USB HS PHY 的 12 MHz 参考时钟就是从这颗晶振来的，所以必须先用 HEXT。
 *   2. bsp_delay_init / bsp_time_init：延时 + 毫秒时基；
 *   3. drv_led_init：先把两颗灯配成熄灭，避免上电闪一下；
 *   4. 从 USD 读回 LED 模式；
 *   5. drv_uart_init：桥接串口（115200 只是默认，主机可用 CDC 改）；
 *   6. bsp_usb_clock_init：OTGHS/PHY 时钟 + 48 MHz PLLU + NVIC；
 *   7. chry_dap_init：注册描述符/端点并启动 USB（DAP 层）；
 *   8. 主循环：DAP 命令处理 + CDC 桥接 + LED 刷新 + 延迟动作派发。
 */

#include "at32f402_405.h"

#include "board_pin_map.h"

#include "bsp/bsp_time.h"
#include "bsp/bsp_usb.h"
#include "drv/drv_control.h"
#include "drv/drv_led.h"
#include "drv/drv_mode_store.h"
#include "drv/drv_uart.h"

#include "dap/dap_main.h"

/* sdk/bsp 提供的延时/时钟（cw32 风格模板的一部分） */
extern void system_clock_config(void);
extern void bsp_delay_init(uint32_t hclk);
extern void bsp_delay_ms(uint32_t ms);

/* sdk/bsp/syscalls.c 里的 printf 出口钩子：接到桥接串口上 */
extern void (*g_sys_console_putc)(uint8_t byte);

/* 上电自检：两颗灯快闪两下，表示固件起来了（约 300 ms，不阻塞 USB 枚举太久） */
static void power_on_blink(void) {
    for (uint32_t i = 0u; i < 2u; i++) {
        drv_led_force(true, true);
        bsp_delay_ms(60u);
        drv_led_force(false, false);
        bsp_delay_ms(60u);
    }
}

int main(void) {
    /* ---- 1. 时钟：216 MHz，HEXT 12 MHz -> PLL ---- */
    system_clock_config();

    /* ---- 2. 时基 ---- */
    bsp_delay_init(system_core_clock);
    bsp_time_init();

    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

    /* ---- 3/4. LED 与保存的 LED 模式 ---- */
    drv_led_init();
    {
        drv_led_mode_t led_mode = DRV_LED_MODE_STATUS;
        if (drv_mode_store_load_led_mode(&led_mode)) {
            drv_led_set_mode(led_mode);
        }
    }

    /* ---- 5. 桥接串口 ---- */
    drv_uart_init(115200u);
    drv_uart_mode_init();
    /* printf 输出到 USART2（不设这个钩子 _write 会把字符丢掉） */
    g_sys_console_putc = drv_uart_putc;

    /* ---- 6. USB 时钟 ---- */
    bsp_usb_clock_init();

    /* 调试器模式（当前只有 CMSIS-DAP 一种，读回值仅用于 vendor 查询） */
    {
        firmware_mode_t mode = FIRMWARE_MODE_CMSIS_DAP;
        (void)drv_mode_store_load(&mode);
        drv_control_init(FIRMWARE_MODE_CMSIS_DAP);
    }

    /* ---- 7. 启动 USB（DAP + CDC 复合设备） ---- */
    chry_dap_init(0u, (uint32_t)BOARD_USB_BASE);

    power_on_blink();

    /* ---- 8. 主循环 ---- */
    for (;;) {
        chry_dap_handle();     /* DAP 命令包处理（含 vendor command） */
        chry_dap_cdc_bridge(); /* CDC <-> USART2 搬运 */
        drv_led_service();     /* 活动指示灯 / 模式变化 */
        drv_control_service(); /* 延迟动作（复位 / 进 bootloader） */
    }
}
