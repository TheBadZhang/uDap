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

// 读取时把「采样」与「拉高」拆开，好把位打包的工作放进低电平期。
// 采样点与原先一致（下降沿后延时一拍），所以数据有效性不变；
// 多出的打包指令不会再拉长高电平期。
#define SWD_FAST_READ_LOW(sample)   \
    do {                            \
        PIN_SWCLK_CLR_READ();       \
        sample = PIN_SWD_PORT->INDR;       \
    } while (0)

#define SWD_FAST_READ_HIGH()  \
    do {                      \
        PIN_SWCLK_SET_READ(); \
        PIN_DELAY_FAST();     \
    } while (0)

#define SWD_FAST_PACK_BARRIER() \
    __asm__ volatile("" : "+r"(packed) : : "memory")

DAP_HIGHCODE_SECTION __attribute__((noinline, optimize("O2")))
static void SWD_FastWriteData(uint32_t val) {
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
        /* 先算第 0 位（k=0 -> (val << 4) & 16），作为流水线初值 */
        "slli t0, %[val], 4\n\t"
        "andi t0, t0, 16\n\t"
        "lui  t2, 0x80\n\t"
        "srl  t0, t2, t0\n\t"
        "lui  t3, 0x40\n\t"
        "or   t0, t0, t3\n\t"
        ".set wb_k, 0\n\t"
        ".rept 32\n\t"
        "sw   t0, 0(%[pa])\n\t"
        ".if (wb_k + 1) < 4\n\t"
        "slli t0, %[val], (3 - wb_k)\n\t"
        ".elseif (wb_k + 1) == 4\n\t"
        "andi t0, %[val], 16\n\t"
        ".else\n\t"
        "srli t0, %[val], (wb_k - 3)\n\t"
        ".endif\n\t"
        ".if (wb_k + 1) != 4\n\t"
        "andi t0, t0, 16\n\t"
        ".endif\n\t"
        "sw   %[hi], 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        "nop\n\t"
        ".set wb_k, wb_k + 1\n\t"
        ".endr\n\t"
        :
        : [val] "r"(val), [pa] "r"(pa), [hi] "r"((uint32_t)PIN_SWCLK)
        : "t0", "t2", "t3", "memory");

    PIN_SWCLK_CLR_SWDIO_OUT(parity);
    PIN_SWD_PORT->BSHR = (PIN_SWCLK << 16);
    PIN_DELAY_FAST();
    PIN_SWCLK_SET();
    PIN_DELAY_FAST();
}

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
    uint32_t packed;

    uint32_t n;

    /* Packet Request —— 8 个 SWCLK（start/APnDP/RnW/A2/A3/parity/stop）
     *
     * 和 SWD_FastWriteData 同样的理由这段 C 用编译器是钉不住的：GCC 永远把
     * 「算下一位 BSHR 值」的 ALU 提到下降沿那条 store **之前**（它总是尽早算值
     * 来隐藏延迟），于是低电平期只剩 1 条 store（~20 ns）、高电平期塞进 5 条 ALU
     * （~140 ns），占空比 1:7。试过 __attribute__((optimize("O2",
     * "no-schedule-insns","no-schedule-insns2")))，没有任何变化 —— 重排来自
     * GCC「算值尽早」的通用策略，不是调度器 pass。所以只能用汇编钉住顺序。
     *
     * 每 bit 7 条指令，排布与 SWD_FastWriteData 完全一致（波形也一致）：
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
    __asm__ volatile(
        /* --- 准备 --- */
        "lui  t2, 0x80\n\t"       /* t2 = 0x00080000 : 清 SWDIO        */
        "lui  t3, 0x40\n\t"       /* t3 = 0x00040000 : 清 SWCLK        */
        "li   t4, 4\n\t"          /* t4 = 4          : 置 SWCLK        */
        "srli t5, %[req], 2\n\t"  /* t5 = request[3:0] 的异或，再 <<4  */
        "xor  t5, t5, %[req]\n\t"
        "srli t1, t5, 1\n\t"
        "xor  t5, t5, t1\n\t"
        "andi t5, t5, 1\n\t"
        "slli t5, t5, 4\n\t"
        "srli t0, t2, 16\n\t"     /* t0 = start 位(恒 1) 的值 0x40008  */
        "or   t0, t0, t3\n\t"
        /* --- start 位：低电平期算 APnDP（低 2 / 高 3，与数据相位同构） --- */
        "sw   t0, 0(%[pa])\n\t"
        "slli t0, %[req], 4\n\t"
        "andi t0, t0, 16\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        "nop\n\t"
        /* --- APnDP 位：低电平期算 RnW --- */
        "sw   t0, 0(%[pa])\n\t"
        "slli t0, %[req], 3\n\t"
        "andi t0, t0, 16\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        "nop\n\t"
        /* --- RnW 位：低电平期算 A2 --- */
        "sw   t0, 0(%[pa])\n\t"
        "slli t0, %[req], 2\n\t"
        "andi t0, t0, 16\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        /* --- A2 位：低电平期算 A3 --- */
        "sw   t0, 0(%[pa])\n\t"
        "slli t0, %[req], 1\n\t"
        "andi t0, t0, 16\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "srl  t0, t2, t0\n\t"
        "or   t0, t0, t3\n\t"
        "nop\n\t"
        /* --- A3 位：低电平期算 parity（t5 已折好，只有 2 条 ALU，高电平期补 nop） --- */
        "sw   t0, 0(%[pa])\n\t"
        "srl  t0, t2, t5\n\t"
        "or   t0, t0, t3\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        /* --- parity 位：低电平期算 stop（恒 0，只有 1 条 ALU，补 nop 凑 2） --- */
        "sw   t0, 0(%[pa])\n\t"
        "lui  t0, 0xc0\n\t"
        "nop\n\t"
        "sw   t4, 0(%[pa])\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        /* --- stop 位：没有数据要算，补 nop 保持每拍 8 个 clk --- */
        "sw   t0, 0(%[pa])\n\t"
        "nop\n\t"
        "nop\n\t"
        "sw   t4, 0(%[pa])\n\t"
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
            /* Read data */
            val = 0U;
            for (n = 8U; n; n--) {
                SWD_FAST_READ_LOW(bit);
                packed = (bit >> PIN_SWDIO_SHIFT) & 1U;
                SWD_FAST_PACK_BARRIER();
                SWD_FAST_READ_HIGH();
                SWD_FAST_READ_LOW(bit);
                packed |= ((bit >> PIN_SWDIO_SHIFT) & 1U) << 1;
                SWD_FAST_PACK_BARRIER();
                SWD_FAST_READ_HIGH();
                SWD_FAST_READ_LOW(bit);
                packed |= ((bit >> PIN_SWDIO_SHIFT) & 1U) << 2;
                SWD_FAST_PACK_BARRIER();
                SWD_FAST_READ_HIGH();
                SWD_FAST_READ_LOW(bit);
                packed |= ((bit >> PIN_SWDIO_SHIFT) & 1U) << 3;
                SWD_FAST_PACK_BARRIER();
                val >>= 4;
                SWD_FAST_READ_HIGH();
                val |= (packed << 28);
            }
            parity = SWD_Parity32(val);
            PIN_SWCLK_CLR_READ();
            bit = PIN_SWDIO_IN();
            PIN_SWCLK_SET_READ(); /* Read Parity */
            if ((parity ^ bit) & 1U) {
                ack = DAP_TRANSFER_ERROR;
            }
            /* Turnaround */
            PIN_SWCLK_CLR();
            if (data) {
                *data = val;
            }
            PIN_SWCLK_SET();
            PIN_SWDIO_OUTPUT(); // turnaround
        } else {
            /* Turnaround */
            PIN_SWCLK_CLR();
            PIN_SWDIO_OUTPUT(); // turnaround
            PIN_SWCLK_SET();

            /* Write data */
            SWD_FastWriteData(*data);
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

// 注意：上面两个 undef 对应下面用到的两个宏，名字改了要同步
#undef SWD_FAST_READ_LOW
#undef SWD_FAST_READ_HIGH
#undef SWD_FAST_PACK_BARRIER

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