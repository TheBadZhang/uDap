#include "drv/drv_mode_store.h"

#include "at32f402_405.h"

/* USD 里 data0 / data1 的字节地址（见头文件里的字布局） */
#define USD_DATA0_ADDR 0x1FFFF804u
#define USD_DATA1_ADDR 0x1FFFF806u

/* data1 的位域 */
#define STORE_UART_MODE_BIT 0u
#define STORE_LED_MODE_SHIFT 1u
#define STORE_LED_MODE_MASK 0x06u
#define STORE_RESERVED_MASK 0xF8u

static bool byte_is_valid(uint16_t word) {
    const uint8_t value = (uint8_t)(word & 0xFFu);
    const uint8_t check = (uint8_t)((word >> 8) & 0xFFu);
    return (uint8_t)(~value) == check;
}

static bool read_data0(uint8_t *out) {
    const uint16_t word = USD->data0;
    if (!byte_is_valid(word)) {
        return false;
    }
    *out = (uint8_t)(word & 0xFFu);
    return true;
}

static bool read_data1(uint8_t *out) {
    const uint16_t word = USD->data1;
    if (!byte_is_valid(word)) {
        return false;
    }
    *out = (uint8_t)(word & 0xFFu);
    return true;
}

/* 擦除 USD 页后重写 data0/data1（flash 库会自己把 fap 原样恢复） */
static bool write_data_bytes(uint8_t data0, uint8_t data1) {
    /* AT32 库的 flash_unlock() 返回 void（解除 OPLK 后由库自己判成功） */
    flash_unlock();

    if (flash_user_system_data_erase() != FLASH_OPERATE_DONE) {
        flash_lock();
        return false;
    }

    if (flash_user_system_data_program(USD_DATA0_ADDR, data0) != FLASH_OPERATE_DONE) {
        flash_lock();
        return false;
    }
    if (flash_user_system_data_program(USD_DATA1_ADDR, data1) != FLASH_OPERATE_DONE) {
        flash_lock();
        return false;
    }

    flash_lock();

    /* 回读校验 */
    uint8_t check0 = 0u;
    uint8_t check1 = 0u;
    return read_data0(&check0) && read_data1(&check1) && check0 == data0 && check1 == data1;
}

static uint8_t load_data0_or_zero(void) {
    uint8_t value = 0u;
    if (!read_data0(&value)) {
        return 0u;
    }
    return value;
}

static uint8_t load_data1_or_zero(void) {
    uint8_t value = 0u;
    if (!read_data1(&value)) {
        return 0u;
    }
    return value;
}

bool drv_mode_store_load(firmware_mode_t *out_mode) {
    uint8_t value = 0u;
    if (out_mode == 0 || !read_data0(&value)) {
        return false;
    }
    if ((uint32_t)value >= (uint32_t)FIRMWARE_MODE_COUNT) {
        return false;
    }
    *out_mode = (firmware_mode_t)value;
    return true;
}

bool drv_mode_store_save(firmware_mode_t mode) {
    if ((uint32_t)mode >= (uint32_t)FIRMWARE_MODE_COUNT) {
        return false;
    }
    const uint8_t current = load_data0_or_zero();
    if (current == (uint8_t)mode) {
        return true; /* 值没变，不擦写选项字节 */
    }
    return write_data_bytes((uint8_t)mode, load_data1_or_zero());
}

bool drv_mode_store_load_uart_mode(drv_uart_mode_t *out_mode) {
    uint8_t packed = 0u;
    if (out_mode == 0 || !read_data1(&packed)) {
        return false;
    }
    if ((packed & STORE_RESERVED_MASK) != 0u) {
        return false;
    }
    const uint8_t value = (uint8_t)((packed >> STORE_UART_MODE_BIT) & 0x01u);
    if ((uint32_t)value >= (uint32_t)DRV_UART_MODE_COUNT) {
        return false;
    }
    *out_mode = (drv_uart_mode_t)value;
    return true;
}

bool drv_mode_store_save_uart_mode(drv_uart_mode_t mode) {
    if ((uint32_t)mode >= (uint32_t)DRV_UART_MODE_COUNT) {
        return false;
    }
    uint8_t packed = load_data1_or_zero();
    const uint8_t led_bits = (uint8_t)(packed & STORE_LED_MODE_MASK);
    const uint8_t uart_bit = (uint8_t)(((uint8_t)mode & 0x01u) << STORE_UART_MODE_BIT);
    if (packed == (uint8_t)(led_bits | uart_bit) && read_data1(&packed)) {
        return true;
    }
    return write_data_bytes(load_data0_or_zero(), (uint8_t)(led_bits | uart_bit));
}

bool drv_mode_store_load_led_mode(drv_led_mode_t *out_mode) {
    uint8_t packed = 0u;
    if (out_mode == 0 || !read_data1(&packed)) {
        return false;
    }
    if ((packed & STORE_RESERVED_MASK) != 0u) {
        return false;
    }
    const uint8_t value = (uint8_t)((packed & STORE_LED_MODE_MASK) >> STORE_LED_MODE_SHIFT);
    if ((uint32_t)value >= (uint32_t)DRV_LED_MODE_COUNT) {
        return false;
    }
    *out_mode = (drv_led_mode_t)value;
    return true;
}

bool drv_mode_store_save_led_mode(drv_led_mode_t mode) {
    if ((uint32_t)mode >= (uint32_t)DRV_LED_MODE_COUNT) {
        return false;
    }
    uint8_t packed = 0u;
    const bool stored_ok = read_data1(&packed);
    const uint8_t uart_bit = (uint8_t)(packed & 0x01u);
    const uint8_t led_bits = (uint8_t)(((uint8_t)mode << STORE_LED_MODE_SHIFT) & STORE_LED_MODE_MASK);
    if (stored_ok && packed == (uint8_t)(uart_bit | led_bits)) {
        return true;
    }
    return write_data_bytes(load_data0_or_zero(), (uint8_t)(uart_bit | led_bits));
}
