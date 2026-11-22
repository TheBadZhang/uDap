#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 板载两颗 LED（引脚见 board_pin_map.h 的 BOARD_LED1/LED2，**低电平点亮**）
 * 的功能切换。
 *
 *   DRV_LED_MODE_STATUS    LED1 = DAP 连接指示，LED2 = 双串口指示（默认）
 *   DRV_LED_MODE_ACTIVITY  LED1 = COM1 收发活动，LED2 = COM2 收发活动
 *   DRV_LED_MODE_MIXED     LED1 = DAP 连接指示，LED2 = COM1 收发活动
 *   DRV_LED_MODE_OFF       两颗都灭
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

/* 当前是不是双串口模式（= LED2 在 STATUS 模式下的语义） */
void drv_led_set_uart_dual(bool dual);

/* 串口有收发：uart_index 传 1（COM1）或 2（COM2）。可以在中断里调用。 */
void drv_led_pulse_uart(uint8_t uart_index);

/* 每轮主循环调用一次：处理活动指示灯的超时与模式变化 */
void drv_led_service(void);

/* 供 main 里的「上电指示」用：两颗灯同时亮/灭（不影响模式状态） */
void drv_led_force(bool led1_on, bool led2_on);
