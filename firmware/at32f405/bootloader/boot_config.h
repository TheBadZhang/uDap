/**
 * @file    boot_config.h
 * @brief   uDap（AT32F405KBU7）bootloader 编译期常量
 *
 * 本文件里的地址/大小/协议常量与 docs/bootloader-protocol.md 一一对应，
 * 改任何一项之前先改规范文档。
 */

#ifndef __BOOT_CONFIG_H
#define __BOOT_CONFIG_H

/* ---------------------------------------------------------------- Flash 布局 */
#define BOOT_FLASH_BASE        0x08000000u  /* bootloader 自身起始（16 KiB） */
#define BOOT_FLASH_SIZE        0x00004000u  /* bootloader 占用 16 KiB */
#define BOOT_APP_BASE          0x08004000u  /* 应用起始（112 KiB） */
#define BOOT_APP_SIZE          0x0001C000u  /* 应用区大小 */
#define BOOT_APP_END           (BOOT_APP_BASE + BOOT_APP_SIZE) /* 0x08020000 */
/* 协议里 GET_INFO 上报的页大小：规范第 4 节写的是 2048，保持字面值不变。 */
#define BOOT_PAGE_SIZE         0x00000800u

/* 真正的 Flash 擦除粒度：AT32F402/405 的 **xB（128 KiB）档扇区是 1 KiB**，
 * 只有 xC（256 KiB）档才是 2 KiB（依据 SDK 例程
 * project/at_start_f402/examples/flash/flash_write_read/src/flash.c:33-37：
 * `#if defined (AT32F402_405xC) SECTOR_SIZE 2048 #else 1024`）。
 * 本板 AT32F405KBU7 是 128 KiB，所以内部一律按 1024 擦除。 */
#define BOOT_SECTOR_SIZE       0x00000400u  /* 扇区 = 1 KiB */
#define BOOT_APP_SECTOR_COUNT  (BOOT_APP_SIZE / BOOT_SECTOR_SIZE) /* 112 个扇区 */

/* --------------------------------------------------------------- RAM 范围 */
/* 用于校验应用向量表里的初始 MSP 是否落在本芯片 RAM 内 */
#define BOOT_RAM_BASE          0x20000000u
#define BOOT_RAM_END           0x20011800u  /* 0x20000000 + 70 KiB */

/* ------------------------------------------------------- 进升级模式的魔术字 */
/* 'UDFB'。应用写 .noinit 里的 g_udap_boot_magic 后软复位，bootloader 读到
 * 本值就清掉它并留在 USB 升级模式；两个固件的 .noinit 段被链接脚本钉在
 * 同一个 RAM 地址（0x20010000），所以这样跨复位传递是可靠的。 */
#define BOOT_MAGIC_ENTER       0x55444642u

/* ------------------------------------------------------------- USB 身份 */
#define BOOT_USBD_VID          0x0D28u
#define BOOT_USBD_PID          0x0205u
#define BOOT_MAX_POWER         500u     /* 电流（mA） */
#define BOOT_LANGID            1033u    /* en-US */

/* ------------------------------------------------- WinUSB 私有批量协议端点 */
#define BOOT_OUT_EP            0x06u
#define BOOT_IN_EP             0x85u
#define BOOT_PACKET_SIZE       512u     /* HS bulk 最大包长 */
#define BOOT_MAX_CHUNK         (BOOT_PACKET_SIZE - 8u) /* 504，见规范第 4 节 */

/* ----------------------------------------------------- 协议版本与固件版本 */
#define BOOT_PROTO_VER         1u
#define BOOT_FW_MAJOR          1u
#define BOOT_FW_MINOR          0u
#define BOOT_FW_PATCH          0u

/* ------------------------------------------------------------------- DFU */
/* wTransferSize 由 CherryUSB 的 DFU_DESCRIPTOR_INIT() 从
 * CONFIG_USBDEV_REQUEST_BUFFER_LEN 取值，bootloader/usb_config.h 里设为 1024。 */
#define BOOT_DFU_TRANSFER_SIZE 1024u

/* --------------------------------------------------------------- 其它 */
/* 发完最后一个 USB 应答后等一会儿再复位：让 IN 传输真正上线，主机才不会
 * 看到「设备没响应就消失」。 */
#define BOOT_REBOOT_DELAY_MS   100u

/* 自检闪灯参数（PB2/PB4 低电平点亮） */
#define BOOT_LED_BLINK_MS      60u
#define BOOT_LED_BLINK_COUNT   2u

#endif /* __BOOT_CONFIG_H */
