#pragma once

#include <stdint.h>

/*
 * bootloader 的虚拟 U 盘（只读 FAT16）。
 *
 * 盘里只有一个文件：index.htm（FAT 目录项里用 NT 大小写标志显示成小写），
 * 内容是一条跳转到项目主页的 HTML。没有 INFO_UF2.TXT、JOIN.HTM，也没有
 * family ID 标记文件 —— 拖拽升级完全不依赖文件名，主机写下来的每个扇区
 * 都会被 boot_uf2_write_sector() 当成 UF2 候选块解析。
 *
 * 盘参数刻意压到最小：2 MiB（4096 簇 × 512 B，FAT16 的下限是 4085 簇），
 * 够放最大的 firmware.uf2（约 42 KiB）几十次。
 *
 * 读写接口与 CherryUSB MSC 类对接：
 *   boot_disk_read_sector()  主机枚举、列目录、读文件时调用
 *   boot_disk_sector_count() usbd_msc_get_cap() 上报的扇区总数
 */

#define BOOT_DISK_SECTOR_SIZE 512u

void boot_disk_init(void);

uint16_t boot_disk_sector_size(void);
uint32_t boot_disk_sector_count(void);

/* sector_count 是扇区个数；每个扇区 512 B。返回 0 表示已处理。 */
int boot_disk_read_sector(uint32_t start_sector, uint8_t *buffer, uint32_t sector_count);
