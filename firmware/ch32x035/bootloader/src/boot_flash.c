#include "boot_flash.h"

#include <string.h>

#include <ch32x035.h>

#include "boot_config.h"
#include "boot_log.h"
#include "boot_time.h"

/*
 * Flash 页擦写（只碰应用区）。
 *
 * ============================ 为什么自己实现 ============================
 * 1. **地址别名**：CH32X035 的 FPEC 只接受 0x0800_0000 别名区的地址
 *    （SDK 里 ValidAddrStart = FLASH_BASE = 0x08000000；WCH 的 IAP 例程同样是
 *    `#define FLASH_Base 0x08005000`）。传 0x0000_5xxx 会直接返回
 *    FLASH_ADR_RANGE_ERROR，Flash 一点不动 —— 这个坑实测踩过。
 * 2. **必须从 RAM 执行**：擦写期间 Flash 无法取指。如果这些指令本身在 Flash 里，
 *    每条取指都要等总线，擦写时间被拉长，而且在「USB 中断里也会擦写」的场景下
 *    极易表现成挂死。所以整个擦写路径放进 `.highcode`（启动时由 startup 从
 *    Flash 拷到 RAM）。
 * 3. SDK 的 FLASH_ROM_WRITE 永远返回 FLASH_COMPLETE（从不检查 WRPRTERR），
 *    写失败是静默的。这里自己查 STATR，并且每页写完回读校验。
 *
 * ============================ 关键约束 ============================
 * 页操作（认领数据 → 擦 → 写 → 回读校验）必须是一个**中断不可打断**的整体：
 * 调用方（src/boot_uf2.c）在进入前关中断。否则 USB 中断里新到的块会改掉源缓冲，
 * 写进去就是「一半旧一半新」，回读校验随机失败（实测：前十几页正常，突然
 * 校验失败 @0x00006100）。
 */

#define BOOT_FLASH_KEY1 ((uint32_t)0x45670123u)
#define BOOT_FLASH_KEY2 ((uint32_t)0xCDEF89ABu)

/* CH32X035 的 Flash 有两套地址：代码按 0x0 起链接/执行，FPEC 只认 0x0800_0000。 */
#define BOOT_FLASH_ALIAS_BASE ((uint32_t)0x08000000u)

/* 常驻 RAM 的代码：noinline 是必需的 —— 否则 LTO 会把它们内联回 Flash 里的调用者，
 * section 属性就白写了。 */
#define BOOT_RAMCODE __attribute__((noinline, section(".highcode")))

/* 私有页缓冲：写入期间中断可能改掉调用方的缓冲，所以先搬到这里。
 * 4 字节对齐：页编程按 32 位字读取源数据。 */
static uint8_t s_stage[BOOT_FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static uint32_t s_last_write_tick = 0xFFFFFFFFu;

static uint32_t flash_alias(uint32_t address) {
    return BOOT_FLASH_ALIAS_BASE + address;
}

static bool address_in_app(uint32_t address) {
    return address >= BOOT_APP_START && address < BOOT_APP_END &&
           (address & (BOOT_FLASH_PAGE_SIZE - 1u)) == 0u;
}

/* ------------------------------------------------------------------------- */
/* 擦写原语（全部在 RAM 里执行）                                              */
/* ------------------------------------------------------------------------- */

BOOT_RAMCODE static void flash_unlock(void) {
    FLASH->KEYR = BOOT_FLASH_KEY1;
    FLASH->KEYR = BOOT_FLASH_KEY2;
    FLASH->MODEKEYR = BOOT_FLASH_KEY1; /* 解锁快速编程/擦除模式 */
    FLASH->MODEKEYR = BOOT_FLASH_KEY2;
}

BOOT_RAMCODE static void flash_lock(void) {
    FLASH->CTLR |= FLASH_CTLR_FLOCK;
    FLASH->CTLR |= FLASH_CTLR_LOCK;
}

/* 擦一页（256 B）。page 是逻辑地址（0x5000 起），内部换算成别名。
 * 调用方必须已经关中断。 */
BOOT_RAMCODE static bool flash_erase_page(uint32_t page) {
    uint32_t stat;

    FLASH->STATR = FLASH_STATR_WRPRTERR; /* RW1C：先清掉上次的标志 */
    FLASH->STATR = FLASH_STATR_EOP;

    flash_unlock();
    FLASH->CTLR &= ~(FLASH_CTLR_OPTER | FLASH_CTLR_FTER | FLASH_CTLR_FTPG);
    FLASH->CTLR |= FLASH_CTLR_FTER; /* 页擦除 */
    FLASH->ADDR = flash_alias(page);
    FLASH->CTLR |= FLASH_CTLR_STRT;
    while ((FLASH->STATR & FLASH_STATR_BSY) != 0u) {
    }
    FLASH->CTLR &= ~FLASH_CTLR_FTER;
    stat = FLASH->STATR;
    flash_lock();

    FLASH->STATR = FLASH_STATR_WRPRTERR;
    FLASH->STATR = FLASH_STATR_EOP;

    return (stat & FLASH_STATR_WRPRTERR) == 0u;
}

/* 编程一页（256 B）。数据取自 s_stage（4 字节对齐）。调用方必须已经关中断。 */
BOOT_RAMCODE static bool flash_program_page(uint32_t page) {
    uint32_t stat;
    uint32_t i;

    FLASH->STATR = FLASH_STATR_WRPRTERR;
    FLASH->STATR = FLASH_STATR_EOP;

    flash_unlock();
    FLASH->CTLR &= ~(FLASH_CTLR_OPTER | FLASH_CTLR_FTER);
    FLASH->CTLR |= FLASH_CTLR_FTPG; /* 快速页编程 */
    FLASH->CTLR |= FLASH_CTLR_BUFRST;
    while ((FLASH->STATR & FLASH_STATR_BSY) != 0u) {
    }

    for (i = 0u; i < (BOOT_FLASH_PAGE_SIZE / 4u); ++i) {
        /* 写这个 Flash 地址就是往页缓冲里装一个 32 位字 */
        *(volatile uint32_t *)(flash_alias(page) + i * 4u) =
            ((const uint32_t *)s_stage)[i];
        FLASH->CTLR |= FLASH_CTLR_BUFLOAD;
        while ((FLASH->STATR & FLASH_STATR_BSY) != 0u) {
        }
    }

    FLASH->ADDR = flash_alias(page);
    FLASH->CTLR |= FLASH_CTLR_STRT;
    while ((FLASH->STATR & FLASH_STATR_BSY) != 0u) {
    }
    FLASH->CTLR &= ~FLASH_CTLR_FTPG;
    stat = FLASH->STATR;
    flash_lock();

    FLASH->STATR = FLASH_STATR_WRPRTERR;
    FLASH->STATR = FLASH_STATR_EOP;

    return (stat & FLASH_STATR_WRPRTERR) == 0u;
}

/* ------------------------------------------------------------------------- */
/* 对外接口                                                                  */
/* ------------------------------------------------------------------------- */

void boot_flash_init(void) {
    s_last_write_tick = 0xFFFFFFFFu;
}

uint32_t boot_flash_last_write_tick(void) {
    return s_last_write_tick;
}

bool boot_flash_invalidate_app(void) {
    bool ok;

    s_last_write_tick = 0xFFFFFFFFu;

    __disable_irq();
    ok = flash_erase_page(BOOT_APP_START);
    __enable_irq();

    if (!ok) {
        boot_log("[flash] 擦除入口页失败 @0x%08x\n", BOOT_APP_START);
    }
    return ok;
}

/* 回读校验：Flash 里的内容必须与 s_stage 逐字节一致；不一致时给出第一个差异的偏移 */
static bool verify_page(uint32_t address, uint32_t *out_offset) {
    const volatile uint8_t *flash = (const volatile uint8_t *)flash_alias(address);
    uint32_t i;

    for (i = 0u; i < BOOT_FLASH_PAGE_SIZE; ++i) {
        if (flash[i] != s_stage[i]) {
            if (out_offset != 0) {
                *out_offset = i;
            }
            return false;
        }
    }
    return true;
}

bool boot_flash_write_page(uint32_t address, const uint8_t *data) {
    uint32_t offset = 0u;
    uint32_t attempt;

    if (data == NULL || !address_in_app(address)) {
        return false;
    }

    /* 中断由调用方关闭（见 boot_flash.h）：从「认领待写页」到这里的 memcpy
     * 之间不能有任何中断，否则源数据会被新到的块改掉。 */
    memcpy(s_stage, data, BOOT_FLASH_PAGE_SIZE);

    // 失败重试一次：擦写的失败偶发（电源抖动、总线时序），重试一次比让整份
    // 升级作废划算得多；真的坏块第二次仍然会失败。
    for (attempt = 0u; attempt < 2u; ++attempt) {
        if (flash_erase_page(address) && flash_program_page(address) &&
            verify_page(address, &offset)) {
            s_last_write_tick = BOOT_SYSTICK_CNTL;
            return true;
        }
    }

    // 两次都失败：把差异打出来 —— flash=0xFF 表示根本没写进去；flash 是期望值的
    // 子集（按位与）表示页没擦干净；与期望完全无关表示数据在排队阶段被改坏了
    {
        const volatile uint8_t *flash = (const volatile uint8_t *)flash_alias(address);

        boot_log("[flash] 校验失败 @0x%08x +%u: flash=0x%02x want=0x%02x\n",
                 address, offset, (uint32_t)flash[offset], (uint32_t)s_stage[offset]);
    }
    return false;
}

bool boot_app_is_valid(void) {
    // 应用入口页（0x5000）的布局：
    //   +0  byte0 : .init 的第一条指令（j handle_reset），不应是擦除态/0
    //   +4  word  : .vector 的第一项 = _start 地址，必须落在应用区内
    // 只查这两处，既能挡住「空片」，也能挡住「只写了一半、入口还没到位」。
    uint32_t first_insn = *(const volatile uint32_t *)BOOT_APP_START;
    uint32_t vector0 = *(const volatile uint32_t *)(BOOT_APP_START + 4u);

    if (first_insn == 0xFFFFFFFFu || first_insn == 0x00000000u) {
        return false;
    }
    if (vector0 < BOOT_APP_START || vector0 >= BOOT_APP_END) {
        return false;
    }
    // RISC-V 的入口地址必须 2 字节对齐（本工程实际是 4 字节对齐）
    return (vector0 & 1u) == 0u;
}

bool boot_flash_page_matches(uint32_t address, const uint8_t *data) {
    if (data == NULL || !address_in_app(address)) {
        return false;
    }
    return memcmp((const void *)flash_alias(address), data, BOOT_FLASH_PAGE_SIZE) == 0;
}
