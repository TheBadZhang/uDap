/**
 * @file    audio_i2s.c
 * @brief   I2S1 + DMA 音频输出实现（AT32F405 -> PCM5102）
 *
 * 为什么 DMA 宽度固定为 16 位：
 *   AT32 的 SPI_DT 只有 [15:0] 有效。24/32 位、32 位声道时，RM 明确要求
 *   每个声道写两次 SPI_DT（先高 16 位，再低 16 位），所以 DMA 统一按半字
 *   搬运；真正的位深只影响 I2S 的 I2SDBN/I2SCBN 以及我们填 DMA 缓冲区时
 *   的打包方式。
 *
 * 缓冲区：
 *   USB 中断把「USB 线上的原始小端 PCM 字节」写进 8 KB 环形缓冲区；
 *   DMA 半/全传输中断把环形缓冲区里的数据按当前格式转换后填进 2 x 1 KB
 *   的 DMA 缓冲区。环形缓冲区空时补 0（静音），满时丢弃新包（由 UAC2
 *   显式反馈把主机速率拉回 I2S 实际速率，正常情况下不会满）。
 */

#include "audio_i2s.h"

#include <string.h>

#include "at32f402_405.h"

#include "audio_pins.h"

#include "audio_eq.h"

/* ------------------------------------------------------------------ 参数 */

#define AUDIO_RING_SIZE        8192u
#define AUDIO_RING_MASK        (AUDIO_RING_SIZE - 1u)
#define AUDIO_DMA_HALF_WORDS   512u /* 半缓冲区 512 个半字 = 1024 字节 */
#define AUDIO_DMA_WORDS        (2u * AUDIO_DMA_HALF_WORDS)

/* 每种格式每个立体声帧的输入字节数 / 输出半字数 */
#define AUDIO_FRAME_IN_16      4u  /* L(2) + R(2)      */
#define AUDIO_FRAME_IN_24      6u  /* L(3) + R(3)      */
#define AUDIO_FRAME_IN_32      8u  /* L(4) + R(4)      */
#define AUDIO_FRAME_OUT_16     2u  /* 2 x 半字         */
#define AUDIO_FRAME_OUT_32     4u  /* 每声道两次半字写 */

/* 栈上转换块：必须同时是 4/6/8 的倍数 */
#define AUDIO_STAGE_BYTES      144u

/* ------------------------------------------------------------------ 状态 */

static uint8_t s_ring[AUDIO_RING_SIZE] __attribute__((aligned(4)));
static volatile uint32_t s_ring_head;
static volatile uint32_t s_ring_tail;

static uint16_t s_dma_buf[AUDIO_DMA_WORDS] __attribute__((aligned(4)));

static volatile bool s_streaming;
static volatile bool s_mute;
static audio_format_t s_format = AUDIO_FORMAT_16BIT;
static uint32_t s_req_rate = 48000u;
static uint32_t s_actual_rate = 48000u;
static uint32_t s_pclk;

/* 调试计数（pyocd 可以直接读；LTO 下用 used 防止被裁掉） */
volatile uint32_t g_audio_dma_half_count __attribute__((used));
volatile uint32_t g_audio_ring_underrun_count __attribute__((used));

/* EQ 负载统计（DWT 周期计数） */
static volatile uint32_t g_audio_eq_cycles = 0u;
static volatile uint32_t g_audio_half_period = 0u;
static volatile uint32_t g_audio_eq_load_permille = 0u;
static uint32_t s_last_half_cycle;

/* EQ 用的 float 暂存区：交织立体声，最多 AUDIO_STAGE_BYTES/4 帧 */
static float s_stage_f[(AUDIO_STAGE_BYTES / AUDIO_FRAME_IN_16) * 2u];

static inline uint32_t dwt_cycles(void) {
    return DWT->CYCCNT;
}

/* ------------------------------------------------------------ 环形缓冲区 */

static inline uint32_t ring_used(void) {
    return s_ring_head - s_ring_tail;
}

static void ring_reset(void) {
    s_ring_head = 0u;
    s_ring_tail = 0u;
    __DMB();
}

/* 从环形缓冲区取出最多 len 字节，返回实际取出的字节数 */
static uint32_t ring_read(uint8_t *dst, uint32_t len) {
    uint32_t used = ring_used();
    if (len > used) {
        len = used;
    }
    if (len == 0u) {
        return 0u;
    }

    uint32_t pos = s_ring_tail & AUDIO_RING_MASK;
    uint32_t first = AUDIO_RING_SIZE - pos;
    if (first > len) {
        first = len;
    }
    memcpy(dst, &s_ring[pos], first);
    if (len > first) {
        memcpy(&dst[first], &s_ring[0], len - first);
    }

    __DMB();
    s_ring_tail += len;
    return len;
}

/* ------------------------------------------------------------ 数据转换 */

/* 16 位：USB 小端半字 -> SPI_DT 半字（I2S 硬件 MSB 先发） */
static uint32_t convert_16(const uint8_t *in, uint32_t bytes, uint16_t *out) {
    uint32_t frames = bytes / AUDIO_FRAME_IN_16;
    uint32_t i;
    for (i = 0u; i < frames; i++) {
        const uint8_t *p = &in[i * AUDIO_FRAME_IN_16];
        out[i * AUDIO_FRAME_OUT_16 + 0u] = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        out[i * AUDIO_FRAME_OUT_16 + 1u] = (uint16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
    }
    return frames * AUDIO_FRAME_OUT_16;
}

/*
 * 24 位：USB 每声道 3 字节（b0 LSB、b1、b2 MSB），I2S 32 位声道帧要求
 * 数据左对齐：先写 (b2,b1)，再写 (b0,0)。
 */
static uint32_t convert_24(const uint8_t *in, uint32_t bytes, uint16_t *out) {
    uint32_t frames = bytes / AUDIO_FRAME_IN_24;
    uint32_t i;
    for (i = 0u; i < frames; i++) {
        const uint8_t *p = &in[i * AUDIO_FRAME_IN_24];
        out[i * AUDIO_FRAME_OUT_32 + 0u] = (uint16_t)(((uint16_t)p[2] << 8) | p[1]);
        out[i * AUDIO_FRAME_OUT_32 + 1u] = (uint16_t)((uint16_t)p[0] << 8);
        out[i * AUDIO_FRAME_OUT_32 + 2u] = (uint16_t)(((uint16_t)p[5] << 8) | p[4]);
        out[i * AUDIO_FRAME_OUT_32 + 3u] = (uint16_t)((uint16_t)p[3] << 8);
    }
    return frames * AUDIO_FRAME_OUT_32;
}

/* 32 位：USB 小端 32 位字 -> 先高 16 位、后低 16 位 */
static uint32_t convert_32(const uint8_t *in, uint32_t bytes, uint16_t *out) {
    uint32_t frames = bytes / AUDIO_FRAME_IN_32;
    uint32_t i;
    for (i = 0u; i < frames; i++) {
        const uint8_t *p = &in[i * AUDIO_FRAME_IN_32];
        out[i * AUDIO_FRAME_OUT_32 + 0u] = (uint16_t)(((uint16_t)p[3] << 8) | p[2]);
        out[i * AUDIO_FRAME_OUT_32 + 1u] = (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
        out[i * AUDIO_FRAME_OUT_32 + 2u] = (uint16_t)(((uint16_t)p[7] << 8) | p[6]);
        out[i * AUDIO_FRAME_OUT_32 + 3u] = (uint16_t)(((uint16_t)p[5] << 8) | p[4]);
    }
    return frames * AUDIO_FRAME_OUT_32;
}

static uint32_t convert_block(const uint8_t *in, uint32_t bytes, uint16_t *out) {
    switch (s_format) {
    case AUDIO_FORMAT_24BIT:
        return convert_24(in, bytes, out);
    case AUDIO_FORMAT_32BIT:
        return convert_32(in, bytes, out);
    case AUDIO_FORMAT_16BIT:
    default:
        return convert_16(in, bytes, out);
    }
}

static inline uint32_t frame_in_bytes(void) {
    switch (s_format) {
    case AUDIO_FORMAT_24BIT:
        return AUDIO_FRAME_IN_24;
    case AUDIO_FORMAT_32BIT:
        return AUDIO_FRAME_IN_32;
    case AUDIO_FORMAT_16BIT:
    default:
        return AUDIO_FRAME_IN_16;
    }
}

static inline uint32_t frame_out_words(void) {
    return (s_format == AUDIO_FORMAT_16BIT) ? AUDIO_FRAME_OUT_16 : AUDIO_FRAME_OUT_32;
}

/* ------------------------------------------------------- float 通路（EQ） */

static inline int32_t float_to_i32(float v, float lo, float hi) {
    if (v > hi) {
        v = hi;
    } else if (v < lo) {
        v = lo;
    }
    return (int32_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/* 输入字节流 -> 交织 float（-1.0 .. +1.0），返回帧数 */
static uint32_t decode_to_float(const uint8_t *in, uint32_t bytes, float *out) {
    uint32_t frames;
    uint32_t i;

    switch (s_format) {
    case AUDIO_FORMAT_24BIT:
        frames = bytes / AUDIO_FRAME_IN_24;
        for (i = 0u; i < frames; i++) {
            const uint8_t *p = &in[i * AUDIO_FRAME_IN_24];
            int32_t l = ((int32_t)(int8_t)p[2] << 16) | ((int32_t)p[1] << 8) | (int32_t)p[0];
            int32_t r = ((int32_t)(int8_t)p[5] << 16) | ((int32_t)p[4] << 8) | (int32_t)p[3];
            out[i * 2u + 0u] = (float)l * (1.0f / 8388608.0f);
            out[i * 2u + 1u] = (float)r * (1.0f / 8388608.0f);
        }
        return frames;
    case AUDIO_FORMAT_32BIT:
        frames = bytes / AUDIO_FRAME_IN_32;
        for (i = 0u; i < frames; i++) {
            const uint8_t *p = &in[i * AUDIO_FRAME_IN_32];
            uint32_t lu = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                          ((uint32_t)p[3] << 24);
            uint32_t ru = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) |
                          ((uint32_t)p[7] << 24);
            out[i * 2u + 0u] = (float)(int32_t)lu * (1.0f / 2147483648.0f);
            out[i * 2u + 1u] = (float)(int32_t)ru * (1.0f / 2147483648.0f);
        }
        return frames;
    case AUDIO_FORMAT_16BIT:
    default:
        frames = bytes / AUDIO_FRAME_IN_16;
        for (i = 0u; i < frames; i++) {
            const uint8_t *p = &in[i * AUDIO_FRAME_IN_16];
            int16_t l = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            int16_t r = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
            out[i * 2u + 0u] = (float)l * (1.0f / 32768.0f);
            out[i * 2u + 1u] = (float)r * (1.0f / 32768.0f);
        }
        return frames;
    }
}

/* 交织 float -> SPI_DT 半字流，返回写入的半字数 */
static uint32_t encode_from_float(const float *in, uint32_t frames, uint16_t *out) {
    uint32_t i;

    switch (s_format) {
    case AUDIO_FORMAT_24BIT:
        for (i = 0u; i < frames; i++) {
            uint32_t l = (uint32_t)float_to_i32(in[i * 2u + 0u] * 8388608.0f, -8388608.0f,
                                                8388607.0f) << 8;
            uint32_t r = (uint32_t)float_to_i32(in[i * 2u + 1u] * 8388608.0f, -8388608.0f,
                                                8388607.0f) << 8;
            out[i * AUDIO_FRAME_OUT_32 + 0u] = (uint16_t)(l >> 16);
            out[i * AUDIO_FRAME_OUT_32 + 1u] = (uint16_t)(l & 0xFFFFu);
            out[i * AUDIO_FRAME_OUT_32 + 2u] = (uint16_t)(r >> 16);
            out[i * AUDIO_FRAME_OUT_32 + 3u] = (uint16_t)(r & 0xFFFFu);
        }
        return frames * AUDIO_FRAME_OUT_32;
    case AUDIO_FORMAT_32BIT:
        for (i = 0u; i < frames; i++) {
            uint32_t l = (uint32_t)float_to_i32(in[i * 2u + 0u] * 2147483648.0f, -2147483648.0f,
                                                2147483647.0f);
            uint32_t r = (uint32_t)float_to_i32(in[i * 2u + 1u] * 2147483648.0f, -2147483648.0f,
                                                2147483647.0f);
            out[i * AUDIO_FRAME_OUT_32 + 0u] = (uint16_t)(l >> 16);
            out[i * AUDIO_FRAME_OUT_32 + 1u] = (uint16_t)(l & 0xFFFFu);
            out[i * AUDIO_FRAME_OUT_32 + 2u] = (uint16_t)(r >> 16);
            out[i * AUDIO_FRAME_OUT_32 + 3u] = (uint16_t)(r & 0xFFFFu);
        }
        return frames * AUDIO_FRAME_OUT_32;
    case AUDIO_FORMAT_16BIT:
    default:
        for (i = 0u; i < frames; i++) {
            out[i * AUDIO_FRAME_OUT_16 + 0u] =
                (uint16_t)(int16_t)float_to_i32(in[i * 2u + 0u] * 32768.0f, -32768.0f, 32767.0f);
            out[i * AUDIO_FRAME_OUT_16 + 1u] =
                (uint16_t)(int16_t)float_to_i32(in[i * 2u + 1u] * 32768.0f, -32768.0f, 32767.0f);
        }
        return frames * AUDIO_FRAME_OUT_16;
    }
}

/* 用环形缓冲区的数据（不足则补 0）填满 DMA 的半缓冲区 */
static void fill_dma_half(uint32_t half) {
    uint16_t *out = &s_dma_buf[half * AUDIO_DMA_HALF_WORDS];
    uint8_t stage[AUDIO_STAGE_BYTES];
    bool use_eq = audio_eq_get_enabled() && (audio_eq_active_bands() != 0u);
    uint32_t eq_cycles = 0u;

    g_audio_dma_half_count++;

    if (s_mute) {
        memset(out, 0, AUDIO_DMA_HALF_WORDS * sizeof(uint16_t));
        return;
    }

    uint32_t frames = AUDIO_DMA_HALF_WORDS / frame_out_words();
    uint32_t need = frames * frame_in_bytes();
    uint32_t done_bytes = 0u;
    uint32_t done_words = 0u;

    while (done_bytes < need) {
        uint32_t chunk = need - done_bytes;
        if (chunk > AUDIO_STAGE_BYTES) {
            chunk = AUDIO_STAGE_BYTES;
        }

        uint32_t got = ring_read(stage, chunk);
        if (got == 0u) {
            break;
        }

        if (use_eq) {
            uint32_t n = decode_to_float(stage, got, s_stage_f);
            uint32_t t0 = dwt_cycles();
            audio_eq_process(s_stage_f, n);
            eq_cycles += dwt_cycles() - t0;
            done_words += encode_from_float(s_stage_f, n, &out[done_words]);
        } else {
            done_words += convert_block(stage, got, &out[done_words]);
        }
        done_bytes += got;

        if (got < chunk) {
            break; /* 环形缓冲区已空 */
        }
    }

    if (done_words < AUDIO_DMA_HALF_WORDS) {
        g_audio_ring_underrun_count++;
        memset(&out[done_words], 0,
               (AUDIO_DMA_HALF_WORDS - done_words) * sizeof(uint16_t));
    }

    g_audio_eq_cycles = eq_cycles;
}

void AUDIO_DMA_IRQHandler(void) {
    uint32_t now = dwt_cycles();
    if (s_last_half_cycle != 0u) {
        uint32_t period = now - s_last_half_cycle;
        if (period != 0u) {
            g_audio_half_period = period;
            g_audio_eq_load_permille = (uint32_t)(((uint64_t)g_audio_eq_cycles * 1000u) / period);
        }
    }
    s_last_half_cycle = now;

    if (dma_flag_get(DMA1_HDT2_FLAG) != RESET) {
        dma_flag_clear(DMA1_HDT2_FLAG);
        fill_dma_half(0u);
    }
    if (dma_flag_get(DMA1_FDT2_FLAG) != RESET) {
        dma_flag_clear(DMA1_FDT2_FLAG);
        fill_dma_half(1u);
    }
}

/* -------------------------------------------------------------- I2S 配置 */

/*
 * 复刻 sdk 的 i2s_init() 分频算法，返回实际采样率。
 * 16 位/16 位声道时一帧 32 个 CK；24/32 位/32 位声道时一帧 64 个 CK。
 */
static uint32_t i2s_divider(uint32_t sample_rate, uint32_t *factor_out) {
    uint32_t factor = (s_format == AUDIO_FORMAT_16BIT) ? 32u : 64u;
    uint32_t freq_index;

    /* frequency_index = (((sclk / factor) * 10) / fs) + 5，再做四舍五入 */
    freq_index = (((s_pclk / factor) * 10u) / sample_rate) + 5u;
    freq_index /= 10u;

    uint32_t odd = freq_index & 1u;
    uint32_t div = (freq_index - odd) / 2u;
    if ((div < 2u) || (div > 0x3FFu)) {
        odd = 0u;
        div = 2u;
    }

    *factor_out = factor;
    return (2u * div) + odd;
}

static void i2s_hw_config(void) {
    i2s_init_type init;
    uint32_t factor;
    uint32_t divider;

    divider = i2s_divider(s_req_rate, &factor);
    s_actual_rate = (s_pclk / (factor * divider));

    spi_i2s_reset(AUDIO_I2S);

    i2s_default_para_init(&init);
    init.audio_protocol = I2S_AUDIO_PROTOCOL_PHILLIPS;
    init.data_channel_format = (s_format == AUDIO_FORMAT_16BIT)
                                   ? I2S_DATA_16BIT_CHANNEL_16BIT
                                   : I2S_DATA_32BIT_CHANNEL_32BIT;
    init.mclk_output_enable = FALSE;
    init.audio_sampling_freq = (i2s_audio_sampling_freq_type)s_req_rate;
    init.clock_polarity = I2S_CLOCK_POLARITY_LOW;
    init.operation_mode = I2S_MODE_MASTER_TX;
    init.i2s_ckin_value = 8000000u;
    init.pcm_sample_clock_selection = I2S_PCM_SAMPLE_CLOCK_FALLING;
    i2s_init(AUDIO_I2S, &init);
}

/* -------------------------------------------------------------- DMA 配置 */

static void dma_hw_config(void) {
    dma_init_type init;

    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    dmamux_enable(DMA1, TRUE);

    dma_reset(AUDIO_DMA);

    dma_default_para_init(&init);
    init.buffer_size = AUDIO_DMA_WORDS;
    init.memory_data_width = DMA_MEMORY_DATA_WIDTH_HALFWORD;
    init.memory_inc_enable = TRUE;
    init.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
    init.peripheral_inc_enable = FALSE;
    init.priority = DMA_PRIORITY_HIGH;
    init.loop_mode_enable = TRUE;
    init.memory_base_addr = (uint32_t)s_dma_buf;
    init.peripheral_base_addr = (uint32_t)AUDIO_I2S_DT_ADDR;
    init.direction = DMA_DIR_MEMORY_TO_PERIPHERAL;
    dma_init(AUDIO_DMA, &init);

    dmamux_init(AUDIO_DMA_MUX, DMAMUX_DMAREQ_ID_SPI1_TX);

    dma_interrupt_enable(AUDIO_DMA, DMA_FDT_INT, TRUE);
    dma_interrupt_enable(AUDIO_DMA, DMA_HDT_INT, TRUE);
    nvic_irq_enable(AUDIO_DMA_IRQn, 2u, 0u);
}

/* ------------------------------------------------------------------ 接口 */

void audio_i2s_init(void) {
    crm_clocks_freq_type clocks;
    gpio_init_type gpio;

    crm_clocks_freq_get(&clocks);
    s_pclk = clocks.sclk_freq;

    crm_periph_clock_enable(AUDIO_I2S_GPIO_CLK, TRUE);
    crm_periph_clock_enable(AUDIO_I2S_CLK, TRUE);
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    dmamux_enable(DMA1, TRUE);

    gpio_default_para_init(&gpio);
    gpio.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio.gpio_pull = GPIO_PULL_NONE;
    gpio.gpio_mode = GPIO_MODE_MUX;
    gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio.gpio_pins = AUDIO_I2S_WS_PIN | AUDIO_I2S_CK_PIN | AUDIO_I2S_SD_PIN;
    gpio_init(AUDIO_I2S_GPIO, &gpio);

    gpio_pin_mux_config(AUDIO_I2S_GPIO, AUDIO_I2S_WS_SOURCE, AUDIO_I2S_MUX);
    gpio_pin_mux_config(AUDIO_I2S_GPIO, AUDIO_I2S_CK_SOURCE, AUDIO_I2S_MUX);
    gpio_pin_mux_config(AUDIO_I2S_GPIO, AUDIO_I2S_SD_SOURCE, AUDIO_I2S_MUX);

    /*
     * PA6 不配置：它和 PA7 在板上短接，保持复位后的浮空输入即可。
     * 如果把 PA6 选成 I2S1_MCK，两个输出会顶在一起。
     */

    ring_reset();
    memset(s_dma_buf, 0, sizeof(s_dma_buf));

    nvic_irq_enable(AUDIO_DMA_IRQn, 2u, 0u);

    /* DWT 周期计数：只用来统计 EQ 占 CPU 的比例 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

bool audio_i2s_start(audio_format_t fmt, uint32_t sample_rate) {
    audio_i2s_stop();

    s_format = fmt;
    s_req_rate = sample_rate;
    s_streaming = false;
    __DMB();

    /* EQ 系数跟着新的采样率重算，滤波器状态清零 */
    audio_eq_set_rate(sample_rate);
    s_last_half_cycle = 0u;
    g_audio_eq_cycles = 0u;
    g_audio_half_period = 0u;
    g_audio_eq_load_permille = 0u;

    ring_reset();
    memset(s_dma_buf, 0, sizeof(s_dma_buf));

    i2s_hw_config();
    dma_hw_config();

    spi_i2s_dma_transmitter_enable(AUDIO_I2S, TRUE);
    dma_channel_enable(AUDIO_DMA, TRUE);
    i2s_enable(AUDIO_I2S, TRUE);

    __DMB();
    s_streaming = true;
    return true;
}

void audio_i2s_stop(void) {
    s_streaming = false;
    __DMB();

    s_last_half_cycle = 0u;
    dma_interrupt_enable(AUDIO_DMA, DMA_FDT_INT, FALSE);
    dma_interrupt_enable(AUDIO_DMA, DMA_HDT_INT, FALSE);
    dma_channel_enable(AUDIO_DMA, FALSE);
    dma_reset(AUDIO_DMA);

    i2s_enable(AUDIO_I2S, FALSE);
    spi_i2s_reset(AUDIO_I2S);

    ring_reset();
}

void audio_i2s_set_rate(uint32_t sample_rate) {
    if (sample_rate < 8000u) {
        sample_rate = 8000u;
    }
    if (sample_rate > 384000u) {
        sample_rate = 384000u;
    }

    s_req_rate = sample_rate;
    if (s_streaming) {
        audio_i2s_start(s_format, sample_rate);
    }
}

void audio_i2s_write(const uint8_t *data, uint32_t len) {
    if (!s_streaming || (data == NULL) || (len == 0u)) {
        return;
    }
    if (len > AUDIO_RING_SIZE) {
        return;
    }
    if ((AUDIO_RING_SIZE - ring_used()) < len) {
        return; /* 溢出：丢弃新包，等待反馈把主机速率拉回来 */
    }

    uint32_t pos = s_ring_head & AUDIO_RING_MASK;
    uint32_t first = AUDIO_RING_SIZE - pos;
    if (first > len) {
        first = len;
    }
    memcpy(&s_ring[pos], data, first);
    if (len > first) {
        memcpy(&s_ring[0], &data[first], len - first);
    }

    __DMB();
    s_ring_head += len;
}

bool audio_i2s_is_streaming(void) {
    return s_streaming;
}

uint32_t audio_i2s_requested_rate(void) {
    return s_req_rate;
}

uint32_t audio_i2s_actual_rate(void) {
    return s_actual_rate;
}

uint32_t audio_i2s_ring_level(void) {
    return ring_used();
}

uint32_t audio_i2s_ring_capacity(void) {
    return AUDIO_RING_SIZE;
}

audio_format_t audio_i2s_format(void) {
    return s_format;
}

uint32_t audio_i2s_feedback(void) {
    uint32_t fs = s_actual_rate;
    if (fs == 0u) {
        fs = s_req_rate;
    }
    if (fs == 0u) {
        fs = 48000u;
    }

    /* 16.16：每微帧采样数 */
    int64_t nominal = ((int64_t)fs << 16) / 8000;
    int64_t fb = nominal;

    /* 用环形缓冲区水位做轻微比例修正：水位低 -> 让主机快一点，反之亦然 */
    int32_t err = (int32_t)(AUDIO_RING_SIZE / 2u) - (int32_t)ring_used();
    int64_t corr = ((int64_t)err * fs) << 16;
    corr /= (16 * (int64_t)AUDIO_RING_SIZE * 8000);
    fb += corr;

    /* 限制在名义值的 ±2% 以内，避免异常水位把主机速率拉飞 */
    int64_t min = nominal * 98 / 100;
    int64_t max = nominal * 102 / 100;
    if (fb < min) {
        fb = min;
    } else if (fb > max) {
        fb = max;
    }

    return (uint32_t)fb;
}

void audio_i2s_set_mute(bool mute) {
    s_mute = mute;
}

bool audio_i2s_get_mute(void) {
    return s_mute;
}

uint32_t audio_i2s_underruns(void) {
    return g_audio_ring_underrun_count;
}

uint32_t audio_i2s_half_count(void) {
    return g_audio_dma_half_count;
}

uint32_t audio_i2s_eq_cycles(void) {
    return g_audio_eq_cycles;
}

uint32_t audio_i2s_half_period(void) {
    return g_audio_half_period;
}

uint32_t audio_i2s_eq_load_permille(void) {
    return g_audio_eq_load_permille;
}
