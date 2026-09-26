#pragma once

#include <stdbool.h>
#include <stdint.h>

// USB CDC <-> USART 双向桥的底层串口驱动。
//
// 引脚：USART4 的 PB0 (TX) / PB1 (RX)。
// PA2/PA3 是 SWCLK/SWDIO，已给 CMSIS-DAP 用了；PB0/PB1 是硬件留出的另一组
// USART4 引脚（原理图网络名 PB0_USART4_TX / PB1_USART4_RX）。
//
// 收发各一个 FIFO（CherryRB），两个方向都是「中断生产、中断/主循环消费」：
//   TX FIFO: 生产者 = USB 中断（CDC OUT 回调），消费者 = USART4 中断
//   RX FIFO: 生产者 = USART4 中断，                 消费者 = 主循环
// 两级缓冲解耦了 USB 的突发速度与 UART 的波特率速度。

// 初始化 USART4。baudrate 传 0 时用默认 115200。
void drv_uart_init(uint32_t baudrate);

// 运行时改波特率（主机设置 CDC 行编码时调用）
void drv_uart_set_baudrate(uint32_t baudrate);
uint32_t drv_uart_baudrate(void);

// 非阻塞发送：写入 TX FIFO，返回实际入队的字节数（FIFO 满时可能少于 length）
//
// 注意：本函数在 USB 中断上下文（CDC OUT 回调）里被调用，**不能阻塞**。
// 防止丢字节靠调用方做背压：先查 drv_uart_tx_free() 确认放得下整包，
// 再重挂端点。详见 drv_uart_tx_free() 与本工程 dap_main.c 的
// chry_dap_cdc_bridge()。
uint32_t drv_uart_write(const uint8_t *data, uint32_t length);

// 从 RX FIFO 取数据，返回实际取出的字节数
uint32_t drv_uart_read(uint8_t *data, uint32_t length);

// RX FIFO 中可读字节数
uint32_t drv_uart_rx_available(void);

// TX FIFO 剩余可写空间。
//
// 这是 USB 侧做背压的依据：CDC OUT 回调在重挂端点前先看这个值。
// USB bulk OUT 天然带流控 —— 端点不重挂，主机就只会收到 NAK，它的
// write() 会自然阻塞在串口波特率上，一个字节都不会丢。反过来若无条件
// 重挂，USB 会以 MB/s 级速度灌进来，超出的字节只能丢弃。
uint32_t drv_uart_tx_free(void);

// TX FIFO 是否已排空（含移位寄存器）
bool drv_uart_tx_idle(void);

// 丢数据计数。
//   rx_overflow = 中断来不及搬（主循环取太慢），丢的是新到的字节
//   tx_overflow = TX FIFO 满（USB 灌太快），丢的是新入队的字节
//
// 注意：CDC OUT 侧已做背压（FIFO 余量不足就不重挂端点），正常收发下
// tx_overflow 应当恒为 0。它不再增长才是预期状态；一旦增长即表示背压失效。
uint32_t drv_uart_rx_overflow_count(void);
uint32_t drv_uart_tx_overflow_count(void);
