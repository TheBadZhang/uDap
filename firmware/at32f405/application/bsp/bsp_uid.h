#pragma once

#include <stdint.h>

/*
 * 芯片唯一 ID（96 位，出厂固化，不可改）。
 *
 * AT32F405 的 UID 放在系统存储区 0x1FFFF7E8 起的 12 个字节里，三个 32 位字：
 *   0x1FFFF7E8  UNIID1
 *   0x1FFFF7EC  UNIID2
 *   0x1FFFF7F0  UNIID3
 * 器件头文件里没有对应的宏（不像 CH32 有 CHIP_UID_1），所以这里自己取。
 *
 * 用途：作为 USB 序列号（CMSIS-DAP 的 HostStatus/serial 字符串、WinUSB 的
 * 设备实例路径），这样同一台电脑插多块板子时不会互相覆盖驱动配置。
 */

#define CHIP_UID_1 ((uint32_t)(*((volatile uint32_t *)(0x1FFFF7E8u))))
#define CHIP_UID_2 ((uint32_t)(*((volatile uint32_t *)(0x1FFFF7ECu))))
#define CHIP_UID_3 ((uint32_t)(*((volatile uint32_t *)(0x1FFFF7F0u))))

#define BSP_UID_HEX16_STR_LEN 17u

/* 芯片 ID（DEBUG_IDCODE 寄存器，可用来粗略判断型号档位） */
uint32_t bsp_get_chip_id(void);

/* 把 UNIID1/UNIID2 的 8 个字节按小端顺序格式化成 16 个大写十六进制字符 */
void bsp_get_uid_hex16(char out_hex[BSP_UID_HEX16_STR_LEN]);
