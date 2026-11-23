/**
 * @file    audio_usb.h
 * @brief   UAC2 speaker 设备层（CherryUSB）
 */

#ifndef __UDAP_AUDIO_USB_H
#define __UDAP_AUDIO_USB_H

#include <stdint.h>

/* 注册描述符 / 接口 / 端点并启动 USB（OTGHS） */
void audio_usb_init(uint8_t busid, uintptr_t reg_base);

/* 主机当前设置的采样率（名义值） */
uint32_t audio_usb_get_sample_rate(void);

/* 从 USB 收到的音频字节总数（诊断用） */
uint32_t audio_usb_get_out_bytes(void);

#endif /* __UDAP_AUDIO_USB_H */
