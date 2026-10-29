#pragma once

/* 启动 UF2 bootloader 的 USB MSC 设备（阻塞式初始化，插上就被主机枚举成 U 盘） */
void boot_usb_init(void);
