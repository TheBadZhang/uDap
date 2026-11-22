/* ---------------------------------------------------------------------------
 * swd_fast_gpio.S — RAM 常驻的 SWD 位翻转内核（AT32F405，Cortex-M4）
 *
 * 目标：每个 SWCLK 周期 ≈8 个 CPU 周期（216 MHz → 27 MHz），全程不碰 Flash。
 * 做法：整段放在 .ramcode 段（链接脚本把它塞进 .data，启动时随 _sidata→_sdata
 *       一起搬进 RAM），内层用 SCR 寄存器的"置位/清零同写"特性：
 *         GPIOA->scr = (clear_mask << 16) | set_mask
 *       一次 32 位写就把 SWDIO 摆到下一拍、同时把 SWCLK 拉低（= 下降沿），
 *       于是每个 bit 只要两次 store。
 *
 * SCR 语义（at32f402_405_gpio.h）：bit[15:0] 写 1 置位 ODT，bit[31:16] 写 1 清零。
 * 关键坑：SCR 只影响"写到的位"，所以 bit=0 时必须显式写 DIO 的清零位，
 *         否则 SWDIO 会保持上一次电平（表现为目标永远回 No ACK）。
 *
 * 引脚（board_pin_map.h）：PA5=SWCLK、PA7=SWDIO(主控驱动)、PA6=SWDIO(主控采样)，
 * 板上 PA6/PA7 短接。
 *
 * 数据相位的写法参考 ch32x035 的 swd_gpio_x035.c：
 *   - 完全展开（.rept + .set），没有循环分支，每 bit 的指令条数就是时序；
 *   - 软件流水：下降沿那条 store 用的是**上一拍算好**的完整 SCR 字，本拍下降沿
 *     之后才去取下一拍的数据位（低电平期 2 条 ALU），高电平期把下一拍的字拼好
 *     （2 条 ALU）。选"置位/清零半字"用一次 `lsr 常量, 0|16` 完成，不需要分支。
 *   - 于是每 bit = 2 次 store + 4 条 ALU ≈ 8 个周期，占空比 1:1。
 * ------------------------------------------------------------------------- */


    .syntax unified
    .cpu    cortex-m4
    .thumb

    .equ GPIOA_SCR,   0x40020018
    .equ GPIOA_IDT,   0x40020010
    .equ SCK_SET,     0x00000020
    .equ SCK_CLR,     0x00200000
    .equ DIO_SET,     0x00000080
    .equ DIO_CLR,     0x00800000
    .equ SWD_DIN_BIT, 6
    .equ SWD_DIO_BIT, 7

/* 每个 bit 的指令条数 = SWCLK 周期数（216 MHz / 8 = 27 MHz）。
 * 写相位：2 store + 4 ALU = 6 条，靠 NOP 补到 8 条；
 * 读相位：2 store + 采样 + 移位 + or = 6 条，采样后要等目标把数据摆稳。 */
    .equ SWD_NOP_FALL, 2
    .equ SWD_NOP_RISE, 0
/* 下降沿到采样的间隔（目标 tDVOD 余量；27 MHz 下半周期只有 4 个周期） */
    .equ SWD_NOP_RX, 2

/* 读数据相位采样时机（ARM ADIv5 / DSTREAM 规范）：
 *   host 在 SWCLK 下降沿更新 SWDIO（写），在 SWCLK 上升沿读取 SWDIO（读）。
 *   => 目标在下降沿之后驱动数据，host 必须在上升沿（及其之后）采样，
 *      才能保证 T_is(>=4 ns) 的建立时间。 */
    .equ SWD_RX_ON_RISE, 1
    .equ SWD_RX_LOW, 4
    .equ SWD_RX_HI, 0

/* 把 r4 里的采样位（在 SWD_DIO_BIT 位上）挪到目标位 target */
    .macro SWD_SHIFT_TO target
    .if \target > SWD_DIO_BIT
    lsl     r4, r4, #(\target - SWD_DIO_BIT)
    .elseif \target == SWD_DIO_BIT
    nop
    .else
    lsr     r4, r4, #(SWD_DIO_BIT - \target)
    .endif
    .endm


    .section .ramcode, "ax", %progbits
    .align 2

/* ---------------------------------------------------------------------------
 * void swd_fast_tx_bits(uint32_t value, uint32_t nbits)
 *   LSB 先发。32 / 8 位走展开流水线，其它长度走通用循环。
 *
 *   每拍流水线的寄存器分工：
 *     r2 = GPIOA->scr        r3 = SCK_CLR          r7 = SCK_SET
 *     r6 = DIO_CLR           r4 = 本拍要写的完整 SCR 字
 *     r5 = 0 / 16（选择置位还是清零半字）
 * ------------------------------------------------------------------------- */
    .globl  swd_fast_tx_bits
    .thumb_func
    .type   swd_fast_tx_bits, %function
swd_fast_tx_bits:
    cmp     r1, #32
    beq     .Ltx32
    cmp     r1, #8
    beq     .Ltx8
    b       .Ltx_loop

/* --- 32 位：完全展开，每 bit 4 条指令 / 2 次 store ---
 *
 * 关键：不再"拼字再存储"，而是把两个候选字（清零半字 / 置位半字，都或上了
 * SWCLK 的清零位）预先放在 r4 / r7，用 `lsrs r0, #1` 把当前 bit 移进进位标志，
 * 再用 IT 块里的两条条件 store 二选一：
 *
 *     lsrs  r0, r0, #1     ; C = 当前 bit（LSB 先发）
 *     ite   cc
 *     strcc r4, [r2]       ; bit==0：SWDIO 清零 + SWCLK 下降沿（一次写）
 *     strcs r7, [r2]       ; bit==1：SWDIO 置位 + SWCLK 下降沿（一次写）
 *     str   r3, [r2]       ; SWCLK 上升沿
 *
 * 相比"取位 → 0/16 → 选半字 → 拼字"四条 ALU，少了三条 ALU 和一条依赖链，
 * 每 bit 从 14.6 个周期降到约 10 个周期（≈21 MHz）。
 * ------------------------------------------------------------------------- */
.Ltx32:
    push    {r4, r5, r6, r7}
    movw    r2, #(GPIOA_SCR & 0xFFFF)
    movt    r2, #(GPIOA_SCR >> 16)     /* r2 = GPIOA->scr */
    movw    r4, #((DIO_CLR | SCK_CLR) & 0xFFFF)
    movt    r4, #((DIO_CLR | SCK_CLR) >> 16)   /* r4 = SWDIO 清零 + SWCLK 下降沿 */
    movw    r7, #((DIO_SET | SCK_CLR) & 0xFFFF)
    movt    r7, #((DIO_SET | SCK_CLR) >> 16)   /* r7 = SWDIO 置位 + SWCLK 下降沿 */
    movs    r3, #SCK_SET               /* r3 = SWCLK 上升沿 */
    .set    k, 0
    .rept   32
    lsrs    r0, r0, #1                 /* C = 当前数据位 */
    ite     cc
    strcc   r4, [r2]                   /* ① 数据=0 + 下降沿 */
    strcs   r7, [r2]                   /* ① 数据=1 + 下降沿 */
    str     r3, [r2]                   /* ② 上升沿 */
    .set    k, k + 1
    .endr
    pop     {r4, r5, r6, r7}
    bx      lr

/* --- 8 位（请求字节）：同一套展开写法，展开 8 次 --- */
.Ltx8:
    push    {r4, r5, r6, r7}
    movw    r2, #(GPIOA_SCR & 0xFFFF)
    movt    r2, #(GPIOA_SCR >> 16)
    movw    r4, #((DIO_CLR | SCK_CLR) & 0xFFFF)
    movt    r4, #((DIO_CLR | SCK_CLR) >> 16)
    movw    r7, #((DIO_SET | SCK_CLR) & 0xFFFF)
    movt    r7, #((DIO_SET | SCK_CLR) >> 16)
    movs    r3, #SCK_SET
    .set    n, 0
    .rept   8
    lsrs    r0, r0, #1
    ite     cc
    strcc   r4, [r2]
    strcs   r7, [r2]
    str     r3, [r2]
    .set    n, n + 1
    .endr
    pop     {r4, r5, r6, r7}
    bx      lr

/* ---------------------------------------------------------------------------
 * 热路径入口：寄存器由调用方（SWD_Transfer）准备好，函数体里一条常量加载都
 * 不做，也没有 push/pop —— 目的就是把 ACK 到数据相位之间那段空档压到最短。
 *
 *   void swd_fast_tx32_hot(uint32_t value, uint32_t scr,
 *                          uint32_t word_clr, uint32_t word_set)
 *   void swd_fast_tx8_hot (uint32_t value, uint32_t scr,
 *                          uint32_t word_clr, uint32_t word_set)
 *
 * r0=value r1=scr r2=SWDIO 清零+下降沿 r3=SWDIO 置位+下降沿，r12=上升沿
 * （r12 是 IP 寄存器，调用方按 ABI 视为易失，不需要保存）。
 * ------------------------------------------------------------------------- */
    .globl  swd_fast_tx32_hot
    .thumb_func
    .type   swd_fast_tx32_hot, %function
.section .text.swdfast, "ax", %progbits
swd_fast_tx32_hot:
    movs    r12, #SCK_SET
    .rept   32
    lsrs    r0, r0, #1
    ite     cc
    strcc   r2, [r1]
    strcs   r3, [r1]
    .rept   SWD_NOP_FALL
    nop
    .endr
    str     r12, [r1]
    .rept   SWD_NOP_RISE
    nop
    .endr
    .endr
    bx      lr
    .size   swd_fast_tx32_hot, . - swd_fast_tx32_hot

    .globl  swd_fast_tx8_hot
    .thumb_func
    .type   swd_fast_tx8_hot, %function
swd_fast_tx8_hot:
    movs    r12, #SCK_SET
    .rept   8
    lsrs    r0, r0, #1
    ite     cc
    strcc   r2, [r1]
    strcs   r3, [r1]
    .rept   SWD_NOP_FALL
    nop
    .endr
    str     r12, [r1]
    .rept   SWD_NOP_RISE
    nop
    .endr
    .endr
    bx      lr
    .size   swd_fast_tx8_hot, . - swd_fast_tx8_hot

/* --- 通用循环（1~3 位、33 位等很少走到的长度） --- */
    .section .ramcode, "ax", %progbits
.Ltx_loop:
    push    {r4, r5, r6, r7}
    movs    r4, r0                     /* 待发数据 */
    movs    r2, #0x18
    movt    r2, #0x4002                /* r2 = GPIOA->scr */
    movs    r3, #SCK_SET               /* r3 = 上升沿 */
    movs    r0, #SCK_SET
    lsls    r0, r0, #16                /* r0 = 下降沿（只清 SCK） */
    movs    r6, #DIO_SET
    lsls    r6, r6, #16                /* r6 = DIO 清零 */
    orrs    r6, r0                     /* r6 = DIO_CLR | SCK_CLR */
    movs    r7, #DIO_SET               /* r7 = DIO 置位 */
    orrs    r7, r0                     /* r7 = DIO_SET | SCK_CLR */
1:
    lsrs    r4, r4, #1                 /* C = 当前 bit */
    mov     r5, r6
    it      cs
    movcs   r5, r7
    str     r5, [r2]                   /* ① 数据 + 下降沿 */
    .rept SWD_NOP_FALL
    nop
    .endr
    str     r3, [r2]                   /* ② 上升沿 */
    .rept SWD_NOP_RISE
    nop
    .endr
    subs    r1, r1, #1
    bne     1b
    pop     {r4, r5, r6, r7}
    bx      lr
    .size   swd_fast_tx_bits, . - swd_fast_tx_bits


/* ---------------------------------------------------------------------------
 * uint32_t swd_fast_rx_bits(uint32_t nbits)
 *   返回右对齐的采样值（LSB = 第一位采到的）。采样点在下降沿之后、
 *   上升沿之前（与 cherrydap SW_DP.c 的 SW_READ_BIT 一致）。
 *
 *   32 位热路径同样完全展开，并且把每一位直接 or 进最终位置（常量移位），
 *   省掉逐位推进/循环分支：每 bit 只有 2 次 store + 采样 + 移位 + or。
 * ------------------------------------------------------------------------- */
    .globl  swd_fast_rx_bits
    .thumb_func
    .type   swd_fast_rx_bits, %function
    .section .text.swdfast, "ax", %progbits
swd_fast_rx_bits:
    cmp     r0, #32
    beq     .Lrx32
    cmp     r0, #3
    beq     .Lrx3
    b       .Lrx_loop

/* --- 32 位：展开 --- */
.Lrx32:
    push    {r4, r5, r6, r7}
    movw    r2, #(GPIOA_SCR & 0xFFFF)
    movt    r2, #(GPIOA_SCR >> 16)     /* r2 = GPIOA->scr */
    movw    r6, #(GPIOA_IDT & 0xFFFF)
    movt    r6, #(GPIOA_IDT >> 16)     /* r6 = GPIOA->idt */
    movw    r5, #(SCK_CLR & 0xFFFF)
    movt    r5, #(SCK_CLR >> 16)       /* r5 = 下降沿 */
    movs    r7, #SCK_SET               /* r7 = 上升沿 */
    movs    r0, #0                     /* 累加器 */
    /* 流水线：ldr 提前一拍发出，用上一拍的 4 条 ALU 掩盖 APB 读延迟 */
    str     r5, [r2]                   /* 第 0 位下降沿 */
    .rept SWD_RX_LOW
    nop
    .endr
    ldr     r4, [r6]                   /* 采样第 0 位 */
    .rept   31
    str     r7, [r2]                   /* 第 k 位上升沿 */
    and     r4, r4, #DIO_SET
    lsls    r4, r4, #(31 - SWD_DIO_BIT)
    lsrs    r0, r0, #1
    orrs    r0, r0, r4
    str     r5, [r2]                   /* 第 k+1 位下降沿 */
    .rept SWD_RX_LOW
    nop
    .endr
    ldr     r4, [r6]                   /* 采样第 k+1 位 */
    .endr
    str     r7, [r2]                   /* 第 31 位上升沿 */
    and     r4, r4, #DIO_SET
    lsls    r4, r4, #(31 - SWD_DIO_BIT)
    lsrs    r0, r0, #1
    orrs    r0, r0, r4
    pop     {r4, r5, r6, r7}
    bx      lr

/* --- 3 位（读 ACK）：结果放在 bit29..bit31，调用方 >>29 取值 --- */
.Lrx3:
    push    {r4, r5, r6, r7}
    movs    r2, #0x18
    movt    r2, #0x4002                /* r2 = GPIOA->scr */
    movs    r6, #0x10
    movt    r6, #0x4002                /* r6 = GPIOA->idt */
    movs    r5, #SCK_CLR
    movs    r7, #SCK_SET
    movs    r0, #0
    .set    m, 0
    .rept   3
    str     r5, [r2]
    .rept SWD_NOP_RX
    nop
    .endr
    ldr     r4, [r6]
    and     r4, r4, #DIO_SET
    str     r7, [r2]
    SWD_SHIFT_TO (29 + m)
    orrs    r0, r0, r4
    .set    m, m + 1
    .endr
    pop     {r4, r5, r6, r7}
    bx      lr

/* --- 通用循环（1 位奇偶校验位、SWJ_Sequence 等） ---
 * 结果与 C 内核同构：第一位落在 bit31，逐位右移（左对齐），
 * 调用方用 k_rx(k) >> (32-k) 取有效位。 */
.Lrx_loop:
    push    {r4, r5, r6, r7}
    movs    r1, r0                     /* 计数 */
    movs    r0, #0                     /* 累加器 */
    movs    r2, #0x10
    movt    r2, #0x4002                /* r2 = GPIOA->idt */
    movs    r5, #0x18
    movt    r5, #0x4002                /* r5 = GPIOA->scr */
    movs    r3, #SCK_SET
    movs    r6, #SCK_CLR
2:
    str     r6, [r5]                   /* 下降沿：目标更新数据 */
    .rept SWD_RX_LOW
    nop
    .endr
    str     r3, [r5]                   /* 上升沿：host 在此刻读取 SWDIO */
    ldr     r4, [r2]                   /* 上升沿之后采样 */
    and     r4, r4, #DIO_SET           /* bit7 → 0/0x80 */
    lsrs    r4, r4, #7                 /* 0/1 */
    lsrs    r0, r0, #1                 /* 右移，最高位空出来 */
    lsls    r4, r4, #31                /* 放到 bit31（左对齐） */
    orrs    r0, r0, r4
    .rept SWD_RX_HI
    nop
    .endr
    subs    r1, r1, #1
    bne     2b
    pop     {r4, r5, r6, r7}
    bx      lr
    .size   swd_fast_rx_bits, . - swd_fast_rx_bits


/* ---------------------------------------------------------------------------
 * void swd_fast_clock_n(uint32_t nbits)
 *   只打时钟、不碰 SWDIO（转向、idle、WAIT/FAULT 后的空转都用它）。
 * ------------------------------------------------------------------------- */
    .globl  swd_fast_clock_n
    .thumb_func
    .type   swd_fast_clock_n, %function
    .section .ramcode, "ax", %progbits
swd_fast_clock_n:
    push    {r4, r5}
    movs    r1, r0
    movs    r2, #0x18
    movt    r2, #0x4002                /* r2 = GPIOA->scr */
    movs    r3, #SCK_SET
    movs    r4, #SCK_CLR
3:
    str     r4, [r2]
    .rept SWD_NOP_FALL
    nop
    .endr
    str     r3, [r2]
    .rept SWD_NOP_RISE
    nop
    .endr
    subs    r1, r1, #1
    bne     3b
    pop     {r4, r5}
    bx      lr
    .size   swd_fast_clock_n, . - swd_fast_clock_n

/* ---------------------------------------------------------------------------
 * 慢速档内核：同样在 RAM、同样完全展开，但每个半周期之间插一段**运行时**
 * 可编程的延时循环（次数由 DAP_Data.clock_delay 换算而来）。
 *
 * 这样低速请求也不再依赖 Flash 里的 C 宏（AT32F405 在 216 MHz 下有 6 个
 * 等待周期，逐拍取指会明显拖慢），做到"一套实现、两档节拍"。
 *
 * 延时循环：movs ip,n / 1: subs ip,#1 / bne 1b  —— 约 2~3 周期/次。
 * ------------------------------------------------------------------------- */

    .macro SWD_DLY_A
    cbz     r7, 1f
    movs    ip, r7
0:  subs    ip, ip, #1
    bne     0b
1:
    .endm

    .macro SWD_TX_BIT_D
    lsrs    r0, r0, #1
    ite     cc
    strcc   r4, [r2]                    /* 数据位 0 + 下降沿 */
    strcs   r5, [r2]                    /* 数据位 1 + 下降沿 */
    SWD_DLY_A
    str     r6, [r2]                    /* 上升沿 */
    SWD_DLY_A
    .endm

/* void swd_fast_tx_bits_d(uint32_t value, uint32_t nbits, uint32_t dly) */
    .globl  swd_fast_tx_bits_d
    .thumb_func
    .type   swd_fast_tx_bits_d, %function
swd_fast_tx_bits_d:
    push    {r4, r5, r6, r7}
    mov     r7, r2
    movs    r2, #0x18
    movt    r2, #0x4002                  /* r2 = GPIOA->scr */
    movw    r4, #0x0000
    movt    r4, #0x00A0                  /* DIO_CLR | SCK_CLR */
    movw    r5, #0x0080
    movt    r5, #0x0020                  /* DIO_SET | SCK_CLR */
    movs    r6, #SCK_SET
    cmp     r1, #32
    beq     .Ltxd32
    cmp     r1, #8
    beq     .Ltxd8
.Ltxd_loop:
    SWD_TX_BIT_D
    subs    r1, r1, #1
    bne     .Ltxd_loop
    b       .Ltxd_done
.Ltxd32:
    .rept 32
    SWD_TX_BIT_D
    .endr
    b       .Ltxd_done
.Ltxd8:
    .rept 8
    SWD_TX_BIT_D
    .endr
.Ltxd_done:
    pop     {r4, r5, r6, r7}
    bx      lr
    .size   swd_fast_tx_bits_d, . - swd_fast_tx_bits_d

/* uint32_t swd_fast_rx_bits_d(uint32_t nbits, uint32_t dly)
 *   返回值右对齐（bit[nbits-1:0]），由调用方左对齐。 */
    .globl  swd_fast_rx_bits_d
    .thumb_func
    .type   swd_fast_rx_bits_d, %function
swd_fast_rx_bits_d:
    push    {r4, r5, r6, r7, r8, r9}
    mov     r7, r1
    movs    r2, #0x10
    movt    r2, #0x4002                  /* r2 = GPIOA->idt */
    movs    r3, #0x18
    movt    r3, #0x4002                  /* r3 = GPIOA->scr */
    movs    r5, #SCK_CLR
    movs    r6, #SCK_SET
    movs    r8, #DIO_SET
    movs    r9, #0
    mov     r9, r0                       /* r9 = nbits */
    movs    r0, #0                       /* r0 = 累加结果 */
    mov     r1, r9
    cmp     r1, #32
    beq     .Lrxd32
    cmp     r1, #3
    beq     .Lrxd3
.Lrxd_loop:
    str     r5, [r3]
    SWD_DLY_A
    ldr     r4, [r2]
    ands    r4, r4, r8
    str     r6, [r3]
    orrs    r0, r0, r4
    SWD_DLY_A
    lsrs    r0, r0, #0
    subs    r1, r1, #1
    bne     .Lrxd_loop
    b       .Lrxd_done
.Lrxd32:
    .set rxd_k, 0
    .rept 32
    str     r5, [r3]
    SWD_DLY_A
    ldr     r4, [r2]
    ands    r4, r4, r8
    str     r6, [r3]
    SWD_SHIFT_TO rxd_k
    orrs    r0, r0, r4
    SWD_DLY_A
    .set rxd_k, rxd_k + 1
    .endr
    b       .Lrxd_done
.Lrxd3:
    .set rxd_k, 0
    .rept 3
    str     r5, [r3]
    SWD_DLY_A
    ldr     r4, [r2]
    ands    r4, r4, r8
    str     r6, [r3]
    SWD_SHIFT_TO rxd_k
    orrs    r0, r0, r4
    SWD_DLY_A
    .set rxd_k, rxd_k + 1
    .endr
.Lrxd_done:
    pop     {r4, r5, r6, r7, r8, r9}
    bx      lr
    .size   swd_fast_rx_bits_d, . - swd_fast_rx_bits_d

/* void swd_fast_clock_n_d(uint32_t nbits, uint32_t dly) */
    .globl  swd_fast_clock_n_d
    .thumb_func
    .type   swd_fast_clock_n_d, %function
swd_fast_clock_n_d:
    push    {r4, r5, r6, r7}
    mov     r7, r1
    movs    r2, #0x18
    movt    r2, #0x4002
    movs    r4, #SCK_CLR
    movs    r5, #SCK_SET
    mov     r6, r0
.Lckd_loop:
    str     r4, [r2]
    SWD_DLY_A
    str     r5, [r2]
    SWD_DLY_A
    subs    r6, r6, #1
    bne     .Lckd_loop
    pop     {r4, r5, r6, r7}
    bx      lr
    .size   swd_fast_clock_n_d, . - swd_fast_clock_n_d

/* ---------------------------------------------------------------------------
 * 中间档（≈13.5 MHz）：与 27 MHz 档同一套「条件存储」结构，但每个半周期插
 * 固定数量的 NOP（编译期常量，不是运行时循环），所以节拍完全均匀。
 * 只加宽写相位：读相位本来就是 ~21 周期/bit，已经够慢，不需要再压。
 * ------------------------------------------------------------------------- */
    .equ SWD_MID_PAD, 3

    .macro SWD_TX_UNROLL_M nbits
    .rept \nbits
    lsrs    r0, r0, #1
    ite     cc
    strcc   r2, [r1]
    strcs   r3, [r1]
    .rept   SWD_MID_PAD
    nop
    .endr
    str     r12, [r1]
    .rept   SWD_MID_PAD
    nop
    .endr
    .endr
    .endm

/* void swd_fast_tx32_mid(uint32_t value, uint32_t scr,
 *                        uint32_t word_clr, uint32_t word_set) */
    .globl  swd_fast_tx32_mid
    .thumb_func
    .type   swd_fast_tx32_mid, %function
swd_fast_tx32_mid:
    movs    r12, #SCK_SET
    SWD_TX_UNROLL_M 32
    bx      lr
    .size   swd_fast_tx32_mid, . - swd_fast_tx32_mid

/* void swd_fast_tx8_mid(uint32_t value, uint32_t scr,
 *                       uint32_t word_clr, uint32_t word_set) */
    .globl  swd_fast_tx8_mid
    .thumb_func
    .type   swd_fast_tx8_mid, %function
swd_fast_tx8_mid:
    movs    r12, #SCK_SET
    SWD_TX_UNROLL_M 8
    bx      lr
    .size   swd_fast_tx8_mid, . - swd_fast_tx8_mid


/* ===========================================================================
 * 27 MHz 档「整字」内核：一次调用 = 请求 8 拍 + 松手 + 转向 + 3 拍 ACK +
 * （ACK 正常时）转向 + 32 拍数据 + 1 拍奇偶，全部展开、无循环、无 C 胶水。
 *
 * 参数块 swd_fast_hw[]（C 侧定义，这里只按固定偏移取）：
 *   0 scr 4 w_clr 8 w_set 12 idt 16 sck_clr 20 sck_set 24 cfgr 28 dio_in 32 dio_out
 *
 * 寄存器分工：
 *   r4=scr  r12=SCK_SET  r9=SCK_CLR  r5=W_CLR  r8=W_SET  r10=idt  r11=hw
 *   r6=待发数据（每拍 lsrs 一次，进位即当前 bit）  r7=临时  r1=ACK 累加
 * ========================================================================= */
    .equ HW_SCR,      0
    .equ HW_W_CLR,    4
    .equ HW_W_SET,    8
    .equ HW_IDT,     12
    .equ HW_SCK_CLR, 16
    .equ HW_SCK_SET, 20
    .equ HW_CFGR,    24
    .equ HW_DIO_IN,  28
    .equ HW_DIO_OUT, 32

    .equ SWD_REQ_PAD_A, 3
    .equ SWD_REQ_PAD_B, 2
    .equ SWD_TURN_PAD, 4
    .equ SWD_ACK_PAD, 2
    .equ SWD_ACK_TAIL, 2
    .equ SWD_DATA_PAD_A, 2
    .equ SWD_DATA_PAD_B, 0
    .equ SWD_RX_PAD, 6
    .equ SWD_PAR_PAD, 6
.equ SWD_PAR_LOW, 10
    .equ SWD_WR_TURN, 0

/* 一拍发送：DIO+SCK 拉低（条件 store 二选一）→ 选通建立 → SCK 拉高 → 保持 */
    .macro SWD_BEAT pa, pb
    lsrs    r6, r6, #1
    ite     cc
    strcc   r5, [r4]
    strcs   r8, [r4]
    .rept   \pa
    nop
    .endr
    str     r12, [r4]
    .rept   \pb
    nop
    .endr
    .endm

/* 一拍 ACK 采样：低电平中间采样第 idx 位 */
    .macro SWD_ACK_BEAT idx
    str     r9, [r4]
    .rept   SWD_ACK_PAD
    nop
    .endr
    ldr     r7, [r10]
    and     r7, r7, #(1 << SWD_DIN_BIT)
    lsr     r7, r7, #(SWD_DIN_BIT - \idx)
    orrs    r1, r1, r7
    str     r12, [r4]
    .rept   SWD_ACK_TAIL
    nop
    .endr
    .endm

/* SWD_SHIFT_R reg, target：把 \reg 里 bit6 的采样值挪到第 target 位 */
    .macro SWD_SHIFT_R reg, target
    .if \target > SWD_DIN_BIT
    lsl     \reg, \reg, #(\target - SWD_DIN_BIT)
    .elseif \target == SWD_DIN_BIT
    nop
    .else
    lsr     \reg, \reg, #(SWD_DIN_BIT - \target)
    .endif
    .endm

/* ---------------------------------------------------------------------------
 * uint32_t swd_fast_wr(const uint32_t *hw, uint32_t reqbyte,
 *                      uint32_t value, uint32_t parity)
 *   返回 ACK（bit0..2，与 DAP_TRANSFER_OK/WAIT/FAULT 同序）
 * ------------------------------------------------------------------------- */
    .section .text.swdfast, "ax", %progbits
    .syntax unified
    .cpu    cortex-m4
    .thumb
    .align  2

    .globl  swd_fast_wr
    .thumb_func
    .type   swd_fast_wr, %function
swd_fast_wr:
    push    {r4-r11}
    ldr     r4, [r0, #HW_SCR]
    ldr     r12,[r0, #HW_SCK_SET]
    ldr     r9, [r0, #HW_SCK_CLR]
    ldr     r5, [r0, #HW_W_CLR]
    ldr     r8, [r0, #HW_W_SET]
    ldr     r10,[r0, #HW_IDT]
    mov     r11, r0
    mov     r6, r1

    .rept   8
    SWD_BEAT SWD_REQ_PAD_A, SWD_REQ_PAD_B
    .endr

    /* 松开 SWDIO（cfgr 一次写） */
    ldr     r1, [r11, #HW_CFGR]
    ldr     r7, [r11, #HW_DIO_IN]
    str     r7, [r1]

    /* 1 拍转向 */
    str     r9, [r4]
    .rept   SWD_TURN_PAD
    nop
    .endr
    str     r12,[r4]
    .rept   SWD_TURN_PAD
    nop
    .endr

    /* 3 拍 ACK */
    movs    r1, #0
    SWD_ACK_BEAT 0
    SWD_ACK_BEAT 1
    SWD_ACK_BEAT 2
    cmp     r1, #1
    bne     .Lwr_out

    /* 切回输出：目标读完 ACK 后要一拍转向才让出 SWDIO，再开始驱动数据 */
    ldr     r0, [r11, #HW_CFGR]
    ldr     r7, [r11, #HW_DIO_OUT]
    str     r7, [r0]
    .if     SWD_WR_TURN
    str     r5, [r4]
    .rept   SWD_TURN_PAD
    nop
    .endr
    str     r12,[r4]
    .rept   SWD_TURN_PAD
    nop
    .endr
    .endif

    /* 32 拍数据 + 1 拍奇偶 */
    mov     r6, r2
    .rept   32
    SWD_BEAT SWD_DATA_PAD_A, SWD_DATA_PAD_B
    .endr
    mov     r6, r3
    SWD_BEAT SWD_DATA_PAD_A, SWD_DATA_PAD_B

.Lwr_out:
    mov     r0, r1
    pop     {r4-r11}
    bx      lr
    .size   swd_fast_wr, . - swd_fast_wr

/* ---------------------------------------------------------------------------
 * uint32_t swd_fast_rd(const uint32_t *hw, uint32_t reqbyte, uint32_t *out)
 *   返回 ACK（bit0..2）| 奇偶位 << 4；数据写入 *out
 * ------------------------------------------------------------------------- */
    .globl  swd_fast_rd
    .thumb_func
    .type   swd_fast_rd, %function
swd_fast_rd:
    push    {r4-r11}
    ldr     r4, [r0, #HW_SCR]
    ldr     r12,[r0, #HW_SCK_SET]
    ldr     r9, [r0, #HW_SCK_CLR]
    ldr     r5, [r0, #HW_W_CLR]
    ldr     r8, [r0, #HW_W_SET]
    ldr     r10,[r0, #HW_IDT]
    mov     r11, r0
    mov     r3, r2                      /* 保存 out 指针 */
    mov     r6, r1

    .rept   8
    SWD_BEAT SWD_REQ_PAD_A, SWD_REQ_PAD_B
    .endr

    ldr     r1, [r11, #HW_CFGR]
    ldr     r7, [r11, #HW_DIO_IN]
    str     r7, [r1]

    str     r9, [r4]
    .rept   SWD_TURN_PAD
    nop
    .endr
    str     r12,[r4]
    .rept   SWD_TURN_PAD
    nop
    .endr

    movs    r1, #0
    SWD_ACK_BEAT 0
    SWD_ACK_BEAT 1
    SWD_ACK_BEAT 2
    cmp     r1, #1
    bne     .Lrd_out

    /* ACK 之后的第一拍就是数据 bit0 */
    movs    r2, #0
    .set    k, 0
    .rept   32
    str     r5, [r2]                   /* 下降沿：目标开始驱动下一位 */
    .rept SWD_RX_LOW
    nop                                /* 等边沿落下 + tDVOD 建立 */
    .endr
    ldr     r4, [r6]                   /* 低电平中后段采样 */
    and     r4, r4, #DIO_SET           /* 只留 PA7/PA6（板上级联）这一位 */
    lsls    r4, r4, #(31 - SWD_DIO_BIT)/* -> bit31 */
    lsrs    r0, r0, #1
    orrs    r0, r0, r4
    str     r7, [r2]                   /* 上升沿（采样之后再发，保证建立时间） */
    .rept SWD_RX_HI
    nop
    .endr
    .endr
    ldr     r7, [r10]
    str     r12,[r4]
    and     r7, r7, #(1 << SWD_DIN_BIT)
    SWD_SHIFT_R r7, k
    orrs    r2, r2, r7
    .set    k, k + 1
    .endr

    str     r9, [r4]
    .rept   SWD_PAR_PAD
    nop
    .endr
    ldr     r7, [r10]
    str     r12,[r4]
    and     r7, r7, #(1 << SWD_DIN_BIT)
    lsrs    r7, r7, #SWD_DIN_BIT
    lsls    r7, r7, #4
    orrs    r1, r1, r7
    str     r2, [r3]

.Lrd_out:
    mov     r0, r1
    pop     {r4-r11}
    bx      lr
    .size   swd_fast_rd, . - swd_fast_rd
/* ===================== 快速请求 + ACK 内核（FLASH） ===================== */
    .section .text.swdfast, "ax", %progbits
    .align  2

/* uint32_t swd_fast_req(const swd_fast_hw_type *hw, uint32_t reqbyte)
 *   8 拍请求（与 tx32_hot 同款，8 槽/bit）→ cfgr 释放 SWDIO
 *   → 1 拍 turnaround → 3 拍 ACK（低电平中后段采样，与 C 版同位置）
 *   返回 ack[0..2]；调用方须保证主机 turnaround == 1
 */
    .globl  swd_fast_req
    .thumb_func
    .type   swd_fast_req, %function
swd_fast_req:
    push    {r4-r7, lr}
    mov     r4, r0
    mov     r6, r1
    ldr     r1, [r4, #HW_SCR]
    ldr     r2, [r4, #HW_W_CLR]
    ldr     r3, [r4, #HW_W_SET]
    movs    r12, #SCK_SET
    .rept   8
    lsrs    r6, r6, #1
    ite     cc
    strcc   r2, [r1]
    strcs   r3, [r1]
    .rept   SWD_NOP_FALL
    nop
    .endr
    str     r12, [r1]
    .rept   SWD_NOP_RISE
    nop
    .endr
    .endr

    /* 释放 SWDIO：cfgr 写回输入配置（含上拉） */
    ldr     r5, [r4, #HW_CFGR]
    ldr     r0, [r4, #HW_DIO_IN]
    str     r0, [r5]

    /* 1 个 turnaround 时钟 */
    ldr     r5, [r4, #HW_SCK_CLR]
    str     r5, [r1]
    nop
    nop
    str     r12, [r1]
    nop

    /* 3 拍 ACK */
    ldr     r6, [r4, #HW_IDT]
    movs    r0, #0
    .set    ka, 0
    .rept   3
    str     r5, [r1]
    nop
    nop
    ldr     r7, [r6]
    and     r7, r7, #(1 << SWD_DIN_BIT)
    lsrs    r7, r7, #(SWD_DIN_BIT - ka)
    orrs    r0, r0, r7
    str     r12, [r1]
    nop
    .set    ka, ka + 1
    .endr
    pop     {r4-r7, pc}
    .size   swd_fast_req, . - swd_fast_req
/* ===================== 读整字内核（数据 + 校验位 + 转向 + 切回输出） ===== */
    .section .text.swdfast, "ax", %progbits
    .align  2

/* uint64_t swd_fast_rx_tail(const swd_fast_hw_type *hw)
 *   32 拍流水线采样 → 校验位（低电平后段采样）→ 1 拍转向 → SWDIO 切回输出
 *   返回 r0 = 数据（右对齐）、r1 = 校验位
 */
    .globl  swd_fast_rx_tail
    .thumb_func
    .type   swd_fast_rx_tail, %function
swd_fast_rx_tail:
    push    {r4, r5, r6, r7}
    mov     r4, r0
    ldr     r2, [r4, #HW_SCR]
    ldr     r6, [r4, #HW_IDT]
    ldr     r5, [r4, #HW_SCK_CLR]
    ldr     r7, [r4, #HW_SCK_SET]
    movs    r0, #0

    str     r5, [r2]
    .rept SWD_RX_LOW
    nop
    .endr
    ldr     r3, [r6]
    .rept   31
    str     r7, [r2]                   /* 第 k 位上升沿 */
    and     r3, r3, #DIO_SET
    lsls    r3, r3, #(31 - SWD_DIO_BIT)
    lsrs    r0, r0, #1
    orrs    r0, r0, r3
    str     r5, [r2]                   /* 第 k+1 位下降沿 */
    .rept SWD_RX_LOW
    nop
    .endr
    ldr     r3, [r6]
    .endr
    str     r7, [r2]                   /* 第 31 位上升沿 */
    and     r3, r3, #DIO_SET
    lsls    r3, r3, #(31 - SWD_DIO_BIT)
    lsrs    r0, r0, #1
    orrs    r0, r0, r3

    /* 校验位：目标驱动更晚，低电平多等几拍 */
    str     r5, [r2]
    .rept SWD_PAR_LOW
    nop
    .endr
    ldr     r1, [r6]
    and     r1, r1, #DIO_SET
    lsrs    r1, r1, #SWD_DIO_BIT       /* 校验位 -> bit0 */
    str     r7, [r2]

    /* 1 拍转向 */
    str     r5, [r2]
    nop
    nop
    str     r7, [r2]
    nop

    /* SWDIO 切回输出 */
    ldr     r3, [r4, #HW_CFGR]
    ldr     r12, [r4, #HW_DIO_OUT]
    str     r12, [r3]
    pop     {r4, r5, r6, r7}
    bx      lr
    .size   swd_fast_rx_tail, . - swd_fast_rx_tail