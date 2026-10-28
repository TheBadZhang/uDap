#pragma once

#include <stdbool.h>

#include <ch32x035.h>

#include "board_pin_map.h"

/*
 * 模式按键：PB3，一端接 GND，内部上拉 —— **按下 = 低电平**（引脚定义见
 * src/board_pin_map.h，要改脚只改那里的 BOARD_BTN_*）。
 *
 * 手势（见 src/main.c）：
 *   - **双击**  单模式固件：切换桥接串口模式（串口1+DTR/RTS <-> 双串口）
 *               双模式固件：切换调试器模式（CMSIS-DAP <-> WCH-Link）
 *               两者都掉电保存（选项字节）
 *   - 单击     不动作（只作为双击的第一次按下被记录）
 *
 * 为什么双模式固件把双击让给调试器模式：CMSIS-DAP + WCH-Link 的固件放不下
 * 双串口所需的 RAM/Flash，所以那个构建里串口固定为单串口，切换串口模式的手势
 * 本来就空着；用它切调试器比「上电时按住」自然得多（不用断电、也不用和
 * bootloader 的长按抢同一个按键）。
 *
 * 上电时长按 PB3 = 进 UF2 bootloader，由 bootloader 自己判定，与这里无关。
 */

#define DRV_MODE_BUTTON_PORT       BOARD_BTN_PORT
#define DRV_MODE_BUTTON_PIN        BOARD_BTN_PIN
#define DRV_MODE_BUTTON_CLK        BOARD_BTN_PORT_CLK
#define DRV_MODE_BUTTON_ACTIVE_LOW BOARD_BTN_ACTIVE_LOW

// 电平稳定多久才算「真的变化」
#define DRV_MODE_BUTTON_DEBOUNCE_MS 20u
// 两次「按下沿」之间不超过这个时间才算双击
#define DRV_MODE_BUTTON_DOUBLE_MS 350u

// 配置引脚为上拉输入，并复位去抖/双击状态机
void drv_mode_button_init(void);

// 去抖后的电平：true = 按键处于按下状态。
// **每次调用都会推进去抖与双击状态机**，所以主循环要周期性调用。
bool drv_mode_button_pressed(void);

// 取出一次双击事件：有则返回 true 并清标志（一次性）
bool drv_mode_button_take_double_click(void);
