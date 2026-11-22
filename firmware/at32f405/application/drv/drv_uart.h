#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 桥接串口（CDC <-> USART2）驱动。
 *
 * 引脚见 board_pin_map.h：PA2 = USART2_TX、PA3 = USART2_RX，AF7。
 *
 * 数据通路：
 *   USB CDC OUT  -> drv_uart_write()  -> TX 环形缓冲 -> USART TDBE 中断逐字节发出
 *   USART RDBF 中断 -> RX 环形缓冲 -> drv_uart_read() -> USB CDC IN
 * 两端都是环形缓冲 + 中断驱动，主循环只管搬运，不忙等。
 *
 * 关于「单串口 / 双串口」：
 * 上游 ch32x035 用 PA2/PA3 在「串口 + DTR/RTS」和「第二路串口」之间复用，
 * 因此有 DRV_UART_MODE_FLOW / DRV_UART_MODE_DUAL 两种模式。本板只有一对
 * PA2/PA3 串口引脚，第二路串口没有引脚可用，所以只保留单串口模式
 * （DRV_UART_MODE_COUNT 为 1，vendor command 请求双串口会收到 BAD_PARAM）。
 * DTR/RTS 复用 PB1/PB0（与 JTAG 的 TDI/TDO 同脚），JTAG 关闭时才可用。
 */

#ifndef UART_ENABLE
#define UART_ENABLE 1
#endif

typedef enum {
    /* 单串口：USART2 走 CDC1，PB1/PB0 作 DTR/RTS */
    DRV_UART_MODE_SINGLE = 0,
    DRV_UART_MODE_COUNT,
} drv_uart_mode_t;

/* ---------------------------------------------------------------- 基本收发 */
void drv_uart_init(uint32_t baudrate);
void drv_uart_set_baudrate(uint32_t baudrate);
uint32_t drv_uart_baudrate(void);

uint32_t drv_uart_write(const uint8_t *data, uint32_t length);

/* printf 的单字节出口（挂到 sdk/bsp/syscalls.c 的 g_sys_console_putc 上） */
void drv_uart_putc(uint8_t byte);
uint32_t drv_uart_read(uint8_t *data, uint32_t length);
uint32_t drv_uart_rx_available(void);
uint32_t drv_uart_tx_free(void);
bool drv_uart_tx_idle(void);
uint32_t drv_uart_rx_overflow_count(void);
uint32_t drv_uart_tx_overflow_count(void);
void drv_uart_rx_resync(void);

/* ------------------------------------------------- 第二路串口（本板未使用） */
void drv_uart2_init(uint32_t baudrate);
void drv_uart2_deinit(void);
bool drv_uart2_ready(void);
void drv_uart2_reset(void);
void drv_uart2_set_baudrate(uint32_t baudrate);
uint32_t drv_uart2_baudrate(void);
uint32_t drv_uart2_write(const uint8_t *data, uint32_t length);
uint32_t drv_uart2_read(uint8_t *data, uint32_t length);
uint32_t drv_uart2_rx_available(void);
uint32_t drv_uart2_tx_free(void);
uint32_t drv_uart2_rx_overflow_count(void);
uint32_t drv_uart2_tx_overflow_count(void);
void drv_uart2_rx_resync(void);

/* ------------------------------------------------------------ DTR/RTS 流控 */
void drv_flow_pins_release(void); /* 释放为浮空输入（交出引脚，JTAG 要 TDI/TDO） */
void drv_flow_pins_enable(void);  /* 推挽输出，并把已记录的状态重新打出去 */
void drv_flow_pins_set_dtr(bool asserted);
void drv_flow_pins_set_rts(bool asserted);
bool drv_flow_pins_dtr(void);
bool drv_flow_pins_rts(void);

/* -------------------------------------------------------- 串口模式（单串口） */
void drv_uart_mode_init(void);
drv_uart_mode_t drv_uart_mode_get(void);
bool drv_uart_mode_is_dual(void);
bool drv_uart_mode_set(drv_uart_mode_t mode);
void drv_uart_mode_toggle(void);
