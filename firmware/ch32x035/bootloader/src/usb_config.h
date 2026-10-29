#pragma once

/*
 * UF2 bootloader 的 CherryUSB 应用侧配置。
 *
 * 与应用固件的 src/usb_config.h 基本相同，多出来的部分是 MSC 类需要的
 * 几个宏（应用不编 MSC）。**本文件必须排在 include 路径上应用那份之前**
 * （见 xmake.lua 的 add_includedirs 顺序），否则 usbd_core.h 会取到应用那份，
 * MSC 类会编译失败。
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

// EP0 控制传输缓冲区
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512

// 使用结构体描述符接口
#define CONFIG_USBDEV_ADVANCE_DESC

// USB 设备总线数量为 1
#define CONFIG_USBDEV_MAX_BUS 1

// ===== MSC 类 =====
// 1 个 LUN（整个盘），单次传输缓冲 512 B（正好一个扇区）
#define CONFIG_USBDEV_MSC_MAX_LUN        1
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE    512
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING "uDap"
#define CONFIG_USBDEV_MSC_PRODUCT_STRING      "UF2 Bootloader"
#define CONFIG_USBDEV_MSC_VERSION_STRING      "1.3 "
