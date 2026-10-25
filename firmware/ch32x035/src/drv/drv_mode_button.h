#pragma once

#include <stdbool.h>

#include <ch32x035.h>

#include "board_pin_map.h"

/*
 * 模式按键：PB3，一端接 GND，内部上拉 —— **按下 = 低电平**（引脚定义见
 * src/board_pin_map.h，要改脚只改那里的 BOARD_BTN_*）。
 *
 * 手势（见 src/main.c 与 src/drv/drv_uart.c）：
 *   - **双击**        切换桥接串口模式（串口1+DTR/RTS <-> 双串口），掉电保存
 *   - **上电时按住**  切到另一种调试器模式（CMSIS-DAP <-> WCH-Link，仅在
 *                     WCH-Link 参与编译时有效），掉电保存
 *   - 单击           不动作（只作为双击的第一次按下被记录）
 * 之所以不用单击切调试器模式：调试器模式切换要重新枚举 USB，误触代价高；
 * 而双击是明确的主动操作。
 */

#define DRV_MODE_BUTTON_PORT       BOARD_BTN_PORT
#define DRV_MODE_BUTTON_PIN        BOARD_BTN_PIN
#define DRV_MODE_BUTTON_CLK        BOARD_BTN_PORT_CLK
#define DRV_MODE_BUTTON_ACTIVE_LOW BOARD_BTN_ACTIVE_LOW

// 电平稳定多久才算「真的变化」
#define DRV_MODE_BUTTON_DEBOUNCE_MS 20u
// 两次「按下沿」之间不超过这个时间才算双击
#define DRV_MODE_BUTTON_DOUBLE_MS 350u

// 配置引脚为上拉输入，采样上电瞬间是否按住，并复位去抖/双击状态机
void drv_mode_button_init(void);

// 去抖后的电平：true = 按键处于按下状态。
// **每次调用都会推进去抖与双击状态机**，所以主循环要周期性调用。
bool drv_mode_button_pressed(void);

// 取出一次双击事件：有则返回 true 并清标志（一次性）
bool drv_mode_button_take_double_click(void);

// 上电时按键是否处于按下状态（drv_mode_button_init 采样）
bool drv_mode_button_held_at_boot(void);

// 阻塞等待按键释放（上电按住切模式后，避免同一次按住影响后续判断）
void drv_mode_button_wait_release(void);
