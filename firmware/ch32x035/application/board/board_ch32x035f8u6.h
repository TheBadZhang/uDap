#pragma once

#include <ch32x035.h>

/*
 * uDap-CH32X035F8U6（初版板）引脚定义。
 *
 * 由 application/board_pin_map.h 按 BOARD_VARIANT 选中，**不要**直接 include；
 * 新增板卡请在本目录加 board_*.h，并在应用侧板级约定上保持一致。
 *
 *   UART1（桥接串口 1）：USART4，PB0 = TX / PB1 = RX
 *   PA2 / PA3：**双用途**，由运行期模式决定（见 drv/drv_uart.h）
 *       · 流控模式：PA2 = DTR、PA3 = RTS（推挽输出，有效 = 低）
 *       · 双串口模式：PA2 = USART2_TX（TX2）、PA3 = USART2_RX（RX2）
 *     CH32X035 的 USART2 默认引脚就是 PA2/PA3，无需重映射。
 *   模式按键：PB3，一端接 GND，内部上拉，**低电平 = 按下**
 *   LED：PB11 = DAP 指示灯、PB12 = 串口模式/活动灯，**低电平点亮**
 *   SWD 调试口：PA5 = SWCLK / PA6 = SWDIO（见 application/swd_pin_map.h）
 *   目标 nRESET：PA7（低有效，开漏模拟）
 */

#define BOARD_NAME "uDap-CH32X035F8U6"

/* ---------------- 桥接串口 1：USART4（PB0 = TX / PB1 = RX） ---------------- */
#define BOARD_UART1_INSTANCE      USART4
#define BOARD_UART1_IRQN          USART4_IRQn
#define BOARD_UART1_IRQ_HANDLER   USART4_IRQHandler
#define BOARD_UART1_CLK_ENABLE()  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART4, ENABLE)
#define BOARD_UART1_CLK_DISABLE() RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART4, DISABLE)
#define BOARD_UART1_PORT          GPIOB
#define BOARD_UART1_PORT_CLK      RCC_APB2Periph_GPIOB
#define BOARD_UART1_TX_PIN        GPIO_Pin_0
#define BOARD_UART1_RX_PIN        GPIO_Pin_1

/* USART4_RX 的 DMA 请求固定落在 DMA1 通道 8（见参考手册的 DMA 请求映射表） */
#define BOARD_UART1_RX_DMA_ISR    DMA1_Channel8_IRQHandler
#define BOARD_UART1_RX_DMA_HW                                                  \
    {                                                                          \
        .rx_ch = DMA1_Channel8, .rx_irqn = DMA1_Channel8_IRQn,                 \
        .rx_ht_it = DMA1_IT_HT8, .rx_tc_it = DMA1_IT_TC8,                      \
        .rx_te_it = DMA1_IT_TE8, .rx_ht_flag = DMA1_FLAG_HT8,                  \
        .rx_tc_flag = DMA1_FLAG_TC8, .rx_te_flag = DMA1_FLAG_TE8               \
    }

/* ---------------- PA2 / PA3：DTR+RTS 或 USART2 ---------------- */
#define BOARD_AUX_PORT            GPIOA
#define BOARD_AUX_PORT_CLK        RCC_APB2Periph_GPIOA
#define BOARD_AUX_DTR_BIT         2
#define BOARD_AUX_RTS_BIT         3
#define BOARD_AUX_DTR_PIN         (1UL << BOARD_AUX_DTR_BIT)
#define BOARD_AUX_RTS_PIN         (1UL << BOARD_AUX_RTS_BIT)
#define BOARD_AUX_DTR_CFGLR_SHIFT (BOARD_AUX_DTR_BIT * 4)
#define BOARD_AUX_RTS_CFGLR_SHIFT (BOARD_AUX_RTS_BIT * 4)

/* 本板有 DTR/RTS 输出脚（就是 PA2/PA3，双串口模式下让给 USART2） */
#define BOARD_AUX_FLOW_PINS       1

/* USART2 默认引脚即 PA2(TX) / PA3(RX)（参考手册 USART 章节的引脚表），无需重映射 */
#define BOARD_AUX_UART_INSTANCE    USART2
#define BOARD_AUX_UART_IRQN        USART2_IRQn
#define BOARD_AUX_UART_IRQ_HANDLER USART2_IRQHandler
#define BOARD_AUX_UART_CLK_ENABLE()  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE)
#define BOARD_AUX_UART_CLK_DISABLE() RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, DISABLE)
#define BOARD_AUX_UART_REMAP_ENABLE() ((void)0)
#define BOARD_AUX_UART_TX_PIN      BOARD_AUX_DTR_PIN
#define BOARD_AUX_UART_RX_PIN      BOARD_AUX_RTS_PIN

/* USART2_RX 的 DMA 请求固定落在 DMA1 通道 6 */
#define BOARD_AUX_UART_RX_DMA_ISR  DMA1_Channel6_IRQHandler
#define BOARD_AUX_UART_RX_DMA_HW                                               \
    {                                                                          \
        .rx_ch = DMA1_Channel6, .rx_irqn = DMA1_Channel6_IRQn,                 \
        .rx_ht_it = DMA1_IT_HT6, .rx_tc_it = DMA1_IT_TC6,                      \
        .rx_te_it = DMA1_IT_TE6, .rx_ht_flag = DMA1_FLAG_HT6,                  \
        .rx_tc_flag = DMA1_FLAG_TC6, .rx_te_flag = DMA1_FLAG_TE6               \
    }

/* DTR/RTS 走裸写 CFGLR 的快路径，两个脚必须落在 CFGLR（bit 0..7）覆盖范围 */
#if (BOARD_AUX_DTR_BIT > 7) || (BOARD_AUX_RTS_BIT > 7)
#error "本板的 DTR/RTS 必须落在 CFGLR 覆盖的 PA0..PA7 内"
#endif

/* ---------------- 模式按键：PB3 ---------------- */
#define BOARD_BTN_PORT            GPIOB
#define BOARD_BTN_PORT_CLK        RCC_APB2Periph_GPIOB
#define BOARD_BTN_PIN             GPIO_Pin_3
#define BOARD_BTN_ACTIVE_LOW      1

/* ---------------- LED：PB11（DAP）/ PB12（串口） ---------------- */
#define BOARD_LED_PORT            GPIOB
#define BOARD_LED_PORT_CLK        RCC_APB2Periph_GPIOB
#define BOARD_LED_DAP_PIN         GPIO_Pin_11
#define BOARD_LED_UART_PIN        GPIO_Pin_12
#define BOARD_LED_ACTIVE_LOW      1

/* ---------------- 默认 SWD 引脚方案 / 目标 nRESET ---------------- */
/* SWD_PIN_VARIANT_PA5_PA6 由 application/swd_pin_map.h 定义（那里也是包含本文件的地方） */
#define BOARD_SWD_DEFAULT_PIN_VARIANT SWD_PIN_VARIANT_PA5_PA6
#define BOARD_NRESET_ENABLE       1
#define BOARD_NRESET_BIT          7

/* ---------------- 串口模式默认值 ---------------- */
/* 有 DTR/RTS 脚：默认「串口 1 + DTR/RTS」，需要时双击按键切到双串口 */
#define BOARD_UART_DEFAULT_DUAL   0
