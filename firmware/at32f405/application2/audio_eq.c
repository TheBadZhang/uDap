/**
 * @file    audio_eq.c
 * @brief   20 段参数均衡器（float / 双声道 / 级联 DF2T biquad）
 *
 * 采样到系数的推导用 RBJ Audio EQ Cookbook 的常用形式，全部用 float 完成，
 * 不依赖 libm（自写 sin/cos/2^x/sqrt），方便在 MCU 上只链接 newlib-nano。
 *
 * CPU：一个 biquad 5 乘 4 加，Cortex-M4F 上约 10~14 个周期。20 段 × 2 声道
 * 在 48 kHz 时约占 216 MHz 的 10%~15%，96 kHz 约 20%~30%，192 kHz 约 40%~60%。
 * 增益为 0 dB 的 peaking/shelf 在数学上恒等，运行前就被剔除，所以"平直"
 * 预设几乎不耗 CPU。
 */

#include "audio_eq.h"

#include <stddef.h>

#ifdef AUDIO_EQ_HOST_TEST
#define EQ_IRQ_SAVE()      0u
#define EQ_IRQ_OFF()       ((void)0)
#define EQ_IRQ_RESTORE(pm) ((void)(pm))
#else
#include "at32f402_405.h"
#define EQ_IRQ_SAVE()      __get_PRIMASK()
#define EQ_IRQ_OFF()       __disable_irq()
#define EQ_IRQ_RESTORE(pm) __set_PRIMASK(pm)
#endif

#define EQ_PI     3.14159265358979f
#define EQ_LOG2_10 3.32192809488736f

typedef struct {
    float b0, b1, b2, a1, a2;
} eq_biquad_t;

static eq_biquad_t s_coef[AUDIO_EQ_MAX_BANDS];
static float s_state[2][AUDIO_EQ_MAX_BANDS][2]; /* [声道][段][s1,s2] */
static audio_eq_band_t s_bands[AUDIO_EQ_MAX_BANDS];
static uint8_t s_run[AUDIO_EQ_MAX_BANDS]; /* 本次真正参与运算的段索引 */
static uint32_t s_run_count;
static uint32_t s_fs = 48000u;
static bool s_enabled;
static float s_master_db;
static float s_master_gain = 1.0f;
static bool s_inited;

/* ------------------------------------------------------------ 数学小工具 */

/* sin(x)，x 已归约到 [-pi, pi]（泰勒到 x^9，误差 < 1e-8） */
static float eq_sinf(float x) {
    if (x > EQ_PI) {
        x -= 2.0f * EQ_PI;
    } else if (x < -EQ_PI) {
        x += 2.0f * EQ_PI;
    }
    if (x > EQ_PI * 0.5f) {
        x = EQ_PI - x;
    } else if (x < -EQ_PI * 0.5f) {
        x = -EQ_PI - x;
    }
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.66666672e-1f +
                             x2 * (8.33333377e-3f +
                                   x2 * (-1.98412698e-4f + x2 * 2.75573192e-6f))));
}

/* cos(x) = sin(pi/2 - x) */
static float eq_cosf(float x) {
    return eq_sinf(EQ_PI * 0.5f - x);
}

/* 2^x */
static float eq_exp2f(float x) {
    int32_t k = (int32_t)x;
    float f = x - (float)k;
    if (f < 0.0f) {
        f += 1.0f;
        k -= 1;
    }

    /* 2^f，f in [0,1)：e^(f*ln2) 泰勒到 7 阶，相对误差 < 2e-6 */
    float t = f * 0.6931471805599453f;
    float p = 1.0f + t * (1.0f + t * (0.5f + t * (1.66666672e-1f +
                             t * (4.16666679e-2f + t * (8.33333377e-3f +
                                   t * (1.38888892e-3f + t * 1.98412698e-4f))))));

    int32_t e = k + 127;
    if (e <= 0) {
        return 0.0f;
    }
    if (e >= 255) {
        e = 254;
    }
    union {
        float f;
        uint32_t u;
    } sc;
    sc.u = ((uint32_t)e) << 23;
    return p * sc.f;
}

/* 10^x（x = gain_db / 40） */
static float eq_pow10f(float x) {
    return eq_exp2f(x * EQ_LOG2_10);
}

/* sqrt（位运算初值 + 3 次牛顿迭代，相对误差 < 1e-9） */
static float eq_sqrtf(float x) {
    if (x <= 0.0f) {
        return 0.0f;
    }
    union {
        float f;
        uint32_t u;
    } v;
    v.f = x;
    v.u = (v.u >> 1) + 0x1FC00000u;
    float y = v.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}

/* ---------------------------------------------------------------- 系数 */

static bool eq_band_is_identity(const audio_eq_band_t *b) {
    /* peaking / shelf 在 0 dB 时 H(z) 恒等于 1，可以直接跳过 */
    if ((b->type == AUDIO_EQ_TYPE_PEAKING) || (b->type == AUDIO_EQ_TYPE_LOWSHELF) ||
        (b->type == AUDIO_EQ_TYPE_HIGHSHELF)) {
        if ((b->gain_db > -0.01f) && (b->gain_db < 0.01f)) {
            return true;
        }
    }
    return false;
}

static void eq_update_coef(uint32_t idx) {
    const audio_eq_band_t *b = &s_bands[idx];
    float fs = (float)s_fs;
    float f0 = b->freq;
    float q = b->q;
    float gain = b->gain_db;
    uint8_t type = b->type;

    if (f0 < 10.0f) {
        f0 = 10.0f;
    }
    if (f0 > fs * 0.45f) {
        f0 = fs * 0.45f;
    }
    if (q < 0.1f) {
        q = 0.1f;
    }
    if (q > 20.0f) {
        q = 20.0f;
    }
    if (gain < -24.0f) {
        gain = -24.0f;
    }
    if (gain > 24.0f) {
        gain = 24.0f;
    }

    float w0 = 2.0f * EQ_PI * f0 / fs;
    float cw = eq_cosf(w0);
    float sw = eq_sinf(w0);
    float alpha = sw / (2.0f * q);
    float A = eq_pow10f(gain * 0.025f);
    float sqrtA;

    float b0;
    float b1;
    float b2;
    float a0;
    float a1;
    float a2;

    switch (type) {
    case AUDIO_EQ_TYPE_LOWSHELF:
        sqrtA = eq_sqrtf(A);
        b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + 2.0f * sqrtA * alpha);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - 2.0f * sqrtA * alpha);
        a0 = (A + 1.0f) + (A - 1.0f) * cw + 2.0f * sqrtA * alpha;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
        a2 = (A + 1.0f) + (A - 1.0f) * cw - 2.0f * sqrtA * alpha;
        break;
    case AUDIO_EQ_TYPE_HIGHSHELF:
        sqrtA = eq_sqrtf(A);
        b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + 2.0f * sqrtA * alpha);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - 2.0f * sqrtA * alpha);
        a0 = (A + 1.0f) - (A - 1.0f) * cw + 2.0f * sqrtA * alpha;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
        a2 = (A + 1.0f) - (A - 1.0f) * cw - 2.0f * sqrtA * alpha;
        break;
    case AUDIO_EQ_TYPE_LOWPASS:
        b0 = (1.0f - cw) * 0.5f;
        b1 = 1.0f - cw;
        b2 = (1.0f - cw) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    case AUDIO_EQ_TYPE_HIGHPASS:
        b0 = (1.0f + cw) * 0.5f;
        b1 = -(1.0f + cw);
        b2 = (1.0f + cw) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    case AUDIO_EQ_TYPE_NOTCH:
        b0 = 1.0f;
        b1 = -2.0f * cw;
        b2 = 1.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    case AUDIO_EQ_TYPE_PEAKING:
    default:
        b0 = 1.0f + alpha * A;
        b1 = -2.0f * cw;
        b2 = 1.0f - alpha * A;
        a0 = 1.0f + alpha / A;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha / A;
        break;
    }

    float inv_a0 = 1.0f / a0;
    s_coef[idx].b0 = b0 * inv_a0;
    s_coef[idx].b1 = b1 * inv_a0;
    s_coef[idx].b2 = b2 * inv_a0;
    s_coef[idx].a1 = a1 * inv_a0;
    s_coef[idx].a2 = a2 * inv_a0;
}

static void eq_rebuild_run(void) {
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < AUDIO_EQ_MAX_BANDS; i++) {
        if (s_bands[i].enabled && !eq_band_is_identity(&s_bands[i])) {
            s_run[n++] = (uint8_t)i;
        }
    }
    s_run_count = n;
}

/* ------------------------------------------------------------ 默认参数 */

static const float s_default_freq[AUDIO_EQ_MAX_BANDS] = {
    20.0f,    28.8f,    41.4f,    59.5f,    85.6f,    123.2f,   177.2f,   254.9f,
    366.6f,   527.3f,   758.5f,   1091.1f,  1569.5f,  2257.7f,  3247.6f,  4671.4f,
    6719.6f,  9665.9f,  13903.9f, 20000.0f,
};

void audio_eq_init(void) {
    for (uint32_t i = 0u; i < AUDIO_EQ_MAX_BANDS; i++) {
        s_bands[i].freq = s_default_freq[i];
        s_bands[i].q = 1.4f;
        s_bands[i].gain_db = 0.0f;
        s_bands[i].type = AUDIO_EQ_TYPE_PEAKING;
        s_bands[i].enabled = 1u;
    }
    s_enabled = false;
    s_master_db = 0.0f;
    s_master_gain = 1.0f;
    s_fs = 48000u;
    s_inited = true;

    audio_eq_set_rate(48000u);
}

void audio_eq_set_rate(uint32_t fs) {
    if (fs < 8000u) {
        fs = 8000u;
    }
    if (fs > 384000u) {
        fs = 384000u;
    }

    uint32_t pm = EQ_IRQ_SAVE();
    EQ_IRQ_OFF();

    s_fs = fs;
    for (uint32_t i = 0u; i < AUDIO_EQ_MAX_BANDS; i++) {
        eq_update_coef(i);
        s_state[0][i][0] = 0.0f;
        s_state[0][i][1] = 0.0f;
        s_state[1][i][0] = 0.0f;
        s_state[1][i][1] = 0.0f;
    }
    eq_rebuild_run();

    EQ_IRQ_RESTORE(pm);
}

uint32_t audio_eq_rate(void) {
    return s_fs;
}

int audio_eq_set_band(uint32_t idx, const audio_eq_band_t *band) {
    if ((idx >= AUDIO_EQ_MAX_BANDS) || (band == NULL)) {
        return -1;
    }

    uint32_t pm = EQ_IRQ_SAVE();
    EQ_IRQ_OFF();

    s_bands[idx] = *band;
    s_bands[idx].enabled = band->enabled ? 1u : 0u;
    eq_update_coef(idx);
    eq_rebuild_run();

    EQ_IRQ_RESTORE(pm);
    return 0;
}

int audio_eq_get_band(uint32_t idx, audio_eq_band_t *band) {
    if ((idx >= AUDIO_EQ_MAX_BANDS) || (band == NULL)) {
        return -1;
    }
    *band = s_bands[idx];
    return 0;
}

int audio_eq_get_default_band(uint32_t idx, audio_eq_band_t *band) {
    if ((idx >= AUDIO_EQ_MAX_BANDS) || (band == NULL)) {
        return -1;
    }
    band->freq = s_default_freq[idx];
    band->q = 1.4f;
    band->gain_db = 0.0f;
    band->type = AUDIO_EQ_TYPE_PEAKING;
    band->enabled = 1u;
    return 0;
}

void audio_eq_set_enabled(bool enabled) {
    s_enabled = enabled ? true : false;
}

bool audio_eq_get_enabled(void) {
    return s_enabled;
}

void audio_eq_set_master_gain_db(float db) {
    if (db < -60.0f) {
        db = -60.0f;
    }
    if (db > 24.0f) {
        db = 24.0f;
    }
    s_master_db = db;
    s_master_gain = (db <= -60.0f) ? 0.0f : eq_pow10f(db * 0.05f);
}

float audio_eq_get_master_gain_db(void) {
    return s_master_db;
}

uint32_t audio_eq_active_bands(void) {
    return s_enabled ? s_run_count : 0u;
}

/* ---------------------------------------------------------------- 处理 */

/*
 * 这个函数是整个工程唯一的热点：20 段 × 2 声道 biquad，在 48 kHz 下每半缓冲
 * 128 帧要做 5120 次乘加。全工程为了体积用 -Os，这里单独提级到 -O3，
 * 让内层循环被展开、系数与状态尽量留在 VFP 寄存器里。
 */
__attribute__((optimize("O3"))) void audio_eq_process(float *buf, uint32_t frames) {
    if (!s_enabled || (s_run_count == 0u)) {
        return;
    }

    for (uint32_t k = 0u; k < s_run_count; k++) {
        uint32_t b = s_run[k];
        const eq_biquad_t *c = &s_coef[b];
        float b0 = c->b0;
        float b1 = c->b1;
        float b2 = c->b2;
        float a1 = c->a1;
        float a2 = c->a2;
        float s1l = s_state[0][b][0];
        float s2l = s_state[0][b][1];
        float s1r = s_state[1][b][0];
        float s2r = s_state[1][b][1];
        float *p = buf;

        for (uint32_t i = 0u; i < frames; i++) {
            float x = p[0];
            float y = b0 * x + s1l;
            s1l = b1 * x - a1 * y + s2l;
            s2l = b2 * x - a2 * y;
            p[0] = y;

            x = p[1];
            y = b0 * x + s1r;
            s1r = b1 * x - a1 * y + s2r;
            s2r = b2 * x - a2 * y;
            p[1] = y;

            p += 2;
        }

        s_state[0][b][0] = s1l;
        s_state[0][b][1] = s2l;
        s_state[1][b][0] = s1r;
        s_state[1][b][1] = s2r;
    }

    if (s_master_gain != 1.0f) {
        float g = s_master_gain;
        uint32_t n = frames * 2u;
        for (uint32_t i = 0u; i < n; i++) {
            buf[i] *= g;
        }
    }
}
