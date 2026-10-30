#pragma once

#include <stdint.h>

/*
 * bootloader 的时间基准：直接用 SysTick 的原始计数，不做任何除法。
 *
 * 只用来量很短的间隔（LED 闪烁、等待主机、写完后的常亮），
 *   * 比较用无符号相减，32 位计数回绕（48 MHz 下约 89 s）也能正确处理；
 *   * 不引 64 位除法（__udivdi3 约 900 B，对 16 KiB 的 bootloader 太贵）。
 */

extern uint32_t SystemCoreClock;

/* 允许主机测试替换掉真实的 SysTick 寄存器（见 bootloader/test/） */
#ifndef BOOT_SYSTICK_CNTL
#define BOOT_SYSTICK_CNTL (*(volatile uint32_t *)0xE000F008UL)
#endif

static inline uint32_t boot_ticks_per_ms(void) {
    return SystemCoreClock / 1000u;
}

static inline uint32_t boot_ticks_from_ms(uint32_t ms) {
    return ms * boot_ticks_per_ms();
}

static inline uint32_t boot_ticks_since(uint32_t start) {
    return BOOT_SYSTICK_CNTL - start;
}
