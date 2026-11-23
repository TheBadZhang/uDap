/**
 * @file    audio_usb.c
 * @brief   UAC2 speaker 设备层（CherryUSB）
 *
 * 描述符结构（bInterfaceNumber）：
 *   0  AudioControl：Clock Source -> Input Terminal(USB Streaming)
 *                    -> Feature Unit(Mute) -> Output Terminal(Speaker)
 *   1  AudioStreaming：alt 0 = 空闲；alt 1/2/3 = 16/24/32 位
 *
 * 每个 alt 都带一个显式反馈端点（0x82）。I2S 的实际时钟由 216 MHz PCLK
 * 整数分频得到，和名义采样率有 0.0x%~2.3% 的误差，因此必须把 I2S 的
 * 实际速率通过反馈端点告诉主机，否则环形缓冲区会持续漂移。
 *
 * Windows 10 的 usbaudio2.sys 对 UAC2 支持不完整，请用 Windows 11 测试。
 */

#include "audio_usb.h"

#include <string.h>

#include "usbd_core.h"
#include "usbd_audio.h"

#include "audio_i2s.h"
#include "audio_winusb.h"

/* ------------------------------------------------------------------ 标识 */

#define USBD_VID            0x0D28u
#define USBD_PID            0x0206u /* 与 application(DAP) 0x0204 / bootloader 0x0205 区分 */
#define USBD_MAX_POWER      100u

/* ------------------------------------------------------------- 实体/端点 */

#define AUDIO_AC_INTF        0x00u
#define AUDIO_STREAMING_INTF 0x01u

#define AUDIO_OUT_EP         0x01u
#define AUDIO_FEEDBACK_EP    0x82u

#define AUDIO_OUT_CLOCK_ID   0x01u
#define AUDIO_IT_ID          0x02u
#define AUDIO_FU_ID          0x03u
#define AUDIO_OT_ID          0x04u

/* HS ISO 端点 bInterval=1 -> 每 1 微帧（125 us）一次。
 * 192 kHz / 32 bit / 2ch 时每包 192 字节，远小于 1024；同时反馈值不需要
 * 再按服务间隔缩放（乘 2^(bInterval-1)），避免主机对反馈格式的解释差异。 */
#define AUDIO_EP_INTERVAL    0x01u

/* 每个 alt 的最大包长（每 1 个微帧）：
 *   16 bit: 192k * 2B * 2ch / 8000 = 96，留余量取 104
 *   24 bit: 192k * 3B * 2ch / 8000 = 144，留余量取 152
 *   32 bit: 192k * 4B * 2ch / 8000 = 192，留余量取 200
 */
#define AUDIO_MAXP_16BIT 104u
#define AUDIO_MAXP_24BIT 152u
#define AUDIO_MAXP_32BIT 200u

/* 允许裁剪 alt 设置（默认三个都开）。测试单个位深时可以在命令行用
 * -DAUDIO_SUPPORT_32BIT=0 之类覆盖；最终固件保持 1/1/1。 */
#ifndef AUDIO_SUPPORT_16BIT
#define AUDIO_SUPPORT_16BIT 1
#endif
#ifndef AUDIO_SUPPORT_24BIT
#define AUDIO_SUPPORT_24BIT 1
#endif
#ifndef AUDIO_SUPPORT_32BIT
#define AUDIO_SUPPORT_32BIT 1
#endif
#define AUDIO_ALT_COUNT (AUDIO_SUPPORT_16BIT + AUDIO_SUPPORT_24BIT + AUDIO_SUPPORT_32BIT)

typedef char audio_alt_count_check[(AUDIO_ALT_COUNT >= 1) ? 1 : -1];

#define AUDIO_CHANNEL_CONFIG 0x00000003u /* FL | FR */
#define AUDIO_OUTPUT_CTRL    DBVAL(AUDIO_V2_CONTROL_MUTE), DBVAL(AUDIO_V2_CONTROL_MUTE), DBVAL(AUDIO_V2_CONTROL_MUTE)

/* 只有 Mute 一个可写控制；音量不做（PCM5102 没有数字音量） */
#define AUDIO_AC_SIZ (AUDIO_V2_SIZEOF_AC_HEADER_DESC +                        \
                      AUDIO_V2_SIZEOF_AC_CLOCK_SOURCE_DESC +                  \
                      AUDIO_V2_SIZEOF_AC_INPUT_TERMINAL_DESC +                \
                      AUDIO_V2_SIZEOF_AC_FEATURE_UNIT_DESC(2) +               \
                      AUDIO_V2_SIZEOF_AC_OUTPUT_TERMINAL_DESC)

/* alt 接口 9 + AS general 16 + format 6 + data EP 7 + CS EP 8 + feedback EP 7 */
#define AUDIO_AS_ALT_LEN (9 + 16 + 6 + 7 + 8 + 7)

#define USB_CONFIG_SIZE (9 +                                                       \
                         AUDIO_V2_AC_DESCRIPTOR_LEN +                              \
                         AUDIO_V2_SIZEOF_AC_CLOCK_SOURCE_DESC +                    \
                         AUDIO_V2_SIZEOF_AC_INPUT_TERMINAL_DESC +                  \
                         AUDIO_V2_SIZEOF_AC_FEATURE_UNIT_DESC(2) +                 \
                         AUDIO_V2_SIZEOF_AC_OUTPUT_TERMINAL_DESC +                 \
                         9 + (AUDIO_ALT_COUNT * AUDIO_AS_ALT_LEN) +                \
                         UDAP_HID_DESCRIPTOR_LEN)

#define USB_CONFIG_INTF_COUNT 0x03 /* AC + AS + HID 控制接口 */

/*
 * UAC2 AS 接口的 alt 描述符（带显式反馈端点）。
 * _maxpkt 由 AUDIO_MAXP_* 给出。
 */
#define AUDIO_AS_ALT(_alt, _subslot, _bitres, _maxpkt)                                                      \
    USB_INTERFACE_DESCRIPTOR_INIT(AUDIO_STREAMING_INTF, _alt, 0x02, USB_DEVICE_CLASS_AUDIO,                \
                                  AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOLv20, 0x00),                  \
    0x10, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_GENERAL, AUDIO_IT_ID, 0x00,                      \
    AUDIO_FORMAT_TYPE_I, DBVAL(AUDIO_V2_FORMAT_PCM), 0x02, DBVAL(AUDIO_CHANNEL_CONFIG), 0x00,               \
    0x06, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_FORMAT_TYPE, AUDIO_FORMAT_TYPE_I,                \
    _subslot, _bitres,                                                                                      \
    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT, AUDIO_OUT_EP, 0x05, WBVAL(_maxpkt), AUDIO_EP_INTERVAL,              \
    0x08, AUDIO_ENDPOINT_DESCRIPTOR_TYPE, AUDIO_ENDPOINT_GENERAL, 0x00, 0x00, 0x00, 0x00, 0x00,             \
    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT, AUDIO_FEEDBACK_EP, 0x11, WBVAL(0x04), AUDIO_EP_INTERVAL

/* ------------------------------------------------------------------ 描述符 */

static const uint8_t device_descriptor[] = {
    /* 控制接口改成 HID 后不再需要 BOS/MS OS 描述符，bcdUSB 可以回到 2.0；
     * bcdDevice=0x0102：Windows 把 PnP 缓存按 VID/PID/bcdDevice 记录，
     * 每次改描述符都 bump 一下，确保不会复用旧的设备节点。 */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0103, 0x01),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, USB_CONFIG_INTF_COUNT, 0x01, USB_CONFIG_BUS_POWERED,
                               USBD_MAX_POWER),

    /* AudioControl：IAD + AC 接口 + AC Header */
    AUDIO_V2_AC_DESCRIPTOR_INIT(AUDIO_AC_INTF, 0x02, AUDIO_AC_SIZ, AUDIO_CATEGORY_SPEAKER, 0x00, 0x00),
    /* Clock Source：内部可编程时钟，支持读写采样频率 */
    AUDIO_V2_AC_CLOCK_SOURCE_DESCRIPTOR_INIT(AUDIO_OUT_CLOCK_ID, 0x03, 0x03),
    /* Input Terminal：USB Streaming */
    AUDIO_V2_AC_INPUT_TERMINAL_DESCRIPTOR_INIT(AUDIO_IT_ID, AUDIO_TERMINAL_STREAMING,
                                               AUDIO_OUT_CLOCK_ID, 2, AUDIO_CHANNEL_CONFIG, 0x0000),
    /* Feature Unit：Mute */
    AUDIO_V2_AC_FEATURE_UNIT_DESCRIPTOR_INIT(AUDIO_FU_ID, AUDIO_IT_ID, AUDIO_OUTPUT_CTRL),
    /* Output Terminal：Speaker */
    AUDIO_V2_AC_OUTPUT_TERMINAL_DESCRIPTOR_INIT(AUDIO_OT_ID, AUDIO_OUTTERM_SPEAKER, AUDIO_FU_ID,
                                                AUDIO_OUT_CLOCK_ID, 0x0000),

    /* AudioStreaming alt 0：空闲、无端点 */
    USB_INTERFACE_DESCRIPTOR_INIT(AUDIO_STREAMING_INTF, 0x00, 0x00, USB_DEVICE_CLASS_AUDIO,
                                  AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOLv20, 0x00),
    /* alt 1/2/3：16/24/32 位（可用 AUDIO_SUPPORT_* 裁剪） */
#if AUDIO_SUPPORT_16BIT
    AUDIO_AS_ALT(1, 2, 16, AUDIO_MAXP_16BIT),
#endif
#if AUDIO_SUPPORT_24BIT
    AUDIO_AS_ALT(2, 3, 24, AUDIO_MAXP_24BIT),
#endif
#if AUDIO_SUPPORT_32BIT
    AUDIO_AS_ALT(3, 4, 32, AUDIO_MAXP_32BIT),
#endif

    /* HID 控制接口：上位机通过它读写 EQ 参数（厂商用途页 0xFF00，64 字节报文）。
     * 它在 usbd_add_interface 里必须是第 3 个注册，才能拿到接口号 2。 */
    UDAP_HID_INTERFACE_DESCRIPTOR_INIT()
};

/* 描述符总长必须与 USB_CONFIG_SIZE 完全一致 */
typedef char audio_config_size_check[(sizeof(config_descriptor) == USB_CONFIG_SIZE) ? 1 : -1];

static const uint8_t device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, 0x01),
};

static const char *string_descriptors[] = {
    (const char[]){0x09, 0x04}, /* Langid */
    "uDAP",                     /* Manufacturer */
    "uDAP Speaker",        /* Product */
    "uDAP-Speaker",          /* Serial Number（改串号强制 Windows 丢弃旧的音频端点/设备节点缓存） */
};

static const uint8_t *device_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed) {
    (void)speed;
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index) {
    (void)speed;
    if (index >= (sizeof(string_descriptors) / sizeof(string_descriptors[0]))) {
        return NULL;
    }
    return string_descriptors[index];
}

static const struct usb_descriptor audio_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

/* --------------------------------------------------------- 采样率范围表 */

/* UAC2 Clock Source Range：4 个离散采样率（dMin == dMax，dRes = 0）
 *
 * 只保留 44.1k / 48k / 96k / 192k：这四档在 216 MHz 主频下的实际 I2S
 * 误差都在异步反馈能覆盖的范围内（<0.7%），而 11.025k/22.05k/32k 这类
 * 低频档在整数分频下要么误差大、要么没有实用价值。
 *
 * 注意：绝对不要在这里加回 8000 Hz！Windows 的 usbaudio2.sys 在
 * IsoStreamOut::AllocateIsochResources 里对 8 kHz 的 UAC2 流会做一次
 * "div r32, r32" 且除数为 0（内核态除零），设备一插入就
 * BSOD 0x0000003B (0xc0000094)，本机 2026-11-23 就是这样被打崩的。
 * 官方问答里有同签名的复现：
 * https://learn.microsoft.com/en-us/answers/questions/3913496/
 *   why-windows-10-11-crashes-when-i-connect-an-usb-ua
 * 16k/44.1k/48k/96k/192k 都不会触发。 */
static const uint8_t sample_freq_table[] = {
    AUDIO_SAMPLE_FREQ_NUM(4),
    AUDIO_SAMPLE_FREQ_4B(44100),  AUDIO_SAMPLE_FREQ_4B(44100),  AUDIO_SAMPLE_FREQ_4B(0),
    AUDIO_SAMPLE_FREQ_4B(48000),  AUDIO_SAMPLE_FREQ_4B(48000),  AUDIO_SAMPLE_FREQ_4B(0),
    AUDIO_SAMPLE_FREQ_4B(96000),  AUDIO_SAMPLE_FREQ_4B(96000),  AUDIO_SAMPLE_FREQ_4B(0),
    AUDIO_SAMPLE_FREQ_4B(192000), AUDIO_SAMPLE_FREQ_4B(192000), AUDIO_SAMPLE_FREQ_4B(0),
};

/* ------------------------------------------------------------- alt 表 */

typedef struct {
    uint8_t alt;
    uint8_t subslot;
    uint8_t bitres;
    uint16_t max_packet;
    audio_format_t format;
} audio_alt_t;

static const audio_alt_t audio_alts[] = {
#if AUDIO_SUPPORT_16BIT
    {1, 2, 16, AUDIO_MAXP_16BIT, AUDIO_FORMAT_16BIT},
#endif
#if AUDIO_SUPPORT_24BIT
    {2, 3, 24, AUDIO_MAXP_24BIT, AUDIO_FORMAT_24BIT},
#endif
#if AUDIO_SUPPORT_32BIT
    {3, 4, 32, AUDIO_MAXP_32BIT, AUDIO_FORMAT_32BIT},
#endif
};

static const audio_alt_t *audio_alt_lookup(uint8_t alt) {
    for (uint32_t i = 0u; i < (sizeof(audio_alts) / sizeof(audio_alts[0])); i++) {
        if (audio_alts[i].alt == alt) {
            return &audio_alts[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- 运行状态 */

#define AUDIO_READ_MAX 1024u

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_read_buffer[AUDIO_READ_MAX];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_feedback_buffer[4];

static volatile bool s_streaming;
static volatile uint32_t s_rate = 48000u;
static volatile uint32_t s_read_len = 400u;

/* 调试计数（pyocd 可以直接读；LTO 下用 used 防止被裁掉） */
volatile uint32_t g_audio_out_irq_count __attribute__((used));
volatile uint32_t g_audio_out_bytes __attribute__((used));
volatile uint32_t g_audio_fb_irq_count __attribute__((used));

/**
 * 反馈值：bInterval=1 时，16.16「采样/微帧」直接就是端点每个服务间隔需要的值。
 * 不要再按 2^(bInterval-1) 缩放；bInterval>1 时主机对缩放的约定并不统一，
 * 这也是本工程固定用 bInterval=1 的原因之一。
 */
static void audio_feedback_fill(void) {
    uint32_t fb = audio_i2s_feedback(); /* 16.16，采样/微帧 */
    s_feedback_buffer[0] = (uint8_t)(fb);
    s_feedback_buffer[1] = (uint8_t)(fb >> 8);
    s_feedback_buffer[2] = (uint8_t)(fb >> 16);
    s_feedback_buffer[3] = (uint8_t)(fb >> 24);
}

static void audio_feedback_start(uint8_t busid) {
    audio_feedback_fill();
    usbd_ep_start_write(busid, AUDIO_FEEDBACK_EP, s_feedback_buffer, 4u);
}

/* ------------------------------------------------------------- 数据端点 */

void usbd_audio_iso_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)ep;

    if (!s_streaming) {
        return;
    }
    g_audio_out_irq_count++;
    if (nbytes != 0u) {
        g_audio_out_bytes += nbytes;
        audio_i2s_write(s_read_buffer, nbytes);
    }
    usbd_ep_start_read(busid, AUDIO_OUT_EP, s_read_buffer, s_read_len);
}

void usbd_audio_iso_out_feedback_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)ep;
    (void)nbytes;

    if (!s_streaming) {
        return;
    }
    g_audio_fb_irq_count++;
    audio_feedback_fill();
    usbd_ep_start_write(busid, AUDIO_FEEDBACK_EP, s_feedback_buffer, 4u);
}

static struct usbd_endpoint audio_out_ep = {
    .ep_cb = usbd_audio_iso_out_callback,
    .ep_addr = AUDIO_OUT_EP,
};

static struct usbd_endpoint audio_feedback_ep = {
    .ep_cb = usbd_audio_iso_out_feedback_callback,
    .ep_addr = AUDIO_FEEDBACK_EP,
};

/* ------------------------------------------------------------- 接口切换 */

static void audio_stream_start(uint8_t busid, const audio_alt_t *alt) {
    s_streaming = false;
    s_read_len = alt->max_packet;

    audio_i2s_start(alt->format, s_rate);

    s_streaming = true;
    usbd_ep_start_read(busid, AUDIO_OUT_EP, s_read_buffer, s_read_len);
    audio_feedback_start(busid);
}

static void audio_stream_stop(void) {
    s_streaming = false;
    audio_i2s_stop();
}

static void audio_as_notify(uint8_t busid, uint8_t event, void *arg) {
    if (event != USBD_EVENT_SET_INTERFACE) {
        return;
    }

    struct usb_interface_descriptor *intf = (struct usb_interface_descriptor *)arg;
    if (intf->bInterfaceNumber != AUDIO_STREAMING_INTF) {
        return;
    }

    const audio_alt_t *alt = audio_alt_lookup(intf->bAlternateSetting);
    if (alt != NULL) {
        audio_stream_start(busid, alt);
    } else {
        audio_stream_stop();
    }
}

/* ------------------------------------------------------------- 音频类回调 */

void usbd_audio_set_sampling_freq(uint8_t busid, uint8_t ep, uint32_t sampling_freq) {
    (void)busid;
    if (ep != AUDIO_OUT_EP) {
        return;
    }
    /* 下限 16 kHz：8 kHz 会触发 usbaudio2.sys 的除零蓝屏（见 sample_freq_table
     * 上方的说明），11.025 kHz 也不在表里，统一抬到 16 kHz。 */
    if (sampling_freq < 16000u) {
        sampling_freq = 16000u;
    }
    /* 上限 192 kHz：I2S 分频精度 + 20 段 EQ 的算力都撑不到 384 kHz
     * （32 bit/384 kHz 需要分频 8.789，只能取 8.5 -> +3.4% 误差）。 */
    if (sampling_freq > 192000u) {
        sampling_freq = 192000u;
    }
    s_rate = sampling_freq;
    audio_i2s_set_rate(sampling_freq);
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t ep) {
    (void)busid;
    return (ep == AUDIO_OUT_EP) ? s_rate : 0u;
}

void usbd_audio_get_sampling_freq_table(uint8_t busid, uint8_t ep, uint8_t **sampling_freq_table) {
    (void)busid;
    if (ep == AUDIO_OUT_EP) {
        *sampling_freq_table = (uint8_t *)sample_freq_table;
    }
}

void usbd_audio_set_mute(uint8_t busid, uint8_t ep, uint8_t ch, bool mute) {
    (void)busid;
    (void)ep;
    (void)ch;
    audio_i2s_set_mute(mute);
}

bool usbd_audio_get_mute(uint8_t busid, uint8_t ep, uint8_t ch) {
    (void)busid;
    (void)ep;
    (void)ch;
    return audio_i2s_get_mute();
}

uint32_t audio_usb_get_sample_rate(void) {
    return s_rate;
}

uint32_t audio_usb_get_out_bytes(void) {
    return g_audio_out_bytes;
}

/* --------------------------------------------------------------- 初始化 */

static void usbd_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;

    udap_usb_event_trace(event);

    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        audio_stream_stop();
        break;
    default:
        break;
    }
}

struct usbd_interface intf0;
struct usbd_interface intf1;

struct audio_entity_info audio_entity_table[] = {
    {.bEntityId = AUDIO_OUT_CLOCK_ID, .bDescriptorSubtype = AUDIO_CONTROL_CLOCK_SOURCE, .ep = AUDIO_OUT_EP},
    {.bEntityId = AUDIO_FU_ID, .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT, .ep = AUDIO_OUT_EP},
};

void audio_usb_init(uint8_t busid, uintptr_t reg_base) {
    usbd_desc_register(busid, &audio_descriptor);

    usbd_add_interface(busid, usbd_audio_init_intf(busid, &intf0, 0x0200, audio_entity_table, 2));
    usbd_add_interface(busid, usbd_audio_init_intf(busid, &intf1, 0x0200, audio_entity_table, 2));

    /* 用我们自己的接口切换逻辑替换音频类默认的 notify（只影响 AS 接口） */
    intf1.notify_handler = audio_as_notify;

    usbd_add_endpoint(busid, &audio_out_ep);
    usbd_add_endpoint(busid, &audio_feedback_ep);

    /* 厂商接口必须最后注册：intf_num 由 usbd_add_interface 的顺序决定，
     * 这里才能拿到配置描述符里写死的接口号 2。 */
    audio_winusb_init(busid);

    usbd_initialize(busid, reg_base, usbd_event_handler);
}
