#include "drv/drv_mode_store.h"

#include <ch32x035.h>
#include "ch32x035_flash.h"

// 用户选择字信息块：4 个 32 位字，擦除/编程以整块（256 B 页）为单位。
//   0x1FFFF800  RDPR / USER
//   0x1FFFF804  Data0 / Data1   <- 本驱动只改这一个字里的 Data0 字节
//   0x1FFFF808  WRPR0 / WRPR1
//   0x1FFFF80C  WRPR2 / WRPR3
#define DRV_MODE_STORE_OB_BASE ((uint32_t)0x1FFFF800u)
#define DRV_MODE_STORE_OB_WORDS 4u

// 用户自定义字节在 Data 字里的位置：
//   [7:0] Data0   [15:8] nData0   [23:16] Data1   [31:24] nData1
#define DRV_MODE_STORE_BYTE0_SHIFT 0u
#define DRV_MODE_STORE_BYTE1_SHIFT 16u
// 反码字节相对数据字节的偏移
#define DRV_MODE_STORE_NBYTE_SHIFT 8u
#define DRV_MODE_STORE_BYTE_PAIR_MASK ((uint32_t)0x0000FFFFu)

static volatile uint32_t *mode_store_ob_words(void) {
    return (volatile uint32_t *)DRV_MODE_STORE_OB_BASE;
}

// 指定位置上的「值 + 反码」是否成对合法
static bool mode_store_byte_ok(uint32_t data_word, uint32_t shift, uint8_t *out_value) {
    uint8_t value = (uint8_t)(data_word >> shift);
    uint8_t nvalue = (uint8_t)(data_word >> (shift + DRV_MODE_STORE_NBYTE_SHIFT));

    if ((uint8_t)(value ^ 0xFFu) != nvalue) {
        return false;
    }
    if (out_value != 0) {
        *out_value = value;
    }
    return true;
}

// 整块擦除后原样回写，只替换 shift 位置上的「值 + 反码」对，
// 保住 RDPR/USER/WRPR 与**另一个**用户字节。
static bool mode_store_write_byte(uint32_t shift, uint8_t value) {
    uint8_t nvalue = (uint8_t)~value;
    uint8_t current;
    uint32_t words[DRV_MODE_STORE_OB_WORDS];
    uint32_t pair_mask = DRV_MODE_STORE_BYTE_PAIR_MASK << shift;
    uint32_t i;

    // 读保护开启时用户选择字不可写（SDK 的 FLASH_UserOptionByteConfig 同样直接拒绝）
    if (FLASH_GetReadOutProtectionStatus() == SET) {
        return false;
    }

    // 已保存相同且反码有效时不再擦写，省擦写寿命、也避开擦写中途掉电的风险
    if (mode_store_byte_ok(mode_store_ob_words()[1], shift, &current) && current == value) {
        return true;
    }

    for (i = 0u; i < DRV_MODE_STORE_OB_WORDS; ++i) {
        words[i] = mode_store_ob_words()[i];
    }
    words[1] = (words[1] & ~pair_mask) |
               ((uint32_t)nvalue << (shift + DRV_MODE_STORE_NBYTE_SHIFT)) |
               ((uint32_t)value << shift);

    // 顺序与 SDK 的 FLASH_OptionBytePR 一致：FLASH_EraseOptionBytes() 内部用
    // OBKEYR 解锁 OBWRE（操作完后只清 LOCK，OBWRE 保持解锁），随后 Unlock_Fast
    // 解锁 LOCK/FLOCK，即可用快速页编程把整块写回。
    if (FLASH_EraseOptionBytes() != FLASH_COMPLETE) {
        return false;
    }

    FLASH_Unlock_Fast();
    FLASH_BufReset();
    for (i = 0u; i < DRV_MODE_STORE_OB_WORDS; ++i) {
        FLASH_BufLoad(DRV_MODE_STORE_OB_BASE + 4u * i, words[i]);
    }
    FLASH_ProgramPage_Fast(DRV_MODE_STORE_OB_BASE);
    FLASH_Lock_Fast();

    // 回读校验，确认这次真的写进去了
    return mode_store_byte_ok(mode_store_ob_words()[1], shift, &current) && current == value;
}

/* ---------------------------------------------------------------------------
 * Data1 的位域（定义见 drv_mode_store.h）
 *
 *   bit0   = 串口模式（0 = 串口1+DTR/RTS，1 = 双串口）
 *   bit2:1 = LED 模式
 *   bit7:3 = 保留，必须为 0（擦除态/旧垃圾值因此被判为无效）
 * ------------------------------------------------------------------------- */
#define MODE_STORE_DATA1_UART_MASK     ((uint8_t)0x01u)
#define MODE_STORE_DATA1_LED_SHIFT     1u
#define MODE_STORE_DATA1_LED_MASK      ((uint8_t)0x06u)
#define MODE_STORE_DATA1_RESERVED_MASK ((uint8_t)0xF8u)

static bool mode_store_data1_decode(uint8_t value) {
    return (value & MODE_STORE_DATA1_RESERVED_MASK) == 0u;
}

// 改 Data1 里的一段位，另一段（以及 Data0）保持原值。
// 读不到有效旧值（擦除态 / 反码不匹配 / 保留位非 0）时按「串口1+DTR/RTS + LED 默认模式」起步。
static bool mode_store_data1_update(uint8_t clear_mask, uint8_t set_value) {
    uint8_t current = 0u;
    uint8_t data1;

    if (mode_store_byte_ok(mode_store_ob_words()[1], DRV_MODE_STORE_BYTE1_SHIFT, &data1) &&
        mode_store_data1_decode(data1)) {
        current = data1;
    }

    current = (uint8_t)((current & (uint8_t)~clear_mask) | set_value);
    return mode_store_write_byte(DRV_MODE_STORE_BYTE1_SHIFT, current);
}

bool drv_mode_store_load(firmware_mode_t *out_mode) {
    uint8_t data0;

    if (out_mode == 0) {
        return false;
    }
    if (!mode_store_byte_ok(mode_store_ob_words()[1], DRV_MODE_STORE_BYTE0_SHIFT, &data0)) {
        return false;
    }
    // 取值从 0 起连续，越界即视为非法记录
    if (data0 >= (uint8_t)FIRMWARE_MODE_COUNT) {
        return false;
    }

    *out_mode = (firmware_mode_t)data0;
    return true;
}

bool drv_mode_store_save(firmware_mode_t mode) {
    uint8_t data0 = (uint8_t)mode;

    if (data0 >= (uint8_t)FIRMWARE_MODE_COUNT) {
        return false;
    }
    return mode_store_write_byte(DRV_MODE_STORE_BYTE0_SHIFT, data0);
}

bool drv_mode_store_load_uart_mode(drv_uart_mode_t *out_mode) {
    uint8_t data1;

    if (out_mode == 0) {
        return false;
    }
    if (!mode_store_byte_ok(mode_store_ob_words()[1], DRV_MODE_STORE_BYTE1_SHIFT, &data1)) {
        return false;
    }
    if (!mode_store_data1_decode(data1)) {
        return false;
    }

    *out_mode = (data1 & MODE_STORE_DATA1_UART_MASK) ? DRV_UART_MODE_DUAL : DRV_UART_MODE_FLOW;
    return true;
}

bool drv_mode_store_save_uart_mode(drv_uart_mode_t mode) {
    if ((uint32_t)mode >= (uint32_t)DRV_UART_MODE_COUNT) {
        return false;
    }
    return mode_store_data1_update(MODE_STORE_DATA1_UART_MASK,
                                   (mode == DRV_UART_MODE_DUAL) ? MODE_STORE_DATA1_UART_MASK : 0u);
}

bool drv_mode_store_load_led_mode(drv_led_mode_t *out_mode) {
    uint8_t data1;

    if (out_mode == 0) {
        return false;
    }
    if (!mode_store_byte_ok(mode_store_ob_words()[1], DRV_MODE_STORE_BYTE1_SHIFT, &data1)) {
        return false;
    }
    if (!mode_store_data1_decode(data1)) {
        return false;
    }

    *out_mode = (drv_led_mode_t)((data1 & MODE_STORE_DATA1_LED_MASK) >> MODE_STORE_DATA1_LED_SHIFT);
    return true;
}

bool drv_mode_store_save_led_mode(drv_led_mode_t mode) {
    if ((uint32_t)mode >= (uint32_t)DRV_LED_MODE_COUNT) {
        return false;
    }
    return mode_store_data1_update(MODE_STORE_DATA1_LED_MASK,
                                   ((uint32_t)mode << MODE_STORE_DATA1_LED_SHIFT) & MODE_STORE_DATA1_LED_MASK);
}
