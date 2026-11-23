/**
 * @file    audio_eq.h
 * @brief   20 段参数均衡器（float，双声道，级联 biquad）
 *
 * 位置：USB(UAC2) -> 环形缓冲区 -> [这里] -> I2S DMA -> PCM5102A
 * 采样率变化时自动按新 fs 重算全部系数（RBJ cookbook），主机侧只需要下发
 * f0 / Q / gain_dB / 类型。全部用 float 计算，Cortex-M4F 的 FPU 直接跑。
 */

#ifndef __UDAP_AUDIO_EQ_H
#define __UDAP_AUDIO_EQ_H

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_EQ_MAX_BANDS 20u

typedef enum {
    AUDIO_EQ_TYPE_PEAKING = 0,
    AUDIO_EQ_TYPE_LOWSHELF = 1,
    AUDIO_EQ_TYPE_HIGHSHELF = 2,
    AUDIO_EQ_TYPE_LOWPASS = 3,
    AUDIO_EQ_TYPE_HIGHPASS = 4,
    AUDIO_EQ_TYPE_NOTCH = 5,
} audio_eq_type_t;

typedef struct {
    float freq;     /* 中心频率 Hz，10 .. fs*0.45 */
    float q;        /* Q 值，0.1 .. 20 */
    float gain_db;  /* 增益 dB，-24 .. +24（LP/HP/NOTCH 忽略） */
    uint8_t type;   /* audio_eq_type_t */
    uint8_t enabled;
} audio_eq_band_t;

void audio_eq_init(void);

/* 采样率变化时调用：重算全部系数，并重置滤波器状态 */
void audio_eq_set_rate(uint32_t fs);
uint32_t audio_eq_rate(void);

/* 返回 0 表示成功；越界返回 -1 */
int audio_eq_set_band(uint32_t idx, const audio_eq_band_t *band);
int audio_eq_get_band(uint32_t idx, audio_eq_band_t *band);
int audio_eq_get_default_band(uint32_t idx, audio_eq_band_t *band);

void audio_eq_set_enabled(bool enabled);
bool audio_eq_get_enabled(void);
void audio_eq_set_master_gain_db(float db);
float audio_eq_get_master_gain_db(void);

/* 当前采样率下实际参与运算的段数（增益为 0 的 peaking/shelf 会被跳过） */
uint32_t audio_eq_active_bands(void);

/* 原地处理交织立体声（L,R,L,R...），frames 为帧数 */
void audio_eq_process(float *interleaved, uint32_t frames);

#endif /* __UDAP_AUDIO_EQ_H */
