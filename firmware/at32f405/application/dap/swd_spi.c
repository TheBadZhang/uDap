/**
 * @file    swd_spi.c
 * @brief   SPI 硬件时钟加速版 SWD 物理层（与 GPIO 位翻转版二选一）
 *
 * 与 cherrydap 的 DAP/Source/SW_DP.c 提供同一组符号
 * （SWJ_Sequence / SWD_Sequence / SWD_Transfer），因此 xmake 里用
 * --swd_phy=gpio|spi 选择编哪一个，上层（DAP.c / JTAG_DP.c）完全不用改。
 *
 * 为什么只把"数据相位"交给 SPI：
 *   板子上 PA6(MISO) 与 PA7(MOSI) 直接并到同一根 SWDIO 上，中间没有串阻。
 *   如果像纯软件那样"边驱动边读"来顶 ACK，总线和目标会互相打架；
 *   而 ACK 只有 3 个 bit、外面还夹着 1 个转向周期，SPI 的 8 bit 帧根本
 *   凑不出这个粒度。所以请求字节、转向、ACK、奇偶校验位仍然用 GPIO
 *   位翻转（这部分只占一个传输约 1/4 的时钟），32 bit 数据相位
 *   （约 3/4 的时钟）交给 SPI1 硬件搬运，CPU 只管塞/取 FIFO。
 *
 * 方向切换：写相位把 PA7 切到 SPI1_MOSI(AF5) 由硬件驱动；
 *          读相位把 PA7 切回 GPIO 输入，让目标独占总线，MISO 从 PA6 采样。
 */

#include "board_pin_map.h"
#include "system_at32f402_405.h"
#include "DAP_config.h"
#include "DAP.h"


#if (DAP_SWD != 0) && defined(SWD_PHY_SPI) && (SWD_PHY_SPI != 0)


/* DAP_config.h 只提供 PIN_SWCLK_TCK_* 这一组名字（SW_DP.c 里自己做了别名），
 * 这里照做一份，保持代码与上游实现长得一样。 */
#define PIN_SWCLK_SET           PIN_SWCLK_TCK_SET
#define PIN_SWCLK_CLR           PIN_SWCLK_TCK_CLR

/* SPI 收发等待的循环上限：正常一个字节几百纳秒就完成，这里主要是防止
 * SPI 因为外设没配好而卡死，把 DAP 主循环一起拖住。 */
#define SPI_BURST_TIMEOUT       100000U

/* 数据相位走哪套实现：
 *   SWD_SPI_TX_VIA_GPIO = 0 → 写数据相位交给 SPI1 硬件移位（提速主力）
 *   SWD_SPI_RX_VIA_GPIO = 0 → 读数据相位也用 SPI（实测可用，见 README 的测速表）
 *
 * 相位关键是「SPI 只搬 32 个数据位」：SPI 一次只能发 8 的整数倍，多发 1 个字节
 * 就是 8 个多余时钟，会被目标当成下一个请求的 start bit，总线立刻失步（再也读
 * 不到 ACK）。所以奇偶位（第 33 拍）一律由 GPIO 补。
 * 读相位 SWDIO 由目标驱动，本板 PA6(MISO) 与 PA7(MOSI) 并线，只能在数据相位短暂
 * 把 PA6 切到 SPI，其余时刻（ACK、奇偶位、转向）都保持 GPIO 输入。
 */
#ifndef SWD_SPI_TX_VIA_GPIO
#define SWD_SPI_TX_VIA_GPIO     0
#endif
#ifndef SWD_SPI_RX_VIA_GPIO
#define SWD_SPI_RX_VIA_GPIO     0
#endif

/* 请求字节（8 bit）也走 SPI：位数正好凑满一个字节，又没有反向数据，
 * 比位翻转省掉 8 次"两个半周期的延时循环"。设 1 关掉即退回位翻转。 */
#ifndef SWD_SPI_REQ_VIA_SPI
#define SWD_SPI_REQ_VIA_SPI     1
#endif


/* -------------------------------------------------------------------------
 * 引脚/寄存器小工具
 *
 * GPIOA 的 cfgr 每 2 bit 描述一个引脚：0=输入 1=输出 2=复用 3=模拟。
 * 直接改寄存器而不是调 gpio_init()，因为每笔传输都要来回切两次方向，
 * 走库函数会把本来就不多的提速吃掉。
 * ---------------------------------------------------------------------- */
#define PIN_IOMC_SHIFT(pin)     ((pin) * 2U)
#define PIN_IOMC_MASK(pin)      (3UL << PIN_IOMC_SHIFT(pin))

#define SCK_PIN_NUM             5U
#define MOSI_PIN_NUM            7U
#define MISO_PIN_NUM            6U

#define SPI1_AF_NUM             GPIO_MUX_5

#define SWD_DELAY()             \
    do {                        \
        if (DAP_Data.fast_clock) {  \
            PIN_DELAY_FAST();   \
        } else {                \
            PIN_DELAY_SLOW(DAP_Data.clock_delay); \
        }                       \
    } while (0)

static void pin_set_mode(uint32_t pin, uint32_t mode)
{
    uint32_t cfgr = GPIOA->cfgr & ~PIN_IOMC_MASK(pin);

    GPIOA->cfgr = cfgr | (mode << PIN_IOMC_SHIFT(pin));
}

/* 热路径用的 CFGR 常量表：先把 SCK/MISO/MOSI 三段的位清掉，之后每次切换
 * 只是一次 store，省掉读-改-写的三步开销（每字约 10 次切换）。 */
static uint32_t s_cfgr_base;

static inline void phy_cfg(uint32_t sck, uint32_t mosi, uint32_t miso)
{
    BOARD_SWD_PORT->cfgr = s_cfgr_base | (sck << (SCK_PIN_NUM * 2U)) |
                           (miso << (MISO_PIN_NUM * 2U)) | (mosi << (MOSI_PIN_NUM * 2U));
}

#define PHY_REQ_WR()    phy_cfg(GPIO_MODE_MUX,    GPIO_MODE_MUX,    GPIO_MODE_INPUT)
#define PHY_RD()        phy_cfg(GPIO_MODE_MUX,    GPIO_MODE_INPUT,  GPIO_MODE_MUX)
#define PHY_ACK()       phy_cfg(GPIO_MODE_OUTPUT, GPIO_MODE_INPUT,  GPIO_MODE_INPUT)
#define PHY_IDLE()      phy_cfg(GPIO_MODE_OUTPUT, GPIO_MODE_OUTPUT, GPIO_MODE_INPUT)

#define sck_to_spi()            pin_set_mode(SCK_PIN_NUM,  GPIO_MODE_MUX)
#define mosi_to_spi()           pin_set_mode(MOSI_PIN_NUM, GPIO_MODE_MUX)
#define mosi_to_gpio_in()       pin_set_mode(MOSI_PIN_NUM, GPIO_MODE_INPUT)

/* 从 SPI 驱动切回 GPIO 驱动之前，必须先把输出锁存写成 1：
 * 否则切回去的一瞬间会把 SWDIO 拉低，紧接着的转向时钟就会被目标当成一个
 * 低电平的 start bit，整个传输随之错位。 */
static void mosi_to_gpio_out(void)
{
    PIN_SET(BOARD_SWD_PORT, BOARD_SWDIO_OUT_PIN);
    pin_set_mode(MOSI_PIN_NUM, GPIO_MODE_OUTPUT);
}

/* 数据相位才把 PA6 交给 SPI1_MISO；ACK/空闲期它必须是普通输入（见 spi_phy_setup） */
static void miso_to_spi(void)
{
    pin_set_mode(MISO_PIN_NUM, GPIO_MODE_MUX);
}

static void miso_to_gpio_in(void)
{
    pin_set_mode(MISO_PIN_NUM, GPIO_MODE_INPUT);
    GPIOA->pull = (GPIOA->pull & ~PIN_IOMC_MASK(MISO_PIN_NUM)) |
                  ((uint32_t)GPIO_PULL_UP << PIN_IOMC_SHIFT(MISO_PIN_NUM));
}

/* SWCLK 回到 GPIO 之前先把输出锁存写成高，避免切回瞬间拉出一个低电平毛刺 */
static void sck_to_gpio(void)
{
    PIN_SWCLK_SET();
    pin_set_mode(SCK_PIN_NUM, GPIO_MODE_OUTPUT);
}


/* -------------------------------------------------------------------------
 * SPI1 初始化
 *
 * SWD 的时序是"下降沿换数据、上升沿采样"，对应 SPI 模式 3（CPOL=1、CPHA=2EDGE）；
 * SWD 又是低位先出，正好对上 SPI 的 LSB-first 帧格式，一个字节 = 8 个 SWCLK。
 * ---------------------------------------------------------------------- */
static uint32_t s_spi_div = 0U;                 /* 当前 MCLK 分频值，0 表示还没初始化 */
static uint32_t s_last_delay = 0xFFFFFFFFU;     /* 上次配置用的 clock_delay */

/* 便于用另一台探针核对「上位机要的 SWCLK」和「SPI 实际分频」：
 * [0] = 请求的阶段（0=慢/1=快）  [1] = s_spi_div
 * [2] = clock_delay              [3] = 实际 SWCLK（kHz）
 * [4]/[5] = SWD_Transfer 累计周期数 / 调用次数（仅 SWD_SPI_STATS=1 时有效）
 * 注意：这里**不能**放 .noinit。应用链接脚本把 .noinit 起始定在 0x20010000，
 * 而 bootloader 正是按这个固定地址读 g_udap_boot_magic；多占 24 字节就会把它
 * 顶到 0x20010018，bootloader 读不到 magic，「进 bootloader」直接失效（踩过）。
 * 调试器在运行时用 pyocd 读 .bss 里的这块照样能看。 */
__attribute__((used))
volatile uint32_t g_swd_spi_stats[24];

/* 想自己核算「一笔传输在固件里花多少周期」就把 SWD_SPI_STATS 改成 1，
 * 然后用另一台探针读 0x20010004 起的 6 个字。默认关掉，省掉每周期的读/写。 */
#ifndef SWD_SPI_STATS
#define SWD_SPI_STATS            0
#endif

#define DWT_CYCCNT_ADDR         0xE0001004UL
#define DWT_CTRL_ADDR           0xE0001000UL
#define DEMCR_ADDR              0xE000EDFCUL
#if SWD_SPI_STATS
#define SWD_STATS_T0()          (*(volatile uint32_t *)DWT_CYCCNT_ADDR)
#define SWD_STATS_DONE()        do { g_swd_spi_stats[4] += (SWD_STATS_T0() - t0); g_swd_spi_stats[5]++; } while (0)
#else
#define SWD_STATS_T0()          0U
#define SWD_STATS_DONE()        do { } while (0)
#endif

static spi_mclk_freq_div_type spi_div_enum(uint32_t div)
{
    switch (div) {
    case 2U:    return SPI_MCLK_DIV_2;
    case 4U:    return SPI_MCLK_DIV_4;
    case 8U:    return SPI_MCLK_DIV_8;
    case 16U:   return SPI_MCLK_DIV_16;
    case 32U:   return SPI_MCLK_DIV_32;
    case 64U:   return SPI_MCLK_DIV_64;
    case 128U:  return SPI_MCLK_DIV_128;
    case 256U:  return SPI_MCLK_DIV_256;
    default:    return SPI_MCLK_DIV_512;
    }
}

/**
 * @brief 按上位机请求的 SWJ 时钟挑一个"不高于它"的 SPI 分频
 *
 * DAP.c 的 SWJ_Clock() 只把结果留在 DAP_Data.clock_delay 里，没有保留原始频率，
 * 所以这里按 MAX_SWJ_CLOCK() 的反函数反推：f = (CPU_CLOCK/2)/(IO_PORT_WRITE_CYCLES+delay)。
 * 宁可慢一点也不超过上位机要求，免得线长一点就 ACK 乱掉。
 */
static void spi_apply_clock(uint32_t clock_delay)
{
    uint32_t target_hz;
    uint32_t apb2_hz = system_core_clock;    /* APB2 与 SCLK 同频（/1） */
    uint32_t div     = 2U;

    /* 上位机要 ≥ MAX_SWJ_CLOCK(DELAY_FAST_CYCLES) 时 DAP.c 会置 fast_clock=1，
     * 此时 clock_delay 被固定成 1、且不再乘 DELAY_SLOW_CYCLES，含义是"跑满"，
     * 上限就是 GPIO 快路径能跑到的那个频率。慢路径下 clock_delay 的单位是
     * DELAY_SLOW_CYCLES 个周期，反推时必须乘回去，否则会超频——上位机改用低速
     * （比如长线、1 MHz）时我们却仍然跑 27 MHz，等于把它的降速请求当没看见。 */
    if (DAP_Data.fast_clock) {
        target_hz = (CPU_CLOCK / 2U) / (IO_PORT_WRITE_CYCLES + DELAY_FAST_CYCLES);
    } else {
        target_hz = (CPU_CLOCK / 2U) / (IO_PORT_WRITE_CYCLES + clock_delay * DELAY_SLOW_CYCLES);
    }

    if (target_hz == 0U) {
        target_hz = 1000000U;
    }
    while (div < 1024U && (apb2_hz / div) > target_hz) {
        div <<= 1;
    }

    if (div != s_spi_div) {
        spi_init_type cfg;

        spi_enable(SPI1, FALSE);
        spi_default_para_init(&cfg);
        cfg.transmission_mode   = SPI_TRANSMIT_FULL_DUPLEX;
        cfg.master_slave_mode   = SPI_MODE_MASTER;
        cfg.mclk_freq_division  = spi_div_enum(div);
        cfg.first_bit_transmission = SPI_FIRST_BIT_LSB;
        cfg.frame_bit_num       = SPI_FRAME_8BIT;
        cfg.clock_polarity      = SPI_CLOCK_POLARITY_HIGH;
        cfg.clock_phase         = SPI_CLOCK_PHASE_2EDGE;
        cfg.cs_mode_selection   = SPI_CS_SOFTWARE_MODE;
        spi_init(SPI1, &cfg);
        /* 软件 CS 模式下如果内部 NSS 是低电平，主机一发送就会触发模式错误
         * （SPI_MMERR）并被强制切成从机，所以这里必须先把它抬高。 */
        spi_software_cs_internal_level_set(SPI1, SPI_SWCS_INTERNAL_LEVEL_HIGHT);
        spi_enable(SPI1, TRUE);

        s_spi_div = div;
    }

    /* 记录给调试器核对：请求档位 / 分频 / 反推出来的实际 SWCLK(kHz) */
    g_swd_spi_stats[0] = (uint32_t)DAP_Data.fast_clock;
    g_swd_spi_stats[1] = div;
    g_swd_spi_stats[2] = clock_delay;
    g_swd_spi_stats[3] = (uint32_t)(apb2_hz / div / 1000U);
}

/**
 * @brief SPI 通路的一次性初始化（DAP_Setup 时调用）
 */
static void spi_phy_setup(void)
{
    gpio_init_type gpio_cfg;

    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);

    /* PA6 平时必须保持普通输入：ACK 是 GPIO 位翻转读的，走的就是 PA6 的 idt。
     * 只有数据相位才把它暂时交给 SPI1_MISO（见 data_phase_read）。 */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins          = GPIO_PINS_6;
    gpio_cfg.gpio_mode          = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull          = GPIO_PULL_UP;
    gpio_init(GPIOA, &gpio_cfg);
    gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE6, SPI1_AF_NUM);

    /* PA5(SCK)/PA7(MOSI) 的复用号只需要配一次：之后 pin_set_mode() 把 iomc
     * 切到"复用"，引脚就会接到已经选好的那个外设上。
     * 漏了这一步的后果很隐蔽：iomc=MUX 但复用号是 AF0，数据相位根本没有时钟，
     * 表现为目标不回 ACK（Unexpected ACK '0'）。 */
    gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE5, SPI1_AF_NUM);
    gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE7, SPI1_AF_NUM);

    /* PA5/PA7 先保持 GPIO，由传输过程按相位切换 */
    gpio_cfg.gpio_pins = GPIO_PINS_5 | GPIO_PINS_7;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init(GPIOA, &gpio_cfg);

#if SWD_SPI_STATS
    /* 打开 DWT 周期计数器，用于统计单笔传输的固件内耗时 */
    *(volatile uint32_t *)DEMCR_ADDR |= (1UL << 24);
    *(volatile uint32_t *)DWT_CYCCNT_ADDR = 0U;
    *(volatile uint32_t *)DWT_CTRL_ADDR |= 1UL;
#endif

    s_cfgr_base = BOARD_SWD_PORT->cfgr & ~((3UL << (SCK_PIN_NUM * 2U)) |
                                           (3UL << (MISO_PIN_NUM * 2U)) |
                                           (3UL << (MOSI_PIN_NUM * 2U)));
    s_spi_div = 0U;
    spi_apply_clock(DAP_Data.clock_delay);
}

/**
 * @brief 收发一整串字节（全双工，SPI 只能是 8 bit 的整数倍）
 *
 * tx 与 rx 允许是同一个缓冲区。只等 TDBE/RDBF，不等 BF，
 * 因为 BF 要等整帧结束，反而多花时间。
 *
 * 这里直接读 STS/写 DT，没走 spi_i2s_flag_get()/spi_i2s_data_transmit()：
 * 一次 SWD 数据相位只有 4 个字节，库函数调用的开销和移位本身是一个量级，
 * 换成寄存器操作后 27 MHz 下的写吞吐从 838 KiB/s 涨到 1003 KiB/s。
 */
#define SPI_STS                 (SPI1->sts)
#define SPI_TDBE                (SPI_STS & SPI_I2S_TDBE_FLAG)
#define SPI_RDBF                (SPI_STS & SPI_I2S_RDBF_FLAG)
#define SPI_BUSY                (SPI_STS & SPI_I2S_BF_FLAG)
#define SPI_TX(byte)            (SPI1->dt = (uint16_t)(byte))
#define SPI_RX()                ((uint8_t)SPI1->dt)

static int spi_burst(uint8_t *buf, uint32_t count)
{
    uint32_t i;
    uint32_t guard;

    for (i = 0U; i < count; i++) {
        guard = SPI_BURST_TIMEOUT;
        while (SPI_TDBE == RESET) {
            if (--guard == 0U) {
                return -1;
            }
        }
        SPI_TX(buf[i]);
        guard = SPI_BURST_TIMEOUT;
        while (SPI_RDBF == RESET) {
            if (--guard == 0U) {
                return -1;
            }
        }
        buf[i] = SPI_RX();
    }

    return 0;
}

/**
 * @brief 只发送，不等接收（写数据相位专用）
 *
 * 写相位根本不需要收到的字节，原来的 spi_burst() 每发一个字节都多等一次 RDBF，
 * 4 个字节就白花 4 次库函数调用（~40 周期/次）。这里只在最后等一次 BF，
 * 顺手把残留的接收数据读掉。
 */
static int spi_burst_tx(const uint8_t *buf, uint32_t count)
{
    uint32_t i;
    uint32_t guard;

    for (i = 0U; i < count; i++) {
        guard = SPI_BURST_TIMEOUT;
        while (SPI_TDBE == RESET) {
            if (--guard == 0U) {
                return -1;
            }
        }
        SPI_TX(buf[i]);
    }

    guard = SPI_BURST_TIMEOUT;
    while (SPI_BUSY != RESET) {
        if (--guard == 0U) {
            return -1;
        }
    }

    if (SPI_RDBF != RESET) {
        (void)SPI_RX();                     /* 清掉多余的接收数据，避免 OVR */
    }

    return 0;
}

/**
 * @brief 请求字节也交给 SPI 发（8 bit 正好一个字节）
 *
 * 请求相位是 8 个 SWCLK，位数刚好是 8 的整数倍、又没有反向数据，
 * 所以能整字节丢给 SPI，省掉 8 次 GPIO 位翻转（每次含两次 PIN_DELAY）。
 * 发完之后 PA7 立刻退回输入，把总线让给目标回 ACK。
 */
/* 转向时钟 + 3 拍 ACK，纯寄存器版：每拍「SCR 清 SCK → 采 IDT → SCR 置 SCK」。
 * 与 SW_CLOCK_CYCLE()/SW_READ_BIT() 的逻辑等价，只是不经过任何宏和函数调用。 */
static void swd_spi_turnaround_ack(uint32_t *ack_out)
{
    volatile uint32_t *scr = &BOARD_SWD_PORT->scr;
    const uint32_t sck_clr = (uint32_t)(1UL << SCK_PIN_NUM) << 16;
    const uint32_t sck_set = (uint32_t)(1UL << SCK_PIN_NUM);
    uint32_t ack = 0U;
    uint32_t n = DAP_Data.swd_conf.turnaround;

    for (; n; n--) {
        *scr = sck_clr;
        *scr = sck_set;
    }

    n = 3U;
    do {
        *scr = sck_clr;
        __NOP();
        __NOP();                        /* 等目标把 ACK 位推上来再采 */
        ack |= ((BOARD_SWD_PORT->idt >> MISO_PIN_NUM) & 1U) << (3U - n);
        *scr = sck_set;
        n--;
    } while (n);

    *ack_out = ack;
}
static void request_phase_spi(uint32_t request)
{
    uint8_t byte;
    uint8_t parity;

    /* start(1) + APnDP + RnW + A2 + A3 + parity + stop(0) + park(1) */
    parity = (uint8_t)(__builtin_parity(request & 0x0FU) & 1U);
    byte   = (uint8_t)(0x01U | ((request & 0x0FU) << 1) | (parity << 5) | 0x80U);

    PIN_SWCLK_SET();                    /* 切 MUX 前锁存先写高 */
    PHY_REQ_WR();
    (void)spi_burst_tx(&byte, 1U);      /* 8 个 SWCLK */
    PIN_SWCLK_SET();
    PHY_ACK();                          /* SWDIO 立即松手，SCK 回 GPIO */
}

/**
 * @brief 数据相位写 32 bit + 奇偶位
 *
 * 33 个 bit 塞进 5 个字节：byte0..3 是数据（低位先出），byte4 bit0 是奇偶位，
 * byte4 bit1..7 填 1 —— 这几个多出来的时钟落在 SWD 的空闲区，目标不采样，
 * 但必须是高电平（不能是 0），否则会被当成下一个包的 start bit。
 */
static void data_phase_write(uint32_t value)
{
    uint8_t buf[4];
    uint32_t parity = value ^ (value >> 16);
    uint32_t n;

    parity ^= parity >> 8;
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;

#if SWD_SPI_TX_VIA_GPIO
    for (n = 32U; n; n--) {
        PIN_SWDIO_OUT(value);
        PIN_SWCLK_CLR();
        SWD_DELAY();
        PIN_SWCLK_SET();
        SWD_DELAY();
        value >>= 1;
    }
    PIN_SWDIO_OUT(parity);
    PIN_SWCLK_CLR();
    SWD_DELAY();
    PIN_SWCLK_SET();
    SWD_DELAY();
#else
    /* 32 个数据位交给 SPI（4 个 8 位帧），第 33 拍（奇偶位）仍用 GPIO：
     * SPI 一次只能发 8 的整数倍，若发 5 个字节就是 40 拍，多出来的 7 拍会被
     * 目标当成下一个请求的 start bit，总线立刻失步（表现为再也读不到 ACK）。
     * 也试过把数据相位改成 16 位帧（2 次写）：改 ctrl1.fbn 必须关掉 SPI，
     * 即使成对 spien 开关，波形仍让目标回 ACK=0，所以保留 8 位帧。
     * 这里不需要 FIFO 连写：TDBE 在字节移入移位器时立刻置位，真正的等待
     * 是移位时间（8 拍 = 8/27MHz ≈ 296 ns），换帧长也省不掉。 */
    buf[0] = (uint8_t)(value >> 0);
    buf[1] = (uint8_t)(value >> 8);
    buf[2] = (uint8_t)(value >> 16);
    buf[3] = (uint8_t)(value >> 24);

    PIN_SWDIO_OUT(1U);
    PIN_SWCLK_SET();
    PHY_REQ_WR();
    (void)spi_burst_tx(buf, 4U);        /* 32 个 SWCLK，只发不收 */
    PIN_SWDIO_OUT(1U);
    PIN_SWCLK_SET();
    PHY_IDLE();                         /* 回 GPIO：SCK/MOSI 都是输出 */

    PIN_SWDIO_OUT(parity & 1U);         /* 第 33 拍：奇偶位 */
    PIN_SWCLK_CLR();
    SWD_DELAY();
    PIN_SWCLK_SET();
    SWD_DELAY();
#endif
}

/**
 * @brief 数据相位读 32 bit + 奇偶位，返回 0 表示奇偶校验对
 *
 * 读相位 PA7 必须是输入（目标独占总线），时钟照样由 SPI 给。
 * 33 个有效 bit 之后多出来的 7 个时钟：目标这时已经松手，线上只有上拉，
 * 读到的是 1，正好是 SWD 的空闲电平。
 */
static uint8_t data_phase_read(uint32_t *value)
{
    uint8_t buf[5];
    uint32_t parity;
    uint32_t i;
    uint32_t bit;
    int rc;

    for (i = 0U; i < 5U; i++) {
        buf[i] = 0xFFU;                     /* 读相位 mosi 不参与，填 1 更安全 */
    }

#if SWD_SPI_RX_VIA_GPIO
    {
        uint32_t val = 0U;

        parity = 0U;
        for (i = 32U; i; i--) {
            PIN_SWCLK_CLR();
            SWD_DELAY();
            bit = PIN_SWDIO_IN();
            PIN_SWCLK_SET();
            SWD_DELAY();
            parity += bit;
            val >>= 1;
            val |= bit << 31;
        }
        PIN_SWCLK_CLR();
        SWD_DELAY();
        bit = PIN_SWDIO_IN();
        PIN_SWCLK_SET();
        SWD_DELAY();

        *value = val;
        return (uint8_t)((parity ^ bit) & 1U);
    }
#else
    /* 同样只让 SPI 搬 32 个数据位（4 字节），奇偶位用 GPIO 采：
     * 多发 8 拍会和写相位一样把目标的状态机带偏。 */
    PIN_SWCLK_SET();
    PHY_RD();                           /* SCK=MUX, SWDIO 输入, MISO=MUX */
    rc = spi_burst(buf, 4U);            /* 32 个 SWCLK */
    PIN_SWCLK_SET();
    PHY_ACK();                          /* 回 GPIO 采样奇偶位 */

    if (rc != 0) {
        return 1U;                      /* 当作奇偶/传输错误，让上层重试 */
    }

    /* 第 33 拍：奇偶位仍由目标驱动，用 GPIO 采 */
    PIN_SWCLK_CLR();
    SWD_DELAY();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();
    SWD_DELAY();

    /* 这里不切回输出：读完后还有 1 个转向时钟，输出由上层
     * （SWD_Transfer 的 PIN_SWDIO_OUT_ENABLE）在转向之后再打开。 */

    *value = ((uint32_t)buf[0]) | ((uint32_t)buf[1] << 8) |
             ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

    parity  = *value ^ (*value >> 16);
    parity ^= parity >> 8;
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;

    return (uint8_t)((parity ^ (uint32_t)bit) & 1U);
#endif
}


/* -------------------------------------------------------------------------
 * 与 SW_DP.c 相同的对外符号
 * ---------------------------------------------------------------------- */
/* 位翻转的延时跟随 DAP 的快/慢路径：上位机要求 ≥6 MHz 时 Set_Clock_Delay()
 * 会把 fast_clock 置 1、clock_delay 固定成 1，若这里还用 PIN_DELAY_SLOW(1)
 * 就是 8 次空转（~50 周期）。ACK 3 拍、奇偶位、转向这几处都要各花两个半周期，
 * 27 MHz 下它们加起来比数据相位还贵，所以快路径改插 NOP。 */
#define PIN_DELAY()             \
    do {                        \
        if (DAP_Data.fast_clock) {  \
            PIN_DELAY_FAST();   \
        } else {                \
            PIN_DELAY_SLOW(DAP_Data.clock_delay); \
        }                       \
    } while (0)

#define SW_CLOCK_CYCLE()        \
    PIN_SWCLK_CLR();            \
    PIN_DELAY();                \
    PIN_SWCLK_SET();            \
    PIN_DELAY()

#define SW_WRITE_BIT(bit)       \
    PIN_SWDIO_OUT(bit);         \
    PIN_SWCLK_CLR();            \
    PIN_DELAY();                \
    PIN_SWCLK_SET();            \
    PIN_DELAY()

#define SW_READ_BIT(bit)        \
    PIN_SWCLK_CLR();            \
    PIN_DELAY();                \
    bit = PIN_SWDIO_IN();       \
    PIN_SWCLK_SET();            \
    PIN_DELAY()

void SWJ_Sequence(uint32_t count, const uint8_t *data)
{
    uint32_t val = 0U;
    uint32_t n = 0U;

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

void SWD_Sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi)
{
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

/* 调试留档：SWD 起不来时最省事的办法是用另一个探针（外部 DAPLink）在本板运行时
 * 用 pyocd 读 .bss 里的 g_swd_spi_dbg（地址见 nm 输出），看最近几笔传输的 ACK。
 * 默认关闭（不影响时序、只占 .bss 里 80 字节）；排查时把下面这行改成 1 重新编译。`r`n * 注意别放回 .noinit：它会把 g_udap_boot_magic 从 0x20010000 顶开，进 bootloader 就失效了。
 * 有它打过两个真实的坑：40 拍失步、PA6 复用号漏配。 */
#define SWD_SPI_DEBUG_RECORD 0

#if SWD_SPI_DEBUG_RECORD
typedef struct {
    uint32_t count;
    uint32_t ack_hist[8];
    uint32_t req_hist[8];
    uint32_t parity_err;
    uint32_t proto_err;
} swd_spi_dbg_type;

__attribute__((used)) volatile swd_spi_dbg_type g_swd_spi_dbg;

static void dbg_record(uint32_t request, uint32_t ack)
{
    uint32_t i = g_swd_spi_dbg.count & 7U;

    g_swd_spi_dbg.ack_hist[i] = ack;
    g_swd_spi_dbg.req_hist[i] = request;
    g_swd_spi_dbg.count++;
}
#else
static inline void dbg_record(uint32_t request, uint32_t ack)
{
    (void)request;
    (void)ack;
}
#endif

/**
 * @brief 一次 SWD 传输（请求 + 转向 + ACK + 数据相位）
 *
 * 流程与 SW_DP.c 完全一致，只是 32 bit 数据相位换成 SPI 硬件搬运。
 * 快/慢路径不再区分：慢速时 SPI 的分频会更保守，但仍是硬件时钟。
 */
uint8_t SWD_Transfer(uint32_t request, uint32_t *data)
{
    uint32_t ack;
    uint32_t bit;
    uint32_t val;
    uint32_t parity;
    uint32_t n;
    uint32_t t0 = SWD_STATS_T0();

    /* 上位机每次 SWJ_Clock() 都会改 DAP_Data.clock_delay，这里懒更新分频，
     * 免得每笔传输都重配一遍 SPI */
    if (s_spi_div == 0U || s_last_delay != DAP_Data.clock_delay) {
        spi_apply_clock(DAP_Data.clock_delay);
        s_last_delay = DAP_Data.clock_delay;
    }

    uint32_t tp;
    /* 请求字节：start, APnDP, RnW, A2, A3, parity, stop, park（低位先发） */
#if SWD_SPI_REQ_VIA_SPI
    tp = SWD_STATS_T0();
    request_phase_spi(request);
    g_swd_spi_stats[6] += SWD_STATS_T0() - tp;
    (void)parity;                       /* 走 SPI 时奇偶位在 request_phase_spi 里算 */
#else
    parity = 0U;
    SW_WRITE_BIT(1U);
    bit = (request >> 0) & 1U;  SW_WRITE_BIT(bit);  parity += bit;
    bit = (request >> 1) & 1U;  SW_WRITE_BIT(bit);  parity += bit;
    bit = (request >> 2) & 1U;  SW_WRITE_BIT(bit);  parity += bit;
    bit = (request >> 3) & 1U;  SW_WRITE_BIT(bit);  parity += bit;
    SW_WRITE_BIT(parity);
    SW_WRITE_BIT(0U);
    SW_WRITE_BIT(1U);
#endif

    /* 转向：主机松手 */
    PIN_SWDIO_OUT_DISABLE();
    for (n = DAP_Data.swd_conf.turnaround; n; n--) {
        SW_CLOCK_CYCLE();
    }

    /* ACK[2:0] */
    tp = SWD_STATS_T0();
    SW_READ_BIT(bit);   ack  = bit << 0;
    SW_READ_BIT(bit);   ack |= bit << 1;
    SW_READ_BIT(bit);   ack |= bit << 2;

    g_swd_spi_stats[7] += SWD_STATS_T0() - tp;
    dbg_record(request, ack);

    if (ack == DAP_TRANSFER_OK) {
        if (request & DAP_TRANSFER_RnW) {
            /* 读：数据相位 + 奇偶校验 */
            tp = SWD_STATS_T0();
            if (data_phase_read(&val) != 0U) {
                ack = DAP_TRANSFER_ERROR;
            }
            g_swd_spi_stats[10] += SWD_STATS_T0() - tp;
            g_swd_spi_stats[13]++;
            tp = SWD_STATS_T0();
            if (data) {
                *data = val;
            }
            for (n = DAP_Data.swd_conf.turnaround; n; n--) {
                SW_CLOCK_CYCLE();
            }
            PIN_SWDIO_OUT_ENABLE();
        } else {
            /* 写：转向之后主机接管，数据相位由 SPI 驱动 */
            for (n = DAP_Data.swd_conf.turnaround; n; n--) {
                SW_CLOCK_CYCLE();
            }
            PIN_SWDIO_OUT_ENABLE();
            tp = SWD_STATS_T0();
            data_phase_write(*data);
            g_swd_spi_stats[11] += SWD_STATS_T0() - tp;
            g_swd_spi_stats[12]++;
            tp = SWD_STATS_T0();
        }

        if (request & DAP_TRANSFER_TIMESTAMP) {
            DAP_Data.timestamp = TIMESTAMP_GET();
        }

        n = 0U;   /* TEMP experiment: 忽略 host 请求的空闲拍 */
        if (n) {
            PIN_SWDIO_OUT(0U);
            for (; n; n--) {
                SW_CLOCK_CYCLE();
            }
        }
        PIN_SWDIO_OUT(1U);
        g_swd_spi_stats[(request & DAP_TRANSFER_RnW) ? 14 : 15] += SWD_STATS_T0() - tp;
        g_swd_spi_stats[(request & DAP_TRANSFER_RnW) ? 16 : 17] += SWD_STATS_T0() - t0;
        if (ack != DAP_TRANSFER_OK) {
            g_swd_spi_stats[18]++;
        }
        g_swd_spi_stats[9] += SWD_STATS_T0() - t0;
        SWD_STATS_DONE();
        return (uint8_t)ack;
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {
            for (n = 32U + 1U; n; n--) {
                SW_CLOCK_CYCLE();
            }
        }
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            SW_CLOCK_CYCLE();
        }
        PIN_SWDIO_OUT_ENABLE();
        if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {
            PIN_SWDIO_OUT(0U);
            for (n = 32U + 1U; n; n--) {
                SW_CLOCK_CYCLE();
            }
        }
        PIN_SWDIO_OUT(1U);
        SWD_STATS_DONE();
        return (uint8_t)ack;
    }

    /* 协议错：把数据相位空转掉，让状态机回到空闲 */
    for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {
        SW_CLOCK_CYCLE();
    }
    PIN_SWDIO_OUT_ENABLE();
    PIN_SWDIO_OUT(1U);

    SWD_STATS_DONE();
        return (uint8_t)ack;
}

/* PORT_SWD_SETUP()/PORT_JTAG_SETUP() 里由 DAP_config.h 调过来 */
void swd_spi_port_setup(void)
{
    spi_phy_setup();
}


#endif  /* (DAP_SWD != 0) && SWD_PHY_SPI */
