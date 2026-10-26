#pragma once

#include <stdint.h>

/*
 * SWD 的校验位（parity）：32 位数据按位异或。
 *
 * 单独放一个头文件是因为两处都要用它：
 *   src/dap/sw_dp.c         位时序路径（奇偶位那一拍之前）
 *   src/dap/swd_spi_x035.c   SPI 路径（塞进第 1 帧移位的空档里算）
 *
 * 算法与 ARM ADI 一致：parity = DATA[0] ^ ... ^ DATA[31]。位时序路径按这个值
 * 发送时，目标（STM32F411 等）接受；SPI 路径读回的奇偶位也用它校验。
 */
static inline __attribute__((always_inline)) uint32_t swd_parity32(uint32_t value) {
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return value & 1U;
}
