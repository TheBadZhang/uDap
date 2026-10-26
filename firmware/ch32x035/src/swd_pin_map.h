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
#define SWD_PIN_VARIANT_PA5_PA6 2 /* PA5 = SWCLK，PA6 = SWDIO */
#define SWD_PIN_VARIANT_PA5_PA7 3 /* 现板：PA5 = SWCLK，PA7 = SWDIO
                                   * （选 PA7 是为了让 PA5=SPI1_SCK、PA7=SPI1_MOSI，
                                   *   可以试硬件 SPI 半双工加速数据相位）*/

#ifndef SWD_PIN_VARIANT
#define SWD_PIN_VARIANT SWD_PIN_VARIANT_PA5_PA7
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
#else
#error "未知的 SWD_PIN_VARIANT（可选 SWD_PIN_VARIANT_PA2_PA3 / SWD_PIN_VARIANT_PA5_PA6）"
#endif

#if (SWD_CLK_BIT > 7) || (SWD_DIO_BIT > 7)
#error "SWD 引脚必须落在 CFGLR 覆盖的 PA0..PA7 内"
#endif
#if SWD_CLK_BIT == SWD_DIO_BIT
#error "SWCLK 与 SWDIO 不能是同一个引脚"
#endif

/*
 * 哪些相位交给硬件 SPI。
 * 只有 PA5/PA7 方案能做：只有这一对脚同时是 SPI1_SCK 与 SPI1_MOSI，
 * 单线半双工（BIDIMODE）正好把 MOSI 当双向线用，与 SWD 的单线 SWDIO 对得上。
 * 实现见 src/dap/swd_spi_x035.c，默认关闭（位时序实现不受影响）。
 *
 * 三个相位可以单独开关，方便 A/B：请求头 8 拍、写数据 32 拍、读数据 32 拍。
 * 其余相位（turnaround、ACK 采样、数据相位的奇偶位、idle cycles）永远走 GPIO。
 */
#ifndef SWD_SPI_DATA_PHASE
#define SWD_SPI_DATA_PHASE 0
#endif
#ifndef SWD_SPI_REQ
/* 请求头默认**不**走 SPI。功能上是对的（查表 + 整字节发，6 次 64 KiB 长块读 0 错误），
 * 但实测慢一倍：写 279~288 → 150~162 KiB/s，稳定可复现。
 * 已排除的原因：WAIT 重试（两种配置 WAIT 计数相近）、余量不足（多补 turnaround 无效）、
 * 接收缓冲残留（打开 drain 更慢）。请求头只有 8 拍 ≈ 1 µs，收益上限也就 1 µs，
 * 所以默认给汇编，开关 + 查表留着备用。 */
#define SWD_SPI_REQ 0
#endif
#ifndef SWD_SPI_WR
#define SWD_SPI_WR SWD_SPI_DATA_PHASE
#endif
#ifndef SWD_SPI_RD
/* 读数据相位默认**不**走 SPI：这颗芯片的主设备 1 线接收能收到数据（6 MHz 下
 * DPIDR 能读对），但奇偶位（第 33 拍）采不到正确的采样点，而且数据会随 SPI
 * 时钟整体错位（6 MHz 偏 1 位、24 MHz 偏 6 位）—— 这条链路由目标驱动 SWDIO，
 * 建立时间不够。而 6 MHz 的 SPI 读并不比位时序快，所以默认留给位时序。
 * 想试的话把 SWD_SPI_RD 打开（并把 SWD_SPI_REQ 关掉，见 sw_dp.c 的说明）。 */
#define SWD_SPI_RD 0
#endif

/* 备注：试过把奇偶位并进 SPI 的连续时钟（5 帧 40 拍：32 数据 + 1 奇偶 + 7 个
 * idle 时钟），好处是数据相位与奇偶位之间完全没有软件空隙，但**目标不接受**：
 * 奇偶位之后多打 7 个时钟会让下一笔事务的 ACK 变成 JUNK（实测 examine 0/6）。
 * 这和读方向"多打时钟"的表现一致，所以奇偶位仍用 GPIO 补。
 */

/* SPI 的时钟分频（CTLR1 的 BR[2:0]，PCLK2 = 48 MHz）：
 *   000 = /2  24 MHz（默认）   001 = /4  12 MHz   010 = /8  6 MHz
 * 手册给的 SPI 上限是 FHCLK/2，所以 24 MHz 也是合法值；能不能跑取决于跳线质量。
 * 实测本板 5 cm 跳线：请求头 + 写数据相位 24 MHz 稳定（目标采样主机驱动的数据，
 * 余量大）；读数据相位由目标驱动 SWDIO，能跑多快取决于那条线的建立时间，
 * 拿不准就退到 001/010（用 tools/swd_link_check.ps1 看 wrong parity / JUNK）。 */
#ifndef SWD_SPI_BR
#define SWD_SPI_BR 0U
#endif

/* 写数据相位的送数方式（见 src/dap/swd_spi_x035.c）：
 *   SWD_SPI_DMA   = 1：DMA1_Channel3 搬 32 位，CPU 只写 CNTR + 使能，奇偶位在搬运时算
 *   SWD_SPI_16BIT = 1：用 16 位帧（2 次 DATAR 写），奇偶位在第 2 次的 TXE 等待里算
 *   都 = 0           ：8 位帧 + 查询（4 次 DATAR 写）
 * 两个可以同时开（DMA 那边按 16 位帧搬 2 个半字）。
 * 16 位帧在这颗芯片上配 1 线模式是原厂例程的用法（sdk/EXAM/SPI/1Lines-half-duplex
 * 就是 BIDIMODE + SPI_DataSize_16b）。 */
#ifndef SWD_SPI_16BIT
#define SWD_SPI_16BIT 0
#endif
#ifndef SWD_SPI_DMA
#define SWD_SPI_DMA 0
#endif

#if (SWD_SPI_DATA_PHASE || SWD_SPI_REQ || SWD_SPI_WR || SWD_SPI_RD) && \
    (SWD_PIN_VARIANT != SWD_PIN_VARIANT_PA5_PA7)
#error "SWD_SPI_* 需要 SWD_PIN_VARIANT = SWD_PIN_VARIANT_PA5_PA7"
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
/* 按给定电平设置 SWDIO 的 ODR（0 / 非 0）。外设收回引脚前用它把 ODR 摆好，
 * 免得切回推挽输出的瞬间在 SWDIO 上顶出一个毛刺。 */
BOARD_SWD_ATTR void board_swd_dio_out(uint32_t level) {
    if (level) {
        SWD_PORT->BSHR = SWD_DIO_PIN;
    } else {
        SWD_PORT->BCR = SWD_DIO_PIN;
    }
}
/* 读 SWDIO 电平（快路径每比特都要读，必须是编译期展开） */
BOARD_SWD_ATTR uint32_t board_swd_dio_level(void) { return (SWD_PORT->INDR & SWD_DIO_PIN) != 0U; }

/* 推挽输出的速度档（CFGLR 低 2 位）：
 *   0x1 = 输出 10 MHz，0x3 = 输出 50 MHz
 * 实测（PA5/PA6 + 跳线接 STM32F411）用 0x3 **更差**：6 MHz 下 DPIDR+examine
 * 0/10，而 0x1 有 2~3/10 —— 边沿太快在长线上振铃，所以保持 10 MHz 档。
 * 若换短排线/加地线后要再试，只改这一个宏即可。 */
#define BOARD_SWD_OUT_CFG 0x1UL

/* SWDIO 方向：切成推挽输出 / 释放为浮空输入（供目标驱动）。
 * 只有 turnaround 用，每事务一次 —— 但仍然内联，保持一致。 */
BOARD_SWD_ATTR void board_swd_dio_output(void) {
    SWD_PORT->CFGLR =
        (SWD_PORT->CFGLR & ~(0xFUL << SWD_DIO_CFGLR_SHIFT)) | (BOARD_SWD_OUT_CFG << SWD_DIO_CFGLR_SHIFT);
}
BOARD_SWD_ATTR void board_swd_dio_input(void) {
    SWD_PORT->CFGLR =
        (SWD_PORT->CFGLR & ~(0xFUL << SWD_DIO_CFGLR_SHIFT)) | (0x4UL << SWD_DIO_CFGLR_SHIFT);
}

/* ---------------------------------------------------------------------------
 * 把引脚交给/收回外设（硬件 SPI 数据相位用，见 src/dap/swd_spi_x035.c）
 * ---------------------------------------------------------------------------
 * AF_CFG = 0x8（复用推挽）| 速度档 0x1，与 SDK 的
 * GPIO_Mode_AF_PP + GPIO_Speed_50MHz 编出来的 CFGLR 值相同。
 *
 * 交接顺序很重要（每一步都是为了让 SWCLK 上没有假边沿）：
 *   交出：SPE 置位后一次 CFGLR 把两脚都切到 AF —— 外设此时已把 SCK 摆在空闲
 *         电平（CPOL = 低），与 GPIO 原电平相同，所以没有边沿
 *   收回：先把 ODR 该位写 0 → 再改 CFGLR 回推挽输出（否则 ODR 残值为 1 会顶出
 *         一个高电平毛刺）
 *
 * 两脚各写一次 CFGLR 也是可以的，但数据相位每个事务都要走一遍，
 * 合并成一次读改写能省下两次寄存器访问。
 */
#define BOARD_SWD_AF_CFG 0x9UL

/* 两个脚一起交给外设 / 一起收回 GPIO */
BOARD_SWD_ATTR void board_swd_clk_dio_to_af(void) {
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (BOARD_SWD_AF_CFG << SWD_CLK_CFGLR_SHIFT) | (BOARD_SWD_AF_CFG << SWD_DIO_CFGLR_SHIFT);
}
BOARD_SWD_ATTR void board_swd_clk_dio_to_gpio(void) {
    SWD_PORT->BCR = SWD_CLK_PIN; /* SCK 回 GPIO 前先把 ODR 拉低（空闲电平 = CPOL = 低） */
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (BOARD_SWD_OUT_CFG << SWD_CLK_CFGLR_SHIFT) | (BOARD_SWD_OUT_CFG << SWD_DIO_CFGLR_SHIFT);
}
/* SCK 交给外设、SWDIO 保持浮空输入（1 线接收方向：目标在驱动 SWDIO）。
 * 引脚配置与 SDK 的 1Lines_half-duplex 例程一致：主设备 1 线发送时用 AF_PP，
 * 接收方向那一端用 IN_FLOATING。 */
BOARD_SWD_ATTR void board_swd_clk_to_af_dio_input(void) {
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (BOARD_SWD_AF_CFG << SWD_CLK_CFGLR_SHIFT) | (0x4UL << SWD_DIO_CFGLR_SHIFT);
}
/* 只收回 SCK（读相位结束时 SWDIO 还得留给目标驱动奇偶位）。
 * 不动 ODR：调用方负责先把 ODR 摆成要留下的电平（写相位低、读相位高），
 * 这样切回推挽输出的瞬间引脚上不会出现毛刺。 */
BOARD_SWD_ATTR void board_swd_clk_to_gpio(void) {
    SWD_PORT->CFGLR =
        (SWD_PORT->CFGLR & ~(0xFUL << SWD_CLK_CFGLR_SHIFT)) | (BOARD_SWD_OUT_CFG << SWD_CLK_CFGLR_SHIFT);
}

/* 端口整体：连接（CLK/DIO 推挽输出、先摆成空闲高）/ 断开（两脚浮空）。
 * 换驱动（swd_gpio_if_t）时由实现自己负责，这里供内置实现与 DAP_config.h 共用。 */
BOARD_SWD_ATTR void board_swd_port_on(void) {
    SWD_PORT->BSHR = SWD_CLK_PIN | SWD_DIO_PIN;
    SWD_PORT->CFGLR = (SWD_PORT->CFGLR &
                       ~((0xFUL << SWD_CLK_CFGLR_SHIFT) | (0xFUL << SWD_DIO_CFGLR_SHIFT))) |
                      (BOARD_SWD_OUT_CFG << SWD_CLK_CFGLR_SHIFT) |
                      (BOARD_SWD_OUT_CFG << SWD_DIO_CFGLR_SHIFT);
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
