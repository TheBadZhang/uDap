/**
 * @file    bsp_vector.c
 * @brief   把中断向量表指到应用自己的起始地址 0x08004000
 *
 * 为什么不用 VECT_TAB_OFFSET 编译宏：
 *   sdk/cmsis/device/source/system_at32f402_405.c 第 39 行是无条件的
 *   `#define VECT_TAB_OFFSET 0x0`，命令行 -DVECT_TAB_OFFSET=0x4000 会被文件内
 *   定义覆盖（还多一条宏重定义告警），改 sdk 里的厂商源码又违反工程约定。
 *
 * 这里用 __attribute__((constructor))：
 *   startup_at32f405.s 的 Reset_Handler 顺序是
 *     bl SystemInit -> bl __libc_init_array（执行构造器）-> bl main
 *   构造器正好在 SystemInit 之后、main 之前运行，把 SystemInit 写进去的
 *   0x08000000 修正为 0x08004000。此时 SysTick / NVIC 都还没使能，
 *   改 VTOR 不会被任何中断打断。bootloader 跳过来时也会先把 VTOR 设成
 *   0x08004000，这里相当于再确认一次。
 */

#include "at32f402_405.h"

/* 应用区起始地址（与 startup/linker/at32f405_flash_128k_70k.ld 的 FLASH ORIGIN 一致） */
#define BSP_APP_VECTOR_BASE 0x08004000u

static void bsp_vector_relocate(void) __attribute__((constructor(101)));

static void bsp_vector_relocate(void) {
    SCB->VTOR = BSP_APP_VECTOR_BASE;
}
