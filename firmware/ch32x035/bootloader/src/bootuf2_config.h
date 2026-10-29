/*
 * bootuf2（来自 CherryDAP 子模块的 MSC/UF2 解析实现）的编译期配置。
 *
 * bootuf2.c 用 `#include <bootuf2_config.h>` 取这些宏，因此本文件必须在
 * include 路径上排在 CherryDAP 自带的那份之前（见 bootloader/xmake.lua 的
 * add_includedirs 顺序）。
 */
#pragma once

#include "boot_config.h"

#define CONFIG_PRODUCT           "uDap"
#define CONFIG_BOARD             "uDap CH32X035F8U6"
#define CONFIG_BOOTUF2_INDEX_URL "https://github.com/TheBadZhang/uDap"
#define CONFIG_BOOTUF2_JOIN_URL  "https://github.com/TheBadZhang/uDap"

/* INFO_UF2.TXT 里的原因占位符，运行时由 bootuf2_SetReason() 替换 */
#define DEFAULT_REASON "DEFAULT_REASON_DEFAULT_REASON_DEFAULT_REASON\r\n"

/* 虚拟 FAT16 参数（与 CherryDAP 的参考配置一致：512 B 扇区、2 扇区/簇） */
#define CONFIG_BOOTUF2_CACHE_SIZE         4096
#define CONFIG_BOOTUF2_SECTOR_SIZE        512
#define CONFIG_BOOTUF2_SECTOR_PER_CLUSTER 2
#define CONFIG_BOOTUF2_SECTOR_RESERVED    1
#define CONFIG_BOOTUF2_NUM_OF_FAT         2
#define CONFIG_BOOTUF2_ROOT_ENTRIES       64

/* 只接受本 family、且总块数不超过应用区大小 */
#define CONFIG_BOOTUF2_FAMILYID      BOOT_UF2_FAMILY_ID
#define CONFIG_BOOTUF2_FLASHMAX      BOOT_APP_SIZE
#define CONFIG_BOOTUF2_PAGE_COUNTMAX 1024

#define CONFIG_BOOTUF2_APP_START BOOT_APP_START
