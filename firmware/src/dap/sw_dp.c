#include "DAP_config.h"

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

// 上升沿 / 下降沿之后各补一段可调等待，用来修 SWCLK 占空比（见 DAP_config.h）。
// 这两个宏在本文件里只以 PIN_SWCLK_SET() / PIN_SWCLK_CLR() 形式使用，所以直接
// 重定义即可覆盖全部调用点，不必逐处修改；后面的 PIN_DELAY_FAST() 仍会执行。
#define PIN_SWCLK_SET() do { PIN_SWCLK_TCK_SET(); __NOP(); } while (0)
#define PIN_SWCLK_CLR() do { PIN_SWCLK_TCK_CLR(); __NOP(); } while (0)

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

#define SW_WRITE_BIT(bit) \
    PIN_SWDIO_OUT(bit);   \
    PIN_SWCLK_CLR();      \
    PIN_DELAY();          \
    PIN_SWCLK_SET();      \
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

#define SWD_FAST_READ_DATA_BIT() \
    do {                         \
        PIN_SWCLK_CLR();         \
        PIN_DELAY_FAST();        \
        bit = PIN_SWDIO_IN();    \
        PIN_SWCLK_SET();         \
        PIN_DELAY_FAST();        \
        val >>= 1;               \
        val |= bit << 31;        \
    } while (0)

#define SWD_FAST_READ_SAMPLE(sample) \
    do {                             \
        PIN_SWCLK_CLR();             \
        PIN_DELAY_FAST();            \
        sample = GPIOA->INDR;        \
        PIN_SWCLK_SET();             \
        PIN_DELAY_FAST();            \
    } while (0)

// 读取时把「采样」与「拉高」拆开，好把位打包的工作放进低电平期。
// 采样点与原先一致（下降沿后延时一拍），所以数据有效性不变；
// 多出的打包指令不会再拉长高电平期。
#define SWD_FAST_READ_LOW(sample)   \
    do {                            \
        PIN_SWCLK_CLR_READ();       \
        sample = GPIOA->INDR;       \
    } while (0)

#define SWD_FAST_READ_HIGH()  \
    do {                      \
        PIN_SWCLK_SET_READ(); \
        PIN_DELAY_FAST();     \
    } while (0)

#define SWD_FAST_PACK_BARRIER() \
    __asm__ volatile("" : "+r"(packed) : : "memory")

// 注意：数据与下降沿在同一条 BSHR 写里完成，不经过 PIN_SWCLK_CLR，
// 所以这里要显式补上低电平期延时，否则写数据位的低电平期仍偏短。
#define SWD_FAST_WRITE_DATA_BIT()     \
    do {                              \
        GPIOA->BSHR = ((GPIO_Pin_3 << 16) >> ((val & 1U) << 4));\
        GPIOA->BSHR = (GPIO_Pin_2 << 16); \
        val >>= 1;                    \
        PIN_DELAY_FAST();             \
        PIN_DELAY_FAST();             \
        PIN_SWCLK_SET();              \
        PIN_DELAY_FAST();             \
    } while (0)

__attribute__((section(".highcode"), noinline, optimize("O2"))) static void SWD_FastWriteData(uint32_t val) {
    uint32_t n;
    uint32_t parity = SWD_Parity32(val);

    // 展开 32 位数据输出，减少逐组循环的分支开销
    #pragma GCC unroll 8
    for (n = 8U; n; n--) {
        SWD_FAST_WRITE_DATA_BIT();
        SWD_FAST_WRITE_DATA_BIT();
        SWD_FAST_WRITE_DATA_BIT();
        SWD_FAST_WRITE_DATA_BIT();
    }
    PIN_SWDIO_OUT_SWCLK_CLR(parity);
    GPIOA->BSHR = (GPIO_Pin_2 << 16);
    PIN_DELAY_FAST();
    PIN_SWCLK_SET();
    PIN_DELAY_FAST();
}

#if ((DAP_SWD != 0) || (DAP_JTAG != 0))
// Generate SWJ Sequence
//   count:  sequence bit count
//   data:   pointer to sequence bit data
//   return: none
__attribute__((section(".highcode"))) void SWJ_Sequence(uint32_t count, const uint8_t *data) {
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
                SW_WRITE_BIT(val);
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
__attribute__((section(".highcode"))) static uint8_t SWD_TransferFast(
    uint32_t request, uint32_t *data) {
    uint32_t ack;
    uint32_t bit, bit1;
    uint32_t val;
    uint32_t parity;
    uint32_t packed;

    uint32_t n;
    const uint32_t swdio_cfglr_base = GPIOA->CFGLR & ~(0xFU << 12);
    const uint32_t swdio_cfglr_output = swdio_cfglr_base | (0x1U << 12);
    const uint32_t swdio_cfglr_input = swdio_cfglr_base | (0x4U << 12);

    /* Packet Request
     *
     * 位序改成「拉低 -> 低电平延时 -> 算位并更新数据 -> 拉高 -> 高电平延时」。
     * 原实现是「算位 -> 更新数据 -> 拉低 -> 延时 -> 拉高 -> 延时」：数据更新与
     * 下降沿同时发生（拉高沿前的建立时间近乎为零），而下一位的移位/异或/写端口
     * 全部落在**高电平期**，实测 SWCLK 占空比 高:低 约 3:1。
     * 改后建立时间充足，且高低电平期长度接近（总周期不变，频率不受影响）。
     */
    parity = 0U;
    PIN_SWDIO_TMS_SET();          /* Start Bit 数据 = 1 */
    PIN_SWCLK_CLR();
    bit = request >> 0;
    PIN_SWCLK_SET();  /* Start Bit */
    parity += bit;
    bit1 = (bit & 1U) << 4;
    __asm__ volatile("" : "+r"(bit1) : : "memory");
    PIN_SWCLK_CLR();
    GPIOA->BSHR = (GPIO_Pin_3 << 16) >> bit1;
    PIN_SWCLK_SET(); /* APnDP Bit */
    bit = request >> 1;
    parity += bit;
    bit1 = (bit & 1U) << 4;
    __asm__ volatile("" : "+r"(bit1) : : "memory");
    PIN_SWCLK_CLR();
    GPIOA->BSHR = (GPIO_Pin_3 << 16) >> bit1;
    PIN_SWCLK_SET(); /* RnW Bit */
    bit = request >> 2;
    parity += bit;
    bit1 = (bit & 1U) << 4;
    __asm__ volatile("" : "+r"(bit1) : : "memory");
    PIN_SWCLK_CLR();
    GPIOA->BSHR = (GPIO_Pin_3 << 16) >> bit1;
    PIN_SWCLK_SET(); /* A2 Bit */
    bit = request >> 3;
    bit1 = (bit & 1U) << 4;
    __asm__ volatile("" : "+r"(bit1) : : "memory");
    PIN_SWCLK_CLR();
    GPIOA->BSHR = (GPIO_Pin_3 << 16) >> bit1;
    PIN_SWCLK_SET(); /* A3 Bit */

    parity += bit;
    bit1 = (parity & 1U) << 4;
    __asm__ volatile("" : "+r"(bit1) : : "memory");
    PIN_SWCLK_CLR();
    GPIOA->BSHR = (GPIO_Pin_3 << 16) >> bit1;
    PIN_SWCLK_SET();
    PIN_DELAY_FAST(); /* Parity Bit */

    PIN_SWDIO_TMS_CLR();
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    PIN_SWCLK_SET();
    PIN_DELAY_FAST(); /* Stop Bit */

    PIN_SWDIO_TMS_SET();
    PIN_SWCLK_CLR();
    GPIOA->CFGLR = swdio_cfglr_input;
    PIN_SWCLK_SET(); /* Park Bit */

    /* Turnaround */
    PIN_DELAY_FAST();
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    PIN_SWCLK_SET();

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
    PIN_SWCLK_SET();
    PIN_DELAY_FAST();
    PIN_DELAY_FAST();
    ack = bit << 0;
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();
    PIN_DELAY_FAST();
    PIN_DELAY_FAST();
    ack |= bit << 1;
    PIN_SWCLK_CLR();
    PIN_DELAY_FAST();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();
    ack |= bit << 2;
#endif

    if (ack == DAP_TRANSFER_OK) { /* OK response */
        /* Data transfer */
        if (request & DAP_TRANSFER_RnW) {
            /* Read data */
            val = 0U;
            for (n = 8U; n; n--) {
                SWD_FAST_READ_LOW(bit);
                packed = (bit >> 3) & 0x01U;
                SWD_FAST_PACK_BARRIER();
                SWD_FAST_READ_HIGH();
                SWD_FAST_READ_LOW(bit);
                packed |= ((bit >> 3) & 0x01U) << 1;
                SWD_FAST_PACK_BARRIER();
                SWD_FAST_READ_HIGH();
                SWD_FAST_READ_LOW(bit);
                packed |= ((bit >> 3) & 0x01U) << 2;
                SWD_FAST_PACK_BARRIER();
                SWD_FAST_READ_HIGH();
                SWD_FAST_READ_LOW(bit);
                packed |= ((bit >> 3) & 0x01U) << 3;
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
            GPIOA->CFGLR = swdio_cfglr_output;
        } else {
            /* Turnaround */
            PIN_SWCLK_CLR();
            GPIOA->CFGLR = swdio_cfglr_output;
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
        GPIOA->CFGLR = swdio_cfglr_output;
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
    GPIOA->CFGLR = swdio_cfglr_output;
    PIN_SWDIO_TMS_SET();
    return ((uint8_t)ack);
}

// 注意：上面两个 undef 对应下面用到的两个宏，名字改了要同步
#undef SWD_FAST_READ_DATA_BIT
#undef SWD_FAST_READ_LOW
#undef SWD_FAST_READ_HIGH
#undef SWD_FAST_PACK_BARRIER
#undef SWD_FAST_WRITE_DATA_BIT

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
    PIN_SWDIO_OUT(1U);
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Start Bit */
    bit = request >> 0;
    PIN_SWDIO_OUT(bit);
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* APnDP Bit */
    parity += bit;
    bit = request >> 1;
    PIN_SWDIO_OUT(bit);
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* RnW Bit */
    parity += bit;
    bit = request >> 2;
    PIN_SWDIO_OUT(bit);
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* A2 Bit */
    parity += bit;
    bit = request >> 3;
    PIN_SWDIO_OUT(bit);
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* A3 Bit */
    parity += bit;
    PIN_SWDIO_OUT(parity);
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Parity Bit */
    PIN_SWDIO_OUT(0U);
    PIN_SWCLK_CLR();
    PIN_DELAY_SLOW(DAP_Data.clock_delay);
    PIN_SWCLK_SET();
    PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Stop Bit */
    PIN_SWDIO_OUT(1U);
    PIN_SWCLK_CLR();
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
                PIN_SWDIO_OUT(val);
                PIN_SWCLK_CLR();
                PIN_DELAY_SLOW(DAP_Data.clock_delay);
                PIN_SWCLK_SET();
                PIN_DELAY_SLOW(DAP_Data.clock_delay); /* Write WDATA[0:31] */
                parity += val;
                val >>= 1;
            }
            PIN_SWDIO_OUT(parity);
            PIN_SWCLK_CLR();
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
__attribute__((section(".highcode"))) uint8_t SWD_Transfer(uint32_t request, uint32_t *data) {
    if (DAP_Data.fast_clock) {
        return SWD_TransferFast(request, data);
    } else {
        return SWD_TransferSlow(request, data);
    }
}

#endif /* (DAP_SWD != 0) */
