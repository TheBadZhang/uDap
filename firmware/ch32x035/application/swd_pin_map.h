#pragma once

/*
 * 板级 SWD 调试引脚映射。
 *
 * CMSIS-DAP 的 SWCLK/SWDIO 与 WCH-Link 的 DCK/DIO 在板上是**同一对引脚**，
 * 两个模式互斥使用（见 src/mode_overlay.h），所以引脚定义集中在本文件：
 * 换板 / 改线只改 SWD_PIN_VARIANT（每块板的默认值在 application/board/*.h 的
 * BOARD_SWD_DEFAULT_PIN_VARIANT），DAP 与 WCH-Link 两条路径一起跟着变，
 * 不需要再去逐个改汇编立即数或 CFGLR 移位。
 *
 * 用到的 CH32X035 GPIO 行为（参考手册 GPIO 章节）：
 *   - CFGLR 管 PA0..PA7，每脚 4 位：0x1 = 推挽输出(10 MHz)，0x3 = 推挽输出(50 MHz)，
 *     0x4 = 浮空输入，0x8 = 上下拉输入（上拉/下拉由 OUTDR 该位选择）
 *   - BSHR 的**低 16 位写 1 置位**、**高 16 位写 1 复位**，一次 store 就能
 *     「复位 A 脚 + 置位 B 脚」；BCR 只能复位；INDR 读引脚电平
 *   - 以上都是「写 1 有效」，读出来永远是 0
 *
 * 两个引脚必须同属 GPIOA 且都落在 CFGLR 覆盖范围（bit 0..7）：快路径用
 * 「一次 BSHR store 同时拉低 SWCLK 并设置 SWDIO」压时序，跨 port 就得写两次。
 * 目前四套方案（pa2_pa3 / pa5_pa6 / pa5_pa7 / pa0_pa1）都满足这一点。
 */

#include <ch32x035.h>

/* 板级表（BOARD_SWD_DEFAULT_PIN_VARIANT / BOARD_NRESET_* 等）。依赖方向是
 * 「本文件 -> board_pin_map.h」：板级头文件不反向包含本文件。 */
#include "board_pin_map.h"

/* 可选方案（同一 port 上的一对引脚） */
#define SWD_PIN_VARIANT_PA2_PA3 1 /* 旧板：PA2 = SWCLK，PA3 = SWDIO */
#define SWD_PIN_VARIANT_PA5_PA6 2 /* uDap-CH32X035F8U6：PA5 = SWCLK，PA6 = SWDIO */
#define SWD_PIN_VARIANT_PA5_PA7 3 /* PA5 = SWCLK，PA7 = SWDIO */
#define SWD_PIN_VARIANT_PA0_PA1 4 /* uDap-CH32X033：PA1 = SWCLK，PA0 = SWDIO */

/* 默认方案来自板级表；构建时仍可用 --swd_pins=... 显式覆盖（A/B 对照用） */
#ifndef SWD_PIN_VARIANT
#define SWD_PIN_VARIANT BOARD_SWD_DEFAULT_PIN_VARIANT
#endif

#if SWD_PIN_VARIANT == SWD_PIN_VARIANT_PA5_PA6
#define SWD_PORT       GPIOA
#define SWD_PORT_CLOCK RCC_APB2Periph_GPIOA
#define SWD_CLK_BIT    5
#define SWD_DIO_BIT    6
// 方案名：用来给实现里的符号加后缀（见 src/dap/swd_gpio.h 的 SWD_GPIO_IMPL）。
// 于是 map / nm 里能直接看出这份固件编的是哪套脚：
//     swd_fast_write_data_pa5_pa6
// 两套方案各编一份实现时符号也不冲突，可以在运行期选。
#define SWD_PIN_VARIANT_NAME pa5_pa6
#define SWD_PIN_VARIANT_STR  "pa5_pa6"
#elif SWD_PIN_VARIANT == SWD_PIN_VARIANT_PA5_PA7
#define SWD_PORT       GPIOA
#define SWD_PORT_CLOCK RCC_APB2Periph_GPIOA
#define SWD_CLK_BIT    5
#define SWD_DIO_BIT    7
#define SWD_PIN_VARIANT_NAME pa5_pa7
#define SWD_PIN_VARIANT_STR  "pa5_pa7"
#elif SWD_PIN_VARIANT == SWD_PIN_VARIANT_PA2_PA3
#define SWD_PORT       GPIOA
#define SWD_PORT_CLOCK RCC_APB2Periph_GPIOA
#define SWD_CLK_BIT    2
#define SWD_DIO_BIT    3
#define SWD_PIN_VARIANT_NAME pa2_pa3
#define SWD_PIN_VARIANT_STR  "pa2_pa3"
#elif SWD_PIN_VARIANT == SWD_PIN_VARIANT_PA0_PA1
#define SWD_PORT       GPIOA
#define SWD_PORT_CLOCK RCC_APB2Periph_GPIOA
#define SWD_CLK_BIT    1
#define SWD_DIO_BIT    0
#define SWD_PIN_VARIANT_NAME pa0_pa1
#define SWD_PIN_VARIANT_STR  "pa0_pa1"
#else
#error "未知的 SWD_PIN_VARIANT（可选 PA2_PA3 / PA5_PA6 / PA5_PA7 / PA0_PA1）"
#endif

#if (SWD_CLK_BIT > 7) || (SWD_DIO_BIT > 7)
#error "SWD 引脚必须落在 CFGLR 覆盖的 PA0..PA7 内"
#endif
#if SWD_CLK_BIT == SWD_DIO_BIT
#error "SWCLK 与 SWDIO 不能是同一个引脚"
#endif


/* 位掩码（GPIO_Pin_x 形式，可直接写 BSHR/BCR/INDR） */
#define SWD_CLK_PIN (1UL << SWD_CLK_BIT)
#define SWD_DIO_PIN (1UL << SWD_DIO_BIT)
/* CFGLR 字段移位：每脚 4 位 */
#define SWD_CLK_CFGLR_SHIFT (SWD_CLK_BIT * 4)
#define SWD_DIO_CFGLR_SHIFT (SWD_DIO_BIT * 4)

/* ---------------------------------------------------------------------------
 * 板级引脚原语（SWD）
 * ---------------------------------------------------------------------------
 * 这里是**唯一**写 SWD 两个脚寄存器的地方；src/dap/DAP_config.h 只负责把
 * CMSIS-DAP 的契约名（PIN_SWCLK_TCK_* 等）转发到这里。所以换板只需改本文件顶部的
 * SWD_PIN_VARIANT，寄存器写法（BSHR/BCR/INDR/CFGLR）集中在这一处。
 *
 * 全部 always_inline + 编译期常量：展开后就是一条 store/load，与直接写在
 * DAP_config.h 里生成的代码完全相同。**不能**改成运行期函数指针 ——
 * 快路径一个 SWCLK 周期就等于两条 store 之间的指令数（见 sw_dp.c 文件头）。
 *
 * 寄存器语义（参考手册 GPIO 章节）：
 *   BSHR 低 16 位写 1 置位、高 16 位写 1 复位；BCR 只复位；INDR 读引脚电平；
 *   CFGLR 每脚 4 位：0x1 = 推挽输出(10 MHz)，0x4 = 浮空输入，0x8 = 上下拉输入
 *   —— 都是「写 1 有效」，读出来永远是 0。
 */
#define BOARD_SWD_ATTR static inline __attribute__((always_inline))

/* SWCLK 电平置高 / 拉低 */
BOARD_SWD_ATTR void board_swd_clk_set(void) { SWD_PORT->BSHR = SWD_CLK_PIN; }
BOARD_SWD_ATTR void board_swd_clk_clr(void) { SWD_PORT->BCR = SWD_CLK_PIN; }
/* 读 SWCLK 电平（仅 DAP.c 的 SWJ_Pins 读回用到） */
BOARD_SWD_ATTR uint32_t board_swd_clk_level(void) { return (SWD_PORT->INDR & SWD_CLK_PIN) != 0U; }

/* SWDIO 电平置高 / 拉低 */
BOARD_SWD_ATTR void board_swd_dio_set(void) { SWD_PORT->BSHR = SWD_DIO_PIN; }
BOARD_SWD_ATTR void board_swd_dio_clr(void) { SWD_PORT->BCR = SWD_DIO_PIN; }
/* 读 SWDIO 电平（快路径每比特都要读，必须是编译期展开） */
BOARD_SWD_ATTR uint32_t board_swd_dio_level(void) { return (SWD_PORT->INDR & SWD_DIO_PIN) != 0U; }

/* 推挽输出的速度档（CFGLR 低 2 位）：
 *   0x1 = 输出 10 MHz，0x3 = 输出 50 MHz
 * 实测（PA5/PA6 + 跳线接 STM32F411）用 0x3 **更差**：6 MHz 下 DPIDR+examine
 * 0/10，而 0x1 有 2~3/10 —— 边沿太快在长线上振铃，所以保持 10 MHz 档。
 * 若换短排线/加地线后要再试，只改这一个宏即可。 */
#define BOARD_SWD_OUT_CFG 0x1UL

/* ---------------------------------------------------------------------------
 * CFGLR 的**两份预算值**：省掉 DIO 方向切换时的外设读
 * ---------------------------------------------------------------------------
 * CFGLR 每脚 4 位、只能读改写（lw CFGLR + and + or + sw），而 SWD 每个事务要切
 * 两次 SWDIO 方向。外设读（AHB→APB）在这颗芯片上比几条指令贵得多，所以把两个
 * 方向的**完整 CFGLR 值**预先算好放在 RAM 里，切换时只剩
 *
 *     lw  a5, board_swd_cfglr_out    ; 或 _in
 *     sw  a5, 0(GPIOA)               ; = CFGLR
 *
 * 两条指令、不占额外寄存器、也不需要 lui/or 现组常量。实测（STM32F411，64 KiB，
 * fork OpenOCD，5 轮）比「缓存 base 再 or」的写法又快约 1%（266.6 vs 263.9 KiB/s
 * 写），而且只在 .highcode 里多 12 字节 —— .highcode 是 RAM，挤出来的都是 SRAM。
 * 代价是 8 字节 RAM（两个 word）。
 *
 * 什么时候必须重建（任何「别人」改过 CFGLR 之后）：
 *   - 本文件的 port_on / port_off：自己改完顺手重建；
 *   - src/drv/drv_uart.c 改 PA2/PA3 之后：调 board_swd_cfglr_resync()；
 *   - DAP_config.h 的 nRESET（默认关闭）同样在改完调一次。
 * 漏掉重建的后果是 PA2/PA3 的配置被旧值覆盖（UART 流控脚失效）。
 */
extern uint32_t board_swd_cfglr_out; /* CFGLR 值：SWDIO 推挽输出 */
extern uint32_t board_swd_cfglr_in;  /* CFGLR 值：SWDIO 浮空输入 */

BOARD_SWD_ATTR void board_swd_cfglr_resync(void) {
    const uint32_t base = SWD_PORT->CFGLR & ~(0xFUL << SWD_DIO_CFGLR_SHIFT);
    board_swd_cfglr_out = base | (BOARD_SWD_OUT_CFG << SWD_DIO_CFGLR_SHIFT);
    board_swd_cfglr_in = base | (0x4UL << SWD_DIO_CFGLR_SHIFT);
}

/* SWDIO 方向：切成推挽输出 / 释放为浮空输入（供目标驱动）。
 * 只有 turnaround 用，每事务两次 —— 读一次 RAM、写一次寄存器。 */
BOARD_SWD_ATTR void board_swd_dio_output(void) {
    SWD_PORT->CFGLR = board_swd_cfglr_out;
}
BOARD_SWD_ATTR void board_swd_dio_input(void) {
    SWD_PORT->CFGLR = board_swd_cfglr_in;
}


/* 端口整体：连接（CLK/DIO 推挽输出、先摆成空闲高）/ 断开（两脚浮空）。
 * 供内置实现（x035_port_setup/off）与 DAP_config.h 的 PORT_SWD_SETUP/PORT_OFF 共用。 */
BOARD_SWD_ATTR void board_swd_port_on(void) {
    SWD_PORT->BSHR = SWD_CLK_PIN | SWD_DIO_PIN;
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (BOARD_SWD_OUT_CFG << SWD_CLK_CFGLR_SHIFT) |
                      (BOARD_SWD_OUT_CFG << SWD_DIO_CFGLR_SHIFT);
    board_swd_cfglr_resync();
}
BOARD_SWD_ATTR void board_swd_port_off(void) {
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (0x4UL << SWD_CLK_CFGLR_SHIFT) | (0x4UL << SWD_DIO_CFGLR_SHIFT);
    board_swd_cfglr_resync();
}

/*
 * nRESET（目标复位，低电平有效）。
 *
 * 引脚来自板级表：uDap-CH32X035F8U6 接到 PA7，uDap-CH32X033 接到 PA9。
 * 两者都在 GPIOA，但前者落在 CFGLR（pin0..7）、后者落在 CFGHR（pin8..15），
 * 访问方式不同（见 application/board_pin_map.h 的约定），所以下面分成两支：
 *   pin0..7  -> 裸写 CFGLR（无缓存），改完重建 SWD 的 CFGLR 影子
 *   pin8..15 -> 走 GPIO_Init()（CFGHR 有 SDK 静态缓存，裸写会让它失步）
 *
 * 若 SWDIO 也用了这一脚（例如 pa5_pa7 且 nRESET 仍是 PA7），编译期检查会报错。
 * SWD_NRESET_BIT 可由构建参数覆盖（-DSWD_NRESET_BIT=n）。
 *
 * nRESET 必须与 SWD 同 port：DAP_SETUP() 只开 SWD_PORT_CLOCK。
 */
#ifndef SWD_NRESET_ENABLE
#define SWD_NRESET_ENABLE BOARD_NRESET_ENABLE
#endif
#ifndef SWD_NRESET_BIT
#define SWD_NRESET_BIT BOARD_NRESET_BIT
#endif

#if SWD_NRESET_ENABLE

#define SWD_NRESET_PORT GPIOA

#if (SWD_NRESET_BIT == SWD_CLK_BIT) || (SWD_NRESET_BIT == SWD_DIO_BIT)
#error "SWD_NRESET_BIT 与 SWCLK/SWDIO 冲突"
#endif
#if SWD_NRESET_BIT > 15
#error "SWD_NRESET_BIT 超出 PA0..PA15"
#endif

#define SWD_NRESET_PIN         (1UL << SWD_NRESET_BIT)
#define SWD_NRESET_CFGLR_SHIFT ((SWD_NRESET_BIT & 7u) * 4u)

/* 读回真实线电平：释放时主机据此判断目标是否自己拉着复位 */
BOARD_SWD_ATTR uint32_t board_nreset_level(void) {
    return (SWD_NRESET_PORT->INDR & SWD_NRESET_PIN) != 0U;
}

/* 开漏模拟：释放（asserted = 0）= 输入 + 上拉（高阻），复位 = 推挽输出低。
 * 两条路径都「先设好输出数据、再改配置」，避免切换瞬间输出旧电平产生毛刺。
 * 只在主机 SWJ_Pins / RESET_TARGET 里调用，不是每比特路径，可以走 GPIO_Init。 */
static inline void board_nreset_drive(uint32_t asserted) {
    if (asserted) {
        SWD_NRESET_PORT->BCR = SWD_NRESET_PIN;  /* ODR 先置 0 */
    } else {
        SWD_NRESET_PORT->BSHR = SWD_NRESET_PIN; /* ODR 先置 1（输入上拉时即上拉） */
    }

#if SWD_NRESET_BIT < 8
    SWD_NRESET_PORT->CFGLR =
        (SWD_NRESET_PORT->CFGLR & ~(0xFUL << SWD_NRESET_CFGLR_SHIFT)) |
        ((asserted ? 0x1UL : 0x8UL) << SWD_NRESET_CFGLR_SHIFT);
    board_swd_cfglr_resync(); /* 动过 CFGLR，SWD 的影子要重建 */
#else
    {
        GPIO_InitTypeDef gpio = {0};

        gpio.GPIO_Pin = SWD_NRESET_PIN;
        gpio.GPIO_Speed = GPIO_Speed_50MHz;
        gpio.GPIO_Mode = asserted ? GPIO_Mode_Out_PP : GPIO_Mode_IPU;
        GPIO_Init(SWD_NRESET_PORT, &gpio);
    }
#endif
}

#endif /* SWD_NRESET_ENABLE */
