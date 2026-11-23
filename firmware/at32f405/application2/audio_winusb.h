/**
 * @file    audio_winusb.h
 * @brief   复合在 UAC2 扬声器里的控制接口（上位机设置 EQ 参数）
 *
 * 接口 2（bInterfaceNumber = 2）：HID 类，两个中断端点
 *   OUT 0x04 / IN 0x83，64 字节定长报文。
 *
 * 为什么用 HID 而不是 WinUSB：
 *   Windows 11 26200 上实测，即使 BOS + MS OS 2.0 描述符集逐字节符合规范
 *   （设备确实收到并回了 wIndex=7 的 178 字节请求），Windows 依然没有给
 *   厂商接口打上 USB\MS_COMP_WINUSB 兼容 ID，设备停在 Code 28。而 HID 类
 *   由系统自带 hidusb/hidclass 直接驱动，不需要 inf、不需要管理员、不依赖
 *   WCID 缓存，稳定性最好。上位机用 hidapi 即可（本机已装 hidapi 0.15.0）。
 */

#ifndef __UDAP_AUDIO_WINUSB_H
#define __UDAP_AUDIO_WINUSB_H

#include <stdint.h>

#include "usbd_core.h"

/* 控制接口的编号与端点（配置描述符与实现共用同一份定义） */
#define UDAP_HID_INTF      2u
#define UDAP_HID_OUT_EP    0x04u
#define UDAP_HID_IN_EP     0x83u
#define UDAP_HID_EP_MPS    64u
#define UDAP_HID_INTERVAL  4u /* HS: 2^(4-1) 微帧 = 1 ms 轮询 */

/* 厂商自定义用途页 0xFF00，输入/输出各 64 字节，无 Report ID */
#define UDAP_HID_REPORT_DESC_LEN 34u

/* 配置描述符里这段的长度：接口 9 + HID 描述符 9 + 两个端点各 7 */
#define UDAP_HID_DESCRIPTOR_LEN (9u + 9u + 7u + 7u)

/* 接口 + HID 描述符 + 两个中断端点，直接展开进配置描述符 */
#define UDAP_HID_INTERFACE_DESCRIPTOR_INIT()                                                                          \
    /* bLength, INTERFACE */                                                                                          \
    0x09, 0x04, UDAP_HID_INTF, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,                                                    \
        /* bLength, HID = 0x21, bcdHID=0x0111, bCountryCode=0, bNumDescriptors=1, REPORT=0x22, wDescriptorLength */    \
        0x09, 0x21, WBVAL(0x0111), 0x00, 0x01, 0x22, WBVAL(UDAP_HID_REPORT_DESC_LEN),                                 \
        /* bLength, ENDPOINT, IN, INTERRUPT, wMaxPacketSize, bInterval */                                             \
        0x07, 0x05, UDAP_HID_IN_EP, USB_ENDPOINT_TYPE_INTERRUPT, WBVAL(UDAP_HID_EP_MPS), UDAP_HID_INTERVAL,           \
        /* bLength, ENDPOINT, OUT, INTERRUPT, wMaxPacketSize, bInterval */                                            \
        0x07, 0x05, UDAP_HID_OUT_EP, USB_ENDPOINT_TYPE_INTERRUPT, WBVAL(UDAP_HID_EP_MPS), UDAP_HID_INTERVAL,

/* HID 报文描述符（audio_winusb.c 里定义，usbd_hid_init_intf 注册） */
extern const uint8_t g_udap_hid_report_descriptor[UDAP_HID_REPORT_DESC_LEN];

/* 注册控制接口与端点；必须在 audio 的两个接口之后调用，才能拿到接口号 2 */
void audio_winusb_init(uint8_t busid);

/* ---------------------------------------------------------------- 枚举跟踪
 * 调试用：把收到的每个 SETUP 包和 USB 事件记录到 RAM，方便用 SWD 读出来
 * 分析 Windows 到底在哪一步枚举失败。
 */
extern volatile uint32_t g_udap_setup_count;
extern volatile uint8_t g_udap_setup_log[64][8];
extern volatile uint32_t g_udap_event_count[16];
extern volatile uint8_t g_udap_reply_ok[64];
extern volatile uint32_t g_udap_reply_len[64];

void udap_usb_setup_trace(const uint8_t *raw_setup);
void udap_usb_event_trace(uint8_t event);
void udap_usb_reply_trace(uint8_t ok, uint32_t len);

#endif /* __UDAP_AUDIO_WINUSB_H */
