/**
 * @file    boot_flash.h
 * @brief   bootloader 的 Flash 擦写、CRC32 与应用有效性检查
 *
 * 所有地址都必须是绝对地址；函数内部统一做「只允许落在应用区
 * 0x08004000..0x08020000」的检查，bootloader 自己的 16 KiB 绝不可被擦写。
 */

#ifndef __BOOT_FLASH_H
#define __BOOT_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/* Flash 操作结果码：与 WinUSB 协议的 status 分离，由 boot_protocol.c 映射 */
typedef enum {
    BOOT_FLASH_OK = 0,          /* 成功 */
    BOOT_FLASH_ERR_RANGE = 1,   /* 地址/长度越界 */
    BOOT_FLASH_ERR_NOT_ERASED = 2, /* 目标扇区未擦除（禁止直接写） */
    BOOT_FLASH_ERR_OPERATION = 3,  /* 擦除/编程失败 */
} boot_flash_result_t;

/* 清空内部「已擦除扇区」位图。bootloader 上电、DFU 开始下载、
 * 每次 ERASE_APP 之前都要调用。 */
void boot_flash_init(void);

/* 擦除整个应用区。成功返回 true（耗时约 1~2 秒，56 个 2 KiB 扇区）。 */
bool boot_flash_erase_app(void);

/**
 * @brief 向应用区写数据
 * @param address    目标绝对地址（必须 4 字节对齐）
 * @param data       数据
 * @param length     字节数（末字不足 4 字节时内部用 0xFF 补齐）
 * @param auto_erase true  = 目标扇区未擦除时自动先擦除（DFU 下载用）
 *                   false = 目标扇区未擦除则直接返回 BOOT_FLASH_ERR_NOT_ERASED
 *                           （WinUSB 协议要求先 ERASE_APP，未擦除就写回 0x04）
 */
boot_flash_result_t boot_flash_write(uint32_t address, const uint8_t *data, uint32_t length,
                                     bool auto_erase);

/* 从应用区读数据（直接内存映射读） */
bool boot_flash_read(uint32_t address, uint8_t *data, uint32_t length);

/* 目标区间是否全是 0xFF（0x08004000 未烧录时的典型值） */
bool boot_flash_is_erased(uint32_t address, uint32_t length);

/* ------------------------------------------------ 应用有效性 / CRC32 */

/* 检查 0x08004000 处的初始 MSP 与 Reset 向量是否合法：
 *   MSP   ∈ [0x20000000, 0x20011800]
 *   Reset ∈ [0x08004000, 0x08020000) 且最低位为 1（Thumb 地址） */
bool boot_app_vector_valid(void);

/* CRC32（IEEE 802.3 多项式 0xEDB88320，初值 0xFFFFFFFF，末尾异或 0xFFFFFFFF），
 * 与 zlib.crc32() 结果一致。分块计算时用 boot_crc32_update()。 */
uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length);
uint32_t boot_crc32(const uint8_t *data, uint32_t length);

#endif /* __BOOT_FLASH_H */
