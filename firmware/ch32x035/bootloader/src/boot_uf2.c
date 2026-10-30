#include "boot_uf2.h"

#include <string.h>

#include "boot_config.h"
#include "boot_flash.h"
#include "boot_time.h"

/* 与 microsoft/uf2 的官方头文件对齐：设备侧解析用的块结构与打包用的
 * uf2conv.py（来自 third_party/uf2）必须描述同一个格式。谁的布局改了都会在
 * 固件构建时直接编不过，而不是等到拖文件时才发现。 */
#include "uf2.h"

_Static_assert(sizeof(UF2_Block) == 512, "UF2_Block must be one 512-byte sector");
_Static_assert(UF2_MAGIC_START0 == 0x0A324655u, "UF2 magic start0 mismatch");
_Static_assert(UF2_MAGIC_START1 == 0x9E5D5157u, "UF2 magic start1 mismatch");
_Static_assert(UF2_MAGIC_END == 0x0AB16F30u, "UF2 magic end mismatch");
_Static_assert(UF2_FLAG_FAMILY_ID == 0x00002000u, "UF2 family flag mismatch");
_Static_assert(UF2_FLAG_NOFLASH == 0x00000001u, "UF2 noflash flag mismatch");
_Static_assert(UF2_FLAG_MD5_CHKSUM == 0x00004000u, "UF2 MD5 flag mismatch");

/* 临界区：保护「待写页」缓冲不被 USB 中断改掉。
 * 固件里用 core_riscv.h 的 __disable_irq()/__enable_irq()；主机测试（没有中断）
 * 由 test/run.ps1 传 -DBOOT_HOST_TEST 关掉。 */
#ifdef BOOT_HOST_TEST
#define boot_irq_disable() \
    do {                   \
    } while (0)
#define boot_irq_enable() \
    do {                  \
    } while (0)
#else
#include <ch32x035.h>
#define boot_irq_disable() __disable_irq()
#define boot_irq_enable()  __enable_irq()
#endif

#define BOOT_UF2_SECTOR_SIZE 512u
#define BOOT_UF2_BLOCKS_MAX  (BOOT_APP_SIZE / BOOT_FLASH_PAGE_SIZE)
#define BOOT_UF2_BLOCK_BITS  ((BOOT_UF2_BLOCKS_MAX + 7u) / 8u)
#define BOOT_UF2_PAGE_COUNT  (BOOT_APP_SIZE / BOOT_FLASH_PAGE_SIZE)

typedef struct {
    uint32_t total;      /* 本轮拷贝的总块数（0 = 还没有开始） */
    uint32_t written;    /* 已落盘块数 */
    uint32_t last_rx;    /* 最近一次收到有效块的 SysTick 计数 */
    uint8_t error;       /* boot_uf2_error_t */
    uint8_t blocks[BOOT_UF2_BLOCK_BITS];
    uint8_t pages[BOOT_UF2_PAGE_COUNT];
    /* 入口页（最后提交）。必须 4 字节对齐：它会被原样交给 FLASH_ROM_WRITE，
     * 后者按 32 位字读取源缓冲，未对齐的指针在 RISC-V 上会触发异常。 */
    uint8_t first_page[BOOT_FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    bool first_page_pending;
} boot_uf2_session_t;

static boot_uf2_session_t s_sess;

/* 待写的一页（把 Flash 擦写从 USB 中断里搬出来）。
 * 中断里只做 256 B 的 memcpy；真正的擦+写+校验在主循环（boot_uf2_service）里做。
 * 如果下一块来得比主循环还快，中断里先把它冲掉（会慢一点，但不会丢数据）。 */
static uint8_t s_pending_page[BOOT_FLASH_PAGE_SIZE] __attribute__((aligned(4)));
static uint32_t s_pending_address;
static bool s_pending_valid;

/* ------------------------------------------------------------------------- */
/* 位图                                                                      */
/* ------------------------------------------------------------------------- */

static bool bit_test(const uint8_t *bits, uint32_t index) {
    return (bits[index / 8u] & (uint8_t)(1u << (index % 8u))) != 0u;
}

static void bit_set(uint8_t *bits, uint32_t index) {
    bits[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

/* 把挂起的一页真正写进 Flash。失败时置错误码并返回 false。 */
static bool flush_pending(void) {
    uint32_t address;
    bool ok;

    if (!s_pending_valid) {
        return true;
    }

    // 「认领 + 写」必须在同一个临界段里：中间只要有一次中断，USB 中断就可能把
    // 新到的块写进 s_pending_page，导致这次写的是「一半旧一半新」的数据。
    boot_irq_disable();
    address = s_pending_address;
    s_pending_valid = false;
    ok = boot_flash_write_page(address, s_pending_page);
    boot_irq_enable();

    if (!ok) {
        s_sess.error = BOOT_UF2_ERR_FLASH;
        return false;
    }
    bit_set(s_sess.pages, (address - BOOT_APP_START) / BOOT_FLASH_PAGE_SIZE);
    return true;
}

/* ------------------------------------------------------------------------- */
/* 会话                                                                      */
/* ------------------------------------------------------------------------- */

void boot_uf2_init(void) {
    s_pending_valid = false;
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.last_rx = BOOT_SYSTICK_CNTL;
}

uint8_t boot_uf2_error(void) {
    return s_sess.error;
}

uint32_t boot_uf2_written_blocks(void) {
    return s_sess.written;
}

uint32_t boot_uf2_total_blocks(void) {
    return s_sess.total;
}

static void session_start(uint32_t total_blocks) {
    boot_uf2_init();
    s_sess.total = total_blocks;

    // 先让应用失效：万一这轮拷贝没写完，下次启动必须停在 bootloader，
    // 而不是跳进写了一半的固件。真正的入口页在整份文件落盘后才写（见 commit）。
    (void)boot_flash_invalidate_app();
}

/* 整份文件都落盘了：最后写入口页。写成功才算升级完成。 */
static bool session_commit(void) {
    bool ok;

    if (!s_sess.first_page_pending) {
        // 文件里没有包含入口页（畸形 UF2）：不能算成功
        return false;
    }

    // 同样整段关中断：入口页只有这一份数据，不能在写入过程中被新会话清掉
    boot_irq_disable();
    ok = boot_flash_write_page(BOOT_APP_START, s_sess.first_page);
    if (ok) {
        s_sess.first_page_pending = false;
    }
    boot_irq_enable();

    return ok;
}

bool boot_uf2_is_done(void) {
    if (s_sess.total == 0u || s_sess.error != BOOT_UF2_OK) {
        return false;
    }
    // 挂起的最后一页要先落盘，否则「完成」是假的
    if (!flush_pending()) {
        return false;
    }
    if (s_sess.written < s_sess.total) {
        return false;
    }
    if (s_sess.first_page_pending) {
        // 所有块都到了，现在提交入口页
        if (!session_commit()) {
            s_sess.error = BOOT_UF2_ERR_COMMIT;
            return false;
        }
    }
    return true;
}

void boot_uf2_service(void) {
    if (s_sess.error != BOOT_UF2_OK) {
        // 出错后主机安静 2 s 就复位会话：用户可以「再拖一次」，不必拔线
        if (boot_ticks_since(s_sess.last_rx) > boot_ticks_from_ms(2000u)) {
            boot_uf2_init();
        }
        return;
    }

    // 正常路径：把挂起的那一页在**主循环**里写掉（不在 USB 中断里长时间关中断）
    (void)flush_pending();
}

/* ------------------------------------------------------------------------- */
/* 块解析                                                                    */
/* ------------------------------------------------------------------------- */

/* 合法的 UF2 块 -> true，并给出它对应的 Flash 页地址 */
static bool block_decode(const uint8_t *raw, uint32_t *out_address) {
    const UF2_Block *block = (const UF2_Block *)raw;

    if (block->magicStart0 != UF2_MAGIC_START0 ||
        block->magicStart1 != UF2_MAGIC_START1 ||
        block->magicEnd != UF2_MAGIC_END) {
        return false;
    }
    // 不带 family 的块、非主 Flash 的块、带 MD5 的块都不处理
    if ((block->flags & UF2_FLAG_FAMILY_ID) == 0u ||
        (block->flags & (UF2_FLAG_NOFLASH | UF2_FLAG_MD5_CHKSUM)) != 0u) {
        return false;
    }
    // UF2 头里第 8 个 32 位字：有 family 标志时是 family ID
    if (block->reserved != BOOT_UF2_FAMILY_ID) {
        return false;
    }
    // 只接受整页块：这样每次落盘都是一次擦+写，且地址天然 256 B 对齐
    if (block->payloadSize != BOOT_FLASH_PAGE_SIZE) {
        return false;
    }
    // 块号先卡在位图能表示的范围内（这是防越界写的关键一条）
    if (block->numBlocks == 0u || block->numBlocks > BOOT_UF2_BLOCKS_MAX) {
        return false;
    }
    if (block->blockNo >= block->numBlocks) {
        return false;
    }
    if ((block->targetAddr & (BOOT_FLASH_PAGE_SIZE - 1u)) != 0u) {
        return false;
    }
    if (block->targetAddr < BOOT_APP_START ||
        block->targetAddr > BOOT_APP_END - BOOT_FLASH_PAGE_SIZE) {
        return false;
    }

    *out_address = block->targetAddr;
    return true;
}

static bool page_content_matches(uint32_t address, const uint8_t *data) {
    return boot_flash_page_matches(address, data);
}

/* ------------------------------------------------------------------------- */
/* MSC 写回调                                                                */
/* ------------------------------------------------------------------------- */

int boot_uf2_write_sector(uint32_t start_sector, const uint8_t *buffer, uint32_t sector_count) {
    uint32_t address;

    (void)start_sector; /* 落盘地址由 UF2 块自己带，与磁盘扇区号无关 */

    for (uint32_t i = 0u; i < sector_count; ++i) {
        const uint8_t *raw = buffer + i * BOOT_UF2_SECTOR_SIZE;
        const UF2_Block *block = (const UF2_Block *)raw;
        uint32_t page;

        if (!block_decode(raw, &address)) {
            continue; /* 目录 / FAT / 无关数据：正常现象 */
        }

        if (s_sess.error != BOOT_UF2_OK) {
            continue; /* 等 boot_uf2_service() 复位会话 */
        }
        s_sess.last_rx = BOOT_SYSTICK_CNTL;

        if (s_sess.total != block->numBlocks) {
            // 新一轮拷贝（第一次，或者主机重来了另一份文件）
            session_start(block->numBlocks);
        }
        if (bit_test(s_sess.blocks, block->blockNo)) {
            continue; /* 同一轮里的重复块（主机重发 WRITE 命令） */
        }
        bit_set(s_sess.blocks, block->blockNo);

        page = (address - BOOT_APP_START) / BOOT_FLASH_PAGE_SIZE;

        if (address == BOOT_APP_START) {
            // 入口页最后写：先暂存，整份文件落盘后才提交（见 session_commit）
            memcpy(s_sess.first_page, block->data, BOOT_FLASH_PAGE_SIZE);
            s_sess.first_page_pending = true;
        } else if (bit_test(s_sess.pages, page)) {
            // 同一页被两个块覆盖：只有内容一致才算正常（例如主机重试）
            if (!page_content_matches(address, block->data)) {
                s_sess.error = BOOT_UF2_ERR_CONFLICT;
                continue;
            }
        } else if (s_pending_valid && s_pending_address == address) {
            // 该页已经在等待写入（块号已去重，正常不会走到这里）
            if (memcmp(s_pending_page, block->data, BOOT_FLASH_PAGE_SIZE) != 0) {
                s_sess.error = BOOT_UF2_ERR_CONFLICT;
                continue;
            }
        } else {
            // 先把上一页写掉（缓冲区只有一页），再把这一页挂起让主循环去写。
            // 中断里只做 256 B memcpy，Flash 擦写的几毫秒留在主循环。
            if (!flush_pending()) {
                continue;
            }
            memcpy(s_pending_page, block->data, BOOT_FLASH_PAGE_SIZE);
            s_pending_address = address;
            s_pending_valid = true;
        }

        s_sess.written++;
    }

    return 0;
}
