#pragma once

#include <stdint.h>

/*
 * uDap 私有 vendor command（CMSIS-DAP 的 ID_DAP_Vendor0 = 0x80）。
 *
 * ============================ 报文格式 ============================
 *   请求： [0x80, <子命令>, <参数...>]
 *   应答： [0x80, <状态>, <数据...>]
 *
 * 为什么全塞进 Vendor0：CMSIS-DAP 规范留了 Vendor0..Vendor31 共 32 个命令号，
 * 但主机工具（pyOCD / OpenOCD）对它们一无所知，全靠我们自己分发；用一个命令号
 * 加一个子命令字节，比 32 个命令号更好记，也给以后留了扩展空间（子命令 0..255）。
 *
 * 大小限制：DAP 包是 64 B（全速），应答必须留有余量，所以 GET_INFO 只回 10 B，
 * UID 单独用 GET_UID 取。
 *
 * ============================ 子命令 ============================
 *   0x00 GET_INFO            -> proto, fw 版本, 当前 3 个模式, 能力位
 *   0x01 SET_UART_MODE      <- uart_mode           串口 1+流控 / 双串口
 *   0x02 SET_LED_MODE       <- led_mode            两颗灯的功能
 *   0x03 ENTER_UF2          （无参数）             写握手区 + 软复位进 UF2
 *   0x04 SET_DEBUGGER_MODE  <- debugger_mode       CMSIS-DAP / WCH-Link（需编入）
 *   0x05 RESET              （无参数）             软复位
 *   0x06 GET_UID            （无参数）             芯片 UID（16 个十六进制字符）
 *
 * 参数只有一个字节；带参数的命令会原样回显参数，便于主机确认。
 *
 * 注意：SET_UART_MODE / SET_DEBUGGER_MODE / ENTER_UF2 / RESET 都会改 USB 连接
 * 状态 —— 应答先正常发出，动作在本模块返回约 30 ms 后由主循环执行
 * （见 src/drv/drv_control.h）。主机侧看到的是「应答 -> 设备重新枚举」。
 */

/* 子命令都挂在 CMSIS-DAP 的 ID_DAP_Vendor0（0x80）下，见下面的报文格式 */

/* 协议版本：子命令/字段布局不兼容时递增 */
#define UDAP_VENDOR_PROTO_VERSION 1u

/* 固件版本（与 xmake.lua 的 set_version 保持一致） */
#define UDAP_FIRMWARE_VERSION_MAJOR 1u
#define UDAP_FIRMWARE_VERSION_MINOR 3u
#define UDAP_FIRMWARE_VERSION_PATCH 0u

typedef enum {
    UDAP_VC_GET_INFO = 0x00,
    UDAP_VC_SET_UART_MODE = 0x01,
    UDAP_VC_SET_LED_MODE = 0x02,
    UDAP_VC_ENTER_UF2 = 0x03,
    UDAP_VC_SET_DEBUGGER_MODE = 0x04,
    UDAP_VC_RESET = 0x05,
    UDAP_VC_GET_UID = 0x06,
} udap_vendor_command_t;

typedef enum {
    UDAP_STATUS_OK = 0x00,
    UDAP_STATUS_BAD_PARAM = 0x01,   /* 参数越界 */
    UDAP_STATUS_UNSUPPORTED = 0x02, /* 本固件没编这项功能 */
    UDAP_STATUS_BUSY = 0x03,        /* 已有延迟动作在排队 */
    UDAP_STATUS_STORE_FAILED = 0x04,/* 选项字擦写失败（掉电保存没成） */
} udap_vendor_status_t;

/* GET_INFO 的能力位 */
#define UDAP_CAP_UART_DUAL   (1u << 0) /* 支持双串口模式 */
#define UDAP_CAP_WCHLINK     (1u << 1) /* 编入了 WCH-Link 调试器 */
#define UDAP_CAP_UF2         (1u << 2) /* 支持通过 vendor command 进 UF2 bootloader */
#define UDAP_CAP_LED_MODES   (1u << 3) /* 支持切换 LED 模式 */
