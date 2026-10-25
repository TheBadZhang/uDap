#pragma once

/*
 * 双模式固件的 RAM overlay 段名。
 *
 * ============================ 背景 ============================
 * CMSIS-DAP 与 WCH-Link 都要占用 PA2=SWCLK/DCK、PA3=SWDIO/DIO，而且各自的
 * USB 身份完全不同（0d28:0204 WinUSB vs 1a86:8010 厂商接口）。所以它们
 * **不可能同时运行**，任意时刻只有一个在跑。
 *
 * 实测全编译（release, -Os）：
 *      Flash 约 51 KiB / 可用 63232 B   —— 够
 *      但 RAM 需要约 25.3 KiB / 可用 20 KiB —— 超约 4.8 KiB
 *
 * 既然两者不同时运行，它们**各自私有的数据**就可以共用同一段地址（overlay）：
 * 进哪个模式就把哪一份清零。共用部分（USB 端口层、CherryUSB 核心、CDC 类、
 * drv_uart、SDK）不属于任何单一模式，仍放在常规区域。
 *
 * ============================ 段划分 ============================
 *   .highcode          共用：USB 端口层热路径（RAM，启动时拷入）
 *   .highcode.dap      CMSIS-DAP 私有代码（sw_dp.c / dap_main.c 的热路径）
 *                      —— 并进常驻 RAM 的 .highcode，启动时拷一次
 *   .overlay.dap       CMSIS-DAP 私有数据（DAP 的 USB 收发缓冲）
 *   .highcode.wchlink  WCH-Link 私有代码（rvswd_phy_gpio.c 的位翻转）
 *                      —— 链接进 .text，**直接在 Flash 上执行**（XIP）
 *   .overlay.wchlink   WCH-Link 私有数据（会话/传输状态与大缓冲）
 *
 * 链接脚本 sdk/Ld/Link_highcode_nv_256B_dual.ld：
 *   - OVERLAY 只把 `.overlay.dap` 与 `.overlay.wchlink` 放到**同一段 VMA**
 *     （数据不能进 Flash，重叠才省 RAM；窗口取较大的 WCH-Link 那份）；
 *   - 代码不进 overlay：`.highcode.dap` 并在 `.highcode` 里由启动代码拷一次，
 *     `.highcode.wchlink` 直接编在 Flash 上。
 * 所以 main.c 在进入各自模式时**只需清零该模式的数据段**，没有代码拷贝
 * （见 src/main.c 的 dap_overlay_init / wchlink_overlay_init）。
 *
 * 代价：WCH-Link 的位翻转代码改为在 Flash 上执行，48 MHz 下每次取指多 2 个等待
 * 周期（FLASH_ACTLR_LATENCY_2），RVSWD 时钟会比放在 RAM 时低。
 */

/* 用途对照（详细说明见本文件开头）：
 *
 *   DAP_HIGHCODE_SECTION      CMSIS-DAP 热路径代码 -> .highcode.dap，链接进常驻
 *                             RAM 段，启动时拷入一次（运行期不拷贝）
 *   DAP_PRIVATE_SECTION       CMSIS-DAP 私有数据   -> .overlay.dap，RAM，
 *                             进 DAP 模式时清零
 *   WCHLINK_HIGHCODE_SECTION  WCH-Link 热路径代码  -> .highcode.wchlink，链接进
 *                             .text，直接在 Flash 上执行（XIP）
 *   WCHLINK_PRIVATE_SECTION   WCH-Link 私有数据    -> .overlay.wchlink，RAM，
 *                             进 WCH-Link 模式时清零
 *
 * 两个 PRIVATE 段共用同一段 VMA（ld OVERLAY），因为它们不同时存在。
 */
#ifndef WCHLINK_ENABLE
#define WCHLINK_ENABLE 0
#endif

/* .highcode.dap 在两种链接脚本里都被并进常驻 RAM 的 .highcode
 * （单模式脚本用 *(.highcode.*) 收集），所以它的定义与模式无关。 */
#define DAP_HIGHCODE_SECTION     __attribute__((section(".highcode.dap")))

#if WCHLINK_ENABLE
/* 双模式：私有**数据**进 overlay 段（两种模式共用同一段 RAM 地址）。
 *
 * 段名必须带 `.bss.` 前缀：这样 GCC 生成的是 **SHT_NOBITS** 段（同 .bss，
 * 没有初值、不占 Flash）。否则零初始化的变量会变成 SHT_PROGBITS，虽然内容
 * 全是 0，链接器仍会为它在 Flash 里排一段 LMA —— 双模式固件的 .bin 因此
 * 白涨 8.2 KiB 并超出 62 KiB。 */
#define DAP_PRIVATE_SECTION      __attribute__((section(".bss.overlay.dap")))
#define WCHLINK_HIGHCODE_SECTION __attribute__((section(".highcode.wchlink")))
#define WCHLINK_PRIVATE_SECTION  __attribute__((section(".bss.overlay.wchlink")))
#else
/* 单模式（只编 CMSIS-DAP）：链接脚本里没有 overlay 段，
 * 私有数据就是普通 .bss（属性留空）。 */
#define DAP_PRIVATE_SECTION
#define WCHLINK_HIGHCODE_SECTION
#define WCHLINK_PRIVATE_SECTION
#endif
