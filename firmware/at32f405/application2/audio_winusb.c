/**
 * @file    audio_winusb.c
 * @brief   复合在 UAC2 扬声器里的 HID 控制接口 + EQ 参数协议
 *
 * ── USB 侧 ────────────────────────────────────────────────────────────
 * 接口 2 是一个 HID 接口（厂商用途页 0xFF00，输入/输出各 64 字节，无 Report
 * ID），两个中断端点 OUT 0x04 / IN 0x83，bInterval 4（HS 下 1 ms）。
 * Windows 自带 hidusb/hidclass 直接驱动，不需要 inf、不需要管理员权限，
 * 也不需要 WCID/MS OS 描述符；上位机用 hidapi 按 VID/PID + 用途页打开设备。
 *
 * ── 协议 ──────────────────────────────────────────────────────────────
 * 每条消息固定 64 字节，小端：
 *     [0]     sync   = 0x5A
 *     [1]     cmd
 *     [2]     status (设备回复：0 = OK)
 *     [3]     index  (band index / 保留)
 *     [4..5]  len    (payload 字节数)
 *     [6..7]  reserved
 *     [8..63] payload (56 字节)
 * 主机发一条请求，设备回同样 cmd 的一条响应；严格一问一答。
 */

#include "audio_winusb.h"

#include <string.h>

#include "usbd_core.h"
#include "usbd_hid.h"

#include "audio_eq.h"
#include "audio_i2s.h"
#include "audio_usb.h"

/* --------------------------------------------------------------- 协议定义 */

#define UDAP_MSG_SIZE   64u
#define UDAP_MSG_SYNC   0x5Au
#define UDAP_PAYLOAD_OFF 8u
#define UDAP_PAYLOAD_MAX (UDAP_MSG_SIZE - UDAP_PAYLOAD_OFF)

#define UDAP_CMD_GET_INFO       0x01u
#define UDAP_CMD_SET_BAND       0x02u
#define UDAP_CMD_GET_BAND       0x03u
#define UDAP_CMD_SET_ENABLE     0x04u
#define UDAP_CMD_SET_MASTER     0x05u
#define UDAP_CMD_RESET_ALL      0x06u

#define UDAP_STATUS_OK          0x00u
#define UDAP_STATUS_BAD_CMD     0x01u
#define UDAP_STATUS_BAD_PARAM   0x02u
#define UDAP_STATUS_BAD_INDEX   0x03u

#define UDAP_FW_VERSION         0x0001u

/* ------------------------------------------------------------- 描述符 */

/* HID 报文描述符：厂商自定义用途页 0xFF00，输入/输出各 64 字节，无 Report ID。
 *   Usage Page (Vendor 0xFF00) / Usage 0x01 / Collection(Application)
 *     Input  : usage 0x02, 8bit x 64 = 64 字节（设备 -> 主机）
 *     Output : usage 0x03, 8bit x 64 = 64 字节（主机 -> 设备） */
const uint8_t g_udap_hid_report_descriptor[UDAP_HID_REPORT_DESC_LEN] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor-Defined 0xFF00) */
    0x09, 0x01,       /* Usage (0x01) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x02,       /*   Usage (0x02) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x40,       /*   Report Count (64) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x09, 0x03,       /*   Usage (0x03) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x40,       /*   Report Count (64) */
    0x91, 0x02,       /*   Output (Data,Var,Abs) */
    0xC0,             /* End Collection */
};

typedef char udap_hid_report_len_check
    [(sizeof(g_udap_hid_report_descriptor) == UDAP_HID_REPORT_DESC_LEN) ? 1 : -1];

/* --------------------------------------------------------------- 端点 */

static uint8_t s_out_buf[UDAP_MSG_SIZE] __attribute__((aligned(4)));
static uint8_t s_in_buf[UDAP_MSG_SIZE] __attribute__((aligned(4)));
static volatile bool s_in_busy;
static volatile bool s_resp_pending;

/* --------------------------------------------------------- 小端序列化 */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_f32(uint8_t *p, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    put_u32(p, u);
}

static float get_f32(const uint8_t *p) {
    uint32_t u = get_u32(p);
    float v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

/* ------------------------------------------------------------- 命令处理 */

static void udap_handle_message(const uint8_t *req) {
    uint8_t *pl = &s_in_buf[UDAP_PAYLOAD_OFF];
    uint8_t cmd = req[1];
    uint8_t index = req[3];
    uint16_t len = 0u;
    uint8_t status = UDAP_STATUS_OK;

    memset(s_in_buf, 0, UDAP_MSG_SIZE);
    s_in_buf[0] = UDAP_MSG_SYNC;
    s_in_buf[1] = cmd;

    switch (cmd) {
    case UDAP_CMD_GET_INFO: {
        put_u16(&pl[0], UDAP_FW_VERSION);
        pl[2] = (uint8_t)AUDIO_EQ_MAX_BANDS;
        pl[3] = (uint8_t)audio_eq_active_bands();
        put_u32(&pl[4], audio_i2s_requested_rate());
        put_u32(&pl[8], audio_i2s_actual_rate());
        pl[12] = (uint8_t)((audio_i2s_format() == AUDIO_FORMAT_16BIT)
                               ? 16
                               : ((audio_i2s_format() == AUDIO_FORMAT_24BIT) ? 24 : 32));
        pl[13] = audio_eq_get_enabled() ? 1u : 0u;
        pl[14] = audio_i2s_is_streaming() ? 1u : 0u;
        pl[15] = audio_i2s_get_mute() ? 1u : 0u;
        put_f32(&pl[16], audio_eq_get_master_gain_db());
        put_u32(&pl[20], audio_i2s_ring_level());
        put_u32(&pl[24], audio_i2s_ring_capacity());
        put_u32(&pl[28], audio_i2s_underruns());
        put_u32(&pl[32], audio_i2s_half_count());
        put_u32(&pl[36], audio_usb_get_out_bytes());
        put_u32(&pl[40], audio_i2s_eq_cycles());
        put_u32(&pl[44], audio_i2s_half_period());
        put_u32(&pl[48], audio_i2s_eq_load_permille());
        put_u32(&pl[52], 0u);
        len = 56u;
        break;
    }
    case UDAP_CMD_GET_BAND: {
        audio_eq_band_t band;
        if (audio_eq_get_band(index, &band) != 0) {
            status = UDAP_STATUS_BAD_INDEX;
            break;
        }
        put_f32(&pl[0], band.freq);
        put_f32(&pl[4], band.q);
        put_f32(&pl[8], band.gain_db);
        pl[12] = band.type;
        pl[13] = band.enabled;
        len = 16u;
        break;
    }
    case UDAP_CMD_SET_BAND: {
        audio_eq_band_t band;
        band.freq = get_f32(&req[UDAP_PAYLOAD_OFF + 0]);
        band.q = get_f32(&req[UDAP_PAYLOAD_OFF + 4]);
        band.gain_db = get_f32(&req[UDAP_PAYLOAD_OFF + 8]);
        band.type = req[UDAP_PAYLOAD_OFF + 12];
        band.enabled = req[UDAP_PAYLOAD_OFF + 13];
        if (band.type > AUDIO_EQ_TYPE_NOTCH) {
            status = UDAP_STATUS_BAD_PARAM;
            break;
        }
        if (audio_eq_set_band(index, &band) != 0) {
            status = UDAP_STATUS_BAD_INDEX;
            break;
        }
        len = 0u;
        break;
    }
    case UDAP_CMD_SET_ENABLE: {
        audio_eq_set_enabled(req[UDAP_PAYLOAD_OFF] != 0u);
        len = 1u;
        pl[0] = audio_eq_get_enabled() ? 1u : 0u;
        break;
    }
    case UDAP_CMD_SET_MASTER: {
        audio_eq_set_master_gain_db(get_f32(&req[UDAP_PAYLOAD_OFF]));
        put_f32(&pl[0], audio_eq_get_master_gain_db());
        len = 4u;
        break;
    }
    case UDAP_CMD_RESET_ALL: {
        audio_eq_init();
        put_u32(&pl[0], audio_eq_active_bands());
        len = 4u;
        break;
    }
    default:
        status = UDAP_STATUS_BAD_CMD;
        break;
    }

    s_in_buf[2] = status;
    s_in_buf[3] = index;
    put_u16(&s_in_buf[4], len);
}

/* --------------------------------------------------------------- 端点回调 */

static void udap_send_response(uint8_t busid) {
    if (s_in_busy) {
        s_resp_pending = true;
        return;
    }
    s_resp_pending = false;
    s_in_busy = true;
    usbd_ep_start_write(busid, UDAP_HID_IN_EP, s_in_buf, UDAP_MSG_SIZE);
}

static void udap_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)ep;

    if ((nbytes >= UDAP_PAYLOAD_OFF) && (s_out_buf[0] == UDAP_MSG_SYNC)) {
        udap_handle_message(s_out_buf);
        udap_send_response(busid);
    }

    usbd_ep_start_read(busid, UDAP_HID_OUT_EP, s_out_buf, UDAP_MSG_SIZE);
}

static void udap_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)ep;
    (void)nbytes;

    s_in_busy = false;
    if (s_resp_pending) {
        s_resp_pending = false;
        s_in_busy = true;
        usbd_ep_start_write(busid, UDAP_HID_IN_EP, s_in_buf, UDAP_MSG_SIZE);
    }
}

static struct usbd_endpoint s_udap_out_ep_desc = {
    .ep_addr = UDAP_HID_OUT_EP,
    .ep_cb = udap_out_callback,
};

static struct usbd_endpoint s_udap_in_ep_desc = {
    .ep_addr = UDAP_HID_IN_EP,
    .ep_cb = udap_in_callback,
};

static struct usbd_interface s_udap_intf;

/* ------------------------------------------------------------ 枚举跟踪（调试）
 * 用 SWD 直接读 RAM 里的 g_udap_setup_log / g_udap_setup_count / g_udap_event_count
 * 就能看到 Windows 发来的完整 SETUP 序列，判断枚举卡在哪一步。
 */
volatile uint32_t g_udap_setup_count;
volatile uint8_t g_udap_setup_log[64][8];
volatile uint32_t g_udap_event_count[16];
volatile uint8_t g_udap_reply_ok[64];
volatile uint32_t g_udap_reply_len[64];

void udap_usb_setup_trace(const uint8_t *raw_setup) {
    uint32_t idx = g_udap_setup_count & 0x3Fu;

    for (uint32_t i = 0; i < 8u; i++) {
        g_udap_setup_log[idx][i] = raw_setup[i];
    }
    g_udap_setup_count++;
}

void udap_usb_reply_trace(uint8_t ok, uint32_t len) {
    uint32_t idx = (g_udap_setup_count - 1u) & 0x3Fu;

    g_udap_reply_ok[idx] = ok;
    g_udap_reply_len[idx] = len;
}

void udap_usb_event_trace(uint8_t event) {
    if (event < 16u) {
        g_udap_event_count[event]++;
    }
}

/* 接口被 SET_CONFIGURATION 选中后才能真正收发数据：端点是在
 * usbd_set_configuration() 里打开的，所以 OUT 读只能在这里挂第一次。 */
static void udap_hid_notify(uint8_t busid, uint8_t event, void *arg) {
    (void)arg;

    if (event != USBD_EVENT_CONFIGURED) {
        return;
    }

    s_in_busy = false;
    s_resp_pending = false;
    usbd_ep_start_read(busid, UDAP_HID_OUT_EP, s_out_buf, UDAP_MSG_SIZE);
}

void audio_winusb_init(uint8_t busid) {
    /* HID 类：注册接口时把报文描述符交给 class 驱动，
     * 它负责 GET_DESCRIPTOR(HID/REPORT)、SET_IDLE 等标准请求。
     * 必须是第 3 个注册的接口 -> intf_num = 2，与描述符里的 bInterfaceNumber 一致。 */
    usbd_add_interface(busid, usbd_hid_init_intf(busid, &s_udap_intf, g_udap_hid_report_descriptor,
                                                 UDAP_HID_REPORT_DESC_LEN));
    s_udap_intf.notify_handler = udap_hid_notify;
    usbd_add_endpoint(busid, &s_udap_out_ep_desc);
    usbd_add_endpoint(busid, &s_udap_in_ep_desc);
}
