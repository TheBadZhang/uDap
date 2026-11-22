/**
 * @file    swd_fast_gpio.h
 * @brief   SWD 位翻转的汇编内核（RAM 常驻）
 *
 * 这几个内核都放在 .ramcode 段（链接脚本把它塞进 .data，启动时随 .data
 * 一起从 Flash 拷进 RAM），因此运行时不受 Flash 等待周期影响。AT32F405
 * 跑 216 MHz 时 Flash 需要 6 个等待周期，位翻转这种"几条指令一个边沿"
 * 的代码留在 Flash 里会被取指拖慢一大截。
 *
 * 引脚是编译期常量（见 swd_fast_gpio.S 顶部），swd_gpio_fast.c 里用
 * _Static_assert 保证它们和 board_pin_map.h 一致：
 *   PA5 = SWCLK / TCK、PA7 = SWDIO 输出、PA6 = SWDIO 输入。
 */
#ifndef SWD_FAST_GPIO_H
#define SWD_FAST_GPIO_H

#include <stdint.h>

/* 必须与 swd_fast_gpio.S 顶部的 .equ 保持一致 */
#define SWD_FAST_SCK_BIT        5U
#define SWD_FAST_DIO_BIT        7U
#define SWD_FAST_DIN_BIT        6U

/** 发送 nbits 位（LSB 先发），每个 bit 都在下降沿更新 SWDIO */
void swd_fast_tx_bits(uint32_t value, uint32_t nbits);

/** 接收 nbits 位（LSB 先收），返回右对齐的位值；nbits 必须 >= 1 */
uint32_t swd_fast_rx_bits(uint32_t nbits);

/** 空打 nbits 个 SWCLK（不改 SWDIO，用于转向与 idle） */
void swd_fast_tx_bits_d(uint32_t value, uint32_t nbits, uint32_t dly);
uint32_t swd_fast_rx_bits_d(uint32_t nbits, uint32_t dly);
void swd_fast_clock_n_d(uint32_t nbits, uint32_t dly);

void swd_fast_clock_n(uint32_t nbits);

/**
 * 热路径：寄存器由调用方准备好，内核里不再加载任何常量、也没有 push/pop。
 * 用来把 ACK 到数据相位之间的空档压到最短（每个字省下约 10~15 个周期）。
 *
 * @param value      要发的数据（LSB 先发）
 * @param scr        &GPIOA->scr
 * @param word_clr   SWDIO 清零 + SWCLK 下降沿 的整字
 * @param word_set   SWDIO 置位 + SWCLK 下降沿 的整字
 */
void swd_fast_tx32_hot(uint32_t value, uint32_t scr, uint32_t word_clr, uint32_t word_set);
void swd_fast_tx8_hot(uint32_t value, uint32_t scr, uint32_t word_clr, uint32_t word_set);
void swd_fast_tx32_mid(uint32_t value, uint32_t scr, uint32_t word_clr, uint32_t word_set);
void swd_fast_tx8_mid(uint32_t value, uint32_t scr, uint32_t word_clr, uint32_t word_set);

/* 27 MHz 档整字内核（hw = C 侧 swd_fast_hw[] 参数块，偏移见 swd_fast_gpio.S）
 *
 * 每个函数一次做完一整个传输相位，全部展开、无循环、无中间 C 胶水：
 *   swd_fast_wr: 请求 8 拍 → 松手 → 转向 → 3 拍 ACK → (ACK=OK) → 切输出 →
 *                转向 → 32 拍数据 → 1 拍奇偶；返回 ACK[2:0]
 *   swd_fast_rd: 请求 8 拍 → 松手 → 转向 → 3 拍 ACK → (ACK=OK) → 转向 →
 *                32 拍采样 → 1 拍奇偶；返回 ACK[2:0] | 奇偶 << 4，数据写 *out
 */
uint32_t swd_fast_wr(const uint32_t *hw, uint32_t reqbyte, uint32_t value, uint32_t parity);
uint32_t swd_fast_rd(const uint32_t *hw, uint32_t reqbyte, uint32_t *out);
uint64_t swd_fast_rx_word32(const uint32_t *hw);
uint32_t swd_fast_req(const uint32_t *hw, uint32_t reqbyte);

uint64_t swd_fast_rx_tail(const uint32_t *hw);
#endif /* SWD_FAST_GPIO_H */