/**
 * @file    board_pin_map.h
 * @brief   uDap(AT32F405KBU7) 板级引脚表
 *
 * 硬件连接（用户给定）：
 *   PB4  LED2（低电平点亮，uDap 上的 UART/状态灯）
 *   PB2  LED1（低电平点亮，uDap 上的 DAP 灯）
 *   HEXT 12MHz 外部高速晶振（OTGHS PHY 的 12MHz 参考时钟必须来自它）
 *
 *   PA2  USART2_TX / (JTAG 关闭时) 串口 TX
 *   PA3  USART2_RX / (JTAG 关闭时) 串口 RX
 *   PA4  目标 nRESET（开漏）
 *   PA5  SWCLK / JTAG TCK（同时也是 SPI1_SCK）
 *   PA6  SWDIO 的一半：MISO（同时也是 SPI1_MISO）
 *   PA7  SWDIO 的一半：MOSI（同时也是 SPI1_MOSI）
 *   PB1  JTAG TDI / (JTAG 关闭时) 串口 DTR
 *   PB0  JTAG TDO / (JTAG 关闭时) 串口 RTS
 *
 * 注：硬件把 PA6 与 PA7 短接在一起接 SWDIO，从而
 *   - GPIO 位翻转实现：PA6 当输入采样、PA7 当输出驱动（方向由软件切换）；
 *   - SPI 实现：SPI1 用 PA5=SCK、PA7=MOSI（双向单线模式），PA6 保持高阻。
 */

#ifndef __BOARD_PIN_MAP_H
#define __BOARD_PIN_MAP_H

#include "at32f402_405.h"

/* ------------------------------------------------------------------ LEDs */
#define BOARD_LED1_PORT           GPIOB
#define BOARD_LED1_PIN            GPIO_PINS_2
#define BOARD_LED2_PORT           GPIOB
#define BOARD_LED2_PIN            GPIO_PINS_4
#define BOARD_LED_GPIO_CLK        CRM_GPIOB_PERIPH_CLOCK
#define BOARD_LED_ON_LEVEL        0 /* 低电平点亮 */

/* ------------------------------------------------------- 调试口（SWD/JTAG） */
#define BOARD_SWD_PORT            GPIOA
#define BOARD_SWD_GPIO_CLK        CRM_GPIOA_PERIPH_CLOCK

#define BOARD_SWCLK_PIN           GPIO_PINS_5  /* PA5：SCK / TCK */
#define BOARD_SWDIO_OUT_PIN       GPIO_PINS_7  /* PA7：MOSI，主控驱动 */
#define BOARD_SWDIO_IN_PIN        GPIO_PINS_6  /* PA6：MISO，主控采样 */
#define BOARD_SWDIO_PINS          (BOARD_SWDIO_OUT_PIN | BOARD_SWDIO_IN_PIN)

#define BOARD_TDI_PIN             GPIO_PINS_1  /* PB1 */
#define BOARD_TDO_PIN             GPIO_PINS_0  /* PB0 */
#define BOARD_JTAG_PORT           GPIOB
#define BOARD_JTAG_GPIO_CLK       CRM_GPIOB_PERIPH_CLOCK

#define BOARD_NRESET_PIN          GPIO_PINS_4  /* PA4，开漏 */
#define BOARD_NRESET_PORT         GPIOA

/* -------------------------------------------------------------- 串口/流控 */
#define BOARD_UART                USART2
#define BOARD_UART_GPIO_CLK       CRM_GPIOA_PERIPH_CLOCK
#define BOARD_UART_CLK            CRM_USART2_PERIPH_CLOCK
#define BOARD_UART_IRQ            USART2_IRQn
#define BOARD_UART_IRQHandler     USART2_IRQHandler
#define BOARD_UART_TX_PIN         GPIO_PINS_2  /* PA2 */
#define BOARD_UART_RX_PIN         GPIO_PINS_3  /* PA3 */
#define BOARD_UART_TX_SOURCE      GPIO_PINS_SOURCE2
#define BOARD_UART_RX_SOURCE      GPIO_PINS_SOURCE3
#define BOARD_UART_MUX            GPIO_MUX_7

#define BOARD_DTR_PORT            GPIOB
#define BOARD_DTR_PIN             GPIO_PINS_1  /* PB1，与 TDI 复用 */
#define BOARD_RTS_PORT            GPIOB
#define BOARD_RTS_PIN             GPIO_PINS_0  /* PB0，与 TDO 复用 */

/* ------------------------------------------------------------------- USB */
/*
 * OTGHS1 的 D-/D+ 是芯片专用引脚（QFN32 的 18/19 脚），不需要 GPIO 复用配置；
 * OTGHS1_R（17 脚）是 PHY 的 12MHz 参考电阻，接 12kΩ 到地（硬件已处理）。
 * 所以 USB 只需要开时钟，不需要 usb_gpio_config()。
 */
#define BOARD_USB_IRQ             OTGHS_IRQn
#define BOARD_USB_IRQHandler      OTGHS_IRQHandler
#define BOARD_USB_BASE            OTGHS_BASE

#endif /* __BOARD_PIN_MAP_H */
