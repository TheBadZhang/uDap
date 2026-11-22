/**
 * @file    boot_flash.c
 * @brief   bootloader 的 Flash 擦写、CRC32 与应用有效性检查
 *
 * 擦写流程照 AT32 SDK 例程（project/at_start_f402/examples/flash/flash_write_read）：
 *   flash_unlock() -> flash_sector_erase() / flash_word_program() -> flash_lock()
 * 这两个底层函数内部**不解锁** Flash，必须由调用者先 flash_unlock()。
 */

#include "boot_flash.h"

#include "at32f402_405.h"
#include "at32f402_405_flash.h"

#include "boot_config.h"

/* 应用区「已擦除」扇区位图：ERASE_APP 之后置位；DFU 懒擦除时逐扇区置位。
 * WinUSB 协议靠它实现「未擦除就写 -> status 0x04」。 */
static uint8_t s_erased_map[(BOOT_APP_SECTOR_COUNT + 7u) / 8u];

/* ------------------------------------------------------------ 内部工具 */

/* 地址 + 长度是否完整落在应用区内（杜绝擦写 bootloader 自己） */
static bool addr_in_app(uint32_t address, uint32_t length) {
    if (address < BOOT_APP_BASE) {
        return false;
    }
    if (length > BOOT_APP_SIZE) {
        return false;
    }
    /* 用减法避免 address + length 溢出 */
    if ((address - BOOT_APP_BASE) > (BOOT_APP_SIZE - length)) {
        return false;
    }
    return true;
}

static bool page_is_erased(uint32_t page) {
    return (s_erased_map[page >> 3] & (uint8_t)(1u << (page & 7u))) != 0u;
}

static void page_mark_erased(uint32_t page) {
    s_erased_map[page >> 3] |= (uint8_t)(1u << (page & 7u));
}

static bool range_is_erased(uint32_t address, uint32_t length) {
    uint32_t first = (address - BOOT_APP_BASE) / BOOT_SECTOR_SIZE;
    uint32_t last = (address + length - 1u - BOOT_APP_BASE) / BOOT_SECTOR_SIZE;

    for (uint32_t page = first; page <= last; page++) {
        if (!page_is_erased(page)) {
            return false;
        }
    }
    return true;
}

/* 擦一个扇区：失败时清错误标志，避免影响后续操作 */
static bool flash_erase_sector(uint32_t address) {
    flash_status_type status;

    flash_unlock();
    flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
    status = flash_sector_erase(address);
    flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
    flash_lock();

    return status == FLASH_OPERATE_DONE;
}

/* 编程一个 4 字节字 */
static bool flash_program_word(uint32_t address, uint32_t data) {
    flash_status_type status;

    flash_unlock();
    flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
    status = flash_word_program(address, data);
    flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
    flash_lock();

    return status == FLASH_OPERATE_DONE;
}

/* 确保 [address, address+length) 覆盖的扇区都已擦除（DFU 懒擦除用） */
static bool ensure_erased(uint32_t address, uint32_t length) {
    uint32_t first = (address - BOOT_APP_BASE) / BOOT_SECTOR_SIZE;
    uint32_t last = (address + length - 1u - BOOT_APP_BASE) / BOOT_SECTOR_SIZE;

    for (uint32_t page = first; page <= last; page++) {
        if (page_is_erased(page)) {
            continue;
        }
        if (!flash_erase_sector(BOOT_APP_BASE + page * BOOT_SECTOR_SIZE)) {
            return false;
        }
        page_mark_erased(page);
    }
    return true;
}

/* ------------------------------------------------------------ 公共接口 */

void boot_flash_init(void) {
    for (uint32_t i = 0u; i < sizeof(s_erased_map); i++) {
        s_erased_map[i] = 0u;
    }
}

bool boot_flash_erase_app(void) {
    for (uint32_t page = 0u; page < BOOT_APP_SECTOR_COUNT; page++) {
        if (!flash_erase_sector(BOOT_APP_BASE + page * BOOT_SECTOR_SIZE)) {
            return false;
        }
        page_mark_erased(page);
    }
    return true;
}

boot_flash_result_t boot_flash_write(uint32_t address, const uint8_t *data, uint32_t length,
                                     bool auto_erase) {
    uint32_t offset;

    if (!addr_in_app(address, length) || length == 0u) {
        return BOOT_FLASH_ERR_RANGE;
    }

    if (auto_erase) {
        if (!ensure_erased(address, length)) {
            return BOOT_FLASH_ERR_OPERATION;
        }
    } else if (!range_is_erased(address, length)) {
        return BOOT_FLASH_ERR_NOT_ERASED;
    }

    /* 逐 4 字节字编程；末字不足 4 字节时用 0xFF 补齐（擦除态就是 0xFF） */
    for (offset = 0u; offset + 4u <= length; offset += 4u) {
        uint32_t word = (uint32_t)data[offset] | ((uint32_t)data[offset + 1u] << 8) |
                        ((uint32_t)data[offset + 2u] << 16) | ((uint32_t)data[offset + 3u] << 24);
        if (!flash_program_word(address + offset, word)) {
            return BOOT_FLASH_ERR_OPERATION;
        }
    }
    if (offset < length) {
        uint32_t word = 0xFFFFFFFFu;

        for (uint32_t i = 0u; i < 4u; i++) {
            if (offset + i < length) {
                word = (word & ~(0xFFu << (8u * i))) | ((uint32_t)data[offset + i] << (8u * i));
            }
        }
        if (!flash_program_word(address + offset, word)) {
            return BOOT_FLASH_ERR_OPERATION;
        }
    }

    return BOOT_FLASH_OK;
}

bool boot_flash_read(uint32_t address, uint8_t *data, uint32_t length) {
    const volatile uint8_t *src;

    if (!addr_in_app(address, length)) {
        return false;
    }
    src = (const volatile uint8_t *)address;
    for (uint32_t i = 0u; i < length; i++) {
        data[i] = src[i];
    }
    return true;
}

bool boot_flash_is_erased(uint32_t address, uint32_t length) {
    const volatile uint8_t *src;

    if (!addr_in_app(address, length)) {
        return false;
    }
    src = (const volatile uint8_t *)address;
    for (uint32_t i = 0u; i < length; i++) {
        if (src[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

bool boot_app_vector_valid(void) {
    uint32_t app_msp = *(const volatile uint32_t *)BOOT_APP_BASE;
    uint32_t app_reset = *(const volatile uint32_t *)(BOOT_APP_BASE + 4u);

    if ((app_msp < BOOT_RAM_BASE) || (app_msp > BOOT_RAM_END)) {
        return false;
    }
    if ((app_reset < BOOT_APP_BASE) || (app_reset >= BOOT_APP_END)) {
        return false;
    }
    /* Cortex-M 的异常向量必须是 Thumb 地址（最低位 = 1） */
    if ((app_reset & 1u) == 0u) {
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- CRC32 */

uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length) {
    while (length-- != 0u) {
        crc ^= (uint32_t)(*data++);
        for (uint32_t bit = 0u; bit < 8u; bit++) {
            /* 用「取负再掩码」代替分支：bit0 为 1 时掩码 0xFFFFFFFF，否则 0 */
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

uint32_t boot_crc32(const uint8_t *data, uint32_t length) {
    return boot_crc32_update(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}
