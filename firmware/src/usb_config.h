#pragma once

/*
 * CherryUSB 应用侧配置（glue）。
 * 本文件与 src/port/usb_ch32x035_dc_usbfs.c 一起构成 CherryUSB 的移植层。
 */

// 关闭 USB 调试日志
#define CONFIG_USB_PRINTF(...) \
    do {                       \
    } while (0)
#define CONFIG_USB_DBG_LEVEL USB_DBG_ERROR

// 不启用数据 Cache
// #define CONFIG_USB_DCACHE_ENABLE

// USB DMA 缓冲区按 4 字节对齐
#define CONFIG_USB_ALIGN_SIZE 4

// USB 缓冲区不使用独立的非 Cache SRAM 段
#define USB_NOCACHE_RAM_SECTION

// 使用标准库 memcpy
#define CONFIG_USB_MEMCPY_DISABLE

// EP0 控制传输缓冲区；MS OS 2.0 描述符集合为 330 字节
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512

// 使用结构体描述符接口
#define CONFIG_USBDEV_ADVANCE_DESC

// USB 设备总线数量为 1，仅使用 USB0
#define CONFIG_USBDEV_MAX_BUS 1
