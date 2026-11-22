#include "bsp/bsp_uid.h"

#include "at32f402_405.h"

static void uid_hex8(uint8_t value, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = hex[(value >> 4) & 0x0F];
    out[1] = hex[value & 0x0F];
}

uint32_t bsp_get_chip_id(void) {
    /* AT32 的这颗寄存器在库里叫 pid（debug idcode 寄存器，位域别名 idcode_bit.pid） */
    return DEBUGMCU->pid;
}

void bsp_get_uid_hex16(char out_hex[BSP_UID_HEX16_STR_LEN]) {
    if (out_hex == 0) {
        return;
    }

    const uint32_t uid_words[2] = {CHIP_UID_1, CHIP_UID_2};

    for (uint8_t i = 0; i < 8u; i++) {
        const uint32_t word = uid_words[i >> 2];
        const uint8_t value = (uint8_t)((word >> ((i & 0x03u) * 8u)) & 0xFFu);
        uid_hex8(value, &out_hex[i * 2u]);
    }
    out_hex[BSP_UID_HEX16_STR_LEN - 1u] = '\0';
}
