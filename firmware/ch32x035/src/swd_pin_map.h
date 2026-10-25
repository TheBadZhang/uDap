#pragma once

/*
 * 板级 SWD 调试引脚映射。
 *
 * CMSIS-DAP 的 SWCLK/SWDIO 与 WCH-Link 的 DCK/DIO 在板上是**同一对引脚**，
 * 两个模式互斥使用（见 src/mode_overlay.h），所以引脚定义集中在本文件：
 * 换板 / 改线只改 SWD_PIN_VARIANT，DAP 与 WCH-Link 两条路径一起跟着变，
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
 */

#include <ch32x035.h>

/* 可选方案（同一 port 上的一对引脚） */
#define SWD_PIN_VARIANT_PA2_PA3 1 /* 旧板：PA2 = SWCLK，PA3 = SWDIO */
#define SWD_PIN_VARIANT_PA5_PA6 2 /* 现板：PA5 = SWCLK，PA6 = SWDIO */

#ifndef SWD_PIN_VARIANT
#define SWD_PIN_VARIANT SWD_PIN_VARIANT_PA5_PA6
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
#elif SWD_PIN_VARIANT == SWD_PIN_VARIANT_PA2_PA3
#define SWD_PORT       GPIOA
#define SWD_PORT_CLOCK RCC_APB2Periph_GPIOA
#define SWD_CLK_BIT    2
#define SWD_DIO_BIT    3
#define SWD_PIN_VARIANT_NAME pa2_pa3
#define SWD_PIN_VARIANT_STR  "pa2_pa3"
#else
#error "未知的 SWD_PIN_VARIANT（可选 SWD_PIN_VARIANT_PA2_PA3 / SWD_PIN_VARIANT_PA5_PA6）"
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

/* SWDIO 方向：切成推挽输出 / 释放为浮空输入（供目标驱动）。
 * 只有 turnaround 用，每事务一次 —— 但仍然内联，保持一致。 */
BOARD_SWD_ATTR void board_swd_dio_output(void) {
    SWD_PORT->CFGLR =
        (SWD_PORT->CFGLR & ~(0xFUL << SWD_DIO_CFGLR_SHIFT)) | (0x1UL << SWD_DIO_CFGLR_SHIFT);
}
BOARD_SWD_ATTR void board_swd_dio_input(void) {
    SWD_PORT->CFGLR =
        (SWD_PORT->CFGLR & ~(0xFUL << SWD_DIO_CFGLR_SHIFT)) | (0x4UL << SWD_DIO_CFGLR_SHIFT);
}

/* 端口整体：连接（CLK/DIO 推挽输出、先摆成空闲高）/ 断开（两脚浮空）。
 * 换驱动（swd_gpio_if_t）时由实现自己负责，这里供内置实现与 DAP_config.h 共用。 */
BOARD_SWD_ATTR void board_swd_port_on(void) {
    SWD_PORT->BSHR = SWD_CLK_PIN | SWD_DIO_PIN;
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (0x1UL << SWD_CLK_CFGLR_SHIFT) | (0x1UL << SWD_DIO_CFGLR_SHIFT);
}
BOARD_SWD_ATTR void board_swd_port_off(void) {
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (0x4UL << SWD_CLK_CFGLR_SHIFT) | (0x4UL << SWD_DIO_CFGLR_SHIFT);
}

/*
 * nRESET（目标复位，低电平有效）。
 *
 * 本板原理图（hardware/ch32x035）里 net /NRST 只引到调试排针，MCU 侧没有
 * 连接；而 PA5 已经让给 SWCLK，所以默认**关闭**：
 *   - PIN_nRESET_OUT() 退化为空操作
 *   - DAP_ResetTarget 返回「未执行厂商复位」，主机（OpenOCD / pyOCD）会退回
 *     SWD 的 SYSRESETREQ，仍然能复位目标
 *
 * 换板后把 SWD_NRESET_BIT 指向实际引脚、SWD_NRESET_ENABLE 置 1 即可启用。
 * 注意 SWD_NRESET_BIT 的默认值沿用旧板的 PA5，启用前必须改 —— 不改会被下面的
 * 编译期检查挡住（PA5 现在是 SWCLK）。
 */
#ifndef SWD_NRESET_ENABLE
#define SWD_NRESET_ENABLE 0
#endif
#ifndef SWD_NRESET_BIT
#define SWD_NRESET_BIT 5
#endif

#if SWD_NRESET_ENABLE
#if (SWD_NRESET_BIT == SWD_CLK_BIT) || (SWD_NRESET_BIT == SWD_DIO_BIT)
#error "SWD_NRESET_BIT 与 SWCLK/SWDIO 冲突"
#endif
#if SWD_NRESET_BIT > 7
#error "nRESET 引脚必须落在 CFGLR 覆盖的 PA0..PA7 内"
#endif
#define SWD_NRESET_PIN         (1UL << SWD_NRESET_BIT)
#define SWD_NRESET_CFGLR_SHIFT (SWD_NRESET_BIT * 4)
#endif
