/**
 * @file    usb_config.h
 * @brief   CherryUSB 应用侧配置（application2：UAC2 speaker）
 *
 * 与 application/usb_config.h 基本一致，只是这个固件没有 CDC/DAP，端点少，
 * 但 EP0 缓冲区仍按 512 字节留够（描述符总长约 260 字节）。
 */

#ifndef __UDAP_AUDIO_USB_CONFIG_H
#define __UDAP_AUDIO_USB_CONFIG_H

#define CONFIG_USB_PRINTF(...) \
    do {                       \
    } while (0)
#define CONFIG_USB_DBG_LEVEL USB_DBG_ERROR

#define CONFIG_USB_ALIGN_SIZE 4

#define USB_NOCACHE_RAM_SECTION

#define CONFIG_USB_MEMCPY_DISABLE

#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512

#define CONFIG_USBDEV_ADVANCE_DESC
#define CONFIG_USB_HS

#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBDEV_EP_NUM 8

/* 本工程只用 device，但 dwc2 glue 会 include usbh_core.h，给它最小值即可 */
#define CONFIG_USBHOST_MAX_ENDPOINTS 4
#define CONFIG_USBHOST_DEV_NAMELEN 16
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 4
#define CONFIG_USBHOST_MAX_INTERFACES 4
#define CONFIG_USBHOST_MAX_EHPORTS 1

#endif /* __UDAP_AUDIO_USB_CONFIG_H */
