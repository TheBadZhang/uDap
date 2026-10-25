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

#define PIN_SWDIO_OUTPUT() { swd_gpio_if()->dio_out_enable(); }
#define PIN_SWDIO_INPUT() { swd_gpio_if()->dio_out_disable(); }


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

    /* Packet Request —— 8 个 SWCLK（start/APnDP/RnW/A2/A3/parity/stop）。
     * 每拍 8 个 clk、低 4 / 高 4，指令顺序必须钉住，所以整段在
     * swd_fast_packet_request() 的手写汇编里（摆位与理由见
     * src/dap/swd_gpio_x035.c）。
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
            /* Read data —— 32 个数据位（SWD 为 LSB first，每 bit 8 个 SWCLK、
             * 低 4 / 高 4）。位循环、奇偶累加全部在 swd_fast_read_data() 的
             * 手写汇编里（摆位与理由见 src/dap/swd_gpio_x035.c）。
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
        } else {
            /* Turnaround */
            PIN_SWCLK_CLR();
            PIN_SWDIO_OUTPUT(); // turnaround
            PIN_SWCLK_SET();

            /* Write data */
            val = *data;
            const uint32_t parity = SWD_Parity32(val);
            swd_gpio_if()->write_data(val);
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
    // 快路径要求当前实现具备三段位时序（swd_gpio.h 约定：全给或全 NULL）。
    // 换成没有快路径的实现（例如纯 GPIO 轮询、外挂 MCU）时整体退回慢路径，
    // 不会出现「请求头走快、数据相位走慢」的半快半慢状态。
    if (DAP_Data.fast_clock && swd_gpio_has_fast_path(swd_gpio_if())) {
        return SWD_TransferFast(request, data);
    } else {
        return SWD_TransferSlow(request, data);
    }
}

#endif /* (DAP_SWD != 0) */