/*
 * 主机测试用：让 boot_time.h 用这个变量代替真实的 SysTick 计数寄存器。
 * 通过编译选项 -DBOOT_SYSTICK_CNTL=(g_host_ticks) 生效（见 run.ps1）。
 */
#pragma once

#include <stdint.h>

extern volatile uint32_t g_host_ticks;
