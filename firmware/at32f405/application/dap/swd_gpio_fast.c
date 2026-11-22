/**
 * @file    swd_gpio_fast.c
 * @brief   GPIO 位翻转版 SWD 物理层（汇编内核 + RAM 常驻），--swd_phy=gpio_fast
 *
 * 与 cheerydap 的 DAP/Source/SW_DP.c 提供同一组符号
 * （SWJ_Sequence / SWD_Sequence / SWD_Transfer），所以上层 DAP.c 不用改。
 *
 * 与 gpio（cherrydap 原版）的区别只有两点：
 *   1. 传输函数放到 .ramcode 段，运行时在 RAM 里执行。AT32F405 216 MHz 时
 *      Flash 要 6 个等待周期，位翻转每条指令都从 Flash 取指会被拖慢；
 *   2. 位循环换成 swd_fast_gpio.S 里的汇编内核（一个 bit ≈ 8~10 个 CPU 周期），
 *      C 只剩"组织一次传输的相位"这一层。
 */
#include <stdint.h>

#include "board_pin_map.h"
#include "system_at32f402_405.h"
#include "DAP_config.h"
#include "DAP.h"
#include "swd_fast_gpio.h"

/* C 参考内核用的位延时：快时钟走 NOP 插入，慢时钟走 PIN_DELAY_SLOW 计数。 */
#ifndef PIN_DELAY
#define PIN_DELAY() do { \
        if (DAP_Data.fast_clock) { PIN_DELAY_FAST(); } \
        else { PIN_DELAY_SLOW(DAP_Data.clock_delay); } \
    } while (0)
#endif
#define PIN_SWCLK_SET PIN_SWCLK_TCK_SET
#define PIN_SWCLK_CLR PIN_SWCLK_TCK_CLR

#if (DAP_SWD != 0) && defined(SWD_PHY_GPIO_FAST) && (SWD_PHY_GPIO_FAST != 0)

/* 汇编内核里的引脚是常量，这里保证它和 board_pin_map.h 没有走叉 */
_Static_assert(BOARD_SWCLK_PIN     == (uint32_t)(1UL << SWD_FAST_SCK_BIT),
               "swd_fast_gpio.S 的 SWCLK 引脚与 board_pin_map.h 不一致");
_Static_assert(BOARD_SWDIO_OUT_PIN == (uint32_t)(1UL << SWD_FAST_DIO_BIT),
               "swd_fast_gpio.S 的 SWDIO 输出引脚与 board_pin_map.h 不一致");
_Static_assert(BOARD_SWDIO_IN_PIN  == (uint32_t)(1UL << SWD_FAST_DIN_BIT),
               "swd_fast_gpio.S 的 SWDIO 输入引脚与 board_pin_map.h 不一致");

/* 放进 .ramcode（链接脚本把它塞进 .data，启动时随 .data 拷进 RAM）。
 * noinline 是必须的：一旦被内联进 Flash 里的调用者，就白搬了。 */
#define SWD_RAMF    __attribute__((section(".ramcode"), noinline, used))

/* ---- SWDIO 方向切换的 cfgr 缓存 ----------------------------------------
 * 一次 SWD 传输要切两回方向（发完请求放手、读完数据收回），原来每次都是
 * 读-改-写 cfgr（3 次总线访问，约 20 周期）。这里在 PORT_SWD_SETUP 之后把
 * 两个目标值算好，切方向就是一条 str。只有本文件（gpio_fast）用，
 * 别处改 GPIOA->cfgr 的引脚（PA4 nRESET 走高阻/开漏，用 ODT 切换）不冲突。 */
static uint32_t s_swdio_cfgr_out;
static uint32_t s_swdio_cfgr_in;

/* 汇编热内核的参数块（偏移见 swd_fast_gpio.S 的 HW_* ）。
 * 一次传输里 kern 只读一次，省掉 C 侧的常量搬运与结构体传参。 */
typedef struct {
    uint32_t scr;       /* 0  GPIOA->scr */
    uint32_t w_clr;     /* 4  SWDIO=0 + SWCLK 下降沿 */
    uint32_t w_set;     /* 8  SWDIO=1 + SWCLK 下降沿 */
    uint32_t idt;       /* 12 GPIOA->idt */
    uint32_t sck_clr;   /* 16 SWCLK 下降沿 */
    uint32_t sck_set;   /* 20 SWCLK 上升沿 */
    uint32_t cfgr;      /* 24 GPIOA->cfgr */
    uint32_t dio_in;    /* 28 SWDIO 切回输入时的 cfgr 值 */
    uint32_t dio_out;   /* 32 SWDIO 切到输出时的 cfgr 值 */
} swd_fast_hw_type;

swd_fast_hw_type swd_fast_hw;
#define SWD_HW  ((const uint32_t *)&swd_fast_hw)

void swd_fast_dio_snapshot(void)
{
    uint32_t cfgr = BOARD_SWD_PORT->cfgr;

    s_swdio_cfgr_in  = cfgr & ~SWDIO_CFGR_MASK;
    s_swdio_cfgr_out = s_swdio_cfgr_in | ((uint32_t)GPIO_MODE_OUTPUT << SWDIO_CFGR_SHIFT);

    swd_fast_hw.scr     = (uint32_t)&BOARD_SWD_PORT->scr;
    swd_fast_hw.idt     = (uint32_t)&BOARD_SWD_PORT->idt;
    swd_fast_hw.cfgr    = (uint32_t)&BOARD_SWD_PORT->cfgr;
    swd_fast_hw.sck_clr = (uint32_t)(1UL << SWD_FAST_SCK_BIT) << 16;
    swd_fast_hw.sck_set = (uint32_t)(1UL << SWD_FAST_SCK_BIT);
    swd_fast_hw.w_clr   = ((uint32_t)(1UL << SWD_FAST_DIO_BIT) << 16) | swd_fast_hw.sck_clr;
    swd_fast_hw.w_set   = ((uint32_t)(1UL << SWD_FAST_DIO_BIT)) | swd_fast_hw.sck_clr;
    swd_fast_hw.dio_in  = s_swdio_cfgr_in;
    swd_fast_hw.dio_out = s_swdio_cfgr_out;
}

#define SWD_DIO_DRIVE()     (BOARD_SWD_PORT->cfgr = s_swdio_cfgr_out)
#define SWD_DIO_RELEASE()   (BOARD_SWD_PORT->cfgr = s_swdio_cfgr_in)

/* 排查开关：1 = 在 .bss 里留档最近几次传输的 request/ACK，可用另一台探针
 * 直接读这几个字（地址用 nm 查 g_swd_fast_dbg）。默认 0，不留开销。 */
#ifndef SWD_FAST_DEBUG
#define SWD_FAST_DEBUG 0
#endif

#if SWD_FAST_DEBUG
__attribute__((used)) volatile uint32_t g_swd_fast_dbg[16];
#else
volatile uint32_t g_swd_fast_dbg[16];
#endif

/* 周期级剖析：1 = 用 DWT->CYCCNT 统计请求/数据相位耗时，结果落在 .noinit
 * （g_swd_prof，地址用 nm 查），可用另一台探针读出来算每 bit 周期数。
 * g_swd_prof[0]=传输次数 [1]=请求相位总周期 [2]=写数据相位总周期
 * [3]=读数据相位总周期 [4]=写数据相位次数 [5]=读数据相位次数
 * [6]=fast_clock [7]=clock_delay [8]=请求->ACK 间隙总周期 [9]=间隙次数 */
#ifndef SWD_PROFILE
#define SWD_PROFILE 0
#endif
#if SWD_PROFILE
__attribute__((used)) volatile uint32_t g_swd_prof[32];

__attribute__((constructor(120))) static void swd_prof_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* 微基准：测一次 GPIOA->scr 写在 216 MHz 下到底几个周期，作为"为什么
 * 汇编内核跑不到 8 周期/bit"的判据。结果写 g_swd_prof[10..13]：
 * [10] 1000 次 scr 写的总周期 [11] 单次周期（×100）
 * [12] 1000 次 volatile 自增的总周期（对照，排除 DWT 读数开销）*/
__attribute__((constructor(121))) static void swd_prof_micro(void)
{
    volatile uint32_t sink = 0U;
    uint32_t t0;
    uint32_t t1;

    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 1000U; i++) {
        BOARD_SWD_PORT->scr = (uint32_t)(1UL << SWD_FAST_SCK_BIT);   /* 置位 SCK */
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[10] = t1 - t0;
    g_swd_prof[11] = ((t1 - t0) * 100U) / 1000U;

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 1000U; i++) {
        sink++;
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[12] = t1 - t0;
    g_swd_prof[13] = sink & 0xFFU;

    /* 直接给汇编内核计时：100 次 32 bit 发送，[14] = 每次的周期数 */
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        swd_fast_tx_bits(0x12345678U, 32U);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[14] = (t1 - t0) / 100U;

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        swd_fast_clock_n(32U);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[15] = (t1 - t0) / 100U;

    /* A：只做 2 次写/bit（不含任何 ALU），看纯写代价 */
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        volatile uint32_t k;
        for (k = 0U; k < 32U; k++) {
            BOARD_SWD_PORT->scr = 0x00800000U | 0x00200000U;   /* DIO 低 + SCK 低 */
            BOARD_SWD_PORT->scr = 0x00000020U;                  /* SCK 高 */
        }
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[16] = (t1 - t0) / 100U / 32U;   /* 每 bit 周期 */

    /* B：2 次写 + 4 条 ALU（还原内核结构），看 ALU 依赖链的代价 */
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        uint32_t v = 0x12345678U;
        uint32_t w = 0x00800000U | 0x00200000U;
        for (uint32_t k = 0U; k < 32U; k++) {
            uint32_t b = (v >> k) & 1U;
            b <<= 4;
            BOARD_SWD_PORT->scr = w;
            BOARD_SWD_PORT->scr = 0x00000020U;
            w = (0x00800000U >> b) | 0x00200000U;
        }
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[17] = (t1 - t0) / 100U / 32U;

    /* C：32 条连续 store（无循环、无 ALU），除以 32 = 单次 APB 写的总线占用 */
    t0 = DWT->CYCCNT;
#define SWD_S32  BOARD_SWD_PORT->scr = 0x00000020U
    SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32;
    SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32;
    SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32;
    SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32; SWD_S32;
#undef SWD_S32
    t1 = DWT->CYCCNT;
    g_swd_prof[18] = t1 - t0;               /* 32 条 store 的总周期 */

    /* D：32 拍「读采样」结构：1 store + 1 load + 1 store */
    t0 = DWT->CYCCNT;
#define SWD_R1  do { BOARD_SWD_PORT->scr = 0x00200000U; \
                     sink = BOARD_SWD_PORT->idt; \
                     BOARD_SWD_PORT->scr = 0x00000020U; } while (0)
    SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1;
    SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1;
    SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1;
    SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1; SWD_R1;
#undef SWD_R1
    t1 = DWT->CYCCNT;
    g_swd_prof[19] = t1 - t0;               /* 32 拍的读采样总周期 */
    g_swd_prof[20] = sink & 0xFFFFU;

    /* E：整字内核单独计时（无 USB 流量、无目标应答）——与实测中的相位
     * 周期一比较，就能判断「慢了 2 倍」是内核结构问题还是总线争用 */
    swd_fast_dio_snapshot();
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        swd_fast_tx_word(SWD_HW, 0x12345678U, 1U);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[21] = (t1 - t0) / 100U;

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        sink ^= (uint32_t)swd_fast_rx_word(SWD_HW);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[22] = (t1 - t0) / 100U;

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        sink ^= swd_fast_req_ack(SWD_HW, 0x0BU);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[23] = (t1 - t0) / 100U;

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        sink ^= swd_prof_txpat_flash(SWD_HW, 0x12345678U);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[24] = (t1 - t0) / 100U;      /* FLASH + IO，每 bit = /32 */

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        sink ^= swd_prof_txpat_ram(SWD_HW, 0x12345678U);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[25] = (t1 - t0) / 100U;      /* RAM + IO */

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        sink ^= swd_prof_txpat_aluflash(SWD_HW, 0x12345678U);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[26] = (t1 - t0) / 100U;      /* FLASH + 纯 ALU（无 store） */

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0U; i < 100U; i++) {
        sink ^= swd_prof_txpat_aluram(SWD_HW, 0x12345678U);
    }
    t1 = DWT->CYCCNT;
    g_swd_prof[27] = (t1 - t0) / 100U;      /* RAM + 纯 ALU */
}
#endif

/* 内核选择：1=汇编内核（swd_fast_gpio.S），0=C 宏参考实现。
 * 三个内核独立开关，排查时逐个切回 C 版本就能定位是哪个内核错。
 *
 * 现状（实测）：TX / RX / CLK 三个内核都已用汇编版（见 README 6.3）。 */
#ifndef SWD_FAST_TX_ASM
#define SWD_FAST_TX_ASM 1
#endif
#ifndef SWD_FAST_RX_ASM
#define SWD_FAST_RX_ASM 1
#endif
#ifndef SWD_FAST_CLK_ASM
#define SWD_FAST_CLK_ASM 1
#endif

/* 慢速档内核：调 swd_fast_gpio.S 里的 *_d 系列（同样在 RAM、同样展开，
 * 只在每个半周期之间插一段运行时延时可编程的循环）。这样低速请求不再走
 * Flash 里的 C 宏，也不依赖 cherrydap 的 SW_DP.c —— 一套实现两档节拍。 */
static inline uint32_t swd_slow_dly(void)
{
    /* DAP_Data.clock_delay 是上游 PIN_DELAY_SLOW() 的迭代次数（按 8 周期/次估算），
     * 汇编里的延时循环约 2~3 周期/次，所以乘 DELAY_SLOW_CYCLES 再除 3。 */
    uint32_t dly = 1U; /* TEMP: 强制性最小延时，用来判定是不是"太慢"导致失败 */
    return dly ? dly : 1U;
}

/* 内核只用汇编（RAM 常驻、完全展开）。
 * 低速档由上游 cherrydap 的 SW_DP.c 承担：它在 xmake 里用 -D 把三个导出符号
 * 改名成 *Ref，入口处按 DAP_Data.fast_clock 分流——请求到最快档才走汇编。 */
static inline __attribute__((always_inline)) void k_tx(uint32_t value, uint32_t nbits)
{
    swd_fast_tx_bits(value, nbits);
}

static inline uint32_t k_rx(uint32_t nbits)
{
    return swd_fast_rx_bits(nbits);
}

static inline __attribute__((always_inline)) void k_clock(uint32_t nbits)
{
    swd_fast_clock_n(nbits);
}

extern uint8_t SWD_TransferRef(uint32_t request, uint32_t *data);
extern void SWJ_SequenceRef(uint32_t count, const uint8_t *data);
extern void SWD_SequenceRef(uint32_t info, const uint8_t *swdo, uint8_t *swdi);

/**
 * @brief 打一个 SWD 时钟周期，同时把 SWDIO 设成 bit（用于 SWJ_Sequence / idle）
 */
static SWD_RAMF void swd_bit_out(uint32_t bit)
{
#if SWD_FAST_DEBUG
    g_swd_fast_dbg[7]++;
#endif
    k_tx(bit & 1U, 1U);
}

void SWJ_Sequence(uint32_t count, const uint8_t *data)
{
    if (!DAP_Data.fast_clock) {
        SWJ_SequenceRef(count, data);
        return;
    }

    uint32_t val = 0U;
    uint32_t n = 0U;

    while (count--) {
        if (n == 0U) {
            val = *data++;
            n = 8U;
        }
        swd_bit_out(val & 1U);
        val >>= 1;
        n--;
    }
}

void SWD_Sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi)
{
    if (!DAP_Data.fast_clock) {
        SWD_SequenceRef(info, swdo, swdi);
        return;
    }

    uint32_t n = info & SWD_SEQUENCE_CLK;

    if (n == 0U) {
        n = 64U;
    }

    if (info & SWD_SEQUENCE_DIN) {
        while (n) {
            uint32_t k = (n >= 8U) ? 8U : n;
            /* 右移进位的累加结果天然是 LSB 在前的位序，>32-k 只是把有效位对齐 */
            *swdi++ = (uint8_t)(k_rx(k) >> (32U - k));
            n -= k;
        }
    } else {
        while (n) {
            uint32_t k = (n >= 8U) ? 8U : n;
            k_tx((uint32_t)(*swdo++), k);
            n -= k;
        }
    }
}

/**
 * @brief 一次 SWD 传输：按「读 / 写 × 快 / 慢」四路分派
 *
 * 结构照 ch32x035：读写的数据相位完全不同（读采样 + 校验奇偶，写驱动 + 送奇偶），
 * 拆成独立函数后每条路径都是直线代码，编译器/手写汇编都能各自调到最优。
 * 慢速档直接交给上游 cherrydap 的 SW_DP.c（改名成 SWD_TransferRef），
 * 时序与上游逐字一致；只在主机请求到最快档时才走本文件的 RAM 汇编快路径。
 */
typedef struct {
    uint32_t scr;       /* GPIOA->scr */
    uint32_t sck_clr;   /* 只把 SWCLK 拉低（IOCB 半字） */
    uint32_t sck_set;   /* 只把 SWCLK 拉高（IOSB 半字） */
    uint32_t w_clr;     /* SWDIO=0 + SWCLK 下降沿，一次写完成 */
    uint32_t w_set;     /* SWDIO=1 + SWCLK 下降沿，一次写完成 */
} swd_fast_ctx_type;


/* 奇偶位：用位折叠 + 0x6996 查表代替 __builtin_parity。
 * 后者在 Cortex-M4 上会调 libgcc 的 __paritysi2（Flash 里的一次函数调用，
 * 216 MHz + 6 等待周期下每次几十周期），而每笔传输要算 2 次。 */
static __attribute__((always_inline)) inline uint32_t swd_parity_nibble(uint32_t v)
{
    return (0x6996U >> (v & 0x0FU)) & 1U;
}

static __attribute__((always_inline)) inline uint32_t swd_parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    return (0x6996U >> (v & 0x0FU)) & 1U;
}

/* ===========================================================================
 * 27 MHz 档快路径（两段式：C 层请求/ACK + 展开汇编数据相位）
 * 数据相位走 swd_fast_tx32_hot / swd_fast_rx_bits，全部展开、无循环。
 * ========================================================================= */

/* 请求字节：start | APnDP | RnW | A2 | A3 | parity | stop | park（LSB 先发） */
static inline __attribute__((always_inline)) uint32_t swd_request_byte(uint32_t request)
{
    return 0x01U | ((request & 0x0FU) << 1) | (swd_parity_nibble(request) << 5) | 0x80U;
}

/* 临时诊断：记录每次快路径传输的 (request<<8 | ack)，用外部探针读回 */
__attribute__((used)) volatile uint32_t g_ack_log[32];
__attribute__((used)) volatile uint32_t g_ack_idx;

static inline __attribute__((always_inline)) void swd_ack_log(uint32_t request, uint32_t ack)
{
    uint32_t i = g_ack_idx;

    g_ack_log[31] = ((uint32_t)DAP_Data.swd_conf.data_phase << 24) |
                    ((uint32_t)DAP_Data.swd_conf.turnaround << 16) |
                    ((uint32_t)DAP_Data.transfer.idle_cycles << 8) |
                    (uint32_t)DAP_Data.clock_delay;
    if (i < 31U) {
        g_ack_log[i] = (swd_request_byte(request) << 16) | ((request & 0xFFU) << 8) | (ack & 0xFFU);
        g_ack_idx = i + 1U;
    }
}

/* 请求 8 拍 + 松手 + 转向（turnaround 拍）+ 3 拍 ACK 采样 */
static inline __attribute__((always_inline)) uint32_t swd_request_ack(const swd_fast_ctx_type ctx,
                                                                     uint32_t request)
{
    uint32_t ack = 0U;
    uint32_t n;

    swd_fast_tx8_hot(swd_request_byte(request), ctx.scr, ctx.w_clr, ctx.w_set);
    SWD_DIO_RELEASE();

    for (n = DAP_Data.swd_conf.turnaround; n; n--) {
        *(volatile uint32_t *)ctx.scr = ctx.sck_clr;
        *(volatile uint32_t *)ctx.scr = ctx.sck_set;
    }
    for (uint32_t i = 0U; i < 3U; i++) {
        *(volatile uint32_t *)ctx.scr = ctx.sck_clr;
        __NOP(); __NOP();
        ack |= (uint32_t)((BOARD_SWD_PORT->idt >> SWD_FAST_DIN_BIT) & 1U) << i;
        *(volatile uint32_t *)ctx.scr = ctx.sck_set;
        __NOP();
    }
    return ack;
}

/* 时间戳 + idle 收尾（读写两条快路径共用） */
static inline __attribute__((always_inline)) void swd_fast_idle_tail(uint32_t request)
{
    uint32_t n;

    if (request & DAP_TRANSFER_TIMESTAMP) {
        DAP_Data.timestamp = TIMESTAMP_GET();
    }
    n = DAP_Data.transfer.idle_cycles;
    if (n) {
        PIN_SWDIO_OUT(0U);
        k_clock(n);
    }
    PIN_SWDIO_OUT(1U);
}

/* ===================== 27 MHz 档：整字合并内核 ========================== */

/**
 * @brief WAIT / FAULT / 协议错误的收尾（与上游 SW_DP.c 同构，只是改用快时钟内核）
 */
static inline __attribute__((always_inline)) uint8_t swd_fast_finish(uint32_t request, uint32_t ack)
{
    uint32_t n;

    if ((ack == (uint32_t)DAP_TRANSFER_WAIT) || (ack == (uint32_t)DAP_TRANSFER_FAULT)) {
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {
            k_clock(32U + 1U);          /* 补读 RDATA[31:0] + 奇偶 */
        }
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            k_clock(1U);
        }
        SWD_DIO_DRIVE();
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {
            PIN_SWDIO_OUT(0U);
            k_clock(32U + 1U);          /* 补写 WDATA[31:0] + 奇偶 */
        }
        PIN_SWDIO_OUT(1U);
        return (uint8_t)ack;
    }

    /* 协议错误：退避 data phase 并重新驱动 SWDIO */
    k_clock(DAP_Data.swd_conf.turnaround + 32U + 1U);
    SWD_DIO_DRIVE();
    PIN_SWDIO_OUT(1U);
    return (uint8_t)ack;
}


/**
 * @brief 27 MHz 写：一次汇编调用完成「请求 8 拍 + 转向 + 3 拍 ACK + 切输出 +
 *        转向 + 32 拍数据 + 奇偶」，C 层只剩取值、奇偶计算和收尾。
 */
static inline __attribute__((always_inline)) uint8_t swd_fast_write_fast(uint32_t request, uint32_t *data)
{
    uint32_t val = data ? *data : 0U;
    uint32_t ack = swd_fast_wr(SWD_HW, swd_request_byte(request), val, swd_parity32(val));

    if (ack != (uint32_t)DAP_TRANSFER_OK) {
        return swd_fast_finish(request, ack);
    }
    swd_fast_idle_tail(request);
    return (uint8_t)ack;
}

/**
 * @brief 27 MHz 读：一次汇编调用完成「请求 8 拍 + 转向 + 3 拍 ACK + 转向 +
 *        32 拍采样 + 奇偶」，数据与奇偶位一起带回。
 */
static inline __attribute__((always_inline)) uint8_t swd_fast_read_fast(uint32_t request, uint32_t *data)
{
    uint32_t val = 0U;
    uint32_t r = swd_fast_rd(SWD_HW, swd_request_byte(request), &val);
    uint32_t ack = r & 7U;

    if (ack != (uint32_t)DAP_TRANSFER_OK) {
        return swd_fast_finish(request, ack);
    }
    if (swd_parity32(val) != ((r >> 4) & 1U)) {
        ack = (uint32_t)DAP_TRANSFER_ERROR;
#if SWD_FAST_DEBUG
        g_swd_fast_dbg[11]++;
#endif
    }
    if (data) {
        *data = val;
    }
    if (DAP_Data.swd_conf.turnaround) {
        k_clock(DAP_Data.swd_conf.turnaround + 1U);
    }
    SWD_DIO_DRIVE();
    swd_fast_idle_tail(request);
    return (uint8_t)ack;
}

/* 请求/ACK + 展开汇编数据相位：写 */
static inline __attribute__((always_inline)) uint8_t swd_fast_write_hot(uint32_t request, uint32_t *data)
{
    const swd_fast_ctx_type ctx = {
        (uint32_t)&BOARD_SWD_PORT->scr,
        (uint32_t)(1UL << SWD_FAST_SCK_BIT) << 16,
        (uint32_t)(1UL << SWD_FAST_SCK_BIT),
        ((uint32_t)(1UL << SWD_FAST_DIO_BIT) << 16) | ((uint32_t)(1UL << SWD_FAST_SCK_BIT) << 16),
        ((uint32_t)(1UL << SWD_FAST_DIO_BIT)) | ((uint32_t)(1UL << SWD_FAST_SCK_BIT) << 16),
    };
    uint32_t ack = swd_request_ack(ctx, request);
    uint32_t val;

    swd_ack_log(request, ack);
    if (ack != (uint32_t)DAP_TRANSFER_OK) {
        return swd_fast_finish(request, ack);
    }

    for (uint32_t tn = DAP_Data.swd_conf.turnaround; tn; tn--) {
        *(volatile uint32_t *)ctx.scr = ctx.sck_clr;
        *(volatile uint32_t *)ctx.scr = ctx.sck_set;
    }
    SWD_DIO_DRIVE();
    val = data ? *data : 0U;
    swd_fast_tx32_hot(val, ctx.scr, ctx.w_clr, ctx.w_set);
    *(volatile uint32_t *)ctx.scr = swd_parity32(val) ? ctx.w_set : ctx.w_clr;
    *(volatile uint32_t *)ctx.scr = ctx.sck_set;

    swd_fast_idle_tail(request);
    return (uint8_t)ack;
}

/* 请求/ACK + 展开汇编数据相位：读 */
static inline __attribute__((always_inline)) uint8_t swd_fast_read_hot(uint32_t request, uint32_t *data)
{
    const swd_fast_ctx_type ctx = {
        (uint32_t)&BOARD_SWD_PORT->scr,
        (uint32_t)(1UL << SWD_FAST_SCK_BIT) << 16,
        (uint32_t)(1UL << SWD_FAST_SCK_BIT),
        ((uint32_t)(1UL << SWD_FAST_DIO_BIT) << 16) | ((uint32_t)(1UL << SWD_FAST_SCK_BIT) << 16),
        ((uint32_t)(1UL << SWD_FAST_DIO_BIT)) | ((uint32_t)(1UL << SWD_FAST_SCK_BIT) << 16),
    };
    uint32_t ack = swd_request_ack(ctx, request);
    uint32_t val;
    uint32_t parity;

    swd_ack_log(request, ack);
    if (ack != (uint32_t)DAP_TRANSFER_OK) {
        return swd_fast_finish(request, ack);
    }
    val = swd_fast_rx_bits(32U);
    /* 校验位由目标较晚驱动：采样点比数据位再晚 4 个周期，否则偶发校验错 */
    *(volatile uint32_t *)ctx.scr = ctx.sck_clr;
    __NOP(); __NOP(); __NOP(); __NOP();
    parity = (uint32_t)((BOARD_SWD_PORT->idt >> SWD_FAST_DIN_BIT) & 1U);
    *(volatile uint32_t *)ctx.scr = ctx.sck_set;
    __NOP();
    if (swd_parity32(val) != (parity & 1U)) {
        ack = (uint32_t)DAP_TRANSFER_ERROR;
    }
    if (data) {
        *data = val;
    }
    if (DAP_Data.swd_conf.turnaround) {
        k_clock(DAP_Data.swd_conf.turnaround + 1U);
    }
    SWD_DIO_DRIVE();
    swd_fast_idle_tail(request);
    return (uint8_t)ack;
}

#ifndef SWD_FAST_PHY_ENABLE
#define SWD_FAST_PHY_ENABLE 0
#endif

/* 档位分派：fast_clock=1（>=27 MHz）走展开汇编；其余交给上游 C 实现 */
static inline __attribute__((always_inline)) uint8_t swd_fast_write(uint32_t request, uint32_t *data)
{
    return swd_fast_write_hot(request, data);
}

static inline __attribute__((always_inline)) uint8_t swd_fast_read(uint32_t request, uint32_t *data)
{
    return swd_fast_read_hot(request, data);
}

SWD_RAMF uint8_t SWD_Transfer(uint32_t request, uint32_t *data)
{
#if SWD_FAST_PHY_ENABLE
    if (!DAP_Data.fast_clock && (DAP_Data.clock_delay > 1U)) {
        return SWD_TransferRef(request, data);
    }
    if (request & DAP_TRANSFER_RnW) {
        return swd_fast_read(request, data);
    }
    return swd_fast_write(request, data);
#else
    (void)swd_fast_read;
    (void)swd_fast_write;
    return SWD_TransferRef(request, data);
#endif
}


#endif /* (DAP_SWD != 0) && SWD_PHY_GPIO_FAST */




