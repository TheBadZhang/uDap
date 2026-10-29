/*
 * bootloader 的最小系统初始化。
 *
 * 不直接复用 src/system_ch32x035.c：那份实现里的 SetSysClock() 会先调用
 * GPIO_IPD_Unused()（把全部 GPIO 配成下拉，省电用），在 bootloader 里既没用
 * 又占约 400 B Flash。这里只保留「48 MHz HSI + 2 个等待周期」这一条路径，
 * 与应用固件最终达到的主频/Flash 时序完全一致（否则 USB 时序会变）。
 *
 * 应用固件里的对应实现见 src/system_ch32x035.c 的 SetSysClockTo48_HSI()。
 */

#include <ch32x035.h>

uint32_t SystemCoreClock = 48000000u;

void SystemInit(void) {
    RCC->CTLR |= (uint32_t)0x00000001;
    RCC->CFGR0 |= (uint32_t)0x00000050;
    RCC->CFGR0 &= (uint32_t)0xF8FFFF5F;

    // Flash 2 个等待周期（48 MHz）
    FLASH->ACTLR &= (uint32_t)(~FLASH_ACTLR_LATENCY);
    FLASH->ACTLR |= (uint32_t)FLASH_ACTLR_LATENCY_2;

    // HCLK = SYSCLK = APB1
    RCC->CFGR0 &= (uint32_t)0xFFFFFF0F;
    RCC->CFGR0 |= (uint32_t)RCC_HPRE_DIV1;
}

// bootloader 不需要运行时改主频：复位后固定 48 MHz。
// （bsp/bsp_delay.c 的 bsp_delay_init() 会调用它。）
void SystemCoreClockUpdate(void) {
    SystemCoreClock = 48000000u;
}
