/*
 * uDap 私有 vendor command 的实现（协议见 udap_vendor.h）。
 *
 * 这个文件替代了 CherryDAP 里的 DAP/Source/DAP_vendor.c（空模板）：DAP.c 的
 * DAP_ProcessVendorCommand 是 __WEAK，这里提供强符号覆盖它。xmake.lua 已经把
 * 上游那份从编译列表里去掉，避免重复定义。
 *
 * 全部命令都在 CMSIS-DAP 模式下可用；WCH-Link 模式有另一套 USB 协议，不经过这里。
 *
 * 应答直接写进 DAP 的 response 缓冲（64 B），不再另设静态中转缓冲 ——
 * 双模式固件的 RAM 只剩个位数字节的余量，能省则省。
 */

#include "dap/udap_vendor.h"

#include <stdbool.h>
#include <stdint.h>

// 走 dap_main.h 而不是直接包含 DAP.h：DAP.h 里的 __STATIC_FORCEINLINE 依赖
// CMSIS 的 cmsis_compiler.h，而那是 DAP_config.h 链上带进来的。
#include "dap_main.h"

#include "bsp/bsp_uid.h"
#include "drv/drv_control.h"
#include "drv/drv_led.h"
#include "drv/drv_mode_store.h"
#include "drv/drv_uart.h"
#include "mode_overlay.h"

/* GET_INFO 的能力位 */
static uint8_t vendor_capabilities(void) {
    uint8_t flags = UDAP_CAP_LED_MODES;

#if !WCHLINK_ENABLE && UART_ENABLE
    // 双模式固件串口固定为单串口（见 drv_uart_mode_init），不支持切换
    flags |= UDAP_CAP_UART_DUAL;
#endif
#if UDAP_UF2_BOOTLOADER
    flags |= UDAP_CAP_UF2;
#endif
#if WCHLINK_ENABLE
    flags |= UDAP_CAP_WCHLINK;
#endif
    return flags;
}

uint32_t DAP_ProcessVendorCommand(const uint8_t *request, uint8_t *response) {
    uint32_t request_len = 2u; /* 命令 + 子命令 */
    uint32_t response_len = 2u;
    uint8_t sub = request[1];

    // 只实现了 Vendor0；其余命令号按 CMSIS-DAP 模板的做法原样回显命令字
    if (request[0] != (uint8_t)ID_DAP_Vendor0) {
        response[0] = request[0];
        return (1u << 16) | 1u;
    }

    response[0] = (uint8_t)ID_DAP_Vendor0;
    response[1] = UDAP_STATUS_OK;

    switch (sub) {
    case UDAP_VC_GET_INFO:
        response[2] = UDAP_VENDOR_PROTO_VERSION;
        response[3] = UDAP_FIRMWARE_VERSION_MAJOR;
        response[4] = UDAP_FIRMWARE_VERSION_MINOR;
        response[5] = UDAP_FIRMWARE_VERSION_PATCH;
#if UART_ENABLE
        response[6] = (uint8_t)drv_uart_mode_get();
#else
        response[6] = 0u; /* --uart=n：没有串口，恒报「单串口」 */
#endif
        response[7] = (uint8_t)drv_led_mode();
        response[8] = (uint8_t)drv_control_runtime_mode();
        response[9] = vendor_capabilities();
        response_len = 10u;
        break;

    case UDAP_VC_SET_UART_MODE: {
        uint8_t want = request[2];
        request_len = 3u;

        if ((uint32_t)want >= (uint32_t)DRV_UART_MODE_COUNT) {
            response[1] = UDAP_STATUS_BAD_PARAM;
            break;
        }
#if WCHLINK_ENABLE
        // 双模式固件串口固定单串口（放不下双串口，手势也让给了调试器模式切换）
        response[1] = UDAP_STATUS_UNSUPPORTED;
        break;
#endif
#if !UART_ENABLE
        // --uart=n 的固件里根本没有串口
        response[1] = UDAP_STATUS_UNSUPPORTED;
        break;
#endif

        if ((drv_uart_mode_t)want != drv_uart_mode_get()) {
            // 接口数会变，必须重新枚举；交给主循环在应答发完之后做
            if (!drv_control_request(DRV_CONTROL_APPLY_UART_MODE, want)) {
                response[1] = UDAP_STATUS_BUSY;
                break;
            }
        }

        response[2] = want;
        response_len = 3u;
        break;
    }

    case UDAP_VC_SET_LED_MODE: {
        uint8_t want = request[2];
        request_len = 3u;

        if ((uint32_t)want >= (uint32_t)DRV_LED_MODE_COUNT) {
            response[1] = UDAP_STATUS_BAD_PARAM;
            break;
        }

        // 灯是纯软件状态，立刻生效；持久化失败也照样亮（下次上电回到旧模式）
        drv_led_set_mode((drv_led_mode_t)want);
        if (!drv_mode_store_save_led_mode((drv_led_mode_t)want)) {
            response[1] = UDAP_STATUS_STORE_FAILED;
        }

        response[2] = want;
        response_len = 3u;
        break;
    }

    case UDAP_VC_ENTER_UF2:
        request_len = 2u;
#if !UDAP_UF2_BOOTLOADER
        // 这个构建没有 bootloader（应用在 0x0），复位后只会回到自己
        response[1] = UDAP_STATUS_UNSUPPORTED;
        break;
#endif
        if (!drv_control_request(DRV_CONTROL_ENTER_UF2, 0u)) {
            response[1] = UDAP_STATUS_BUSY;
        }
        break;

    case UDAP_VC_SET_DEBUGGER_MODE: {
        uint8_t want = request[2];
        request_len = 3u;

        if ((uint32_t)want >= (uint32_t)FIRMWARE_MODE_COUNT) {
            response[1] = UDAP_STATUS_BAD_PARAM;
            break;
        }
#if !WCHLINK_ENABLE
        if (want == (uint8_t)FIRMWARE_MODE_WCHLINK) {
            // 本固件只编了 CMSIS-DAP，切过去会变成「没有任何调试器」
            response[1] = UDAP_STATUS_UNSUPPORTED;
            break;
        }
#endif
        if ((firmware_mode_t)want != drv_control_runtime_mode()) {
            // 换 USB 身份，只能复位后重来；模式由 drv_control 落选项字
            if (!drv_control_request(DRV_CONTROL_APPLY_DEBUGGER, want)) {
                response[1] = UDAP_STATUS_BUSY;
                break;
            }
        }

        response[2] = want;
        response_len = 3u;
        break;
    }

    case UDAP_VC_RESET:
        request_len = 2u;
        if (!drv_control_request(DRV_CONTROL_SOFT_RESET, 0u)) {
            response[1] = UDAP_STATUS_BUSY;
        }
        break;

    case UDAP_VC_GET_UID:
        request_len = 2u;
        bsp_get_uid_hex16((char *)&response[2]);
        response_len = 2u + 16u;
        break;

    default:
        response[1] = UDAP_STATUS_UNSUPPORTED;
        break;
    }

    return (request_len << 16) | response_len;
}
