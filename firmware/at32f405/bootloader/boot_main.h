/**
 * @file    boot_main.h
 * @brief   bootloader 主流程里对其它模块暴露的动作接口
 */

#ifndef __BOOT_MAIN_H
#define __BOOT_MAIN_H

#include <stdint.h>

/* 延时（依赖 bsp_delay_init()，在 main 开头已初始化） */
void boot_delay_ms(uint32_t ms);

/* 跳转到 0x08004000 的应用（不返回）：清 magic、关中断、关 SysTick、
 * 设 SCB->VTOR 与 MSP，最后跳 Reset 向量。 */
void boot_jump_to_app(void) __attribute__((noreturn));

/* 请求「延时后跳应用」：WinUSB FINISH 校验通过后调用，让应答先发出去 */
void boot_request_jump_to_app(void);

/* 请求「延时后复位」：WinUSB REBOOT / DFU 完成时调用 */
void boot_request_reboot(void);

/* 主循环里周期调用：执行上面两个延时动作 */
void boot_service(void);

#endif /* __BOOT_MAIN_H */
