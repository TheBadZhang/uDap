#include "DAP_config.h"
#include <stdint.h>
#include "mode_overlay.h"
#include "swd_gpio.h"

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
// SWD 两个引脚（引脚号见 src/swd_pin_map.h）同属一个 port，合并写 BSHR 时用这个别名。
#define PIN_SWD_PORT SWD_PORT
#define PIN_SWCLK SWD_CLK_PIN
#define PIN_SWCLK_SHIFT SWD_CLK_BIT
#define PIN_SWDIO SWD_DIO_PIN
#define PIN_SWDIO_SHIFT SWD_DIO_BIT

// 上升沿 / 下降沿之后各补一段可调等待，用来修 SWCLK 占空比（见 DAP_config.h）。
// 这两个宏在本文件里只以 PIN_SWCLK_SET() / PIN_SWCLK_CLR() 形式使用，所以直接
// 重定义即可覆盖全部调用点，不必逐处修改；后面的 PIN_DELAY_FAST() 仍会执行。
#define PIN_SWCLK_SET() { PIN_SWCLK_TCK_SET(); __NOP(); }
#define PIN_SWCLK_CLR() { PIN_SWCLK_TCK_CLR(); __NOP(); }
// #define PIN_SWDIO_SET() {}
// #define PIN_SWDIO_CLR() {}

// DIO 方向切换：**直接写寄存器**，不走 swd_gpio_if() 的接口表。
//
// 接口表里的成员是函数指针，编译器无论如何都不可能把它内联（LTO 只做到去虚化：
// 把 `swd_gpio_if()->dio_out_enable()` 变成一条 auipc+jalr 的**直接调用**到
// x035_dio_out_enable，函数体本身仍是独立的 out-of-line 函数）。每个事务切两次
// 方向，于是每个事务白花两次「调用 + 返回 + 重新加载 CFGLR 基准」，还多占了
// 一段 .text。
//
// 展开后就是 lw(board_swd_cfglr_in/out) + sw 两条指令（见 src/swd_pin_map.h）。
// 与每比特的 CLK/DIO 原语一样属于**编译期绑定** —— 换驱动要连 swd_pin_map.h 一起换，
// 这一点在 src/dap/swd_gpio.h 的文件头已说明。
#define PIN_SWDIO_OUTPUT() { board_swd_dio_output(); }
#define PIN_SWDIO_INPUT() { board_swd_dio_input(); }

// 在 BSHR 的置位与复位半字间选择，避免每个数据位都执行条件分支
#define PIN_SWDIO_OUT(bit) { PIN_SWD_PORT->BSHR = (PIN_SWDIO << 16) >> ((bit & 1U) << 4); }

#define PIN_SWCLK_CLR_SWDIO_CLR() { PIN_SWD_PORT->BSHR = (PIN_SWCLK << 16) | (PIN_SWDIO << 16); }
#define PIN_SWCLK_CLR_SWDIO_SET() { PIN_SWD_PORT->BSHR = (PIN_SWDIO | PIN_SWCLK << 16); }
// 设置 SWDIO 数据，并将 SWCLK 原子拉低，为下一个 SWD 数据位准备
#define PIN_SWCLK_CLR_SWDIO_OUT(bit) { PIN_SWD_PORT->BSHR = (PIN_SWCLK << 16) | ((PIN_SWDIO << 16) >> ((bit & 1U) << 4)); }

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

/* SWD 的校验位：32 位数据按位异或（与 ARM ADI 一致，目标实测接受这个算法）。 */
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
// ===========================================================================
// 快路径：请求相位 + ACK 采样
// ===========================================================================
// 抽成 always_inline 的理由：这段的时序由固定指令数决定（PIN_DELAY_FAST() 就是
// 一个 __NOP()），内联后读、写两个函数各自展开一份，指令序列与拆分前的对应分支
// 完全一致；抽成普通函数则会多一次调用/返回，直接改变 SWCLK 相位。
//
// ⚠️ **不要**把「请求相位」和「ACK 采样」拆成两个函数。试过：两段都 always_inline、
// 指令条数也没变，但拆开后寄存器分配/code layout 变了，读路径的长块读开始偶发
// "Failed to read memory"（HEAD 版本 3/3 次 64 KiB 全对，拆开版本每次错 1 个）。
// 所以整段保持在一个函数里。
static inline __attribute__((always_inline))
uint32_t swd_fast_ack_phase(uint32_t request) {
    uint32_t ack;
    uint32_t bit;

    /* Packet Request —— 8 个 SWCLK（start/APnDP/RnW/A2/A3/parity/stop）。
     * 每拍 8 个 clk、低 4 / 高 4，指令顺序必须钉住，所以整段在
     * x035_packet_request() 的手写汇编里（摆位与理由见 src/dap/swd_gpio_x035.c）。
     */
    swd_gpio_if()->packet_request(request);
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

    return ack;
}

// 快路径：ACK 不是 OK 时的收尾（WAIT/FAULT 的 dummy 相位、以及协议错误的 back-off）
static inline __attribute__((always_inline))
uint8_t swd_fast_finish(uint32_t ack, uint32_t is_read) {
    uint32_t n;

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        /* WAIT or FAULT response */
        if (DAP_Data.swd_conf.data_phase && is_read) {
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
        if (DAP_Data.swd_conf.data_phase && !is_read) {
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

// 快路径：OK 之后的收尾（时间戳 / idle cycles / 把 SWDIO 留在高电平）
static inline __attribute__((always_inline))
uint8_t swd_fast_finish_ok(uint32_t ack) {
    uint32_t n;

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

// ---------------------------------------------------------------------------
// 快路径 · 读：请求 + ACK + 32 位数据 + 奇偶校验 + turnaround
//   数据相位是 32 个 bit 的手写汇编（x035_read_data），顺带算出奇偶位。
// ---------------------------------------------------------------------------
DAP_HIGHCODE_SECTION static uint8_t SWD_TransferFastRead(
    uint32_t request, uint32_t *data) {
    uint32_t ack;
    uint32_t bit;
    uint32_t val;
    uint32_t parity;

    /* 请求相位（走 x035 的手写汇编：8 拍 + park + turnaround + ACK 采样） */
    ack = swd_fast_ack_phase(request);
    if (ack != DAP_TRANSFER_OK) {
        return swd_fast_finish(ack, 1U);
    }

    /* Read data —— 32 个数据位（SWD 为 LSB first，每 bit 8 个 SWCLK、
     * 低 4 / 高 4）。位循环与奇偶累加都在 x035_read_data() 的手写汇编里
     * （摆位与理由见 src/dap/swd_gpio_x035.c），回来后再用 GPIO 采第 33 拍（奇偶位）。
     */
    swd_gpio_if()->read_data(&val, &parity);
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

    return swd_fast_finish_ok(ack);
}

// ---------------------------------------------------------------------------
// 快路径 · 写：请求 + ACK + 32 位数据 + 奇偶位 + 收尾
//   数据相位是 32 个 bit 的手写汇编（x035_write_data），奇偶位用位运算算。
// ---------------------------------------------------------------------------
DAP_HIGHCODE_SECTION static uint8_t SWD_TransferFastWrite(
    uint32_t request, uint32_t *data) {
    uint32_t ack;
    uint32_t val;
    uint32_t parity;

    /* 请求相位（走 x035 的手写汇编：8 拍 + park + turnaround + ACK 采样） */
    ack = swd_fast_ack_phase(request);
    if (ack != DAP_TRANSFER_OK) {
        return swd_fast_finish(ack, 0U);
    }

    /* Turnaround */
    PIN_SWCLK_CLR();
    PIN_SWDIO_OUTPUT(); // turnaround
    PIN_SWCLK_SET();

    /* Write data：数据相位是整块汇编，先把它发出去；奇偶位在数据相位之后、
     * 奇偶位那一拍之前算 —— 和位时序"在 bit 之间算"的节奏一致，不占发起时间。 */
    val = *data;
    swd_gpio_if()->write_data(val);
    parity = SWD_Parity32(val);
    PIN_SWCLK_CLR_SWDIO_OUT(parity); /* 奇偶位 */
    PIN_DELAY_FAST();
    PIN_DELAY_FAST();
    PIN_SWCLK_SET();
    PIN_DELAY_FAST();

    return swd_fast_finish_ok(ack);
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

// ===========================================================================
// 慢路径：请求相位 + ACK 采样（读、写共用，always_inline 同上）
// ===========================================================================
__attribute__((noinline)) static uint32_t swd_slow_ack_phase(uint32_t request) {
    uint32_t ack;
    uint32_t bit;
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

    return ack;
}

// 慢路径：ACK 不是 OK 时的收尾
__attribute__((noinline)) static uint8_t swd_slow_finish(uint32_t ack, uint32_t is_read) {
    uint32_t n;

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        /* WAIT or FAULT response */
        if (DAP_Data.swd_conf.data_phase && is_read) {
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
        if (DAP_Data.swd_conf.data_phase && !is_read) {
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

// 慢路径：OK 之后的收尾（时间戳 / idle cycles）
__attribute__((noinline)) static uint8_t swd_slow_finish_ok(uint32_t ack, uint32_t request) {
    uint32_t n;

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

// ---------------------------------------------------------------------------
// 慢路径 · 读：请求 + ACK + 32 位数据 + 奇偶校验 + turnaround
// ---------------------------------------------------------------------------
__attribute__((noinline)) static uint8_t SWD_TransferSlowRead(
    uint32_t request, uint32_t *data) {
    uint32_t ack;
    uint32_t bit;
    uint32_t val;
    uint32_t parity;
    uint32_t n;

    ack = swd_slow_ack_phase(request);
    if (ack != DAP_TRANSFER_OK) {
        return swd_slow_finish(ack, 1U);
    }

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

    return swd_slow_finish_ok(ack, request);
}

// ---------------------------------------------------------------------------
// 慢路径 · 写：请求 + ACK + 32 位数据 + 奇偶位 + 收尾
// ---------------------------------------------------------------------------
__attribute__((noinline)) static uint8_t SWD_TransferSlowWrite(
    uint32_t request, uint32_t *data) {
    uint32_t ack;
    uint32_t val;
    uint32_t parity;
    uint32_t n;

    ack = swd_slow_ack_phase(request);
    if (ack != DAP_TRANSFER_OK) {
        return swd_slow_finish(ack, 0U);
    }

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

    return swd_slow_finish_ok(ack, request);
}

// SWD Transfer I/O
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
//
// 读、写在这里就分派到各自的实现：两者的数据相位完全不同（读采样+校验奇偶，
// 写驱动+送奇偶），拆开后每条路径都是直线代码。
DAP_HIGHCODE_SECTION uint8_t SWD_Transfer(uint32_t request, uint32_t *data) {
    // 快路径要求当前实现具备三段位时序（swd_gpio.h 约定：全给或全 NULL）。
    // 换成没有快路径的实现（例如纯 GPIO 轮询、外挂 MCU）时整体退回慢路径，
    // 不会出现「请求头走快、数据相位走慢」的半快半慢状态。
    const uint32_t fast = (DAP_Data.fast_clock && swd_gpio_has_fast_path(swd_gpio_if())) ? 1U : 0U;

    if (request & DAP_TRANSFER_RnW) {
        return fast ? SWD_TransferFastRead(request, data)
                    : SWD_TransferSlowRead(request, data);
    }
    return fast ? SWD_TransferFastWrite(request, data)
                : SWD_TransferSlowWrite(request, data);
}

#endif /* (DAP_SWD != 0) */