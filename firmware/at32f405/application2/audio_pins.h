/**
 * @file    audio_pins.h
 * @brief   application2（UAC2 speaker -> I2S1 -> PCM5102）引脚定义
 *
 * 硬件与 application/ 相同（uDap AT32F405KBU7-4 板），只是把板上原本作为
 * 「目标 SWD 信号」的排针改接 PCM5102 模块：
 *
 *   PA4  -> PCM5102 LCK   I2S1_WS   (GPIO_MUX_5)
 *   PA5  -> PCM5102 BCK   I2S1_CK   (GPIO_MUX_5)
 *   PA6  -> PCM5102 DIN   ——板上 PA6 与 PA7 短接在同一根 SWDIO 网络上
 *   PA7  -> I2S1_SD       (GPIO_MUX_5)，由 PA7 驱动该网络
 *
 * 关键点：AT32F405 的 PA6 复用表里没有 I2S1_SD（PA6 = SPI1_MISO / I2S1_MCK），
 * 真正的 I2S1 数据脚是 PA7。uDap 板上 PA6/PA7 本来就是短接的（见
 * application/board_pin_map.h），所以把 PA7 配成 I2S1_SD 后，PA6 排针上看到的
 * 就是 PCM5102 的串行数据。PA6 必须保持高阻（不要选 I2S1_MCK），否则会和
 * PA7 打架。
 */

#ifndef __UDAP_AUDIO_PINS_H
#define __UDAP_AUDIO_PINS_H

#include "at32f402_405.h"

#define AUDIO_I2S                SPI1
#define AUDIO_I2S_CLK            CRM_SPI1_PERIPH_CLOCK
#define AUDIO_I2S_DT_ADDR        (&(SPI1->dt))

#define AUDIO_I2S_GPIO           GPIOA
#define AUDIO_I2S_GPIO_CLK       CRM_GPIOA_PERIPH_CLOCK
#define AUDIO_I2S_MUX            GPIO_MUX_5

#define AUDIO_I2S_WS_PIN         GPIO_PINS_4
#define AUDIO_I2S_WS_SOURCE      GPIO_PINS_SOURCE4

#define AUDIO_I2S_CK_PIN         GPIO_PINS_5
#define AUDIO_I2S_CK_SOURCE      GPIO_PINS_SOURCE5

/* PCM5102 的 DIN 接在排针 PA6 上，电气上等于 PA7（板上短接） */
#define AUDIO_I2S_SD_PIN         GPIO_PINS_7
#define AUDIO_I2S_SD_SOURCE      GPIO_PINS_SOURCE7

/* PA6：保持高阻，绝不能配成 I2S1_MCK（会和 PA7 的 SD 输出短接） */
#define AUDIO_I2S_MCK_PIN        GPIO_PINS_6

/* I2S1 TX 的 DMA 请求源（DMA1 任意通道，这里用通道 2） */
#define AUDIO_DMA                DMA1_CHANNEL2
#define AUDIO_DMA_MUX            DMA1MUX_CHANNEL2
#define AUDIO_DMA_IRQn           DMA1_Channel2_IRQn
#define AUDIO_DMA_IRQHandler     DMA1_Channel2_IRQHandler

#endif /* __UDAP_AUDIO_PINS_H */
