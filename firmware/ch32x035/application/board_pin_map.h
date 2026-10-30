#pragma once

#include <ch32x035.h>

/*
 * 板级引脚映射的**选择器**。
 *
 * 每块板一张「信号 -> 引脚」表，放在 board/ 下：
 *   board/board_ch32x035f8u6.h   uDap-CH32X035F8U6（初版板，--board=ch32x035，默认）
 *   board/board_ch32x033.h       uDap-CH32X033（同一份固件换脚，--board=ch32x033）
 *
 * 选择方式（xmake 的 option("board")）：
 *   xmake f -P . --board=ch32x033 -y        # 或 .\build.ps1 -Board ch32x033
 * 它只定义 BOARD_VARIANT（BOARD_CH32X035 / BOARD_CH32X033），不产生任何代码
 * 分支：所有差异都在这两张表里。
 *
 * ---------------------------------------------------------------------------
 * 板级头文件的契约（换板/新增板卡就照着填）
 * ---------------------------------------------------------------------------
 *   UART1（桥接串口 1）
 *     BOARD_UART1_INSTANCE / _IRQN / _IRQ_HANDLER（IRQ 服务函数名）
 *     BOARD_UART1_CLK_ENABLE() / _CLK_DISABLE()（USART1 挂在 APB2，其余在 APB1）
 *     BOARD_UART1_PORT / _PORT_CLK / _TX_PIN / _RX_PIN
 *     BOARD_UART1_RX_DMA_HW（uart_dma_hw_t 的初始化器）/ _RX_DMA_ISR（中断函数名）
 *   UART2 + DTR/RTS（「辅助那一对」）
 *     BOARD_AUX_UART_* 同上；BOARD_AUX_UART_REMAP_ENABLE()（默认映射时为空操作）
 *     BOARD_AUX_FLOW_PINS：本板是否有 DTR/RTS 输出脚（0 时 drv_uart.c 整段不编）
 *     有流控脚时：BOARD_AUX_DTR/RTS_PIN + _CFGLR_SHIFT（必须落在 CFGLR 的 bit0..7）
 *   按键 / LED
 *     BOARD_BTN_PORT / _PORT_CLK / _PIN / _ACTIVE_LOW
 *     BOARD_LED_PORT / _PORT_CLK / _DAP_PIN / _UART_PIN / _ACTIVE_LOW
 *     （两颗灯必须同 port：drv_led.c 与 bootloader 用一次 store 同时写两颗）
 *   SWD / nRESET
 *     BOARD_SWD_DEFAULT_PIN_VARIANT（SWD_PIN_VARIANT_*，见 application/swd_pin_map.h）
 *     BOARD_NRESET_ENABLE / BOARD_NRESET_BIT
 *   串口模式默认值
 *     BOARD_UART_DEFAULT_DUAL（选项字无效时的默认模式：0 = 单串口 + DTR/RTS）
 *
 * 注意 CH32X035/X033 的 GPIO 配置寄存器分两段：CFGLR 管 pin0..7、CFGHR 管 pin8..15。
 * SDK 的 GPIO_Init() 对 CFGHR 走静态缓存（CFGHR_tmpA/B/C），**直接写 CFGHR
 * 会让缓存失步**；CFGLR 没有缓存，可以直接写。所以本工程约定：
 *   pin0..7  -> 允许裸写 CFGLR（DAP/SWD 快路径需要）
 *   pin8..15 -> 一律走 GPIO_Init()
 */

#define BOARD_CH32X035 1 /* uDap-CH32X035F8U6（board/board_ch32x035f8u6.h） */
#define BOARD_CH32X033 2 /* uDap-CH32X033（board/board_ch32x033.h） */

#ifndef BOARD_VARIANT
#define BOARD_VARIANT BOARD_CH32X035
#endif

#if BOARD_VARIANT == BOARD_CH32X035
#include "board/board_ch32x035f8u6.h"
#elif BOARD_VARIANT == BOARD_CH32X033
#include "board/board_ch32x033.h"
#else
#error "未知的 BOARD_VARIANT（可选 BOARD_CH32X035 / BOARD_CH32X033）"
#endif
