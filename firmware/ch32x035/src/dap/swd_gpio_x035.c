#include "swd_gpio.h"

#include "mode_overlay.h"
#include "swd_pin_map.h"

/* CFGLR 基准值（见 src/swd_pin_map.h 的说明）：只在 port_on/off 或别人改过 CFGLR 后重建 */
uint32_t board_swd_cfglr_base;

/*
 * SWD 物理层的**芯片内置实现**（swd_gpio_if_x035）。
 *
 * 三件事都在这个文件里：
 *   1. GPIO 描述符（引脚来自 src/swd_pin_map.h，换板只改那个头文件）
 *   2. 端口生命周期与 DIO 方向切换（PORT_SWD_SETUP / PORT_OFF / turnaround）
 *   3. 三段位时序手写汇编（原先内联在 src/dap/sw_dp.c 的 SWD_TransferFast 里）
 * 对外的接口定义见 src/dap/swd_gpio.h：换驱动时照着那个接口再写一份、
 * 启动时 swd_gpio_bind() 一下即可，sw_dp.c 与 DAP_config.h 不用动。
 *
 * ---------------------------------------------------------------------------
 * 以下为三段汇编的原始说明
 * ---------------------------------------------------------------------------
 *
 * 单独成文件的理由：这些立即数全是「引脚相关常量」——
 *   SWD_CLK_SET = 1 << SWD_CLK_BIT        BSHR 置位半字（只有低 16 位有效）
 *   SWD_CLK_CLR = 1 << (SWD_CLK_BIT + 16) BSHR 复位半字
 *   SWD_DIO_SET / SWD_DIO_CLR             同上，对应 SWDIO
 *   SWD_DIO_SET 同时是 INDR 的采样掩码
 * 集中到一处后，换引脚只改 src/swd_pin_map.h 的 SWD_PIN_VARIANT，这里由 GAS 的
 * .set 从 C 宏自动推导（见 SWD_ASM_PIN_CONSTANTS），不必逐条改立即数。
 *
 * GPIO 寄存器偏移（CH32X035，与 ch32x035.h 的 GPIO_TypeDef 一致）：
 *   0 = BSHR（低 16 位置位 / 高 16 位复位），+4 = BCR（复位），-8 = INDR（采样）
 *
 * ⚠️ 每段汇编里的指令**条数就是时序**：快路径的 SWCLK 周期由「下降沿 store 与
 * 上升沿 store 之间的指令数」决定，两半的指令数故意不相等（外设 store 的退休
 * 延迟不对称），推导见 sw_dp.c 文件头。增删指令时请成对增删 nop，否则占空比
 * 和速率都会变。
 */

#define SWD_ASM_(x) #x
#define SWD_ASM(x) SWD_ASM_(x)

/* 把 C 的引脚编号交给 GAS：后面所有立即数都由这几个绝对符号算出来 */
#define SWD_ASM_PIN_CONSTANTS                           \
    ".set SWD_CLK_BIT, " SWD_ASM(SWD_CLK_BIT) "\n\t"    \
    ".set SWD_DIO_BIT, " SWD_ASM(SWD_DIO_BIT) "\n\t"    \
    ".set SWD_CLK_SET, (1 << SWD_CLK_BIT)\n\t"          \
    ".set SWD_CLK_CLR, (1 << (SWD_CLK_BIT + 16))\n\t"   \
    ".set SWD_DIO_SET, (1 << SWD_DIO_BIT)\n\t"          \
    ".set SWD_DIO_CLR, (1 << (SWD_DIO_BIT + 16))\n\t"

/*
 * Packet Request —— 8 个 SWCLK（start/APnDP/RnW/A2/A3/parity/stop）
 *
 * 为什么必须用汇编：GCC 永远把「算下一位 BSHR 值」的 ALU 提到下降沿那条 store
 * **之前**（它总是尽早算值来隐藏延迟），于是低电平期只剩 1 条 store、高电平期
 * 塞进 5 条 ALU，占空比 1:7。试过
 * __attribute__((optimize("O2","no-schedule-insns","no-schedule-insns2")))，
 * 没有任何变化 —— 重排来自 GCC「算值尽早」的通用策略，不是调度器 pass。
 * 所以只能用汇编钉住顺序。
 *
 * 每 bit 7 条指令：
 *   ① sw t0 : 清 SWCLK + 按位设/清 SWDIO  （下降沿；t0 上一拍已算好）
 *   -- 低电平期：2 条 ALU（slli + andi）--
 *   ② sw t4 : 置 SWCLK                     （上升沿，t4 = SWD_CLK_SET）
 *   -- 高电平期：2 条 ALU（srl + or）+ 1 个 nop --
 *
 * 为什么是「低 2 条 / 高 3 条」而不是「低 2 / 高 2」，也就是为什么低电平 N_low
 * 条、高电平 N_high 条时实测是：
 *     低 clk = N_low + 2      高 clk = N_high + 1     周期 = N_low + N_high + 3
 * 因为两条外设 store 的退休延迟不对称：下降沿那条在低电平期**开头**又被计一次，
 * 上升沿那条在高电平期只计一次。两条实测都吻合：
 *   请求相位 4+0+1nop (N_low=4,N_high=1) -> 低 6 / 高 2（用户实测）✓
 *   数据相位 2+2+1nop (N_low=2,N_high=3) -> 低 4 / 高 4（用户实测「完美」）✓
 *
 * 位序：start=1, APnDP=b0, RnW=b1, A2=b2, A3=b3, parity=四位异或, stop=0。
 * 取位用「按位常量掩码」（slli 4-k 再 andi 16），省掉逐位推进 request；
 * parity 在准备段一次性折叠出来。
 * t2 = SWD_DIO_CLR  t3 = SWD_CLK_CLR  t4 = SWD_CLK_SET
 * A3/parity/stop 三拍没有 4 条 ALU 可填，用 nop 补齐到同样的 低2/高3，
 * 这样整帧每一拍都是 8 个 clk，波形完全均匀。
 */
DAP_HIGHCODE_SECTION static void SWD_GPIO_IMPL(swd_fast_packet_request)(uint32_t request) {
    volatile uint32_t *const pa = &SWD_PORT->BSHR;

    /* 逐行对照的原 C（下面注释里的「原 C:」都指这段合并前的写法）：
     *
     *   PIN_SWCLK_CLR_SWDIO_OUT(1U);   // start 位 = 1
     *   bit = request >> k;            // k = 0..3 -> APnDP / RnW / A2 / A3
     *   PIN_SWCLK_CLR_SWDIO_OUT(bit);  // BSHR = (SWCLK<<16) | (SWDIO<<16) >> ((bit&1)<<4)
     *   parity += bit;                 // 四位异或（这里提前折成 t5）
     *   PIN_SWCLK_SET();               // 每拍上升沿 = BSHR = SWD_CLK_SET
     *   PIN_SWCLK_CLR_SWDIO_OUT(parity);
     *   PIN_SWCLK_CLR_SWDIO_OUT(0U);   // stop 位 = 0
     *
     * 每拍排布「低 2 条 / 高 3 条」= 下降沿与上升沿之间 2 条、上升沿之后 3 条，
     * 于是每拍 8 个 clk（低 4 / 高 4）。每拍都是「算下一位、当前位已备好」。
     */
    __asm__ volatile(
        SWD_ASM_PIN_CONSTANTS
        /* === 准备：常量 + request 的奇偶，一次算完 === */
        "lui  t2, (SWD_DIO_CLR >> 12)\n\t" /* t2 = SWDIO 的复位半字 */
        "lui  t3, (SWD_CLK_CLR >> 12)\n\t" /* t3 = SWCLK 的复位半字 */
        "li   t4, SWD_CLK_SET\n\t"         /* t4 = PIN_SWCLK_SET 写的 BSHR 值 */
        "srli t5, %[req], 2\n\t"           /* ┐ 原 C 是逐拍 parity += bit；这里提前折算： */
        "xor  t5, t5, %[req]\n\t"          /* │ t5 = req[3:0] ^ (req[3:0] >> 2)，即 r0^r2 | r1^r3 */
        "srli t1, t5, 1\n\t"               /* │ 再 ^ (自身 >> 1)，两次移位异或覆盖 4 位 */
        "xor  t5, t5, t1\n\t"              /* ┘ 此时 t5[0] = r0^r1^r2^r3 = 四位奇偶 */
        "andi t5, t5, 1\n\t"               /* t5 = 0/1（= 原 C 的 parity & 1U）*/
        "slli t5, t5, 4\n\t"               /* t5 = 0 或 16：换成后面 srl 要的移位量 */
        "srli t0, t2, 16\n\t"              /* ┐ t0 = SWDIO 置位半字 = SWD_DIO_SET */
        "or   t0, t0, t3\n\t"              /* ┘ | 清 SWCLK，即原 C 的 PIN_SWCLK_CLR_SWDIO_OUT(1U) */
        /* === start 位（恒 1）：低电平期算 APnDP = request[0] === */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿 + 写 SWDIO：原 C PIN_SWCLK_CLR_SWDIO_OUT(1U) */
        "slli t0, %[req], 4\n\t"           /* ┐ shift = request[0] << 4 = 0 或 16 */
        "andi t0, t0, 16\n\t"              /* ┘（= 原 C 的 (bit & 1U) << 4，bit = request >> 0）*/
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"              /* t0 = (SWDIO<<16) >> shift：选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成 APnDP 拍下降沿要写的 BSHR 值 */
        "nop\n\t"                          /* 高电平期第 3 个 slot（占空比）*/
        /* === APnDP 位：低电平期算 RnW = request[1] === */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 0 */
        "slli t0, %[req], 3\n\t"           /* ┐ shift = request[1] << 4（<< 3 再 & 16）*/
        "andi t0, t0, 16\n\t"              /* ┘ */
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"              /* 选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成 RnW 拍的值 */
        "nop\n\t"                          /* 高电平期第 3 个 slot */
        /* === RnW 位：低电平期算 A2 = request[2] === */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 1 */
        "slli t0, %[req], 2\n\t"           /* ┐ shift = request[2] << 4 */
        "andi t0, t0, 16\n\t"              /* ┘ */
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"              /* 选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成 A2 位的值 */
        /* ⚠️ 这一拍（RnW，算 A2）没有补尾部 nop：高电平期只有 srl/or 2 条，
         * 所以它的周期是 7 个 clk（低 4 / 高 3），比相邻几拍少 1 个。
         * 这是沿用原实现留下的唯一一处不对称（整帧其余各拍都是 8 clk）。*/
        /* === A2 位：低电平期算 A3 = request[3] === */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 2 */
        "slli t0, %[req], 1\n\t"           /* ┐ shift = request[3] << 4 */
        "andi t0, t0, 16\n\t"              /* ┘ */
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿：原 C PIN_SWCLK_SET() */
        "srl  t0, t2, t0\n\t"              /* 选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成 A3 拍的值 */
        "nop\n\t"                          /* 高电平期第 3 个 slot */
        /* === A3 位：低电平期算 parity（t5 已折好，只需 2 条 ALU）=== */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(bit)，bit = request >> 3 */
        "srl  t0, t2, t5\n\t"              /* t0 = (SWDIO<<16) >> t5（t5 已是 0/16）—— 省掉 slli/andi */
        "or   t0, t0, t3\n\t"              /* 合成 parity 拍的值 */
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿：原 C PIN_SWCLK_SET() */
        "nop\n\t"                          /* ┐ 高电平期补齐 3 条 */
        "nop\n\t"                          /* │ */
        "nop\n\t"                          /* ┘ */
        /* === parity 位：低电平期算 stop（恒 0，只需 1 条 ALU）=== */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(parity) */
        "lui  t0, ((SWD_DIO_CLR | SWD_CLK_CLR) >> 12)\n\t" /* t0 = 清 SWDIO + 清 SWCLK（数据位 = 0）*/
        "nop\n\t"                          /* 低电平期补齐 2 条 */
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿：原 C PIN_SWCLK_SET() */
        "nop\n\t"                          /* ┐ 高电平期补齐 3 条 */
        "nop\n\t"                          /* │ */
        "nop\n\t"                          /* ┘ */
        /* === stop 位：没有下一位要算，纯补 nop 维持每拍 8 个 clk === */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿：原 C PIN_SWCLK_CLR_SWDIO_OUT(0U) */
        "nop\n\t"                          /* ┐ 低电平期补齐 2 条 */
        "nop\n\t"                          /* ┘ */
        "sw   t4, 0(%[pa])\n\t"            /* 上升沿：原 C PIN_SWCLK_SET() */
        :
        : [req] "r"(request), [pa] "r"(pa)
        : "t0", "t1", "t2", "t3", "t4", "t5", "memory");
}

/*
 * 读数据相位（32 位）
 *
 * 排布与 x035_write_data 完全同构：每 bit 8 个 SWCLK、低 4 / 高 4，
 * 所以读数据相位的时钟也是 6 MHz、占空比 1:1，与写数据相位一致。
 *
 * 为什么必须用汇编：这段 C 在 GCC 下每个 bit 要 10~11 个 clk —— 采样后还要
 * srli/andi/or 打包，而 PIN_SWCLK_SET_READ() 自带 3 个 nop、外层再加
 * PIN_DELAY_FAST() 1 个，高电平期被补到 5 个 clk。手写后每 bit 只要 8 个 clk
 * （读数据相位整体缩短约 2 成）。
 *
 * 每 bit 的排布（与写路径逐条对齐）：
 *   ① sw t1,4(%[pa])  : BCR 清 SWCLK       （下降沿）
 *   ② lw t0,-8(%[pa]) : 采样 INDR          ← 采样点与原实现一致（紧贴下降沿）
 *      andi t0,t0,DIO_SET : 取出 SWDIO      -- 低电平期 2 条 --
 *   ③ sw t1,0(%[pa])  : BSHR 置 SWCLK      （上升沿）
 *      srli/slli      : 把该位挪到 val 的第 k 位
 *      or %[val]      : 收进 val
 *      nop
 *   -- 高电平期 3 条 --
 *
 * 关键：不用「每 4 位攒进 packed、再 val>>=4 | packed<<28」那套移位寄存器，
 * 而是每个 bit 直接 or 进 val 的**最终位置** —— 于是没有循环、没有组间开销、
 * 也不需要在 val 上做移位。第 n 个收到的数据位就落在 val 的第 n 位：
 *   第 0 .. SHIFT-1 位 ：andi 后 w=SWD_DIO_SET，右移 (SHIFT-k) -> val[k]
 *   第 SHIFT 位        ：不动（掩码后正好在原位）
 *   第 SHIFT+1 位起    ：左移 (k-SHIFT) -> val[k]
 * 因为每位移位量都是编译期常量，用 GAS 的 .set + .if 按位生成。
 * 32 位各自 or 进互不重叠的位置，所以 or 之间没有依赖，不存在写后写气泡。
 *
 * 奇偶位顺手算出来：每个 cell 高电平期的那个 nop 换成
 * `xor %[par],%[par],t0` —— 此时 t0 还是 SWD_DIO_SET/0（移位前），所以 32 位的
 * 异或全部累加到 par 的 bit SWD_DIO_BIT，循环后右移该位数即得奇偶。
 * 于是 `SWD_Parity32(val)` 那 10 条指令（5 次移位+异或折叠）整个省掉，
 * 换成 2 条（srli+andi）。注意 xor 必须排在移位**之前**：移位会把 t0 改成
 * 1<<k，那时异或出来的是 val 本身而不是奇偶。
 *
 * ⚠️ 实测结论，别重复走这条路：**这不提升吞吐**。省下的那 10 条折叠指令本来
 * 就被 GCC 穿插进「读奇偶位」那拍的 3 个 nop 里，基本不影响周期。实测
 * （pyocd，64 KiB × 10 轮，各跑 3 次，组内重复性 ±0.1%）：
 *     写 254.68 -> 260.91 KiB/s (+2.4%)
 *     读 235.86 -> 233.72 KiB/s (-0.9%)
 * 但**写路径源码一个字没动** —— 差别来自「多了一个贯穿 32 cell 的活跃寄存器
 * （奇偶累加器）改变了整个函数的寄存器分配」，写循环的生成代码也跟着变了。
 * 所以这是寄存器分配的副产物，不是奇偶吸收本身的收益。
 * 净收益是**代码体积**：SWD_Transfer 少 14 条指令（-24 B）。
 * 想调吞吐必须动「每 bit 的周期数」，动指令数只在能缩短关键路径时才有用。
 *
 * 寄存器：t0 = 采样临时  t1 = SWD_CLK_SET  %[par] = 奇偶累加器
 */
DAP_HIGHCODE_SECTION static void SWD_GPIO_IMPL(swd_fast_read_data)(uint32_t *value, uint32_t *parity) {
    uint32_t val = 0U;
    uint32_t par = 0U;
    volatile uint32_t *const pa = &SWD_PORT->BSHR;

    /* 逐行对照的原 C（合并前的循环版）：
     *
     *   for (n = 8U; n; n--) {
     *     PIN_SWCLK_CLR_READ();               // = PIN_SWCLK_TCK_CLR()
     *     sample = PIN_SWD_PORT->INDR;        // 下降沿后紧贴采样
     *     packed  = (sample >> SWD_DIO_BIT) & 1U;
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
        SWD_ASM_PIN_CONSTANTS
        "li   t1, SWD_CLK_SET\n\t"        /* t1 = 后面每拍「置 SWCLK」的立即数 */
        ".set rb_k, 0\n\t"                /* rb_k = 当前在收第几位（0..31），GAS 编译期计数 */
        ".rept 32\n\t"                    /* 展开 32 位 —— 每 bit 就是一轮（原 C 的 n 循环）*/
        "sw   t1, 4(%[pa])\n\t"           /* 下降沿：原 C PIN_SWCLK_CLR_READ()（BCR = PIN_SWCLK）*/
        "lw   t0, -8(%[pa])\n\t"          /* 采样：原 C sample = PIN_SWD_PORT->INDR（-8 = INDR 偏移）*/
        "andi t0, t0, SWD_DIO_SET\n\t"    /* 取 SWDIO：留着原位不右移，下面按 rb_k 左/右移时正好落位 */
        "sw   t1, 0(%[pa])\n\t"           /* 上升沿：原 C PIN_SWCLK_SET_READ()（BSHR = PIN_SWCLK）*/
        "xor  %[par], %[par], t0\n\t"     /* 奇偶：原 C 循环后的 SWD_Parity32(val)。此时 t0 还是
                                           * SWD_DIO_SET/0，所以 32 次异或全落在 par 的该位，最后 >> 即得奇偶 */
        ".if rb_k < SWD_DIO_BIT\n\t"      /* ┐ 把该位挪到 val 的第 rb_k 位： */
        "srli t0, t0, (SWD_DIO_BIT - rb_k)\n\t" /* │ 掩码位之前的位右移 (SWD_DIO_BIT - rb_k) */
        ".elseif rb_k == SWD_DIO_BIT\n\t" /* │ 正好在 API 位上时不动 */
        "nop\n\t"                         /* │ */
        ".else\n\t"                       /* │ 之后的位左移 (rb_k - SWD_DIO_BIT) */
        "slli t0, t0, (rb_k - SWD_DIO_BIT)\n\t" /* ┘ 等价于原 C 的「packed << 28 | val >> 4」位打包 */
        ".endif\n\t"                      /*   （原 C 靠移位寄存器攒 4 位，这里每位移位量是常量，直接到位）*/
        "or   %[val], %[val], t0\n\t"     /* 收进 val：原 C 的 val |= (packed << 28) */
        ".set rb_k, rb_k + 1\n\t"         /* 编译器计数器 +1（检查：只算 val[0..31]，32 位无越界）*/
        ".endr\n\t"                       /* 展开结束 */
        : [val] "+r"(val), [par] "+r"(par)
        : [pa] "r"(pa)
        : "t0", "t1", "memory");

    *value = val;
    /* 累加器的 bit SWD_DIO_BIT 就是 32 位数据的奇偶（见上面说明） */
    *parity = (par >> SWD_DIO_BIT) & 1U; /* 原 C SWD_Parity32 的收尾：& 1U */
}

/*
 * 写数据相位（32 位）：内联汇编 + 按位常量掩码取位
 *
 * 为什么必须用汇编：用 C 写时 GCC 会把「算下一位 BSHR 值」的 ALU 重新贴到
 * 下降沿那条 store **之前**，于是低电平期 0 条指令（实测 20 ns）、高电平期
 * 5 条（120 ns），占空比 6:1。volatile + memory clobber 才能钉住顺序。
 *
 * 用「按位常量掩码」而不是逐位 `val >>= 1`：
 *   shift_k = 第 k 位 << 4 = 0 或 16
 *     k <  4 : (val << (4 - k)) & 16
 *     k == 4 : val & 16
 *     k >  4 : (val >> (k - 4)) & 16
 * 两种写法取出 shift 都是 2 条指令，但常量掩码**不必推进 val**，
 * 于是每 bit 少一条 `srli val, val, 1`（32 个 bit 共省 32 条）。
 * 代价是每个 bit 的立即数不同，用 GAS 的 .set + .if 按 k 生成。
 *
 * 每 bit 的排布（ALU 拆 2+2，让占空比接近 1:1）：
 *   ① sw t0 : 清 SWCLK + 按位设/清 SWDIO  （下降沿；t0 在上一轮已算好）
 *   -- 低电平期：slli/andi 2 条 --
 *   ② sw hi : 置 SWCLK                    （上升沿）
 *   -- 高电平期：srl/or 2 条 + 可调 NOP --
 *
 * 这 4 条 ALU 只依赖 val，不依赖①要写的 t0，所以拆到上升沿两侧**不会**引入
 * 额外停顿；而 or 算完后隔着 nop + sw 才是下一轮的 sw t0，也没写后写气泡。
 * 想继续调：把 srl/or 之一搬到②之前，或删掉 nop。
 *
 * 寄存器：t0 = 当前位 BSHR 值  t2 = SWD_DIO_CLR  t3 = SWD_CLK_CLR
 */
DAP_HIGHCODE_SECTION static void SWD_GPIO_IMPL(swd_fast_write_data)(uint32_t value) {
    volatile uint32_t *const pa = &SWD_PORT->BSHR;

    /* 逐行对照的原 C（合并前的写法）：
     *
     *   PIN_SWCLK_CLR_SWDIO_OUT(val);   // 每 bit：BSHR = (SWCLK<<16) | (SWDIO<<16) >> ((val&1)<<4)
     *   PIN_SWCLK_SET();               // BSHR = SWD_CLK_SET
     *   最后 PIN_SWCLK_CLR_SWDIO_OUT(parity) 送奇偶位
     *
     * 每 cell 7 条指令、低 2 / 高 3 → 8 个 clk（低 4 / 高 4），与读同构。
     */
    __asm__ volatile(
        SWD_ASM_PIN_CONSTANTS
        /* === 准备：先算第 0 位的 BSHR 值，作为流水线初值 === */
        "slli t0, %[val], 4\n\t"           /* ┐ shift = (val << 4) */
        "andi t0, t0, 16\n\t"              /* ┘ & 16 = (val & 1) << 4 = 0 或 16（原 C 的 (val&1U)<<4）*/
        "lui  t2, (SWD_DIO_CLR >> 12)\n\t" /* t2 = SWDIO 的复位半字 */
        "srl  t0, t2, t0\n\t"              /* t0 选置位 / 复位半字（= SWDIO 的部分）*/
        "lui  t3, (SWD_CLK_CLR >> 12)\n\t" /* t3 = SWCLK 的复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成第 0 位的值 ≈ 原 C PIN_SWCLK_CLR_SWDIO_OUT(val) */
        ".set wb_k, 0\n\t"                 /* wb_k = 当前正在输出的位（0..31），GAS 编译期计数 */
        ".rept 32\n\t"                     /* 展开 32 位 */
        "sw   t0, 0(%[pa])\n\t"            /* 下降沿 + 写 SWDIO（t0 是上一轮低电平期算好的）*/
        ".if (wb_k + 1) < 4\n\t"           /* ┐ 算**下一位**(wb_k+1)的 shift 量（0 或 16）： */
        "slli t0, %[val], (3 - wb_k)\n\t"  /* │ wb_k+1 < 4：(val << (3-wb_k)) 再 & 16 取该位 */
        ".elseif (wb_k + 1) == 4\n\t"      /* │ wb_k+1 == 4：直接用 val & 16（省掉一次 slli）*/
        "andi t0, %[val], 16\n\t"          /* │ */
        ".else\n\t"                        /* │ wb_k+1 > 4：(val >> (wb_k-3)) 再 & 16 */
        "srli t0, %[val], (wb_k - 3)\n\t"  /* ┘ 等价于原 C 的 (val >> k) & 1U 后 << 4 */
        ".endif\n\t"
        ".if (wb_k + 1) != 4\n\t"          /* ┐ wb_k+1 == 4 那条已经是 & 16 的结果，不用再掩码 */
        "andi t0, t0, 16\n\t"              /* ┘ */
        ".endif\n\t"
        "sw   %[hi], 0(%[pa])\n\t"         /* 上升沿：原 C PIN_SWCLK_SET()（%[hi] = SWD_CLK_PIN）*/
        "srl  t0, t2, t0\n\t"              /* 选置位 / 复位半字 */
        "or   t0, t0, t3\n\t"              /* 合成下一位的 BSHR 值（留到下一轮下降沿用）*/
        "nop\n\t"                          /* 高电平期第 3 个 slot（占空比旋钮）*/
        ".set wb_k, wb_k + 1\n\t"          /* 计数器 +1 */
        ".endr\n\t"
        :
        : [val] "r"(value), [pa] "r"(pa), [hi] "r"((uint32_t)SWD_CLK_PIN)
        : "t0", "t2", "t3", "memory");
}


// ---------------------------------------------------------------------------
// GPIO 描述符：数据全部来自 src/swd_pin_map.h（引脚的唯一来源）
// ---------------------------------------------------------------------------
static const swd_gpio_desc_t SWD_GPIO_IMPL(swd_gpio_x035_desc) = {
    // 名字里带上引脚方案，便于诊断时一眼看出当前绑的是哪套脚
    .name = "ch32x035-gpio-fast/" SWD_PIN_VARIANT_STR,
    .port = SWD_PORT,
    .clk_pin = (uint32_t)SWD_CLK_PIN,
    .dio_pin = (uint32_t)SWD_DIO_PIN,
    .clk_shift = (uint8_t)SWD_CLK_CFGLR_SHIFT,
    .dio_shift = (uint8_t)SWD_DIO_CFGLR_SHIFT,
};

// 端口操作直接复用板级原语（src/swd_pin_map.h 的 board_swd_port_on/off），
// 不再在这里重复一遍寄存器写法 —— 换板只改引脚表，两个使用方（DAP_config.h
// 的 PORT_SWD_SETUP/PORT_OFF 与本实现）自动保持一致。
static void SWD_GPIO_IMPL(x035_port_setup)(void) {
    board_swd_port_on();
}

static void SWD_GPIO_IMPL(x035_port_off)(void) {
    board_swd_port_off();
}

// DIO 方向切换同样复用板级原语
static void SWD_GPIO_IMPL(x035_dio_out_enable)(void) {
    board_swd_dio_output();
}

static void SWD_GPIO_IMPL(x035_dio_out_disable)(void) {
    board_swd_dio_input();
}

// ---------------------------------------------------------------------------
// 实现表 + 默认绑定
// ---------------------------------------------------------------------------
const swd_gpio_if_t swd_gpio_if_x035 = {
    .desc = &SWD_GPIO_IMPL(swd_gpio_x035_desc),
    .port_setup = SWD_GPIO_IMPL(x035_port_setup),
    .port_off = SWD_GPIO_IMPL(x035_port_off),
    .dio_out_enable = SWD_GPIO_IMPL(x035_dio_out_enable),
    .dio_out_disable = SWD_GPIO_IMPL(x035_dio_out_disable),
    .packet_request = SWD_GPIO_IMPL(swd_fast_packet_request),
    .read_data = SWD_GPIO_IMPL(swd_fast_read_data),
    .write_data = SWD_GPIO_IMPL(swd_fast_write_data),
};

// 默认就用芯片内置实现：不调用 swd_gpio_bind() 也能正常工作
const swd_gpio_if_t *swd_gpio_current = &swd_gpio_if_x035;

void swd_gpio_bind(const swd_gpio_if_t *iface) {
    swd_gpio_current = (iface != NULL) ? iface : &swd_gpio_if_x035;
}
