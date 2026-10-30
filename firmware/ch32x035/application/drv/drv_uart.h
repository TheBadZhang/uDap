#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 是否编入串口桥（USB CDC + USART）。
 *
 *   y（默认）  两个 USB 接口块：CMSIS-DAP + CDC（单串口或双串口）
 *   n          只有 CMSIS-DAP 的 vendor bulk 接口，固件里没有 CDC、没有 USART、
 *              没有 DMA 环形缓冲（--uart=n）。用于「只要最快 SWD」的场景：
 *              主机不会再轮询那几组 CDC 端点，主循环里也没有串口服务。
 *
 * 由 xmake 的 option("uart") 定义；头文件里给默认值，别的编译方式下也不会漏。
 */
#ifndef UART_ENABLE
#define UART_ENABLE 1
#endif

/*
 * 串口驱动（两路 UART + 流控脚 + 串口模式 + CDC 回调）——**单一入口**。
 *
 * 这一对文件（drv_uart.h / drv_uart.c）取代了原来的 6 个模块：
 *
 *   drv_uart_dma      RX 的 DMA 环形引擎（两路共用，现已 internal）
 *   drv_flow_pins     DTR / RTS 输出（低有效；板级可选）
 *   drv_uart          COM1：桥接串口 1
 *   drv_uart2         COM2：桥接串口 2（仅双串口模式）
 *   drv_uart_mode     串口模式切换 + 串口灯 + 选项字持久化
 *   drv_cdc_line      CDC ACM 行编码 / DTR / RTS 回调（两种调试器模式共用）
 *
 * 合并的理由：这几块共用同一组引脚（有 DTR/RTS 的板子上 COM2 与流控脚是同一对）、
 * 同一套 DMA 引擎、同一份「谁在占用引脚」的约定（drv_uart_mode），拆开时改一处
 * 往往要同步改三四处。
 *
 * ---------------------------------------------------------------------------
 * 引脚与模式（具体是哪两个脚见 application/board_pin_map.h 的板级表）
 * ---------------------------------------------------------------------------
 *   COM1   UART1 板级表 BOARD_UART1_*
 *          uDap-CH32X035F8U6：USART4  PB0 = TX / PB1 = RX
 *          uDap-CH32X033    ：USART2  PA2 = TX / PA3 = RX
 *   COM2   板级表 BOARD_AUX_UART_*（仅双串口模式）
 *          uDap-CH32X035F8U6：USART2  PA2 = TX2 / PA3 = RX2
 *          uDap-CH32X033    ：USART1  PA10 = TX2 / PA11 = RX2
 *   DTR/RTS  板级可选（BOARD_AUX_FLOW_PINS）：只有 uDap-CH32X035F8U6 有，
 *            就是 PA2/PA3，与 COM2 复用；uDap-CH32X033 没有流控脚。
 *
 * ---------------------------------------------------------------------------
 * 收发实现
 * ---------------------------------------------------------------------------
 *   两路 RX 都走 DMA 环形缓冲（通道由板级表给：USART4_RX = Ch8、
 *   USART2_RX = Ch6、USART1_RX = Ch5，各 1 KB，零拷贝）：逐字节 RXNE 中断在
 *   高波特率下会因 ISR 延迟来不及读 DATAR，硬件置 ORE 直接丢字节；改成 DMA
 *   后只剩「每半个缓冲一次」的中断。
 *
 *   两路 TX 都用逐字节 TXE 中断 + CherryRB 环形 FIFO。TX 挂 DMA 在本芯片上实测
 *   会让串口卡死（原因未定位），所以保持中断。
 *
 *   USB 侧的背压语义：drv_uart_tx_free() / drv_uart2_tx_free() 是 CDC OUT 端点
 *   是否重挂的判据（余量不足就不重挂，主机被 NAK 挡住自然降速）。
 */

/* ===========================================================================
 * COM1：桥接串口 1（引脚/实例由板级表 BOARD_UART1_* 决定）
 *   uDap-CH32X035F8U6：USART4，PB0 = TX / PB1 = RX
 *   uDap-CH32X033    ：USART2，PA2 = TX / PA3 = RX
 * =========================================================================== */

/* 初始化 COM1 的串口外设与引脚，并启动 RX 的 DMA。baudrate 传 0 用默认 115200。 */
void drv_uart_init(uint32_t baudrate);

/* 运行时改波特率（主机设置 CDC 行编码时调用） */
void drv_uart_set_baudrate(uint32_t baudrate);
uint32_t drv_uart_baudrate(void);

/* 非阻塞发送：写入 TX FIFO，返回实际入队的字节数（FIFO 满时可能少于 length）
 *
 * 注意：本函数在 USB 中断上下文（CDC OUT 回调）里被调用，**不能阻塞**。
 * 防止丢字节靠调用方做背压：先查 drv_uart_tx_free() 确认放得下整包再重挂端点。 */
uint32_t drv_uart_write(const uint8_t *data, uint32_t length);

/* 从 RX 环形缓冲取数据，返回实际取出的字节数 */
uint32_t drv_uart_read(uint8_t *data, uint32_t length);

/* RX 环形缓冲里可读字节数 */
uint32_t drv_uart_rx_available(void);

/* TX FIFO 剩余可写空间（USB 侧背压依据） */
uint32_t drv_uart_tx_free(void);

/* TX 是否已排空（含移位寄存器） */
bool drv_uart_tx_idle(void);

/* 丢数据计数：
 *   rx_overflow = 软件来不及取、被 DMA 绕圈覆盖掉的字节数
 *   tx_overflow = TX FIFO 满（USB 灌太快），正常路径下恒为 0 */
uint32_t drv_uart_rx_overflow_count(void);
uint32_t drv_uart_tx_overflow_count(void);

/* 重新对齐 RX 的 DMA 环形计数。
 * 擦写 Flash（写选项字）等长时间关中断之后必须调用一次，否则 DMA 在这段时间
 * 绕圈覆盖会让生产者位置失真，读出来的数据位置会错。 */
void drv_uart_rx_resync(void);

/* ===========================================================================
 * COM2：桥接串口 2（引脚/实例由板级表 BOARD_AUX_UART_* 决定，仅双串口模式）
 *   uDap-CH32X035F8U6：USART2，PA2 = TX2 / PA3 = RX2
 *   uDap-CH32X033    ：USART1，PA10 = TX2 / PA11 = RX2（USART1_RM = 01）
 * =========================================================================== */

/* 初始化 COM2 的串口外设与引脚（含需要的重映射），并启动 RX 的 DMA。
 * baudrate 传 0 用默认 115200。 */
void drv_uart2_init(uint32_t baudrate);

/* 停掉 COM2 并把引脚交出去（有 DTR/RTS 的板子上随后由 drv_flow_pins_enable 接管） */
void drv_uart2_deinit(void);

/* 当前是否已初始化（双串口模式）。未初始化时写/读都是空操作。 */
bool drv_uart2_ready(void);

/* 清空 TX FIFO 与溢出计数（切换模式时调用，避免把上一模式的数据带过去） */
void drv_uart2_reset(void);

void drv_uart2_set_baudrate(uint32_t baudrate);
uint32_t drv_uart2_baudrate(void);

uint32_t drv_uart2_write(const uint8_t *data, uint32_t length);
uint32_t drv_uart2_read(uint8_t *data, uint32_t length);
uint32_t drv_uart2_rx_available(void);
uint32_t drv_uart2_tx_free(void);
uint32_t drv_uart2_rx_overflow_count(void);
uint32_t drv_uart2_tx_overflow_count(void);

/* 同 drv_uart_rx_resync()，作用于 COM2 */
void drv_uart2_rx_resync(void);

/* ===========================================================================
 * DTR / RTS（**有效 = 低电平**，FT232 的 DTR#/RTS# 习惯；板级可选）
 * ===========================================================================
 * 主机通过 CDC 的 SET_CONTROL_LINE_STATE 下发；只在「单串口 + DTR/RTS」模式下
 * 有效 —— 双串口模式下这两脚已交给 COM2 的串口外设，set_* 自动变成空操作。
 * 板级表 BOARD_AUX_FLOW_PINS = 0 的板子（uDap-CH32X033）没有这两个脚，
 * 所有函数都是空操作、drv_flow_pins_dtr/rts() 恒为 false。 */

void drv_flow_pins_release(void); /* 释放为浮空输入（交出引脚） */
void drv_flow_pins_enable(void);  /* 推挽输出，并把已记录的状态重新打出去 */
void drv_flow_pins_set_dtr(bool asserted);
void drv_flow_pins_set_rts(bool asserted);

/* 当前记录的状态（true = 已断言 = 输出低）。引脚已释放时返回 false。 */
bool drv_flow_pins_dtr(void);
bool drv_flow_pins_rts(void);

/* ===========================================================================
 * 串口工作模式（第二路串口 / 流控脚那一对引脚的用途）
 * ===========================================================================
 *   DRV_UART_MODE_FLOW  「单串口 [+ DTR/RTS 流控]」
 *       COM1 <-> 板级表的 BOARD_UART1_*；有流控脚的板子上这些脚是 DTR/RTS。
 *       **USB 里只枚举 1 个 CDC**，COM2 完全不存在。
 *   DRV_UART_MODE_DUAL  「双串口」
 *       COM1 + COM2（板级表的 BOARD_AUX_UART_*）。
 *
 * 默认模式由板级表 BOARD_UART_DEFAULT_DUAL 决定（全新芯片/选项字无效时）：
 *   有 DTR/RTS 脚的板子 -> FLOW；没有的 -> DUAL。
 *
 * 模式由按键**双击**或 vendor command 切换，并存进用户选择字 Data1（掉电保存）；
 * 注意双模式固件（CMSIS-DAP + WCH-Link）里串口**固定**为单串口 + DTR/RTS，
 * 双击手势让给调试器模式切换（见 src/main.c）。
 * 切换会重新枚举 USB（描述符里的接口数变了），主机侧看到一次插拔。
 * 指示：由 LED 模块决定（见 src/drv/drv_led.h 的 DRV_LED_MODE_STATUS 模式：
 * 串口灯亮 = 双串口模式，灭 = 单串口模式，低电平点亮）。 */

typedef enum {
    DRV_UART_MODE_FLOW = 0, /* 单串口 [+ DTR/RTS] */
    DRV_UART_MODE_DUAL = 1, /* 双串口 */
    DRV_UART_MODE_COUNT,
} drv_uart_mode_t;

/* 上电初始化：从选项字恢复模式 + 按模式配置第二路串口/流控脚 */
void drv_uart_mode_init(void);

drv_uart_mode_t drv_uart_mode_get(void);
bool drv_uart_mode_is_dual(void);

/* 切换到指定模式并写入选项字持久化；返回 false 表示参数非法。
 * 调用方负责重新枚举 USB（描述符里的 CDC 接口数变了）。 */
bool drv_uart_mode_set(drv_uart_mode_t mode);

/* 切换到另一个模式，并写入选项字持久化。由按键双击触发。 */
void drv_uart_mode_toggle(void);

/* ===========================================================================
 * CDC ACM 行编码（两种调试器模式共用）
 * =========================================================================== */

/* 恢复默认行编码（115200 8N1）。两种模式各自初始化 USB 前调用。 */
void drv_cdc_line_init(void);
