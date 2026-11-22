/**
 * @file    boot_usb.h
 * @brief   bootloader 的 USB 设备栈（接口 0 = DFU，接口 1 = WinUSB 私有协议）
 */

#ifndef __BOOT_USB_H
#define __BOOT_USB_H

/* 初始化 USB 时钟、序列号与设备栈（DFU 接口在前 = 接口 0，WinUSB 在后 = 接口 1） */
void boot_usb_init(void);

/* 主循环里周期调用：处理收到的 WinUSB 命令并回应答。
 * 命令处理放在主循环而不是中断里，因为 ERASE_APP 要擦 112 KiB（约 1 秒）。 */
void boot_usb_poll(void);

#endif /* __BOOT_USB_H */
