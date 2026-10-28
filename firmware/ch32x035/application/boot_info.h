#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 应用固件 <-> UF2 bootloader 的「一次启动」握手区。
 *
 * ============================ 为什么需要这块 RAM ============================
 * 应用需要请 bootloader「这次复位后别跳回来」：写完 vendor command 的
 * ENTER_UF2 之后软复位，bootloader 读到这条请求就留在 UF2 模式等主机拖文件。
 *
 * RAM 的内容在软复位（NVIC_SystemReset）后保留，所以用它传递最省事：不用擦写
 * Flash（选项字扇区一擦就是整页，还带着读保护位），也不用额外的寄存器。
 *
 * 方向是单向的：**应用写、bootloader 读**（读走即清）。调试器模式的切换不再
 * 经过这里 —— 那是运行期双击 PB3 的事，不需要跨复位传递。
 *
 * ============================ 地址与保护 ============================
 * 约定放在 RAM 起始的 16 字节（0x20000000..0x2000000F）。
 * **两份链接脚本都必须把 RAM 区域的 ORIGIN 后移 16 字节**（见
 * sdk/Ld/Link_highcode_nv_256B*.ld 与 bootloader/ld/Link_bootloader.ld 的
 * `RAM (xrw) : ORIGIN = 0x20000010`），否则 .highcode/.data 会直接盖住它。
 *
 * 上电瞬间 RAM 内容是随机的，所以用「magic + 校验字」双重判断：
 * 只有 magic 正确且 check 与计算值一致才认这个记录，其余一律当作「没有信息」。
 * 读走即清（boot_info_take），避免同一条请求被后续启动重复消费。
 */

#define BOOT_INFO_ADDR  ((uint32_t)0x20000000u)
#define BOOT_INFO_MAGIC ((uint32_t)0x42324455u) /* "UD2B" */
#define BOOT_INFO_CHECK_XOR ((uint32_t)0x5AA5C33Cu)

/*
 * 本构建是否带 UF2 bootloader（xmake 的 bootloader 选项 -> -DUDAP_UF2_BOOTLOADER=1）。
 *
 * 不带时这块 RAM 并没有被链接脚本保留（RAM 从 0x20000000 开始分配），写进去会
 * 直接破坏 .highcode，所以所有访问都必须编译成空操作。
 * 此时应用的行为与加 bootloader 之前完全一致：上电按住 PB3 由应用自己采样。
 */
#ifndef UDAP_UF2_BOOTLOADER
#define UDAP_UF2_BOOTLOADER 0
#endif

/* 请求原因（boot_info_t.reason）
 *
 *   NONE          无记录
 *   REQUEST_UF2   应用请求：这次复位后停在 UF2 bootloader
 *   APP_STAGE     应用自检路标：arg = 阶段号（见 src/main.c 的 boot_stage 调用）。
 *                 bootloader 下次启动时把它打出来 —— 应用反复重启时，这能直接
 *                 指出它死在哪一步（连不上串口也一样有用）。
 *   APP_PANIC     应用异常：arg 的高 8 位 = mcause，低 24 位 = mepc。
 *                 这样一条 16 字节记录就能说清「崩在哪条指令、什么原因」。
 */
typedef enum {
    BOOT_INFO_REASON_NONE = 0,        /* 无请求（正常启动） */
    BOOT_INFO_REASON_REQUEST_UF2 = 1, /* 应用请求：留在 UF2 bootloader */
    BOOT_INFO_REASON_APP_STAGE = 2,   /* 应用路标（arg = 阶段号） */
    BOOT_INFO_REASON_APP_PANIC = 3,   /* 应用异常（arg = mcause<<24 | mepc） */
} boot_info_reason_t;

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t reason; /* boot_info_reason_t */
    volatile uint32_t arg;    /* 备用参数（当前一律写 0） */
    volatile uint32_t check;  /* magic ^ reason ^ arg ^ BOOT_INFO_CHECK_XOR */
} boot_info_t;

#if UDAP_UF2_BOOTLOADER

static inline boot_info_t *boot_info_mem(void) {
    return (boot_info_t *)BOOT_INFO_ADDR;
}

static inline uint32_t boot_info_checksum(uint32_t reason, uint32_t arg) {
    return BOOT_INFO_MAGIC ^ reason ^ arg ^ BOOT_INFO_CHECK_XOR;
}

/* 写入一条记录（覆盖旧记录） */
static inline void boot_info_write(uint32_t reason, uint32_t arg) {
    boot_info_t *info = boot_info_mem();

    /* check 最后写：它能保证别人只看到「已经写完整」的记录 */
    info->magic = 0u;
    info->reason = reason;
    info->arg = arg;
    info->check = boot_info_checksum(reason, arg);
    info->magic = BOOT_INFO_MAGIC;
}

static inline void boot_info_clear(void) {
    boot_info_mem()->magic = 0u;
}

/* 应用自检路标：bootloader 下次启动会打印「上次应用停在 stage N」。
 * 应用反复重启时，这能直接指出它死在哪一步（不需要串口接到应用那侧）。 */
static inline void boot_info_stage(uint32_t stage) {
    boot_info_write(BOOT_INFO_REASON_APP_STAGE, stage);
}

/* 应用异常记录：高 8 位 mcause、低 24 位 mepc（我们的地址空间 0x5000..0xF7FF
 * 完全放得下）。配合 ELF 就能还原到具体函数/行。 */
static inline void boot_info_panic(uint32_t mepc, uint32_t mcause) {
    boot_info_write(BOOT_INFO_REASON_APP_PANIC,
                    ((mcause & 0xFFu) << 24) | (mepc & 0x00FFFFFFu));
}

/* 读取并消费一条记录；没有有效记录时返回 false（*out_reason 不被修改） */
static inline bool boot_info_take(uint32_t *out_reason, uint32_t *out_arg) {
    boot_info_t *info = boot_info_mem();
    uint32_t magic = info->magic;
    uint32_t reason;
    uint32_t arg;
    uint32_t check;

    if (magic != BOOT_INFO_MAGIC) {
        return false;
    }

    reason = info->reason;
    arg = info->arg;
    check = info->check;

    /* 读完立刻清 magic：这条记录只属于这一次启动 */
    info->magic = 0u;

    if (check != boot_info_checksum(reason, arg)) {
        return false;
    }

    if (out_reason != 0) {
        *out_reason = reason;
    }
    if (out_arg != 0) {
        *out_arg = arg;
    }
    return true;
}

#else /* !UDAP_UF2_BOOTLOADER：没有 bootloader，也没有保留那块 RAM */

static inline void boot_info_write(uint32_t reason, uint32_t arg) {
    (void)reason;
    (void)arg;
}

static inline void boot_info_stage(uint32_t stage) {
    (void)stage;
}

static inline void boot_info_panic(uint32_t mepc, uint32_t mcause) {
    (void)mepc;
    (void)mcause;
}

static inline void boot_info_clear(void) {
}

static inline bool boot_info_take(uint32_t *out_reason, uint32_t *out_arg) {
    (void)out_reason;
    (void)out_arg;
    return false;
}

#endif /* UDAP_UF2_BOOTLOADER */
