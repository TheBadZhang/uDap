#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 板载两颗 LED（引脚见 application/board_pin_map.h 的 BOARD_LED_*，**低电平点亮**）
 * 的功能切换。
 *
 * 以前两颗灯是写死的：
 *   DAP 灯    = DAP 连接指示（CMSIS-DAP 的 Connect LED，由调试器软件置位）
 *   串口灯    = 串口模式指示（亮 = 双串口）
 * 但不同人用这块板子的方式不同：有人只当调试器（灯最好别闪），有人主要当
 * 串口桥（灯用来提示收发），有人把板子塞进壳里根本看不见。所以改成 4 种
 * 模式，由 vendor command 或选项字持久化切换（见 src/dap/udap_vendor.h）。
 *
 *   DRV_LED_MODE_STATUS    DAP 灯 = DAP 连接，串口灯 = 双串口（历史默认行为）
 *   DRV_LED_MODE_ACTIVITY  DAP 灯 = COM1 收发，串口灯 = COM2 收发（串口桥模式）
 *   DRV_LED_MODE_MIXED     DAP 灯 = DAP 连接，串口灯 = COM1 收发
 *   DRV_LED_MODE_OFF       两颗都灭（避免漏光干扰目标板 / 省电）
 *
 * 谁调用谁负责：
 *   - DAP_config.h 的 LED_CONNECTED_OUT() -> drv_led_set_dap_connected()
 *   - drv_uart.c 的串口模式切换           -> drv_led_set_uart_dual()
 *   - dap_main.c 的 CDC 桥接              -> drv_led_pulse_uart()
 *   - 主循环每轮                          -> drv_led_service()
 */

typedef enum {
    DRV_LED_MODE_STATUS = 0,
    DRV_LED_MODE_ACTIVITY = 1,
    DRV_LED_MODE_MIXED = 2,
    DRV_LED_MODE_OFF = 3,
    DRV_LED_MODE_COUNT,
} drv_led_mode_t;

/* 配好两颗灯的 GPIO（推挽输出、初始熄灭）。上电时调用一次。 */
void drv_led_init(void);

/* 切换 LED 模式（只改表现，不写 Flash；持久化由 drv_mode_store 负责） */
void drv_led_set_mode(drv_led_mode_t mode);
drv_led_mode_t drv_led_mode(void);

/* DAP 主机是否已连接（CMSIS-DAP 的 HostStatus 命令） */
void drv_led_set_dap_connected(bool connected);

/* 当前是不是双串口模式（= 串口灯在 STATUS 模式下的语义） */
void drv_led_set_uart_dual(bool dual);

/* 串口有收发：uart_index 传 1（COM1）或 2（COM2）。
 * 可以在中断里调用（只置一个标志），实际的亮灯时长由 drv_led_service() 计算。 */
void drv_led_pulse_uart(uint8_t uart_index);

/* 每轮主循环调用一次：处理活动指示灯的超时与模式变化 */
void drv_led_service(void);
