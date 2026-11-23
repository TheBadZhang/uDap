/**
 * @file    audio_i2s.h
 * @brief   I2S1 + DMA 音频输出（AT32F405 -> PCM5102）
 *
 * 数据通路：
 *   USB ISO OUT（原始 PCM 小端交织数据）
 *     -> 环形缓冲区（字节流，USB 中断写入）
 *     -> DMA1 半/全传输中断里按当前位深转换
 *     -> SPI1/I2S1 16 位数据寄存器（24/32 位模式每个声道写两次）
 *     -> PA4/PA5/PA7 -> PCM5102
 */

#ifndef __UDAP_AUDIO_I2S_H
#define __UDAP_AUDIO_I2S_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AUDIO_FORMAT_16BIT = 0,
    AUDIO_FORMAT_24BIT = 1,
    AUDIO_FORMAT_32BIT = 2,
    AUDIO_FORMAT_COUNT
} audio_format_t;

/* 配置 GPIO / SPI1 / DMA 时钟，不启动传输 */
void audio_i2s_init(void);

/* 选择格式（16/24/32 位）和名义采样率后启动输出 */
bool audio_i2s_start(audio_format_t fmt, uint32_t sample_rate);

/* 停止输出并清空环形缓冲区 */
void audio_i2s_stop(void);

/* 运行中改采样率（会重启 I2S/DMA，导致一个很短的断音） */
void audio_i2s_set_rate(uint32_t sample_rate);

/* USB 回调里调用：把一包 PCM 数据塞进环形缓冲区（中断安全） */
void audio_i2s_write(const uint8_t *data, uint32_t len);

/* 当前状态 / 统计 */
bool audio_i2s_is_streaming(void);
uint32_t audio_i2s_requested_rate(void);
uint32_t audio_i2s_actual_rate(void);
uint32_t audio_i2s_ring_level(void);
uint32_t audio_i2s_ring_capacity(void);
audio_format_t audio_i2s_format(void);

/* 统计：环形缓冲区下溢次数 / DMA 半缓冲中断次数 / EQ 占 CPU 千分比 */
uint32_t audio_i2s_underruns(void);
uint32_t audio_i2s_half_count(void);
uint32_t audio_i2s_eq_cycles(void);
uint32_t audio_i2s_half_period(void);
uint32_t audio_i2s_eq_load_permille(void);

/* UAC2 显式反馈值（16.16，单位：采样/微帧，未按 bInterval 缩放） */
uint32_t audio_i2s_feedback(void);

/* 软件静音（USB Feature Unit 的 MUTE 控制） */
void audio_i2s_set_mute(bool mute);
bool audio_i2s_get_mute(void);

#endif /* __UDAP_AUDIO_I2S_H */
