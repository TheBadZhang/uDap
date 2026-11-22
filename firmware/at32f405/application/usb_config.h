/**
 * @file    usb_config.h
 * @brief   CherryUSB 应用侧配置（AT32F405 OTGHS，480Mbps High Speed）
 *
 * 端口：firmware/third_party/cherryusb/port/dwc2
 *   usb_glue_at.c 里已经带了 AT32F402/405 的 glue（__has_include("at32f402_405.h")），
 *   当 reg_base == OTGHS_BASE 时选用 param_pb14_pb15（UTMI PHY、DMA 开、FIFO 1008 word），
 *   也就是片上 HS PHY，不需要我们改 CherryUSB 的代码。
 *
 * 硬件前提：OTGHS 的 PHY 参考时钟必须是 12MHz HEXT（板上 12M 晶振），
 *   见 application/bsp/bsp_usb.c 的 bsp_usb_clock_init()。
 */

#ifndef __UDAP_USB_CONFIG_H
#define __UDAP_USB_CONFIG_H

/* 关闭 USB 调试日志（固件里没有可用的 printf 后端时会拖慢中断） */
#define CONFIG_USB_PRINTF(...) \
    do {                       \
    } while (0)
#define CONFIG_USB_DBG_LEVEL USB_DBG_ERROR

/* 数据 Cache：AT32F405 没有 D-Cache，这里只是声明对齐粒度 */
#define CONFIG_USB_ALIGN_SIZE 4

/* USB 缓冲区不放在独立的非 Cache 段 */
#define USB_NOCACHE_RAM_SECTION

/* 用 Newton 库的 memcpy（CherryUSB 自带的版本更耗 Flash） */
#define CONFIG_USB_MEMCPY_DISABLE

/* EP0 控制传输缓冲区：WinUSB MS OS 2.0 描述符集合 + 512 字节包 */
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512

/* 用结构体描述符接口 + 支持 device qualifier（HS 设备需要） */
#define CONFIG_USBDEV_ADVANCE_DESC
#define CONFIG_USB_HS

/* 只用一路 USB（OTGHS） */
#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBDEV_EP_NUM 8

/* Host 侧的数组尺寸：本工程只用 device，但 CherryUSB 的 dwc2 glue
 * (port/dwc2/usb_glue_at.c) 会 include usbh_core.h，那里的结构体要用到这几个
 * 配置。给最小值即可，不会占用 Flash/RAM（没有 host 实例）。 */
#define CONFIG_USBHOST_MAX_ENDPOINTS 4
#define CONFIG_USBHOST_DEV_NAMELEN 16
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#define CONFIG_USBHOST_MAX_INTERFACES 4
#define CONFIG_USBHOST_MAX_EHPORTS 1

#endif /* __UDAP_USB_CONFIG_H */
