#pragma once

#include <ch32x035.h>

/*
 * uDap-CH32X033 引脚定义（同一份固件，只是引脚换了）。
 *
 * 由 application/board_pin_map.h 按 BOARD_VARIANT 选中，**不要**直接 include。
 *
 * CH32X033 与 CH32X035 的 SDK/外设完全相同（差别只有引脚与 USB PD/CC），
 * 所以这里只描述「哪些信号接在哪个脚」，上层代码一行都不用分叉。
 *
 *   UART1（桥接串口 1）：USART2，PA2 = TX / PA3 = RX（USART2 默认引脚，无重映射）
 *   UART2（桥接串口 2）：USART1，PA10 = TX2 / PA11 = RX2
 *       这两个脚要用 USART1_RM = 01（部分重映射 1）；USART1 的默认映射在 PB10/PB11。
 *       本板**没有** DTR/RTS 输出脚，单串口模式下 CDC 的 DTR/RTS 不下发到板上。
 *   模式按键：PC3，一端接 GND，内部上拉，**低电平 = 按下**
 *   LED：PB7 = DAP 指示灯、PB1 = 串口模式/活动灯，**低电平点亮**
 *   SWD 调试口：PA1 = SWCLK / PA0 = SWDIO
 *   目标 nRESET：PA9（低有效，开漏模拟；PA9 落在 CFGHR 覆盖的 bit 8..15 内）
 */

#define BOARD_NAME "uDap-CH32X033"

/* ---------------- 桥接串口 1：USART2（PA2 = TX / PA3 = RX） ---------------- */
#define BOARD_UART1_INSTANCE      USART2
#define BOARD_UART1_IRQN          USART2_IRQn
#define BOARD_UART1_IRQ_HANDLER   USART2_IRQHandler
#define BOARD_UART1_CLK_ENABLE()  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE)
#define BOARD_UART1_CLK_DISABLE() RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, DISABLE)
#define BOARD_UART1_PORT          GPIOA
#define BOARD_UART1_PORT_CLK      RCC_APB2Periph_GPIOA
#define BOARD_UART1_TX_PIN        GPIO_Pin_2
#define BOARD_UART1_RX_PIN        GPIO_Pin_3

/* USART2_RX 的 DMA 请求固定落在 DMA1 通道 6（见参考手册的 DMA 请求映射表） */
#define BOARD_UART1_RX_DMA_ISR    DMA1_Channel6_IRQHandler
#define BOARD_UART1_RX_DMA_HW                                                  \
    {                                                                          \
        .rx_ch = DMA1_Channel6, .rx_irqn = DMA1_Channel6_IRQn,                 \
        .rx_ht_it = DMA1_IT_HT6, .rx_tc_it = DMA1_IT_TC6,                      \
        .rx_te_it = DMA1_IT_TE6, .rx_ht_flag = DMA1_FLAG_HT6,                  \
        .rx_tc_flag = DMA1_FLAG_TC6, .rx_te_flag = DMA1_FLAG_TE6               \
    }

/* ---------------- UART2：USART1 部分重映射 1（PA10 = TX2 / PA11 = RX2） ---------------- */
#define BOARD_AUX_PORT            GPIOA
#define BOARD_AUX_PORT_CLK        RCC_APB2Periph_GPIOA

/* 本板没有 DTR/RTS 输出脚：drv_uart.c 里的流控脚逻辑整段不编 */
#define BOARD_AUX_FLOW_PINS       0

#define BOARD_AUX_UART_INSTANCE    USART1
#define BOARD_AUX_UART_IRQN        USART1_IRQn
#define BOARD_AUX_UART_IRQ_HANDLER USART1_IRQHandler
#define BOARD_AUX_UART_CLK_ENABLE()  RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE)
#define BOARD_AUX_UART_CLK_DISABLE() RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, DISABLE)
/* USART1 默认映射是 PB10/PB11，PA10/PA11 需要 RM=01（PartialRemap1） */
#define BOARD_AUX_UART_REMAP_ENABLE()                                          \
    do {                                                                       \
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);                   \
        GPIO_PinRemapConfig(GPIO_PartialRemap1_USART1, ENABLE);                \
    } while (0)
#define BOARD_AUX_UART_TX_PIN      GPIO_Pin_10
#define BOARD_AUX_UART_RX_PIN      GPIO_Pin_11

/* USART1_RX 的 DMA 请求固定落在 DMA1 通道 5 */
#define BOARD_AUX_UART_RX_DMA_ISR  DMA1_Channel5_IRQHandler
#define BOARD_AUX_UART_RX_DMA_HW                                               \
    {                                                                          \
        .rx_ch = DMA1_Channel5, .rx_irqn = DMA1_Channel5_IRQn,                 \
        .rx_ht_it = DMA1_IT_HT5, .rx_tc_it = DMA1_IT_TC5,                      \
        .rx_te_it = DMA1_IT_TE5, .rx_ht_flag = DMA1_FLAG_HT5,                  \
        .rx_tc_flag = DMA1_FLAG_TC5, .rx_te_flag = DMA1_FLAG_TE5               \
    }

/* ---------------- 模式按键：PC3 ---------------- */
#define BOARD_BTN_PORT            GPIOC
#define BOARD_BTN_PORT_CLK        RCC_APB2Periph_GPIOC
#define BOARD_BTN_PIN             GPIO_Pin_3
#define BOARD_BTN_ACTIVE_LOW      1

/* ---------------- LED：PB7（DAP）/ PB1（串口） ---------------- */
#define BOARD_LED_PORT            GPIOB
#define BOARD_LED_PORT_CLK        RCC_APB2Periph_GPIOB
#define BOARD_LED_DAP_PIN         GPIO_Pin_7
#define BOARD_LED_UART_PIN        GPIO_Pin_1
#define BOARD_LED_ACTIVE_LOW      1

/* ---------------- 默认 SWD 引脚方案 / 目标 nRESET ---------------- */
/* SWD_PIN_VARIANT_PA0_PA1 由 application/swd_pin_map.h 定义（那里也是包含本文件的地方） */
#define BOARD_SWD_DEFAULT_PIN_VARIANT SWD_PIN_VARIANT_PA0_PA1
#define BOARD_NRESET_ENABLE       1
#define BOARD_NRESET_BIT          9

/* ---------------- 串口模式默认值 ---------------- */
/* 没有 DTR/RTS 脚：默认就用双串口（两路 CDC 都能直接当桥使用） */
#define BOARD_UART_DEFAULT_DUAL   1
