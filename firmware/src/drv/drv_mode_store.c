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

// Data0 字里的字节位置（低字节 = Data0，次低字节 = nData0 反码）
#define DRV_MODE_STORE_DATA0_SHIFT 0u
#define DRV_MODE_STORE_NDATA0_SHIFT 8u
#define DRV_MODE_STORE_DATA0_MASK ((uint32_t)0x0000FFFFu)

static volatile uint32_t *mode_store_ob_words(void) {
    return (volatile uint32_t *)DRV_MODE_STORE_OB_BASE;
}

// Data0 / nData0 是否构成一对合法的「值 + 反码」
static bool mode_store_complement_ok(uint32_t data_word, uint8_t *out_data0) {
    uint8_t data0 = (uint8_t)(data_word >> DRV_MODE_STORE_DATA0_SHIFT);
    uint8_t ndata0 = (uint8_t)(data_word >> DRV_MODE_STORE_NDATA0_SHIFT);

    if ((uint8_t)(data0 ^ 0xFFu) != ndata0) {
        return false;
    }
    if (out_data0 != 0) {
        *out_data0 = data0;
    }
    return true;
}

// 可持久化的模式集合：取值落在 [0, FIRMWARE_MODE_COUNT) 才合法（取值从 0 起连续）。
// **新增模式时只需在 firmware_mode_t 末尾追加成员**，这里自动放行。
static bool mode_is_valid(uint8_t data0) {
    return data0 < (uint8_t)FIRMWARE_MODE_COUNT;
}

bool drv_mode_store_load(firmware_mode_t *out_mode) {
    uint8_t data0;

    if (out_mode == 0) {
        return false;
    }
    if (!mode_store_complement_ok(mode_store_ob_words()[1], &data0)) {
        return false;
    }
    if (!mode_is_valid(data0)) {
        return false;
    }

    *out_mode = (firmware_mode_t)data0;
    return true;
}

bool drv_mode_store_save(firmware_mode_t mode) {
    uint8_t data0 = (uint8_t)mode;
    uint8_t ndata0 = (uint8_t)~data0;
    uint8_t current;
    uint32_t words[DRV_MODE_STORE_OB_WORDS];
    uint32_t i;
    firmware_mode_t verify;

    if (!mode_is_valid(data0)) {
        return false;
    }

    // 读保护开启时用户选择字不可写（SDK 的 FLASH_UserOptionByteConfig 同样直接拒绝）
    if (FLASH_GetReadOutProtectionStatus() == SET) {
        return false;
    }

    // 已保存相同且反码有效时不再擦写，省擦写寿命、也避开擦写中途掉电的风险
    if (mode_store_complement_ok(mode_store_ob_words()[1], &current) && current == data0) {
        return true;
    }

    // 先整块读出，擦除后原样回写，只替换 Data0/nData0，保住 RDPR/USER/WRPR/Data1
    for (i = 0u; i < DRV_MODE_STORE_OB_WORDS; ++i) {
        words[i] = mode_store_ob_words()[i];
    }
    words[1] = (words[1] & ~DRV_MODE_STORE_DATA0_MASK) |
               ((uint32_t)ndata0 << DRV_MODE_STORE_NDATA0_SHIFT) |
               ((uint32_t)data0 << DRV_MODE_STORE_DATA0_SHIFT);

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
    return drv_mode_store_load(&verify) && verify == mode;
}
