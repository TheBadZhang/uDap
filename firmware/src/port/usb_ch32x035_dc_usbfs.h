#pragma once

/*
 * CH32X035 USBFS 设备端口的扩展接口（WCH-Link 适配层使用）。
 *
 * CherryUSB 的 usbd_ep_* 接口没有「取消一次尚未被主机读取的 IN 传输」的语义，
 * 而 WCH-Link 协议必须能放弃残留的回复，否则主机会话会永久阻塞。这里给出
 * 该芯片端口私有的实现，语义与参考工程 ch32x035-usb-wchlink-main 的同名函数
 * 完全一致：清空 TX_LEN 并把端点回到 NAK，**保留当前 data toggle**，
 * 使下一次写入沿用同一个 Bulk 会话的 DATA PID。
 */

#include <stdint.h>

// 取消尚未被主机读取的 IN 传输，保留当前端点的切换位
int ch32x035_usbd_ep_abort_in(uint8_t busid, uint8_t ep);
