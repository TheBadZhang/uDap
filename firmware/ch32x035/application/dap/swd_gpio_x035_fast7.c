#include "swd_gpio_fast7.h"

#include "mode_overlay.h"
#include "swd_pin_map.h"

/*
 * SWD 位时序的**低速版实现**：每比特 7 个 SWCLK（现役 swd_gpio_x035.c 是 8 个）。
 *
 * 这套实现目前**没有被任何调用点使用**（见 swd_gpio_fast7.h 的宏），
 * 保留它是为了让「每比特少一个 slot」这件事可以直接上板对比。
 *
 * ---------------------------------------------------------------------------
 * 为什么是 7 而不是 6：槽位预算
 * ---------------------------------------------------------------------------
 * 现役实现的文件头总结了实测规律（CH32X035，48 MHz，BSHR/BCR 直接写）：
 *
 *     低电平 = N_low  + 2 个 clk
 *     高电平 = N_high + 1 个 clk
 *     周期   = N_low + N_high + 3
 *
 * 其中 N_low / N_high 是「两条边沿 store 之间 / 之后」的**指令条数（slot）**。
 * 现役数据相位 N_low=2、N_high=3（2 ALU + 1 nop）= 4 个 slot → 7+1 = 8 clk。
 *
 * 每比特的固定工作量是 4 条指令，任何分配方式都省不掉：
 *   写：取位（slli + andi 2 条） + 选通并合成 BSHR（srl + or 2 条）
 *   读：采样（lw） + 取出 DIO（andi） + 归位（srli/slli） + 累加（or）
 *   请求：取位（2 条） + 选通合成（2 条）
 * 所以周期 = 4 + 3 = **7 clk**，而不是 6。
 *
 * 要做到 6 clk 必须把每比特压到 3 个 slot，只有这两条路（都不在本次实现里）：
 *   1. 把 SWDIO 的更新挪到「上升沿之后」（省掉选通那一条）—— 违反目标端在
 *      上升沿的保持时间要求，长线/慢目标上会随机读错；
 *   2. 改编码：例如用 BCR 单独清 SWCLK、BSHR 只写 SWDIO，让选通不必再 or
 *      时钟位 —— 少一条 or，但多一条独立的 store，slot 数不变。
 * 换句话说 6 clk 在这颗芯片的 GPIO 写模型下拿不到；7 clk（≈6.9 MHz）是上限。
 *
 * ⚠️ 上板前请用示波器确认 `+3` 这个常数：它是用「两条边沿都写 BSHR」的写法
 * 标定的，而读路径的下降沿用 BCR（现役实现也是），边缘退休特性可能差 1 个 clk。
 * 若实测低/高电平不是 4/3，按偏差±1 调整对应相位的 nop 即可（每相位只加一个
 * nop 会同时改变周期，所以宁可先测再改）。
 *
 * 与现役实现相同的约定：
 *   - 符号带引脚方案后缀（SWD_GPIO_IMPL），换板只改 swd_pin_map.h；
 *   - 32 个 cell 的指令条数必须完全一致（任何一位少一条都会让那一位变窄）；
 *   - 这里只写寄存器顺序，DIO 方向切换仍由 sw_dp.c 调 board_swd_dio_* 完成。
 */

#define SWD_ASM_(x) #x
#define SWD_ASM(x) SWD_ASM_(x)

/* 与 swd_gpio_x035.c 相同：把 C 的引脚编号交给 GAS，立即数由 .set 推导 */
#define SWD7_ASM_PIN_CONSTANTS                          \
    ".set SWD_CLK_BIT, " SWD_ASM(SWD_CLK_BIT) "\n\t"    \
    ".set SWD_DIO_BIT, " SWD_ASM(SWD_DIO_BIT) "\n\t"    \
    ".set SWD_CLK_SET, (1 << SWD_CLK_BIT)\n\t"          \
    ".set SWD_CLK_CLR, (1 << (SWD_CLK_BIT + 16))\n\t"   \
    ".set SWD_DIO_SET, (1 << SWD_DIO_BIT)\n\t"          \
    ".set SWD_DIO_CLR, (1 << (SWD_DIO_BIT + 16))\n\t"

/* ===========================================================================
 * Packet Request —— 7 拍（start / APnDP / RnW / A2 / A3 / parity / stop）
 * 每拍 7 clk：低 4 / 高 3（N_low = 2，N_high = 2）
 * =========================================================================== */
DAP_HIGHCODE_SECTION SWD_GPIO_NOINLINE void SWD_GPIO_IMPL(swd7_fast_packet_request)(uint32_t request) {
    volatile uint32_t *const pa = &SWD_PORT->BSHR;

    __asm__ volatile(
        SWD7_ASM_PIN_CONSTANTS
        /* === 准备 === */
        "lui  t2, (SWD_DIO_CLR >> 12)\n\t" /* t2 = SWDIO 复位半字 */
        "lui  t3, (SWD_CLK_CLR >> 12)\n\t" /* t3 = SWCLK 复位半字 */
        "li   t4, SWD_CLK_SET\n\t"         /* t4 = 上升沿写 BSHR 的值 */
        /* 四位奇偶：t5 = r0^r1^r2^r3，再换成 srl 要的 0/16 移位量 */
        "srli t5, %[req], 2\n\t"
        "xor  t5, t5, %[req]\n\t"
        "srli t1, t5, 1\n\t"
        "xor  t5, t5, t1\n\t"
        "andi t5, t5, 1\n\t"
        "slli t5, t5, 4\n\t"
        /* start 位（恒 1）的下降沿值：SWDIO 置位 | SWCLK 复位 */
        "srli t0, t2, 16\n\t"              /* SWDIO 置位半字 */
        "or   t0, t0, t3\n\t"
        /* === start：低电平期算 APnDP = request[0] === */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿 */
        "slli t0, %[req], 4\n\t"           /* ┐ shift = req[0] << 4 */
        "andi t0, t0, 16\n\t"              /* ┘ */
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿 */
        "srl  t0, t2, t0\n\t"              /* 选置位/复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成 APnDP 拍的下降沿值 */
        /* === APnDP：算 RnW = request[1] === */
        "sw   t0, 0(%[pa])\n\t"
        "slli t0, %[req], 3\n\t"
        "andi t0, t0, 16\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        /* === RnW：算 A2 = request[2] === */
        "sw   t0, 0(%[pa])\n\t"
        "slli t0, %[req], 2\n\t"
        "andi t0, t0, 16\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        /* === A2：算 A3 = request[3] === */
        "sw   t0, 0(%[pa])\n\t"
        "slli t0, %[req], 1\n\t"
        "andi t0, t0, 16\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        /* === A3：算 parity（t5 已是 0/16）=== */
        "sw   t0, 0(%[pa])\n\t"
        "srl  t0, t2, t5\n\t"
        "nop\n\t"                          /* 低电平期补第 2 个 slot */
        "sw   t4, 0(%[pa])\n\t"
        "or   t0, t0, t3\n\t"
        "nop\n\t"                          /* 高电平期第 2 个 slot */
        /* === parity：算 stop（恒 0）=== */
        "sw   t0, 0(%[pa])\n\t"
        "lui  t0, ((SWD_DIO_CLR | SWD_CLK_CLR) >> 12)\n\t"
        "nop\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "nop\n\t"
        "nop\n\t"
        /* === stop：没有下一位要算，用 nop 维持 7 clk === */
        "sw   t0, 0(%[pa])\n\t"
        "nop\n\t"
        "nop\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "nop\n\t"
        "nop\n\t"
        :
        : [req] "r"(request), [pa] "r"(pa)
        : "t0", "t1", "t2", "t3", "t4", "t5", "memory");
}

/* ===========================================================================
 * 读数据相位（32 位）—— 每比特 7 clk：低 4 / 高 3（N_low = 2，N_high = 2）
 *
 * 与现役实现的差别：
 *   1. 低电平期在 BCR 上清 SWCLK，采样 INDR；高电平期把该位放到 val 的第 k 位
 *      再 or 进去 —— 与现役相同，只是少了高电平期那个 nop；
 *   2. **奇偶位不再边收边算**：现役把高电平期的 nop 换成 xor，把 32 次异或摊在
 *      位循环里；这里高电平期只剩 2 个 slot，放不下，改成循环后用
 *      SWD_Parity32 折叠一次（5 条指令）。对 7 clk 的时序没有影响。
 * =========================================================================== */
DAP_HIGHCODE_SECTION SWD_GPIO_NOINLINE void SWD_GPIO_IMPL(swd7_fast_read_data)(uint32_t *value, uint32_t *parity) {
    uint32_t val = 0U;
    uint32_t par;
    volatile uint32_t *const pa = &SWD_PORT->BSHR;

    __asm__ volatile(
        SWD7_ASM_PIN_CONSTANTS
        "li   t1, SWD_CLK_SET\n\t"        /* 同一个常量既写 BCR（清 CLK）也写 BSHR（置 CLK）*/
        ".set r7_k, 0\n\t"
        ".rept 32\n\t"
        "sw   t1, 4(%[pa])\n\t"           /* 下降沿（BCR 清 SWCLK）*/
        "lw   t0, -8(%[pa])\n\t"          /* 采样 INDR（INDR 在 BSHR - 8 处）*/
        "andi t0, t0, SWD_DIO_SET\n\t"    /* 取出 SWDIO 位（留在原位）*/
        "sw   t1, 0(%[pa])\n\t"           /* 上升沿（BSHR 置 SWCLK）*/
        ".if r7_k < SWD_DIO_BIT\n\t"      /* ┐ 把该位挪到 val 的第 r7_k 位 */
        "srli t0, t0, (SWD_DIO_BIT - r7_k)\n\t"
        ".elseif r7_k == SWD_DIO_BIT\n\t"
        "nop\n\t"                         /* │ 正好在原位 */
        ".else\n\t"
        "slli t0, t0, (r7_k - SWD_DIO_BIT)\n\t"
        ".endif\n\t"                      /* ┘ */
        "or   %[val], %[val], t0\n\t"     /* 收进 val（32 位互不重叠，无写后写气泡）*/
        ".set r7_k, r7_k + 1\n\t"
        ".endr\n\t"
        : [val] "+r"(val)
        : [pa] "r"(pa)
        : "t0", "t1", "memory");

    /* 奇偶位：与 sw_dp.c 的 SWD_Parity32 相同（32 位折 5 次）*/
    par = val;
    par ^= par >> 16;
    par ^= par >> 8;
    par ^= par >> 4;
    par ^= par >> 2;
    par ^= par >> 1;

    *value = val;
    *parity = par & 1U;
}

/* ===========================================================================
 * 写数据相位（32 位）—— 每比特 7 clk：低 4 / 高 3（N_low = 2，N_high = 2）
 *
 * 与现役实现的唯一差别：高电平期的第 3 个 slot（那个纯占空比 nop）被去掉。
 * 每拍仍是「本拍的值在上一拍高电平期算好」，所以低电平期只有取位 2 条指令。
 * =========================================================================== */
DAP_HIGHCODE_SECTION SWD_GPIO_NOINLINE void SWD_GPIO_IMPL(swd7_fast_write_data)(uint32_t value) {
    volatile uint32_t *const pa = &SWD_PORT->BSHR;

    __asm__ volatile(
        SWD7_ASM_PIN_CONSTANTS
        /* === 准备：第 0 位的 BSHR 值 === */
        "slli t0, %[val], 4\n\t"           /* ┐ shift = (val & 1) << 4 */
        "andi t0, t0, 16\n\t"              /* ┘ */
        "lui  t2, (SWD_DIO_CLR >> 12)\n\t" /* t2 = SWDIO 复位半字 */
        "srl  t0, t2, t0\n\t"              /* 选置位 / 复位半字 */
        "lui  t3, (SWD_CLK_CLR >> 12)\n\t" /* t3 = SWCLK 复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成下降沿要写的值 */
        "li   t4, SWD_CLK_SET\n\t"         /* t4 = 上升沿写 BSHR 的值 */
        ".set w7_k, 0\n\t"
        ".rept 32\n\t"
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿：清 SWCLK + 写 SWDIO */
        ".if (w7_k + 1) < 4\n\t"           /* ┐ 低电平期 2 个 slot：取**下一位**的 shift */
        "slli t0, %[val], (3 - w7_k)\n\t"  /* │ */
        "andi t0, t0, 16\n\t"              /* │ */
        ".elseif (w7_k + 1) == 4\n\t"      /* │ 这一位正好是 & 16 的结果，省掉 slli */
        "andi t0, %[val], 16\n\t"          /* │ 用 nop 把 slot 补齐，否则这一位会变窄 */
        "nop\n\t"                          /* │ */
        ".else\n\t"                        /* │ */
        "srli t0, %[val], (w7_k - 3)\n\t"  /* │ */
        "andi t0, t0, 16\n\t"              /* ┘ */
        ".endif\n\t"
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿 */
        "srl  t0, t2, t0\n\t"              /* 高电平期 2 个 slot：合成下一位的值 */
        "or   t0, t0, t3\n\t"
        ".set w7_k, w7_k + 1\n\t"
        ".endr\n\t"
        :
        : [val] "r"(value), [pa] "r"(pa)
        : "t0", "t2", "t3", "t4", "memory");
}
