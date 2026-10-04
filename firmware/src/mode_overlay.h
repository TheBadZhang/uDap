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
 * 既然两者不同时运行，它们**各自私有的**代码与数据就可以共用同一段地址
 * （overlay）：进哪个模式就初始化哪一份。共用部分（USB 端口层、CherryUSB 核心、
 * CDC 类、drv_uart、SDK）不属于任何单一模式，仍放在常规区域。
 *
 * ============================ 段划分 ============================
 *   .highcode          共用：USB 端口层热路径
 *   .highcode.dap      CMSIS-DAP 私有代码（sw_dp.c / dap_main.c 的热路径）
 *   .overlay.dap       CMSIS-DAP 私有数据（DAP 的 USB 收发缓冲）
 *   .highcode.wchlink  WCH-Link 私有代码（rvswd_phy_gpio.c 的位翻转）
 *   .overlay.wchlink   WCH-Link 私有数据（会话/传输状态与大缓冲）
 *
 * 链接脚本 sdk/Ld/Link_highcode_nv_256B_dual.ld 用 ld 的 OVERLAY 命令把
 * `.highcode.dap`+`.overlay.dap` 与 `.highcode.wchlink`+`.overlay.wchlink`
 * 放到**同一段 VMA** 上；main.c 在进入各自模式时把代码从 Flash 拷进这段 RAM、
 * 并把该模式的 BSS 清零（见 src/main.c 的 dap_overlay_init / wchlink_overlay_init）。
 */

#define DAP_HIGHCODE_SECTION     __attribute__((section(".highcode.dap")))
#define DAP_PRIVATE_SECTION      __attribute__((section(".overlay.dap")))
#define WCHLINK_HIGHCODE_SECTION __attribute__((section(".highcode.wchlink")))
#define WCHLINK_PRIVATE_SECTION  __attribute__((section(".overlay.wchlink")))
