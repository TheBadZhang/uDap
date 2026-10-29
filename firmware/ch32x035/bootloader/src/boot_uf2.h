#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * UF2 接收状态机（bootloader 侧）。
 *
 * MSC 的扇区写回调把主机写下来的每个 512 B 扇区交给 boot_uf2_write_sector()：
 * 目录/FAT 之类的扇区被忽略，真正是 UF2 块的扇区按「块号去重 + 按页落盘」处理。
 *
 * ============================ 为什么要自己实现 ============================
 * CherryDAP 子模块里的 bootuf2.c 自带一份写路径（4 KiB 缓存 + 位图 + 完成判定），
 * 但它的状态是静态的、**一旦开始就再也回不到初始态**（`bootuf2_init()` 不清
 * 位图），而它的「是否写完」判据把 FLASHMAX（字节数）当块号上限用，遇到畸形块
 * 会越界写位图。对「拖文件失败后想直接再拖一次」来说这两点都很致命，所以写路径
 * 由本文件负责；上游只保留虚拟 FAT16 的**读**路径（主机枚举/列目录用）。
 *
 * ============================ 抗中断 / 抗重试 ============================
 *   * 同一轮拷贝里重复到达的块直接忽略（主机重发 WRITE 命令很常见）；
 *   * 同一页被两个内容不同的块覆盖 -> 报错（畸形的 UF2）；
 *   * **入口页（0x5000）最后写**：整份文件都落盘后才编程第一页。中途断开时
 *     入口页仍是擦除态，boot_app_is_valid() 判为无固件，设备停在 bootloader，
 *     不会跳进一个写了一半的固件；
 *   * 每页写完都回读校验，失败立刻报错（见 boot_flash.c）；
 *   * 出错后由 boot_uf2_service() 在主机安静下来后复位会话，拔线不是必须的。
 */

typedef enum {
    BOOT_UF2_OK = 0,
    BOOT_UF2_ERR_FLASH = 2,    /* 擦写或回读校验失败 */
    BOOT_UF2_ERR_CONFLICT = 3, /* 同一页被两个不同的块覆盖（畸形 UF2） */
    BOOT_UF2_ERR_COMMIT = 4,   /* 最后提交入口页失败 */
} boot_uf2_error_t;

/* 复位会话（清位图、清错误、丢弃挂起的入口页） */
void boot_uf2_init(void);

/* MSC 扇区写回调（boot_usb.c 调用）。sector_count 是**扇区个数**（不是字节数）。
 * 返回 0 表示已处理（含忽略）。 */
int boot_uf2_write_sector(uint32_t start_sector, const uint8_t *buffer, uint32_t sector_count);

/* 一轮拷贝是否已经完整落盘 */
bool boot_uf2_is_done(void);

/* 最近的错误码（BOOT_UF2_OK = 无错） */
uint8_t boot_uf2_error(void);

/* 错误码的可读文本（日志用） */
const char *boot_uf2_error_text(uint8_t error);

uint32_t boot_uf2_written_blocks(void);
uint32_t boot_uf2_total_blocks(void);

/* 主循环定期调用：出错后主机安静一段时间就复位会话，允许直接重拖一次。 */
void boot_uf2_service(void);
