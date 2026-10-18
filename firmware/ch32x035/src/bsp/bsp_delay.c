#include "bsp/bsp_delay.h"

#include <stdbool.h>

#include <ch32x035.h>

#define SYSTICK_CTLR (*(volatile uint32_t *)0xE000F000UL)
#define SYSTICK_SR   (*(volatile uint32_t *)0xE000F004UL)
#define SYSTICK_CNTL (*(volatile uint32_t *)0xE000F008UL)
#define SYSTICK_CNTH (*(volatile uint32_t *)0xE000F00CUL)
#define SYSTICK_CMPL (*(volatile uint32_t *)0xE000F010UL)
#define SYSTICK_CMPH (*(volatile uint32_t *)0xE000F014UL)

#define SYSTICK_CTLR_STCLK_HCLK (1UL << 2U)
#define SYSTICK_CTLR_ENABLE     (1UL << 0U)

static bool delay_initialized;
static uint32_t systick_ticks_per_us;
static uint32_t systick_ticks_per_ms;

static void systick_read_counter(uint32_t *high, uint32_t *low) {
    uint32_t high_before;
    uint32_t high_after;

    do {
        high_before = SYSTICK_CNTH;
        *low = SYSTICK_CNTL;
        high_after = SYSTICK_CNTH;
    } while (high_before != high_after);

    *high = high_after;
}

// 使用编译器除法，避免逐次执行 32 轮移位拖慢 DAP 主循环
// 先换算完整计数值，再取低 32 位，保持时间接口的自然回绕
static uint32_t systick_counter_to_units(uint32_t high, uint32_t low, uint32_t divisor) {
    return (uint32_t)((((uint64_t)high << 32) | low) / divisor);
}

static void systick_wait_ticks(uint32_t ticks) {
    uint32_t start = SYSTICK_CNTL;

    while ((uint32_t)(SYSTICK_CNTL - start) < ticks) {
    }
}

static void delay_units(uint32_t units, uint32_t ticks_per_unit) {
    if (!delay_initialized || units == 0u || ticks_per_unit == 0u) {
        return;
    }

    const uint32_t max_units = UINT32_MAX / ticks_per_unit;
    while (units != 0u) {
        const uint32_t chunk = units > max_units ? max_units : units;
        systick_wait_ticks(chunk * ticks_per_unit);
        units -= chunk;
    }
}

void bsp_delay_init(void) {
    if (delay_initialized) {
        return;
    }

    SystemCoreClockUpdate();
    SYSTICK_CTLR = 0u;
    SYSTICK_SR = 0u;
    SYSTICK_CNTL = 0u;
    SYSTICK_CNTH = 0u;
    SYSTICK_CMPL = UINT32_MAX;
    SYSTICK_CMPH = UINT32_MAX;

    systick_ticks_per_us = SystemCoreClock / 1000000u;
    systick_ticks_per_ms = SystemCoreClock / 1000u;
    if (systick_ticks_per_us == 0u || systick_ticks_per_ms == 0u) {
        return;
    }
    SYSTICK_CTLR = SYSTICK_CTLR_ENABLE | SYSTICK_CTLR_STCLK_HCLK;
    delay_initialized = true;
}

void bsp_delay_us(uint32_t us) {
    delay_units(us, systick_ticks_per_us);
}

void bsp_delay_ms(uint32_t ms) {
    delay_units(ms, systick_ticks_per_ms);
}

uint32_t bsp_time_us(void) {
    uint32_t high;
    uint32_t low;

    if (!delay_initialized) {
        return 0u;
    }
    systick_read_counter(&high, &low);
    return systick_counter_to_units(high, low, systick_ticks_per_us);
}

uint32_t bsp_time_ms(void) {
    uint32_t high;
    uint32_t low;

    if (!delay_initialized) {
        return 0u;
    }
    systick_read_counter(&high, &low);
    return systick_counter_to_units(high, low, systick_ticks_per_ms);
}

/*
 * WCH 外设库兼容层。
 *
 * 本工程不编译 sdk/Debug/debug.c（它无条件定义 _write / _sbrk，与
 * src/bsp/bsp_print.c 的 retarget 冲突），而 debug.c 同时是 SDK 公共延时接口
 * Delay_Init() / Delay_Us() / Delay_Ms() 的提供者（声明见 sdk/Debug/debug.h）。
 * 为保证 SDK 里任何调用它们的地方都能链接，这里提供等价实现，
 * 转发到本工程基于 SysTick 的延时。
 *
 * 目前唯一曾依赖它们的 sdk/Peripheral/src/ch32x035_pwr.c 已改为内联延时
 * （与参考工程 ch32x035-usb-dap-main 的版本一致，见 PWR_VDD_SupplyVoltage），
 * 所以这几个函数当前可能不被引用；保留是为了在 SDK 升级后重新引入
 * Delay_* 调用时不会链接失败。未被引用时会被 -Wl,--gc-sections 回收，无额外开销。
 */
void Delay_Init(void) { bsp_delay_init(); }

void Delay_Us(uint32_t n) { bsp_delay_us(n); }

void Delay_Ms(uint32_t n) { bsp_delay_ms(n); }
