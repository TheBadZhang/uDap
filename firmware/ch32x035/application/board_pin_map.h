#pragma once

#include <ch32x035.h>

/*
 * 板级引脚映射（uDap-CH32X035F8U6）。
 *
 * 本文件只管「板上接了哪些信号、接在哪」，不含任何驱动逻辑：
 *   - UART1（桥接串口 1）：USART4，PB0 = TX / PB1 = RX
 *   - PA2 / PA3：**双用途**，由运行期模式决定（见 src/drv/drv_uart.h）
 *       · 流控模式：PA2 = DTR、PA3 = RTS（推挽输出，有效 = 低）
 *       · 双串口模式：PA2 = USART2_TX（TX2）、PA3 = USART2_RX（RX2）
 *     CH32X035 的 USART2 默认引脚就是 PA2/PA3，无需重映射。
 *   - 模式按键：PB3，一端接 GND，内部上拉，**低电平 = 按下**
 *   - LED：PB11 = LED 1、PB12 = LED 2，**低电平点亮**。
 *     两颗灯的具体含义由 LED 模式决定（见 src/drv/drv_led.h）：
 *     默认 PB11 = DAP 连接指示、PB12 = 串口模式指示（亮 = 双串口）。
 *   - SWD 调试口：PA5 = SWCLK / PA7 = SWDIO（见 src/swd_pin_map.h）
 *
 * 注意 CH32X035 的 GPIO 配置寄存器分两段：CFGLR 管 pin0..7、CFGHR 管 pin8..15。
 * SDK 的 GPIO_Init() 对 CFGHR 走静态缓存（CFGHR_tmpA/B/C），**直接写 CFGHR
 * 会让缓存失步**；CFGLR 没有缓存，可以直接写。所以本工程约定：
 *   pin0..7  -> 允许裸写 CFGLR（DAP/SWD 快路径需要）
 *   pin8..15 -> 一律走 GPIO_Init()
 */

/* ---------------- 桥接串口 1：USART4 ---------------- */
#define BOARD_UART1_INSTANCE   USART4
#define BOARD_UART1_IRQN       USART4_IRQn
#define BOARD_UART1_CLK        RCC_APB1Periph_USART4
#define BOARD_UART1_PORT       GPIOB
#define BOARD_UART1_PORT_CLK   RCC_APB2Periph_GPIOB
#define BOARD_UART1_TX_PIN     GPIO_Pin_0
#define BOARD_UART1_RX_PIN     GPIO_Pin_1
#define BOARD_UART1_TX_CFGLR_SHIFT 0
#define BOARD_UART1_RX_CFGLR_SHIFT 4

/* ---------------- PA2 / PA3：DTR+RTS 或 USART2 ---------------- */
#define BOARD_AUX_PORT       GPIOA
#define BOARD_AUX_PORT_CLK   RCC_APB2Periph_GPIOA
#define BOARD_AUX_DTR_BIT    2
#define BOARD_AUX_RTS_BIT    3
#define BOARD_AUX_DTR_PIN    (1UL << BOARD_AUX_DTR_BIT)
#define BOARD_AUX_RTS_PIN    (1UL << BOARD_AUX_RTS_BIT)
#define BOARD_AUX_DTR_CFGLR_SHIFT (BOARD_AUX_DTR_BIT * 4)
#define BOARD_AUX_RTS_CFGLR_SHIFT (BOARD_AUX_RTS_BIT * 4)

/* USART2 默认引脚即 PA2(TX) / PA3(RX)（参考手册 USART 章节的引脚表） */
#define BOARD_AUX_UART_INSTANCE   USART2
#define BOARD_AUX_UART_IRQN       USART2_IRQn
#define BOARD_AUX_UART_CLK        RCC_APB1Periph_USART2
#define BOARD_AUX_UART_TX_PIN     BOARD_AUX_DTR_PIN
#define BOARD_AUX_UART_RX_PIN     BOARD_AUX_RTS_PIN
#define BOARD_AUX_UART_TX_SHIFT   BOARD_AUX_DTR_CFGLR_SHIFT
#define BOARD_AUX_UART_RX_SHIFT   BOARD_AUX_RTS_CFGLR_SHIFT

/* ---------------- 模式按键：PB3 ---------------- */
#define BOARD_BTN_PORT       GPIOB
#define BOARD_BTN_PORT_CLK   RCC_APB2Periph_GPIOB
#define BOARD_BTN_PIN        GPIO_Pin_3
#define BOARD_BTN_ACTIVE_LOW 1

/* ---------------- LED：PB11 / PB12 ---------------- */
#define BOARD_LED_PORT       GPIOB
#define BOARD_LED_PORT_CLK   RCC_APB2Periph_GPIOB
#define BOARD_LED_DAP_PIN    GPIO_Pin_11
#define BOARD_LED_UART_PIN   GPIO_Pin_12
#define BOARD_LED_ACTIVE_LOW 1
