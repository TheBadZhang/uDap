#include "DAP_config.h"
#include <stdint.h>
#include "mode_overlay.h"

#include <DAP.h>

// 把快路径的半时钟等待换成带余量的实现（见 DAP_config.h 的说明）。
// DAP.h 里 PIN_DELAY_FAST 是函数定义而非宏，而本文件在 include 之后按名字把
// 它重定义成宏，就能覆盖本文件内 48 处调用点，不需要逐处修改；
// DAP.h 的原函数仍存在，只是不再被本文件使用。
// 必须在 DAP.h 之后定义：否则会先取代掉 DAP.h 里的函数定义本身。
#define PIN_DELAY_FAST() __NOP()

// 慢路径的单次位延时也换成紧凑实现（见 DAP_config.h 的说明）。
// DAP.h 里 PIN_DELAY_SLOW 是函数定义而非宏，按名字重定义成宏即可覆盖本文件内
// 全部调用点（SWD_TransferSlow 与 SWD_Sequence 的 SW_CLOCK_CYCLE/SW_WRITE_BIT/
// SW_READ_BIT）,不需要逐处修改；DAP.h 的原函数仍存在，只是不再被本文件使用。
// 注意本文件里的 `#define PIN_DELAY() PIN_DELAY_SLOW(...)` 在这个宏之后展开，
// 所以也会自动走到这里。
#define PIN_DELAY_SLOW(delay) x035_pin_delay_slow(delay)

// SW Macros
// SWD 两个引脚（SWCLK=PA2、SWDIO=PA3）同属 GPIOA，合并写 BSHR 时用这个别名。
#define PIN_SWD_PORT GPIOA
#define PIN_SWCLK GPIO_Pin_2
#define PIN_SWCLK_SHIFT 2
#define PIN_SWDIO GPIO_Pin_3
#define PIN_SWDIO_SHIFT 3

// 上升沿 / 下降沿之后各补一段可调等待，用来修 SWCLK 占空比（见 DAP_config.h）。
// 这两个宏在本文件里只以 PIN_SWCLK_SET() / PIN_SWCLK_CLR() 形式使用，所以直接
// 重定义即可覆盖全部调用点，不必逐处修改；后面的 PIN_DELAY_FAST() 仍会执行。
#define PIN_SWCLK_SET() { PIN_SWCLK_TCK_SET(); __NOP(); }
#define PIN_SWCLK_CLR() { PIN_SWCLK_TCK_CLR(); __NOP(); }
// #define PIN_SWDIO_SET() {}
// #define PIN_SWDIO_CLR() {}

#define PIN_SWDIO_OUTPUT() { PIN_SWD_PORT->CFGLR = PIN_SWD_PORT->CFGLR & ~(0xFU << 12) | (0x1U << 12); }
#define PIN_SWDIO_INPUT() { PIN_SWD_PORT->CFGLR = PIN_SWD_PORT->CFGLR & ~(0xFU << 12) | (0x4U << 12); }


// 在 BSHR 的置位与复位半字间选择，避免每个数据位都执行条件分支
#define PIN_SWDIO_OUT(bit) {GPIOA->BSHR = (GPIO_Pin_3 << 16) >> ((bit & 1U) << 4);}

#define PIN_SWCLK_CLR_SWDIO_CLR() { PIN_SWD_PORT->BSHR = (PIN_SWCLK << 16) | (PIN_SWDIO << 16); }
#define PIN_SWCLK_CLR_SWDIO_SET() { PIN_SWD_PORT->BSHR = (PIN_SWDIO | PIN_SWCLK << 16); }
// 设置 SWDIO 数据，并将 SWCLK 原子拉低，为下一个 SWD 数据位准备
#define PIN_SWCLK_CLR_SWDIO_OUT(bit) {GPIOA->BSHR = (GPIO_Pin_2 << 16) | (GPIO_Pin_3 << 16) >> ((bit & 1U) << 4);}

// 读路径专用的时钟宏。
// 读路径的采样（读 INDR）与位打包都落在**低电平期**，低电平期本来就长，
// 再叠加写路径的 low_extra 会过度失衡（实测读循环 低:高 ≈ 7:2）。
// 故读采样处改用这一组，让高电平期补足（见 DAP_config.h 的说明）。
#define PIN_SWCLK_CLR_READ() do { PIN_SWCLK_TCK_CLR(); } while (0)
#define PIN_SWCLK_SET_READ() do { PIN_SWCLK_TCK_SET(); __NOP();__NOP();__NOP(); } while (0)

#define SW_CLOCK_CYCLE() \
    PIN_SWCLK_CLR();     \
    PIN_DELAY();         \
    PIN_SWCLK_SET();     \
    PIN_DELAY()

#define SW_READ_BIT(bit)  \
    PIN_SWCLK_CLR();      \
    PIN_DELAY();          \
    bit = PIN_SWDIO_IN(); \
    PIN_SWCLK_SET();      \
    PIN_DELAY()

#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)

__STATIC_FORCEINLINE uint32_t SWD_Parity32(uint32_t value) {
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return value & 1U;
}

// 读数据相位（32 位）已改成手写汇编，见 SWD_TransferFast 里的读段落 ——
// 排布与 SWD_FastWriteData 完全同构（每 bit 8 个 clk、低 4 / 高 4）。
// PIN_SWCLK_CLR_READ / PIN_SWCLK_SET_READ 仍被「读奇偶位」那一拍使用，保留。


#if ((DAP_SWD != 0) || (DAP_JTAG != 0))
// Generate SWJ Sequence
//   count:  sequence bit count
//   data:   pointer to sequence bit data
//   return: none
DAP_HIGHCODE_SECTION void SWJ_Sequence(uint32_t count, const uint8_t *data) {
    uint32_t val;
    uint32_t n;

    val = 0U;
    n = 0U;
    while (count--) {
        if (n == 0U) {
            val = *data++;
            n = 8U;
        }
        if (val & 1U) {
            PIN_SWDIO_TMS_SET();
        } else {
            PIN_SWDIO_TMS_CLR();
        }
        SW_CLOCK_CYCLE();
        val >>= 1;
        n--;
    }
}
#endif

#if (DAP_SWD != 0)
// Generate SWD Sequence
//   info:   sequence information
//   swdo:   pointer to SWDIO generated data
//   swdi:   pointer to SWDIO captured data
//   return: none
void SWD_Sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi) {
    uint32_t val;
    uint32_t bit;
    uint32_t n, k;

    n = info & SWD_SEQUENCE_CLK;
    if (n == 0U) {
        n = 64U;
    }

    if (info & SWD_SEQUENCE_DIN) {
        while (n) {
            val = 0U;
            for (k = 8U; k && n; k--, n--) {
                SW_READ_BIT(bit);
                val >>= 1;
                val |= bit << 7;
            }
            val >>= k;
            *swdi++ = (uint8_t)val;
        }
    } else {
        while (n) {
            val = *swdo++;
            for (k = 8U; k && n; k--, n--) {
                PIN_SWCLK_CLR_SWDIO_OUT(val);
                PIN_DELAY();
                PIN_SWCLK_SET();
                PIN_DELAY();
                val >>= 1;
            }
        }
    }
}
#endif

#if (DAP_SWD != 0)
// SWD Transfer I/O Fast
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
DAP_HIGHCODE_SECTION static uint8_t SWD_TransferFast(
    uint32_t request, uint32_t *data) {
    uint32_t ack;
    uint32_t bit;
    uint32_t val;
    uint32_t parity;

    uint32_t n;

    /* Packet Request —— 8 个 SWCLK（start/APnDP/RnW/A2/A3/parity/stop）
     *
     * GCC 永远把
     * 「算下一位 BSHR 值」的 ALU 提到下降沿那条 store **之前**（它总是尽早算值
     * 来隐藏延迟），于是低电平期只剩 1 条 store（~20 ns）、高电平期塞进 5 条 ALU
     * （~140 ns），占空比 1:7。试过 __attribute__((optimize("O2",
     * "no-schedule-insns","no-schedule-insns2")))，没有任何变化 —— 重排来自
     * GCC「算值尽早」的通用策略，不是调度器 pass。所以只能用汇编钉住顺序。
     *
     * 每 bit 7 条指令：
     *   ① sw t0 : 清 SWCLK + 按位设/清 SWDIO  （下降沿；t0 上一拍已算好）
     *   -- 低电平期：2 条 ALU（slli + andi）--
     *   ② sw t4 : 置 SWCLK                     （上升沿，t4 = 4）
     *   -- 高电平期：2 条 ALU（srl + or）+ 1 个 nop --
     *
     * 为什么是「低 2 条 / 高 3 条」而不是「低 2 / 高 2」，也就是为什么
     * 低电平 N_low 条、高电平 N_high 条时实测是：
     *     低 clk = N_low + 2      高 clk = N_high + 1     周期 = N_low + N_high + 3
     * 因为两条外设 store 的退休延迟不对称：下降沿那条在低电平期**开头**又被
     * 计一次，上升沿那条在高电平期只计一次。两条实测都吻合：
     *   本次 4+0+1nop (N_low=4,N_high=1) -> 低 6 / 高 2（用户实测）✓
     *   数据相位 2+2+1nop (N_low=2,N_high=3) -> 低 4 / 高 4（用户实测「完美」）✓
     * 所以要 4/4 就得 N_low=2、N_high=3 —— 两半的**指令数不相等**，差 1。这就是
     * 之前我按「2+2 各半」算会得到 高 4 / 低 3 的原因：忽略了那条不对称延迟。
     * 总拍数 N_low+N_high=5 固定（4 条 ALU + 1 个 nop），所以重新分配不改变周期。
     *
     * 位序：start=1, APnDP=b0, RnW=b1, A2=b2, A3=b3, parity=四位异或, stop=0。
     * 取位用「按位常量掩码」（slli 4-k 再 andi 16），省掉逐位推进 request；
     * parity 在准备段一次性折叠出来。
     * t2 = 0x00080000（清 SWDIO）  t3 = 0x00040000（清 SWCLK）
     * A3/parity/stop 三拍没有 4 条 ALU 可填，用 nop 补齐到同样的 低2/高3，
     * 这样整帧每一拍都是 8 个 clk，波形完全均匀。
     */
    volatile uint32_t *const pa = &PIN_SWD_PORT->BSHR;
    /* 逐行对照的原 C（下面注释里的「原 C:」都指这段合并前的写法）：
     *
     *   PIN_SWCLK_CLR_SWDIO_OUT(1U);   // start 位 = 1
     *   bit = request >> k;            // k = 0..3 -> APnDP / RnW / A2 / A3
     *   PIN_SWCLK_CLR_SWDIO_OUT(bit);  // BSHR = (SWCLK<<16) | (SWDIO<<16) >> ((bit&1)<<4)
     *   parity += bit;                 // 四位异或（这里提前折成 t5）
     *   PIN_SWCLK_SET();               // 每拍上升沿 = BSHR = PIN_SWCLK (= 4)
     *   PIN_SWCLK_CLR_SWDIO_OUT(parity);
     *   PIN_SWCLK_CLR_SWDIO_OUT(0U);   // stop 位 = 0
     *
     * 每拍排布「低 2 条 / 高 3 条」= 下降沿与上升沿之间 2 条、上升沿之后 3 条，
     * 于是每拍 8 个 clk（低 4 / 高 4）—— 两条外设 store 的退休延迟不对称，
     * 所以两半的**指令数不相等**，推导见文件头。每拍都是「算下一位、当前位已备好」。
     */
    __asm__ volatile(
        /* === 准备：常量 + request 的奇偶，一次算完 === */
        "lui  t2, 0x80\n\t"       /* t2 = PIN_SWDIO << 16 = 0x00080000：SWDIO 的复位半字 */
        "lui  t3, 0x40\n\t"       /* t3 = PIN_SWCLK << 16 = 0x00040000：SWCLK 的复位半字 */
        "li   t4, 4\n\t"          /* t4 = PIN_SWCLK = 4：PIN_SWCLK_SET 写的 BSHR 值 */
        "srli t5, %[req], 2\n\t"  /* ┐ 原 C 是逐拍 parity += bit；这里提前折算： */
        "xor  t5, t5, %[req]\n\t" /* │ t5 = req[3:0] ^ (req[3:0] >> 2)，即 r0^r2 | r1^r3 */
        "srli t1, t5, 1\n\t"      /* │ 再 ^ (自身 >> 1)，两次移位异或覆盖 4 位 */
        "xor  t5, t5, t1\n\t"     /* ┘ 此时 t5[0] = r0^r1^r2^r3 = 四位奇偶 */
        "andi t5, t5, 1\n\t"      /* t5 = 0/1（= 原 C 的 parity & 1U）*/
        "slli t5, t5, 4\n\t"      /* t5 = 0 或 16：换成后面 srl 要的移位量 */
        "srli t0, t2, 16\n\t"     /* ┐ t0 = SWDIO 置位半字 = 0x00000008 */
        "or   t0, t0, t3\n\t"     /* ┘ | 清 SWCLK = 0x00040008，即原 C 的 PIN_SWCLK_CLR_SWDIO_OUT(1U) */
        /* === start 位（恒 1）：低电平期算 APnDP = request[0] === */
        "sw   t0, 0(%[pa])\n\t"   /* 下降沿 + 写 SWDIO：原 C PIN_SWCLK_CLR_SWDIO_OUT(1U) */
        "slli t0, %[req], 4\n\t"  /* ┐ shift = request[0] << 4 = 0 或 16 */
        "andi t0, t0, 16\n\t"     /* ┘（= 原 C 的 (bit & 1U) << 4，bit = request >> 0）*/
        "sw   t4, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"     /* t0 = (SWDIO<<16) >> shift：选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"     /* 合成 APnDP 拍下降沿要写的 BSHR 值 */
        "nop\n\t"                 /* 高电平期第 3 个 slot（占空比）*/
        /* === APnDP 位：低电平期算 RnW = request[1] === */
        "sw   t0, 0(%[pa])\n\t"   /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 0 */
        "slli t0, %[req], 3\n\t"  /* ┐ shift = request[1] << 4（<< 3 再 & 16）*/
        "andi t0, t0, 16\n\t"     /* ┘ */
        "sw   t4, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"     /* 选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"     /* 合成 RnW 拍的值 */
        "nop\n\t"                 /* 高电平期第 3 个 slot */
        /* === RnW 位：低电平期算 A2 = request[2] === */
        "sw   t0, 0(%[pa])\n\t"   /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 1 */
        "slli t0, %[req], 2\n\t"  /* ┐ shift = request[2] << 4 */
        "andi t0, t0, 16\n\t"     /* ┘ */
        "sw   t4, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"     /* 选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"     /* 合成 A2 位的值 */
        /* ⚠️ 这一拍（RnW，算 A2）没有补尾部 nop：高电平期只有 srl/or 2 条，
         * 所以它的周期是 7 个 clk（低 4 / 高 3），比相邻几拍少 1 个。
         * 这是沿用原实现留下的唯一一处不对称（整帧其余各拍都是 8 clk）。*/
        /* === A2 位：低电平期算 A3 = request[3] === */
        "sw   t0, 0(%[pa])\n\t"   /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 2 */
        "slli t0, %[req], 1\n\t"  /* ┐ shift = request[3] << 4 */
        "andi t0, t0, 16\n\t"     /* ┘ */
        "sw   t4, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"     /* 选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"     /* 合成 A3 拍的值 */
        "nop\n\t"                 /* 高电平期第 3 个 slot */
        /* === A3 位：低电平期算 parity（t5 已折好，只需 2 条 ALU）=== */
        "sw   t0, 0(%[pa])\n\t"   /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 3 */
        "srl  t0, t2, t5\n\t"     /* t0 = (SWDIO<<16) >> t5（t5 已是 0/16）—— 省掉 slli/andi */
        "or   t0, t0, t3\n\t"     /* 合成 parity 拍的值 */
        "sw   t4, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET() */
        "nop\n\t"                 /* ┐ 高电平期补齐 3 条 */
        "nop\n\t"                 /* │ */
        "nop\n\t"                 /* ┘ */
        /* === parity 位：低电平期算 stop（恒 0，只需 1 条 ALU）=== */
        "sw   t0, 0(%[pa])\n\t"   /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(parity) */
        "lui  t0, 0xc0\n\t"       /* t0 = 0x000C0000 = 清 SWDIO + 清 SWCLK（数据位 = 0）*/
        "nop\n\t"                 /* 低电平期补齐 2 条 */
        "sw   t4, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET() */
        "nop\n\t"                 /* ┐ 高电平期补齐 3 条 */
        "nop\n\t"                 /* │ */
        "nop\n\t"                 /* ┘ */
        /* === stop 位：没有下一位要算，纯补 nop 维持每拍 8 个 clk === */
        "sw   t0, 0(%[pa])\n\t"   /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(0U) */
        "nop\n\t"                 /* ┐ 低电平期补齐 2 条 */
        "nop\n\t"                 /* ┘ */
        "sw   t4, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET() */
        :
        : [req] "r"(request), [pa] "r"(pa)
        : "t0", "t1", "t2", "t3", "t4", "t5", "memory");
    PIN_SWCLK_CLR_SWDIO_OUT(1U);
    PIN_SWDIO_INPUT();  //  Turnaround
    PIN_SWCLK_SET(); /* Park Bit */

    /* Turnaround */
    PIN_DELAY_FAST();
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    PIN_SWCLK_SET();
    PIN_DELAY_FAST();

    /* Acknowledge response */
#if 0
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    PIN_SWCLK_SET();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_CLR();
    ack = bit << 0;
    PIN_SWCLK_SET();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_CLR();
    ack |= bit << 1;
    PIN_SWCLK_SET();
    bit = PIN_SWDIO_IN();
    ack |= bit << 2;
#else
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();        // 上升沿采样
    PIN_DELAY_FAST();
    PIN_DELAY_FAST();
    ack = bit << 0;
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();        // 上升沿采样
    PIN_DELAY_FAST();
    PIN_DELAY_FAST();
    ack |= bit << 1;
    PIN_SWCLK_CLR();
    bit = PIN_SWDIO_IN();
    ack |= bit << 2;
    PIN_SWCLK_SET();        // 上升沿采样
#endif

    if (ack == DAP_TRANSFER_OK) { /* OK response */
        /* Data transfer */
        if (request & DAP_TRANSFER_RnW) {
            /* Read data —— 32 个数据位（SWD 为 LSB first）
             *
             * 排布与 SWD_FastWriteData 完全同构：每 bit 8 个 SWCLK、低 4 / 高 4，
             * 所以读数据相位的时钟也是 6 MHz、占空比 1:1，与写数据相位一致。
             *
             * 为什么必须用汇编：这段 C 在 GCC 下每个 bit 要 10~11 个 clk ——
             * 采样后还要 srli/andi/or 打包，而 PIN_SWCLK_SET_READ() 自带 3 个
             * nop、外层再加 PIN_DELAY_FAST() 1 个，高电平期被补到 5 个 clk。
             * 手写后每 bit 只要 8 个 clk（读数据相位整体缩短约 2 成）。
             *
             * 每 bit 的排布（与写路径逐条对齐）：
             *   ① sw t1,4(%[pa])  : BCR 清 SWCLK       （下降沿）
             *   ② lw t0,-8(%[pa]) : 采样 INDR          ← 采样点与原实现一致（紧贴下降沿）
             *      andi t0,t0,8   : 取出 PA3（SWDIO）    -- 低电平期 2 条 --
             *   ③ sw t1,0(%[pa])  : BSHR 置 SWCLK      （上升沿）
             *      srli/slli      : 把该位挪到 val 的第 k 位
             *      or %[val]      : 收进 val
             *      nop
             *   -- 高电平期 3 条 --
             *
             * 关键：不再用「每 4 位攒进 packed、再 val>>=4 | packed<<28」那套移位
             * 寄存器，而是每个 bit 直接 or 进 val 的**最终位置** —— 于是没有循环、
             * 没有组间开销、也不需要在 val 上做移位。第 n 个收到的数据位就落在
             * val 的第 n 位：
             *   第 0~3 位（k=0..3）：andi 后 w=0x08，右移 (3-k) -> val[k]
             *   第 4 位起（k>=4）  ：w=0x08，左移 (k-3) -> val[k]（k=4->1 … 31->28）
             * 因为每位移位量都是编译期常量，用 GAS 的 .set + .if 按位生成。
             * 32 位各自 or 进互不重叠的位置，所以 or 之间没有依赖，不存在写后写气泡。
             *
             * 奇偶位顺手算出来：每个 cell 高电平期的那个 nop 换成
             * `xor %[par],%[par],t0` —— 此时 t0 还是 0x08/0（移位前），所以
             * 32 位的异或全部累加到 par 的 **bit 3**，循环后右移 3 位即得奇偶。
             * 于是 `SWD_Parity32(val)` 那 10 条指令（5 次移位+异或折叠）整个省掉，
             * 换成 2 条（srli+andi）。注意 xor 必须排在移位**之前**：移位会把 t0
             * 改成 1<<k，那时异或出来的是 val 本身而不是奇偶。
             *
             * ⚠️ 实测结论，别重复走这条路：**这不提升吞吐**。省下的那 10 条折叠
             * 指令本来就被 GCC 穿插进「读奇偶位」那拍的 3 个 nop 里，基本不影响
             * 周期。实测（pyocd，64 KiB × 10 轮，各跑 3 次，组内重复性 ±0.1%）：
             *     写 254.68 -> 260.91 KiB/s (+2.4%)
             *     读 235.86 -> 233.72 KiB/s (-0.9%)
             * 但**写路径源码一个字没动** —— 差别来自「多了一个贯穿 32 cell 的活跃
             * 寄存器（奇偶累加器）改变了整个函数的寄存器分配」，写循环的生成代码
             * 也跟着变了。所以这是寄存器分配的副产物，不是奇偶吸收本身的收益。
             * 净收益是**代码体积**：SWD_Transfer 少 14 条指令（-24 B）。
             * 想调吞吐必须动「每 bit 的周期数」，动指令数只在能缩短关键路径时才有用。
             *
             * 寄存器：t0 = 采样临时  t1 = 4（PIN_SWCLK）  %[par] = 奇偶累加器
             */
            val = 0U;
            parity = 0U;
            /* 逐行对照的原 C（合并前的循环版）：
             *
             *   for (n = 8U; n; n--) {
             *     PIN_SWCLK_CLR_READ();               // = PIN_SWCLK_TCK_CLR()
             *     sample = PIN_SWD_PORT->INDR;        // 下降沿后紧贴采样
             *     packed  = (sample >> 3) & 1U;       // 取 PA3
             *     PIN_SWCLK_SET_READ();               // = SET + 3 个 nop
             *     ... 同法再收 3 位到 packed 的 bit1..3 ...
             *     val >>= 4;
             *     val |= (packed << 28);              // 每 4 位倒着塞进 val 高位
             *   }
             *   parity = SWD_Parity32(val);           // 这里改成边收边异或
             *
             * 现在每个 cell 直接 or 进 val 的最终位置，没有循环、没有组间开销；
             * 每 cell 7 条指令、低 2 / 高 3 → 8 个 clk（低 4 / 高 4），与写同构。
             */
            __asm__ volatile(
                "li   t1, 4\n\t"          /* t1 = PIN_SWCLK = 4：后面每拍「置 SWCLK」的立即数 */
                ".set rb_k, 0\n\t"        /* rb_k = 当前在收第几位（0..31），GAS 编译期计数 */
                ".rept 32\n\t"            /* 展开 32 位 —— 每 bit 就是一轮（原 C 的 n 循环）*/
                "sw   t1, 4(%[pa])\n\t"   /* 下降沿：原 C PIN_SWCLK_CLR_READ()（BCR = PIN_SWCLK）*/
                "lw   t0, -8(%[pa])\n\t"  /* 采样：原 C sample = PIN_SWD_PORT->INDR（-8 = INDR 偏移）*/
                "andi t0, t0, 8\n\t"      /* 取 PA3：原 C 的 (sample >> 3) & 1U；留着 bit3 不右移，
                                           * 因为下面按 rb_k 左/右移时 bit3 正好落在目标位上 */
                "sw   t1, 0(%[pa])\n\t"   /* 上升沿：原 C PIN_SWCLK_SET_READ()（BSHR = PIN_SWCLK）*/
                "xor  %[par], %[par], t0\n\t" /* 奇偶：原 C 循环后的 SWD_Parity32(val)。此时 t0 还是
                                           * 0x08/0，所以 32 次异或全落在 par 的 bit3，最后 >>3 即得奇偶 */
                ".if rb_k < 3\n\t"        /* ┐ 把 bit3 挪到 val 的第 rb_k 位： */
                "srli t0, t0, (3 - rb_k)\n\t" /* │ 前 4 位（rb_k = 0..2）右移 (3-rb_k) -> val[0..2] */
                ".elseif rb_k == 3\n\t"   /* │ rb_k = 3 时不动（bit3 已在 val[3]）*/
                "nop\n\t"                 /* │ */
                ".else\n\t"               /* │ rb_k >= 4：左移 (rb_k-3) -> val[rb_k] */
                "slli t0, t0, (rb_k - 3)\n\t" /* ┘ 等价于原 C 的「packed << 28 | val >> 4」位打包 */
                ".endif\n\t"              /*   （原 C 靠移位寄存器攒 4 位，这里每位移位量是常量，直接到位）*/
                "or   %[val], %[val], t0\n\t" /* 收进 val：原 C 的 val |= (packed << 28) */
                ".set rb_k, rb_k + 1\n\t" /* 编译器计数器 +1（检查：只算 val[0..31]，32 位无越界）*/
                ".endr\n\t"               /* 展开结束 */
                : [val] "+r"(val), [par] "+r"(parity)
                : [pa] "r"(pa)
                : "t0", "t1", "memory");
            /* 累加器的 bit 3 就是 32 位数据的奇偶（见上面说明） */
            parity = (parity >> PIN_SWDIO_SHIFT) & 1U; /* 原 C SWD_Parity32 的收尾：& 1U */
            PIN_SWCLK_CLR_READ();
            bit = PIN_SWDIO_IN();
            PIN_SWCLK_SET_READ(); /* Read Parity */
            if ((parity ^ bit) & 1U) {
                ack = DAP_TRANSFER_ERROR;
            }
            /* Turnaround */
            PIN_SWCLK_CLR();
            *data = val;
            PIN_SWCLK_SET();
            PIN_SWDIO_OUTPUT(); // turnaround
        } else {
            /* Turnaround */
            PIN_SWCLK_CLR();
            PIN_SWDIO_OUTPUT(); // turnaround
            PIN_SWCLK_SET();

            /* Write data */
            val = *data;
            const uint32_t parity = SWD_Parity32(val);
            volatile uint32_t *const pa = &PIN_SWD_PORT->BSHR;

            // -----------------------------------------------------------------------
            // 32 位数据相位：内联汇编 + 按位常量掩码取位
            // -----------------------------------------------------------------------
            // 为什么必须用汇编：用 C 写时 GCC 会把「算下一位 BSHR 值」的 ALU 重新贴到
            // 下降沿那条 store **之前**，于是低电平期 0 条指令（实测 20 ns）、高电平期
            // 5 条（120 ns），占空比 6:1。volatile + memory clobber 才能钉住顺序。
            //
            // 用「按位常量掩码」而不是逐位 `val >>= 1`：
            //   shift_k = 第 k 位 << 4 = 0 或 16
            //     k <  4 : (val << (4 - k)) & 16
            //     k == 4 : val & 16
            //     k >  4 : (val >> (k - 4)) & 16
            // 两种写法取出 shift 都是 2 条指令，但常量掩码**不必推进 val**，
            // 于是每 bit 少一条 `srli val, val, 1`（32 个 bit 共省 32 条）。
            // 代价是每个 bit 的立即数不同，用 GAS 的 .set + .if 按 k 生成。
            //
            // 每 bit 的排布（ALU 拆 2+2，让占空比接近 1:1）：
            //   ① sw t0 : 清 SWCLK + 按位设/清 SWDIO  （下降沿；t0 在上一轮已算好）
            //   -- 低电平期：slli/andi 2 条 --
            //   ② sw hi : 置 SWCLK                    （上升沿）
            //   -- 高电平期：srl/or 2 条 + 可调 NOP --
            //
            // 这 4 条 ALU 只依赖 val，不依赖①要写的 t0，所以拆到上升沿两侧**不会**引入
            // 额外停顿；而 or 算完后隔着 nop + sw 才是下一轮的 sw t0，也没写后写气泡。
            // 想继续调：把 srl/or 之一搬到②之前，或删掉 nop。
            //
            // 寄存器：t0 = 当前位 BSHR 值  t2 = 0x00080000（清 SWDIO）  t3 = 0x00040000（清 SWCLK）
            __asm__ volatile(
                /* 逐行对照的原 C（合并前的写法）：
                 *
                 *   PIN_SWCLK_CLR_SWDIO_OUT(val);   // 每 bit：BSHR = (SWCLK<<16) | (SWDIO<<16) >> ((val&1)<<4)
                 *   PIN_SWCLK_SET();               // BSHR = PIN_SWCLK (= 4)
                 *   最后 PIN_SWCLK_CLR_SWDIO_OUT(parity) 送奇偶位
                 *
                 * 每 cell 7 条指令、低 2 / 高 3 → 8 个 clk（低 4 / 高 4），与读同构。
                 */
                /* === 准备：先算第 0 位的 BSHR 值，作为流水线初值 === */
                "slli t0, %[val], 4\n\t"  /* ┐ shift = (val << 4) */
                "andi t0, t0, 16\n\t"     /* ┘ & 16 = (val & 1) << 4 = 0 或 16（原 C 的 (val&1U)<<4）*/
                "lui  t2, 0x80\n\t"       /* t2 = PIN_SWDIO << 16 = 0x00080000 */
                "srl  t0, t2, t0\n\t"     /* t0 选置位 / 复位半字（= SWDIO 的部分）*/
                "lui  t3, 0x40\n\t"       /* t3 = PIN_SWCLK << 16 = 0x00040000 */
                "or   t0, t0, t3\n\t"     /* 合成第 0 位的值 ≈ 原 C PIN_SWCLK_CLR_SWDIO_OUT(val) */
                ".set wb_k, 0\n\t"        /* wb_k = 当前正在输出的位（0..31），GAS 编译期计数 */
                ".rept 32\n\t"            /* 展开 32 位 */
                "sw   t0, 0(%[pa])\n\t"   /* 下降沿 + 写 SWDIO（t0 是上一轮低电平期算好的）*/
                ".if (wb_k + 1) < 4\n\t"  /* ┐ 算**下一位**(wb_k+1)的 shift 量（0 或 16）： */
                "slli t0, %[val], (3 - wb_k)\n\t" /* │ wb_k+1 < 4：(val << (3-wb_k)) 再 & 16 取该位 */
                ".elseif (wb_k + 1) == 4\n\t" /* │ wb_k+1 == 4：直接用 val & 16（省掉一次 slli）*/
                "andi t0, %[val], 16\n\t" /* │ */
                ".else\n\t"               /* │ wb_k+1 > 4：(val >> (wb_k-3)) 再 & 16 */
                "srli t0, %[val], (wb_k - 3)\n\t" /* ┘ 等价于原 C 的 (val >> k) & 1U 后 << 4 */
                ".endif\n\t"
                ".if (wb_k + 1) != 4\n\t" /* ┐ wb_k+1 == 4 那条已经是 & 16 的结果，不用再掩码 */
                "andi t0, t0, 16\n\t"     /* ┘ */
                ".endif\n\t"
                "sw   %[hi], 0(%[pa])\n\t" /* 上升沿：原 C PIN_SWCLK_SET()（%[hi] = PIN_SWCLK = 4）*/
                "srl  t0, t2, t0\n\t"     /* 选置位 / 复位半字 */
                "or   t0, t0, t3\n\t"     /* 合成下一位的 BSHR 值（留到下一轮下降沿用）*/
                "nop\n\t"                 /* 高电平期第 3 个 slot（占空比旋钮）*/
                ".set wb_k, wb_k + 1\n\t" /* 计数器 +1 */
                ".endr\n\t"
                :
                : [val] "r"(val), [pa] "r"(pa), [hi] "r"((uint32_t)PIN_SWCLK)
                : "t0", "t2", "t3", "memory");
            PIN_SWCLK_CLR_SWDIO_OUT(parity); /* 奇偶位：原 C 同上，由 SWD_Parity32(val) 得到 */
            PIN_DELAY_FAST();
            PIN_DELAY_FAST();
            PIN_SWCLK_SET();
            PIN_DELAY_FAST();
        }
        /* Capture Timestamp */
        #if 0
        if (request & DAP_TRANSFER_TIMESTAMP) {
            DAP_Data.timestamp = TIMESTAMP_GET();
        }
        #endif
        /* Idle cycles */
        n = DAP_Data.transfer.idle_cycles;
        if (n) {
            PIN_SWDIO_TMS_CLR();
            for (; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_FAST();
                PIN_SWCLK_SET();
            }
        }
        PIN_SWDIO_TMS_SET();
        return ((uint8_t)ack);
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        /* WAIT or FAULT response */
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {
            for (n = 32U + 1U; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_FAST();
                PIN_SWCLK_SET();
            }
        }
        /* Turnaround */
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            PIN_SWCLK_CLR();
            PIN_DELAY_FAST();
            PIN_SWCLK_SET();
        }
        PIN_SWDIO_OUTPUT(); // turnaround
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {
            PIN_SWDIO_OUT(0U);
            for (n = 32U + 1U; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_FAST();
                PIN_SWCLK_SET(); /* Dummy Write WDATA[0:31] + Parity */
            }
        }
        PIN_SWDIO_TMS_SET();
        return ((uint8_t)ack);
    }

    /* Protocol error */
    for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {
        PIN_SWCLK_CLR();
        PIN_DELAY_FAST();
        PIN_SWCLK_SET();/* Back off data phase */
    }
    PIN_SWDIO_OUTPUT(); // Back off data phase
    PIN_SWDIO_TMS_SET();
    return ((uint8_t)ack);
}

// 慢路径不补占空比额外延时。
//
// 上面的 PIN_SWCLK_SET/CLR 带 x035_pin_delay_*_extra()，只适合快路径 ——
// 快路径的定时完全由固定指令数决定，补几条就能把占空比拉平。
// 慢路径的定时由 Set_Clock_Delay 算出的 clock_delay 控制（每半周期调用
// PIN_DELAY_SLOW），低频时那个延时远大于几条 NOP，再补固定延时既没必要、
// 又会把低电平期拉得比高电平期还长，造成反向不对称。
// 所以这里换回不带额外延时的版本。
#undef PIN_SWCLK_SET
#undef PIN_SWCLK_CLR
#define PIN_SWCLK_SET() PIN_SWCLK_TCK_SET()
#define PIN_SWCLK_CLR() PIN_SWCLK_TCK_CLR()

// SWD Transfer I/O Slow
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
__attribute__((noinline)) static uint8_t SWD_TransferSlow(
    uint32_t request, uint32_t *data) {
    uint32_t ack;
    uint32_t bit;
    uint32_t val;
    uint32_t parity;

    uint32_t n;

    /* Packet Request */
    parity = 0U;
    PIN_SWCLK_CLR_SWDIO_OUT(1U);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Start Bit */
    bit = request >> 0;
    PIN_SWCLK_CLR_SWDIO_OUT(bit);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* APnDP Bit */
    parity += bit;
    bit = request >> 1;
    PIN_SWCLK_CLR_SWDIO_OUT(bit);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* RnW Bit */
    parity += bit;
    bit = request >> 2;
    PIN_SWCLK_CLR_SWDIO_OUT(bit);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* A2 Bit */
    parity += bit;
    bit = request >> 3;
    PIN_SWCLK_CLR_SWDIO_OUT(bit);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* A3 Bit */
    parity += bit;
    PIN_SWCLK_CLR_SWDIO_OUT(parity);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Parity Bit */
    PIN_SWCLK_CLR_SWDIO_OUT(0U);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Stop Bit */
    PIN_SWCLK_CLR_SWDIO_OUT(1U);
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Park Bit */

    /* Turnaround */
    PIN_SWDIO_OUT_DISABLE();
    for (n = DAP_Data.swd_conf.turnaround; n; n--) {
        PIN_SWCLK_CLR();
        PIN_DELAY_SLOW(DAP_Data.clock_delay);
        PIN_SWCLK_SET();
        PIN_DELAY_SLOW(DAP_Data.clock_delay);
    }

    /* Acknowledge response */
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    ack = bit << 0;
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    ack |= bit << 1;
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    ack |= bit << 2;

    if (ack == DAP_TRANSFER_OK) { /* OK response */
        /* Data transfer */
        if (request & DAP_TRANSFER_RnW) {
            /* Read data */
            val = 0U;
            parity = 0U;
            for (n = 32U; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                bit = PIN_SWDIO_IN();
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Read RDATA[0:31] */
                parity += bit;
                val >>= 1;
                val |= bit << 31;
            }
            PIN_SWCLK_CLR();
            PIN_DELAY_SLOW(DAP_Data.clock_delay);
            bit = PIN_SWDIO_IN();
            PIN_SWCLK_SET();
            PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Read Parity */
            if ((parity ^ bit) & 1U) {
                ack = DAP_TRANSFER_ERROR;
            }
            if (data) {
                *data = val;
            }
            /* Turnaround */
            for (n = DAP_Data.swd_conf.turnaround; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
            }
            PIN_SWDIO_OUT_ENABLE();
        } else {
            /* Turnaround */
            for (n = DAP_Data.swd_conf.turnaround; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
            }
            PIN_SWDIO_OUT_ENABLE();
            /* Write data */
            val = *data;
            parity = 0U;
            for (n = 32U; n; n--) {
                PIN_SWCLK_CLR_SWDIO_OUT(val);
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Write WDATA[0:31] */
                parity += val;
                val >>= 1;
            }
            PIN_SWCLK_CLR_SWDIO_OUT(parity);
            PIN_DELAY_SLOW(DAP_Data.clock_delay);
            PIN_SWCLK_SET();
            PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Write Parity Bit */
        }
        /* Capture Timestamp */
        if (request & DAP_TRANSFER_TIMESTAMP) {
            DAP_Data.timestamp = TIMESTAMP_GET();
        }
        /* Idle cycles */
        n = DAP_Data.transfer.idle_cycles;
        if (n) {
            PIN_SWDIO_OUT(0U);
            for (; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
            }
        }
        PIN_SWDIO_OUT(1U);
        return ((uint8_t)ack);
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        /* WAIT or FAULT response */
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {
            for (n = 32U + 1U; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Dummy Read RDATA[0:31] + Parity */
            }
        }
        /* Turnaround */
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            PIN_SWCLK_CLR();
            PIN_DELAY_SLOW(DAP_Data.clock_delay);
            PIN_SWCLK_SET();
            PIN_DELAY_SLOW(DAP_Data.clock_delay);
        }
        PIN_SWDIO_OUT_ENABLE();
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {
            PIN_SWDIO_OUT(0U);
            for (n = 32U + 1U; n; n--) {
                PIN_SWCLK_CLR();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Dummy Write WDATA[0:31] + Parity */
            }
        }
        PIN_SWDIO_OUT(1U);
        return ((uint8_t)ack);
    }

    /* Protocol error */
    for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {
        PIN_SWCLK_CLR();
        PIN_DELAY_SLOW(DAP_Data.clock_delay);
        PIN_SWCLK_SET();
        PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Back off data phase */
    }
    PIN_SWDIO_OUT_ENABLE();
    PIN_SWDIO_OUT(1U);
    return ((uint8_t)ack);
}

// SWD Transfer I/O
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
DAP_HIGHCODE_SECTION uint8_t SWD_Transfer(uint32_t request, uint32_t *data) {
    if (DAP_Data.fast_clock) {
        return SWD_TransferFast(request, data);
    } else {
        return SWD_TransferSlow(request, data);
    }
}

#endif /* (DAP_SWD != 0) */