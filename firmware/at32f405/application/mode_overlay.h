#pragma once

/*
 * 段属性与编译期开关（AT32F405 版）。
 *
 * 上游 ch32x035 工程用这个头文件做「CMSIS-DAP / WCH-Link 双模式 overlay」：
 * 两种模式的私有数据共用同一段 RAM 地址，进哪个模式就清哪一份。AT32F405
 * 版暂时只有 CMSIS-DAP 一种模式（WCH-Link 是 RISC-V 调试器，与 Cortex-M
 * 目标无关），所以这里只保留**段属性**这一层，overlay 的链接脚本技巧不需要。
 *
 * 保留这个头文件而不是直接删掉，是因为：
 *   - dap_main.c / sw_dp.c / udap_vendor.c 里的 DAP_HIGHCODE_SECTION、
 *     DAP_PRIVATE_SECTION 宏来自这里，删了要改三处；
 *   - 以后想加「AT32 自己的高速 SWD 位翻转放 RAM 跑」时，只需要改这里
 *     和链接脚本，DAP 层代码不用动。
 *
 * ---------------------------------------------------------------------------
 * AT32F405 的 RAM 执行问题（为什么这里默认什么都不做）
 * ---------------------------------------------------------------------------
 * AT32F405 跑 216 MHz 时 Flash 需要 6 个等待周期（FLASH_WAIT_CYCLE_6），
 * 顺序取指靠预取缓冲基本能跑满，但 SWD 位翻转是「几百字节、无分支、紧耦合」
 * 的代码，放 Flash 上执行时每次取指的等待都会直接变成时钟抖动。
 *
 * 上游把这段代码放进常驻 RAM 的 .highcode（启动时从 Flash 拷入）。本工程
 * 第一版先不做（RAM 70 KiB 要留给 8 个 512 B 的 USB 收发缓冲 + CDC 环形
 * 缓冲），所以 DAP_HIGHCODE_SECTION 展开为空 —— 代码留在 Flash 里执行。
 * 需要时把下面的宏改成 __attribute__((section(".highcode")))，并在链接脚本
 * 里加 .highcode 段的「VMA=RAM / LMA=Flash」定义，启动代码会自动拷。
 */

#ifndef WCHLINK_ENABLE
#define WCHLINK_ENABLE 0
#endif

#ifndef UDAP_UF2_BOOTLOADER
#define UDAP_UF2_BOOTLOADER 0
#endif

#ifndef UART_ENABLE
#define UART_ENABLE 1
#endif

/* DSP/HOTPATH 代码段：当前为空（留在 Flash，见文件头的说明） */
#define DAP_HIGHCODE_SECTION

/* 模式私有数据段：单模式下就是普通 .bss */
#define DAP_PRIVATE_SECTION

/* 上游为 WCH-Link 预留的两个属性，保留为空定义，便于代码里条件编译共用 */
#define WCHLINK_HIGHCODE_SECTION
#define WCHLINK_PRIVATE_SECTION
