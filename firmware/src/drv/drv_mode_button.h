#pragma once

#include <stdbool.h>

#include <ch32x035.h>

/*
 * 模式切换按键（仅双模式固件使用）。
 *
 * 本板（uDap2）**没有**按键，需要外接：一端接下面这个 GPIO，另一端接 GND。
 * 内部上拉已使能，所以不按 = 高电平，按下 = 低电平。
 *
 * 引脚选择依据（QFN-20 CH32X035F8U 的全部引脚）：
 *   已占用：PA2/PA3 = SWCLK/SWDIO，PA5 = nRESET，PB0/PB1 = USART4，
 *           PB12 = 状态灯，PC16/PC17 = USB D-/D+
 *   空闲可用：PA0、PA1、PA4、PA6、PA7、PB3、PB11、PC18、PC19
 *
 * 默认选 PA4：与 SWD/nRESET 同在 GPIOA，方便在同一排上就近引线，
 * 且本工程没有参考工程里的 SBU MUX（PA4 在那块板上是 mux 交换控制），
 * 所以在本板上确定是空闲的。**要改引脚只改下面 4 个宏。**
 */

#define DRV_MODE_BUTTON_PORT       GPIOA
#define DRV_MODE_BUTTON_PIN        GPIO_Pin_4
#define DRV_MODE_BUTTON_CLK        RCC_APB2Periph_GPIOA
#define DRV_MODE_BUTTON_ACTIVE_LOW 1

// 按下后在主循环轮询里判定为"稳定按下"所需的毫秒数
#define DRV_MODE_BUTTON_DEBOUNCE_MS 20u

// 配置引脚为上拉输入，并采样一次初始电平（避免上电瞬间被误判成按下）
void drv_mode_button_init(void);

// 去抖后的电平：true = 按键处于按下状态。每次调用都会推进去抖状态机。
bool drv_mode_button_pressed(void);

// 阻塞等待按键释放。用于切换模式后，避免同一次按住立刻触发反向切换。
void drv_mode_button_wait_release(void);
