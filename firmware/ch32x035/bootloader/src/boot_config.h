#pragma once

#include <stdint.h>

/*
 * uDap UF2 bootloader 的板级/布局常量。
 *
 * ============================ Flash 布局 ============================
 *
 *   0x00000000 .. 0x00003FFF   bootloader（16 KiB，本工程）
 *   0x00004000 .. 0x0000F6FF   应用固件（firmware.uf2 的落点）
 *   0x0000F700 .. 0x0000F7FF   预留（NV 配置页，应用链接脚本同样把它排除在外）
 *
 * 16 KiB 是预留量：USB 核心 + MSC 类 + 虚拟 FAT16 + 启动/落盘逻辑现在实际约
 * 14.5 KiB（GCC15 release 构建 14 828 B，GCC12 则是 15 640 B），余量约 1.5 KiB
 * （用 GCC12 只剩 0.7 KiB）—— 再加功能要盯着 xmake 的 after_build 打出的占用。
 * 若以后还要缩小/放大，三处必须同时改：
 *   本文件的 BOOT_APP_START / BOOT_LOADER_SIZE
 *   bootloader/ld/Link_bootloader.ld  的 FLASH LENGTH
 *   应用链接脚本（application/ld/Link_highcode_nv_256B_uf2.ld）的 FLASH ORIGIN
 * 并且要重新烧 bootloader —— 新旧 UF2 的落点不同，不能混用。
 *
 * 两份链接脚本必须与这里保持一致：
 *   应用   application/ld/Link_highcode_nv_256B_uf2.ld   FLASH ORIGIN = 0x00004000
 *   boot   bootloader/ld/Link_bootloader.ld       FLASH ORIGIN = 0x00000000
 */

#define BOOT_APP_START ((uint32_t)0x00004000u)
#define BOOT_APP_END   ((uint32_t)0x0000F700u)
#define BOOT_APP_SIZE  ((uint32_t)(BOOT_APP_END - BOOT_APP_START))

#define BOOT_FLASH_PAGE_SIZE ((uint32_t)256u)
#define BOOT_FLASH_END       ((uint32_t)0x0000F800u)

/* bootloader 自身的 Flash 上限（与 ld 脚本的 FLASH 区域一致） */
#define BOOT_LOADER_SIZE ((uint32_t)0x00004000u)

/*
 * UF2 family ID：0x75446170 = ASCII "uDap"。
 *
 * 这个值同时出现在三处，必须一致：
 *   1. 本文件（bootloader 校验 family，不匹配的 UF2 直接忽略）
 *   2. firmware/ch32x035/build.ps1 调 uf2conv.py 时的 -f 参数
 *   3. 文档（doc/ 下 build 一节）
 * 上游 uf2families.json 里没有 CH32X035，所以用自定义值；uf2conv.py 的 -f
 * 接受裸十六进制，不要求出现在 uf2families.json 里。
 */
#define BOOT_UF2_FAMILY_ID ((uint32_t)0x75446170u)

/* 上电时按住按键多久进入 UF2（一直按到超时才算；提前松开 = 普通上电） */
#define BOOT_BUTTON_HOLD_MS ((uint32_t)1200u)

/* USB 身份：MSC 是标准类设备，不需要驱动，VID/PID 只用于区分设备 */
#define BOOT_USB_VID ((uint16_t)0x1209u) /* pid.codes（开源硬件测试 VID） */
#define BOOT_USB_PID ((uint16_t)0x7544u)

#define BOOT_VERSION_MAJOR 1u
#define BOOT_VERSION_MINOR 3u
#define BOOT_VERSION_PATCH 0u
