#include "bsp/bsp_time.h"

#include "at32f402_405.h"

static volatile uint32_t s_ms;

void bsp_time_init(void) {
    /* 1 ms 周期：216 MHz / 1000 = 216000，减 1 后写入重装值 */
    SysTick->LOAD = (uint32_t)(system_core_clock / 1000u) - 1u;
    SysTick->VAL = 0u;
    /* CLKSOURCE=1（内核时钟）、TICKINT=1、ENABLE=1 */
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

uint32_t bsp_time_ms(void) {
    return s_ms;
}

void SysTick_Handler(void) {
    s_ms++;
}

/* cmsis_gcc.h 里的 __get_PRIMASK/__disable_irq 已经够用，这里不做额外封装 */
