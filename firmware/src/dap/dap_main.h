#ifndef DAP_MAIN_H
#define DAP_MAIN_H

#include "DAP_config.h"

// CherryUSB 的 class/cdc/usbd_cdc_acm.h 用了 bool 但自己没包含 <stdbool.h>，
// 它假定引用者已经引入过 —— 而下面 usbd_cdc.h 排在 usbd_core.h（内含
// <stdbool.h>）之前，所以必须在这里先引入。
// （参考工程 ch32x035-usb-dap-main 是在 cherryusb 子模块里直接改这个头文件，
//   本工程不改 submodule，把等价修复放在自己的 glue 里。）
#include <stdbool.h>

#include <DAP.h>
#include <usbd_cdc.h>
#include <usbd_core.h>
#include <usbd_hid.h>
#include <usbd_msc.h>

#define DAP_IN_EP  0x85
#define DAP_OUT_EP 0x06

#define CDC_IN_EP  0x81
#define CDC_OUT_EP 0x02
#define CDC_INT_EP 0x83

#define MSC_IN_EP  0x86
#define MSC_OUT_EP 0x07

#define HID_IN_EP  0x88
#define HID_OUT_EP 0x09

#define USBD_VID           0x0D28
#define USBD_PID           0x0204
#define USBD_MAX_POWER     500
#define USBD_LANGID_STRING 1033

#ifdef CONFIG_USB_HS
#if DAP_PACKET_SIZE != 512
#error "DAP_PACKET_SIZE must be 512 in hs"
#endif
#else
#if DAP_PACKET_SIZE != 64
#error "DAP_PACKET_SIZE must be 64 in fs"
#endif
#endif

#ifdef CONFIG_USB_HS
#define HID_PACKET_SIZE 1024
#else
#define HID_PACKET_SIZE 64
#endif

#ifndef CONFIG_CHERRYDAP_USE_MSC
#define CONFIG_CHERRYDAP_USE_MSC 0
#endif

#ifndef CONFIG_CHERRYDAP_USE_CUSTOM_HID
#define CONFIG_CHERRYDAP_USE_CUSTOM_HID 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern char serial_number_dynamic[36];

extern struct usbd_interface hid_intf;

void chry_dap_init(uint8_t busid, uint32_t reg_base);

void chry_dap_handle(void) __attribute__((section(".highcode")));

// COM 口 <-> 目标串口（USART4）的周期性服务，两个方向都在这里推进：
//   OUT 方向：CDC OUT 背压恢复（TX FIFO 余量足够后重挂端点）
//   IN  方向：从 UART RX FIFO 取数据送 CDC IN
//
// 必须由主循环反复调用。OUT 那一支不能省略 —— 背压停止重挂端点后就再也不会
// 有 USB 中断，「FIFO 已排空」只能在主循环里察觉。
// IN 那一支不能放在中断里，因为 usbd_ep_start_write 不是中断安全的。
void chry_dap_cdc_bridge(void);

/* implment by user */
extern void hid_custom_notify_handler(uint8_t busid, uint8_t event, void *arg);

/* implment by user */
extern void usbd_hid_custom_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

/* implment by user */
extern void usbd_hid_custom_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

#ifdef __cplusplus
}
#endif

#endif
