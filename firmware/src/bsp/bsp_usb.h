#pragma once

/*
 * CH32X035F8U USBFS 硬件初始化（必选）。
 *
 * CherryUSB 的端口层 src/port/usb_ch32x035_dc_usbfs.c 把这两个函数声明为
 * __WEAK 空实现，USB 硬件（时钟、PC16/PC17、AFIO PHY、NVIC）必须由应用侧提供，
 * 否则 usbd_initialize() 之后 USB 完全不会工作（既不枚举、也不进中断）。
 *
 * 调用时机：usb_dc_init() 内部会自动调用 usb_dc_low_level_init()，
 * 也就是 usbd_initialize() 时。注意在此之前应用需先调用
 * NVIC_PriorityGroupConfig() 设定优先级分组。
 */

#include <ch32x035.h>

void usb_dc_low_level_init(void);
void usb_dc_low_level_deinit(void);
