/**
 * @file    usb_config.h
 * @brief   CherryUSB 在 bootloader 里的配置（AT32F405 OTGHS，480Mbps HS）
 *
 * 与应用侧 application/usb_config.h 基本相同，只有一处区别：
 *   CONFIG_USBDEV_REQUEST_BUFFER_LEN = 1024。
 * 这个值同时决定两件事：
 *   1. DFU_DESCRIPTOR_INIT() 里的 wTransferSize（规范要求 = 1024）；
 *   2. EP0 控制传输缓冲大小 —— DFU DNLOAD 一包 1024 字节，缓冲必须放得下，
 *      否则 usbd_core 会直接 stall（usbd_core.c 里 wLength > 该值即拒绝）。
 */

#ifndef __UDAP_BOOT_USB_CONFIG_H
#define __UDAP_BOOT_USB_CONFIG_H

/* 关闭 USB 调试日志（bootloader 里没有 printf 后端） */
#define CONFIG_USB_PRINTF(...) \
    do {                       \
    } while (0)
#define CONFIG_USB_DBG_LEVEL USB_DBG_ERROR

/* 数据 Cache：AT32F405 没有 D-Cache，这里只是声明对齐粒度 */
#define CONFIG_USB_ALIGN_SIZE 4

/* USB 缓冲区不放在独立的非 Cache 段 */
#define USB_NOCACHE_RAM_SECTION

/* 用 Newlib 的 memcpy（CherryUSB 自带的版本更耗 Flash） */
#define CONFIG_USB_MEMCPY_DISABLE

/* EP0 控制传输缓冲区：DFU wTransferSize = 1024 必须放得下 */
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 1024

/* 用结构体描述符接口 + 支持 device qualifier（HS 设备需要） */
#define CONFIG_USBDEV_ADVANCE_DESC
#define CONFIG_USB_HS

/* 只用一路 USB（OTGHS） */
#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBDEV_EP_NUM 8

/* Host 侧数组尺寸：只用 device，但 dwc2 glue 会 include usbh_core.h */
#define CONFIG_USBHOST_MAX_ENDPOINTS 4
#define CONFIG_USBHOST_DEV_NAMELEN 16
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#define CONFIG_USBHOST_MAX_INTERFACES 4
#define CONFIG_USBHOST_MAX_EHPORTS 1

#endif /* __UDAP_BOOT_USB_CONFIG_H */
