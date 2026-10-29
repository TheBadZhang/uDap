/*
 * UF2 接收路径的主机端测试（不需要硬件）。
 *
 * 被测试的是**固件里真正在用的那份逻辑**：
 *   src/boot_uf2.c   UF2 块校验 + 去重 + 落盘顺序 + 完成判定
 *   third_party/.../bootuf2.c  虚拟 FAT16 的读路径（主机枚举/列目录用）
 *
 * 硬件相关的部分（Flash 擦写、SysTick）由本文件用模拟实现替代。
 *
 * 用例：
 *   1. 正常路径      —— 落盘内容与 firmware.bin 逐字节一致，且报告「完成」
 *   2. 错误 family   —— 一页都不写，不报告完成
 *   3. 越界块号      —— 该块被忽略，其余正常写，不报告完成（不会越界写位图）
 *   4. 重复块        —— 主机重发同一块，仍然完成，且每页只写一次
 *   5. 中断的拷贝    —— 丢掉最后一块：应用入口页必须仍是擦除态（不能跳进半截固件）
 *
 * 用法（由 run.ps1 调用）：
 *   uf2_host_test <firmware.uf2> <firmware.bin>
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_config.h"
#include "boot_uf2.h"
#include "bootuf2.h"

/* ------------------------------------------------------------------------- */
/* 模拟硬件                                                                  */
/* ------------------------------------------------------------------------- */

/* boot_time.h 用这个变量代替 SysTick（见 -DBOOT_SYSTICK_CNTL） */
volatile uint32_t g_host_ticks;
uint32_t SystemCoreClock = 48000000u;

static uint8_t s_flash[BOOT_APP_SIZE];
static uint32_t s_page_writes;      /* boot_flash_write_page 调用次数 */
static uint32_t s_page_write_count[BOOT_APP_SIZE / BOOT_FLASH_PAGE_SIZE];
static uint32_t s_last_write_tick = 0xFFFFFFFFu;

static void flash_model_reset(void) {
    memset(s_flash, 0xFF, sizeof(s_flash));
    memset(s_page_write_count, 0, sizeof(s_page_write_count));
    s_page_writes = 0u;
    s_last_write_tick = 0xFFFFFFFFu;
}

/* Flash 语义：擦除 = 全 0xFF，编程 = 位只能 1 -> 0 */
static void flash_program_bits(uint32_t offset, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        s_flash[offset + i] &= data[i];
    }
}

bool boot_flash_write_page(uint32_t address, const uint8_t *data) {
    uint32_t offset;

    if (address < BOOT_APP_START || address >= BOOT_APP_END ||
        (address & (BOOT_FLASH_PAGE_SIZE - 1u)) != 0u) {
        fprintf(stderr, "FAIL: boot_flash_write_page 收到非法地址 0x%08x\n", address);
        return false;
    }

    offset = address - BOOT_APP_START;
    s_page_writes++;
    s_page_write_count[offset / BOOT_FLASH_PAGE_SIZE]++;

    flash_program_bits(offset, data, BOOT_FLASH_PAGE_SIZE);
    s_last_write_tick = g_host_ticks;

    /* 和真机一样回读校验 */
    return memcmp(&s_flash[offset], data, BOOT_FLASH_PAGE_SIZE) == 0;
}

bool boot_flash_invalidate_app(void) {
    memset(&s_flash[0], 0xFF, BOOT_FLASH_PAGE_SIZE);
    s_last_write_tick = 0xFFFFFFFFu;
    return true;
}

void boot_flash_init(void) {
    s_last_write_tick = 0xFFFFFFFFu;
}

uint32_t boot_flash_last_write_tick(void) {
    return s_last_write_tick;
}

bool boot_flash_page_matches(uint32_t address, const uint8_t *data) {
    uint32_t offset = address - BOOT_APP_START;

    if (address < BOOT_APP_START || address >= BOOT_APP_END) {
        return false;
    }
    return memcmp(&s_flash[offset], data, BOOT_FLASH_PAGE_SIZE) == 0;
}

/* 上游 bootuf2.c 里那份不用的写路径仍引用这两个符号（见 src/boot_flash.c） */
void boot2uf2_flash_init(void) {
}

int bootuf2_flash_write(uint32_t address, const uint8_t *data, size_t size) {
    (void)address;
    (void)data;
    (void)size;
    return -1;
}

/* 与固件里 boot_app_is_valid() 等价的判定（固件版本读的是绝对地址） */
static bool model_app_is_valid(void) {
    uint32_t first_insn = (uint32_t)s_flash[0] | ((uint32_t)s_flash[1] << 8) |
                          ((uint32_t)s_flash[2] << 16) | ((uint32_t)s_flash[3] << 24);
    uint32_t vector0 = (uint32_t)s_flash[4] | ((uint32_t)s_flash[5] << 8) |
                       ((uint32_t)s_flash[6] << 16) | ((uint32_t)s_flash[7] << 24);

    if (first_insn == 0xFFFFFFFFu || first_insn == 0x00000000u) {
        return false;
    }
    if (vector0 < BOOT_APP_START || vector0 >= BOOT_APP_END) {
        return false;
    }
    return (vector0 & 1u) == 0u;
}

/* ------------------------------------------------------------------------- */
/* 工具                                                                      */
/* ------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long size;

    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        free(buf);
        return NULL;
    }
    fclose(fp);
    *out_size = (size_t)size;
    return buf;
}

/* 逐个 512 B 块喂给 MSC 写回调（等价于主机逐扇区写文件） */
static void feed_uf2(const uint8_t *uf2, size_t size, size_t skip_last_blocks) {
    size_t blocks = size / 512u;

    if (skip_last_blocks >= blocks) {
        blocks = 0u;
    } else {
        blocks -= skip_last_blocks;
    }

    for (size_t i = 0; i < blocks; ++i) {
        g_host_ticks += 48000u; /* 每块之间过 1 ms */
        boot_uf2_write_sector((uint32_t)(600u + i), &uf2[i * 512u], 1u);
    }
}

/* 块内偏移：NumberOfBlock=16，BlockIndex=20，TargetAddress=12，FamilyID=28 */
static void patch_u32(uint8_t *uf2, size_t block, size_t offset, uint32_t value) {
    memcpy(&uf2[block * 512u + offset], &value, 4u);
}

static bool flash_matches_bin(const uint8_t *bin, size_t bin_size) {
    if (memcmp(s_flash, bin, bin_size) != 0) {
        for (size_t i = 0; i < bin_size; ++i) {
            if (s_flash[i] != bin[i]) {
                fprintf(stderr, "  differ at 0x%zx: flash=0x%02x expect=0x%02x\n",
                        i, s_flash[i], bin[i]);
                break;
            }
        }
        return false;
    }
    {
        size_t padded = (bin_size + 255u) & ~(size_t)255u;

        for (size_t i = bin_size; i < padded; ++i) {
            if (s_flash[i] != 0x00) {
                fprintf(stderr, "  tail padding at 0x%zx is 0x%02x, expect 0x00\n",
                        i, s_flash[i]);
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    uint8_t *uf2;
    uint8_t *bin;
    size_t uf2_size = 0;
    size_t bin_size = 0;
    size_t blocks;
    uint8_t *work;
    int rc = 1;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <firmware.uf2> <firmware.bin>\n", argv[0]);
        return 2;
    }

    uf2 = read_file(argv[1], &uf2_size);
    bin = read_file(argv[2], &bin_size);
    if (uf2 == NULL || bin == NULL) {
        goto out;
    }
    if (uf2_size % 512u != 0u || bin_size > BOOT_APP_SIZE) {
        fprintf(stderr, "FAIL: 输入文件大小不对（uf2=%zu, bin=%zu）\n", uf2_size, bin_size);
        goto out;
    }
    blocks = uf2_size / 512u;
    printf("用例输入：%zu 块 UF2，%zu B bin\n", blocks, bin_size);

    /* ---- 1. 正常路径 ---- */
    flash_model_reset();
    boot_uf2_init();
    feed_uf2(uf2, uf2_size, 0u);
    if (!boot_uf2_is_done()) {
        fprintf(stderr, "FAIL[1]: 没有报告完成（written=%u total=%u error=%u）\n",
                boot_uf2_written_blocks(), boot_uf2_total_blocks(), boot_uf2_error());
        goto out;
    }
    if (!flash_matches_bin(bin, bin_size)) {
        fprintf(stderr, "FAIL[1]: 落盘内容与 bin 不一致\n");
        goto out;
    }
    if (!model_app_is_valid()) {
        fprintf(stderr, "FAIL[1]: 完成后应用入口页无效\n");
        goto out;
    }
    for (size_t i = 0; i < BOOT_APP_SIZE / BOOT_FLASH_PAGE_SIZE; ++i) {
        if (s_page_write_count[i] > 1u) {
            fprintf(stderr, "FAIL[1]: 第 %zu 页被写了 %u 次\n", i, s_page_write_count[i]);
            goto out;
        }
    }
    printf("OK[1]: 完成，内容与 bin 一致，%u 页各写 1 次\n", s_page_writes);

    /* ---- 2. 错误 family ---- */
    work = malloc(uf2_size);
    if (work == NULL) {
        goto out;
    }
    memcpy(work, uf2, uf2_size);
    for (size_t i = 0; i < blocks; ++i) {
        patch_u32(work, i, 28u, BOOT_UF2_FAMILY_ID ^ 0xFFu);
    }
    flash_model_reset();
    boot_uf2_init();
    feed_uf2(work, uf2_size, 0u);
    if (s_page_writes != 0u || boot_uf2_is_done()) {
        fprintf(stderr, "FAIL[2]: 错误 family 被接受（writes=%u done=%d）\n",
                s_page_writes, (int)boot_uf2_is_done());
        free(work);
        goto out;
    }
    printf("OK[2]: 错误 family 被忽略，一页都没写\n");

    /* ---- 3. 越界块号（会让上游那份实现越界写位图） ---- */
    memcpy(work, uf2, uf2_size);
    patch_u32(work, 1u, 20u, (uint32_t)(blocks + 10u)); /* BlockIndex = 越界值 */
    flash_model_reset();
    boot_uf2_init();
    feed_uf2(work, uf2_size, 0u);
    if (boot_uf2_is_done()) {
        fprintf(stderr, "FAIL[3]: 越界块号的 UF2 竟然报告完成\n");
        free(work);
        goto out;
    }
    printf("OK[3]: 越界块号被忽略（written=%u/%u），不报告完成\n",
           boot_uf2_written_blocks(), boot_uf2_total_blocks());

    /* ---- 4. 重复块（主机重发 WRITE 命令） ---- */
    flash_model_reset();
    boot_uf2_init();
    feed_uf2(uf2, uf2_size, 0u);
    feed_uf2(uf2, uf2_size, 0u); /* 整份再来一遍 */
    if (!boot_uf2_is_done() || !flash_matches_bin(bin, bin_size)) {
        fprintf(stderr, "FAIL[4]: 重复下发同一份文件后结果不对\n");
        free(work);
        goto out;
    }
    for (size_t i = 0; i < BOOT_APP_SIZE / BOOT_FLASH_PAGE_SIZE; ++i) {
        if (s_page_write_count[i] > 1u) {
            fprintf(stderr, "FAIL[4]: 第 %zu 页被写了 %u 次\n", i, s_page_write_count[i]);
            free(work);
            goto out;
        }
    }
    printf("OK[4]: 重复下发被正确去重，仍然完成\n");

    /* ---- 5. 中断的拷贝 ---- */
    flash_model_reset();
    boot_uf2_init();
    feed_uf2(uf2, uf2_size, 1u); /* 丢掉最后一块 */
    if (boot_uf2_is_done()) {
        fprintf(stderr, "FAIL[5]: 少一块竟然报告完成\n");
        free(work);
        goto out;
    }
    if (model_app_is_valid()) {
        fprintf(stderr, "FAIL[5]: 中断后应用入口页仍然有效（下次启动会跳进半截固件）\n");
        free(work);
        goto out;
    }
    printf("OK[5]: 中断的拷贝不会让设备跳进半截固件（written=%u/%u）\n",
           boot_uf2_written_blocks(), boot_uf2_total_blocks());

    free(work);
    printf("全部通过。\n");
    rc = 0;

out:
    free(uf2);
    free(bin);
    return rc;
}
