#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * bootloader 的 Flash 操作（只碰应用区）。
 *
 * 所有接口都以 **256 B 页**为单位：CH32X035 的 ROM 擦写接口（FLASH_ROM_ERASE /
 * FLASH_ROM_WRITE）要求起始地址 256 B 对齐、长度是 256 B 的整数倍，而 UF2 块
 * 本身正好是一页（PayloadSize == 256）。按页做，逻辑最简单，也最不容易错。
 *
 * 写入后**一定回读校验**：升级路径上「写进去了」不能靠猜 —— 校验失败要报错，
 * 而不是跳进半截固件。
 */

/* 复位内部的运行状态 */
void boot_flash_init(void);

/* 擦掉应用区首页，使 boot_app_is_valid() 立刻判定为「没有固件」。
 * 每次开始一轮 UF2 拷贝时先调用它：中途拔线/断电时设备会停在 bootloader，
 * 而不是跳进一个写了一半的固件。 */
bool boot_flash_invalidate_app(void);

/* 擦一页 + 编程 + 回读校验。地址必须 256 B 对齐且落在应用区。
 *
 * **调用方必须自己关中断**（`boot_uf2.c` 在「认领待写页 → 写页」这一段里保证）：
 * 擦写期间若被中断打断，中断里新到的块会改掉源缓冲、甚至重入这条路径，
 * 结果是随机的数据错误 + 回读校验失败。
 *
 * 成功返回 true。 */
bool boot_flash_write_page(uint32_t address, const uint8_t *data);

/* 应用区是否像是有固件（入口页有效）。 */
bool boot_app_is_valid(void);

/* 该页当前内容是否与 data 一致（读 Flash 比对）。 */
bool boot_flash_page_matches(uint32_t address, const uint8_t *data);

/* 最近一次成功写入的 SysTick 计数，用于 LED 的「正在写」指示
 * （比较方式见 boot_time.h）。 */
uint32_t boot_flash_last_write_tick(void);
