/*
 * 硬件 SPI 位相位（CH32X035 SPI1，单线半双工 BIDIMODE）
 *
 * 接线：SWCLK = PA5 = SPI1_SCK，SWDIO = PA7 = SPI1_MOSI。
 * 单线半双工把 MOSI 一根线当双向用，正好对上 SWD 的 SWDIO：
 *   BIDIMODE = 1，BIDIOE = 1 发送 / 0 接收（参考手册 16.2.4 节，
 *   SDK 的 sdk/EXAM/SPI/1Lines-half-duplex 例程也是这么配的）。
 *
 * 三个相位（其余一律 GPIO 位时序）：
 *   请求头 8 拍      SPI 发送 1 字节  ——**仅写事务**，SPI 模式 0（空闲低）
 *   写数据 32 拍     SPI 发送 4 字节  ——SPI 模式 0
 *   读数据 32 拍     SPI 接收 4 字节  ——默认关闭（SWD_SPI_RD），原因见文件尾
 *
 * 写方向的入口相位是低（ACK 之后有一个 turnaround 时钟），所以用模式 0：
 * 第一位由 GPIO 先顶住、SCK 拉低，再把引脚交给外设。
 *
 * ---------------------------------------------------------------------------
 * 交接方式：**只切引脚模式，不动 SPI 本身**（SPE 常开）
 * ---------------------------------------------------------------------------
 * 引脚处于 GPIO 模式时，外设输出与引脚是脱开的（CFGLR 的 MODE 位选输出源），
 * 所以「谁在驱动 SCK/MOSI」只取决于 CFGLR。于是每个相位只做一次 CFGLR 切换：
 *   GPIO 模式 → 预置电平（MOSI = 第一位、SCK = 低）→ AF 模式 → 写 DATAR 让它移位
 * 每相位少两次 CTLR1 写，也没有 SPE 0→1 的状态机跳变。
 *
 * 切到 AF 的瞬间 MOSI 会改由外设驱动、电平可能抖一下，但那一刻 SCK 是空闲的，
 * 而 SWD（和 SPI）都只在时钟边沿采样，所以这一下抖动无害。
 *
 * ---------------------------------------------------------------------------
 * 踩过的坑（都实测过，别再走一遍）
 * ---------------------------------------------------------------------------
 * 1. **SPI1 的时钟必须先开**（RCC_APB2PCENR 的 SPI1EN）。时钟没开时寄存器照样
 *    能读能写、回读值完全正常，但状态机不跑：写 DATAR 后 TXE 一直是 0、BSY
 *    一直是 0，看起来像「BIDIMODE 不支持」。用调试口直接试寄存器时最容易踩到。
 * 2. 引脚交接前必须让 SPI 的空闲电平与 GPIO 当前电平一致（见上面的模式选择），
 *    否则交接本身就会在 SCK 上多出一个边沿。
 * 3. SPE = 0 时写 DATAR 不会进发送缓冲（实测随后置 SPE 一个字节都发不出去），
 *    所以别想着"预装 + 后启动"。
 * 4. 写相位收尾要「先补一帧时间 → 再等 BSY 归零」。BSY 在数据刚写进缓冲时还没
 *    置位，直接等 BSY 会提前成功、把最后一帧掐断（残帧留到下一次）。
 * 5. 半双工下发送相位也会往接收缓冲里塞数据；RXNE 残留会让 TXE 卡在 0，
 *    而且 OVR 是 **RW0（写 0 清除）**，不是读清。
 */
#include "swd_spi.h"

#include "swd_parity.h"
#include "swd_pin_map.h"

#if SWD_PIN_VARIANT != SWD_PIN_VARIANT_PA5_PA7
#error "SWD_SPI_DATA_PHASE 只支持 PA5/PA7 方案（PA5 = SPI1_SCK，PA7 = SPI1_MOSI）"
#endif

/*
 * CTLR1 的公共位：
 *   MSTR     主机
 *   LSBFIRST SWD 是低位先出（SDK 注明该位只在主模式有效）
 *   BR       PCLK2 / 2^(BR+1)，见 swd_pin_map.h 的 SWD_SPI_BR
 *   SSM|SSI  软件片选：NSS 没接，硬件片选会被噪声拉低触发 MODF
 * 单线半双工只支持 8 位帧（DFF 必须为 0）。
 */
#define SWD_SPI_CTLR1_BASE                                                                \
    ((uint16_t)(SPI_CTLR1_MSTR | SPI_CTLR1_LSBFIRST | SPI_CTLR1_SSM | SPI_CTLR1_SSI | \
                ((uint16_t)(SWD_SPI_BR) << 3)))
/* 写：模式 0（CPOL = 0 / CPHA = 0，空闲低、上升沿采样）+ 1 线发送（BIDIOE = 1）。
 * 这个组合**常驻**：引脚在 GPIO 模式时外设输出与引脚脱开，所以不需要来回开关 SPE。
 * SWD_SPI_16BIT 时用 16 位帧（DFF = 1）：32 位数据只要两次 DATAR 写，
 * 省掉两次 TXE 轮询，而且奇偶位正好能在两次写之间算（见 swd_spi_send32_end）。 */
#if SWD_SPI_16BIT
#define SWD_SPI_CTLR1_TX                                                                  \
    ((uint16_t)(SWD_SPI_CTLR1_BASE | SPI_CTLR1_BIDIMODE | SPI_CTLR1_BIDIOE | SPI_CTLR1_DFF | \
                SPI_CTLR1_SPE))
#else
#define SWD_SPI_CTLR1_TX \
    ((uint16_t)(SWD_SPI_CTLR1_BASE | SPI_CTLR1_BIDIMODE | SPI_CTLR1_BIDIOE | SPI_CTLR1_SPE))
#endif
/* 读：模式 1（CPOL = 0 / CPHA = 1，空闲低、下降沿移位 / 上升沿采样）+ 1 线接收。
 * 实测这颗芯片的主设备 1 线接收只在 CPOL = 0 时能收到数据：CPOL = 1（模式 3）
 * 时 DATAR 恒为 0。而模式 1 的采样点正好是 SWD 规定的上升沿，比位时序
 * "贴着下降沿采"余量大。（入口那半个时钟见 swd_spi_recv32 的说明。） */
#define SWD_SPI_CTLR1_RX \
    ((uint16_t)(SWD_SPI_CTLR1_BASE | SPI_CTLR1_BIDIMODE | SPI_CTLR1_CPHA | SPI_CTLR1_SPE))

#ifndef SWD_SPI_TX_DRAIN
/* 每个发送相位收尾是否清接收缓冲（读 DATAR + STATR 写 0 清 OVR）：
 *   = 1：多 3 次外设总线访问，实测写吞吐 287 → 267 KiB/s
 *   = 0（默认）：RXNE/OVR 会一直留着，但本配置下（SPE 常开、只有写数据相位用 SPI）
 *        它既不阻塞 TXE 也不影响数据 —— 8/8 链路检查 + 6 次 64 KiB 长块读 0 错误
 * 只有当以后把请求头/读数据相位也接到 SPI（一次事务里出现两个 SPI 相位）时，
 * 才需要打开它兜底。 */
#define SWD_SPI_TX_DRAIN 0
#endif

/* 有界自旋。
 * 这里是**不能**用死循环的：这段代码跑在 DAP 命令处理里，一旦 SPI 没响应
 * （时钟没开、模式不支持、标志位和预期不一致……）整颗芯片就再也不回 USB，
 * 主机侧只能看到「USB 超时」，连哪一步挂的都看不出来。有界自旋的代价是
 * 发出/收到垃圾数据 —— 但链路会以 parity / IDCODE 错误的形式表现出来。 */
#define SWD_SPI_SPIN 2000U

#ifndef SWD_SPI_TX_NOP_TIMING
/* 帧间等待（写第 2~4 个字节之前要等发送缓冲空出来）用哪种方式：
 *   = 0（默认）：读 STATR 等 TXE —— 绝对安全，慢一点点（每次 ~5 个 clk）
 *   = 1：用固定 NOP 顶掉这段等待（长度按帧长用 SWD_SPI_FRAME_PAD 换算）
 * 1 的好处是省掉 3 次外设总线读；风险是延时靠标定：给少了会覆盖未移出的字节
 * （丢数据）。中断只会让我们更晚写（安全方向），但换芯片/换 BR 要重新标定。 */
#define SWD_SPI_TX_NOP_TIMING 0
#endif

#if SWD_SPI_DMA
static volatile uint32_t s_dma_word; /* DMA 源缓冲（地址固定，省得每个事务改 CMAR） */
#endif

static uint32_t swd_spi_wait_set(uint32_t mask) {
    uint32_t n = SWD_SPI_SPIN;

    while (((SPI1->STATR & mask) == 0U) && (--n != 0U)) {
    }
    return (n != 0U) ? 1U : 0U;
}

static uint32_t swd_spi_wait_clear(uint32_t mask) {
    uint32_t n = SWD_SPI_SPIN;

    while (((SPI1->STATR & mask) != 0U) && (--n != 0U)) {
    }
    return (n != 0U) ? 1U : 0U;
}

/* 等最后一帧移完（见文件头坑 4）。
 *
 * 结构：先补一小段 NOP 让 BSY 起来，再用 `wait_clear(BSY)` 兜住剩余移位时间。
 * BSY 在「字节从发送缓冲进移位寄存器」时置位，延迟约 1 个波特率节拍（2~8 个
 * CPU clk，随 BR 变），所以 4<<BR 次循环（每次约 3 clk）有 6 倍以上余量。
 *
 * 实测（各测 2~3 次，其它条件相同）：
 *   8<<BR（16 次循环，约 0.67 µs）—— 写 288.6 / 288.1 / 287.7 KiB/s
 *   4<<BR（8 次循环，约 0.33 µs）  —— 写 288.8 / 289.2 / 288.9   ← 采用
 *   2<<BR（4 次循环）              —— 写 290.7 / 290.7（更快，但余量只剩 6 倍，不取）
 *   "轮询 BSY 起来再等它落下"      —— 写 265~279（读 STATR 在传输中有额外开销，更慢）
 *
 * ⚠️ 这段延时不能省：BSY 在数据刚写进缓冲时是 0，直接 `wait_clear` 会「提前成功」，
 * 紧接着把引脚收回 GPIO 就把最后一帧掐断（残帧留到下一次事务）。
 * 这也是「SPI 发送后的时钟空隙」的主要来源，本函数就是它的长度。
 *
 * 长度要按帧长缩放：一帧 = 8 位（DFF=0）或 16 位（DFF=1），即
 * 8 << BR / 16 << BR 个 PCLK2 周期；循环一次约 3 个 CPU clk，所以取 4<<BR（8 位帧）
 * 或 8<<BR（16 位帧）。用 8 位帧的延时配 16 位帧会让剩下的半帧变成 STATR 轮询，
 * 实测写吞吐 298 → 267 KiB/s。 */
#if SWD_SPI_16BIT
#define SWD_SPI_FRAME_PAD (8U << SWD_SPI_BR)
#else
#define SWD_SPI_FRAME_PAD (4U << SWD_SPI_BR)
#endif

#if SWD_SPI_TX_NOP_TIMING
/* 一帧的纯 NOP 延时（替代 TXE 轮询）。见 SWD_SPI_TX_NOP_TIMING 的说明。 */
static void swd_spi_frame_delay(void) {
    uint32_t n = SWD_SPI_FRAME_PAD;

    while (n-- != 0U) {
        __NOP();
    }
}
#endif

static void swd_spi_wait_last_frame(void) {
    uint32_t n = SWD_SPI_FRAME_PAD;

    while (n-- != 0U) {
        __NOP();
    }
    (void)swd_spi_wait_clear(SPI_STATR_BSY);
}

/* 清接收缓冲 + OVR（见文件头坑 5）。 */
static void swd_spi_drain(void) {
    volatile uint16_t sink;

    sink = SPI1->DATAR;
    sink = SPI1->DATAR;
    SPI1->STATR = 0U; /* OVR 是 RW0：写 0 清除 */
    (void)sink;
}

/* 切换 SPI1 工作模式。CPOL/CPHA/BIDIOE 只能在 SPE = 0 时改（参考手册 CTLR1 说明），
 * 所以要「关 → 写 → 开」。
 * 只有读数据相位（SWD_SPI_RD，默认关闭）用得到；写方向全程保持 TX 模式、不切 CTLR1。 */
static void swd_spi_set_mode(uint16_t ctlr1) {
    if (SPI1->CTLR1 == ctlr1) {
        return; /* 已经是这个模式，别白折腾一次 SPE 跳变 */
    }
    SPI1->CTLR1 = 0U;
    SPI1->CTLR2 = 0;
    SPI1->CTLR1 = ctlr1;
}

void swd_spi_init(void) {
    GPIO_InitTypeDef miso = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE); /* ← 没有这一步 SPI 就是个哑巴寄存器 */

    /* MISO(PA6) 用不上，但也要保持高阻，别去顶别的电路 */
    miso.GPIO_Pin = GPIO_Pin_6;
    miso.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    miso.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &miso);

    /* 一次配好、**一直保持使能**：引脚在 GPIO 模式时外设的输出与引脚是脱开的，
     * 所以 SCK/MOSI 由 GPIO 驱动还是由 SPI 驱动，只取决于 CFGLR 的引脚模式。
     * 见 swd_spi_send_begin 的说明。 */
    SPI1->CTLR1 = SWD_SPI_CTLR1_TX; /* 含 SPE：常驻使能 */
    SPI1->CTLR2 = 0;
    swd_spi_drain();

#if SWD_SPI_DMA
    {
        DMA_InitTypeDef dma = {0};

        RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
        dma.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
        dma.DMA_MemoryBaseAddr = (uint32_t)&s_dma_word;
        dma.DMA_DIR = DMA_DIR_PeripheralDST;
        dma.DMA_BufferSize = 4;
        dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
#if SWD_SPI_16BIT
        dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
        dma.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
#else
        dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
        dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
#endif
        dma.DMA_Mode = DMA_Mode_Normal;
        dma.DMA_Priority = DMA_Priority_High;
        dma.DMA_M2M = DMA_M2M_Disable;
        DMA_Init(DMA1_Channel3, &dma);
        DMA_Cmd(DMA1_Channel3, DISABLE);
        DMA_ClearFlag(DMA1_FLAG_GL3);
        SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);
    }
#endif
}

/*
 * 发送 frames 个 8 位帧（拆成 begin / end 两步）。
 *
 * 交接**只切引脚模式，不动 SPI 本身**：SPE 常开，CFGLR 在「GPIO 推挽」和「AF 推挽」
 * 之间切。引脚是 GPIO 时外设输出与引脚脱开、数据被 GPIO 的 ODR 接管；切到 AF 时
 * 外设接管。这样每相位少两次 CTLR1 写，也避免 SPE 0→1 的状态机跳变。
 *
 * 顺序（每一步都为了 SWDIO/SWCLK 上的电平连续）：
 *   ① SWDIO 先用 GPIO 顶住第一位（value 的 bit0），SCK 由 GPIO 拉低
 *      —— 这是这一相位的第一个下降沿。此时 SPI 的空闲电平也是低，切 AF 不会出边沿
 *   ② 切到 AF：SCK 交给外设（仍是低）；MOSI 改由 SPI 驱动，电平可能瞬间变一下，
 *      但 **SCK 是空闲的**，目标只在时钟边沿采样，所以这一下抖动无害
 *   ③ frames 帧按 TXE 逐个填进发送缓冲；第一个下降沿由 SPI 产生，bit0 在
 *      第一个上升沿（半个周期后）被目标采到，与其余各位的余量一致
 *   （begin 返回时最后一帧通常还在移位 —— 调用方可以在这段时间里干别的活，
 *     见 swd_spi_send32_begin/end 的说明）
 *   ④ end：等 TXE + 最后一帧移完 → 把 ODR 摆成目标电平 → 切回 GPIO 模式
 */
static void swd_spi_send_begin(uint32_t value, uint32_t frames) {
    uint32_t i;

    /* 这两步都不能省：
     *   - SWDIO 先摆成第一位（切 AF 后 MOSI 由 SPI 驱动，它此刻的电平是不确定的）
     *   - SCK 必须先由 GPIO 拉低：进入本相位前位时序刚把它留在高电平
     *     （swd_fast_finish_ok 结尾的 PIN_SWCLK_SET()），而 SPI 的空闲电平是低，
     *     不先拉低的话切 AF 的瞬间会在 SCK 上多出一个下降沿 = 一个假时钟。 */
    board_swd_dio_out(value & 1U);
    board_swd_clk_clr();
    board_swd_clk_dio_to_af(); /* 只切引脚，不动 SPI */

    /* 第一帧**不用**等 TXE：进入本相位时发送缓冲一定是空的（每个相位收尾都等过
     * TXE，初始化也清过），少读一次 STATR 就少一段"发送前"的空隙。 */
    SPI1->DATAR = (uint16_t)(value & 0xFFU);
    value >>= 8;

    /* 后续帧必须等 TXE：否则会把还没移进移位寄存器的字节覆盖掉（丢数据）。 */
    for (i = 1; i < frames; i++) {
        (void)swd_spi_wait_set(SPI_STATR_TXE);
        SPI1->DATAR = (uint16_t)(value & 0xFFU);
        value >>= 8;
    }
}

static void swd_spi_send_end(uint32_t last_bit) {
    (void)swd_spi_wait_set(SPI_STATR_TXE);
    swd_spi_wait_last_frame();

    /* 只摆 SWDIO 的 ODR（切回 GPIO 后引脚电平要等于刚发出去的最后一位）。
     * SCK 的 ODR 在 begin 里已经写成 0 了，这期间没人动过，不用再写一次。
     * 摆好 ODR 再切 CFGLR，交接瞬间引脚上不会出现毛刺。 */
    board_swd_dio_out(last_bit);
    board_swd_clk_dio_to_gpio();

    /* 半双工发送时接收缓冲也在收（每帧 1 字节）→ RXNE/OVR 会残留。
     * 本配置下不用清（见文件头 SWD_SPI_TX_DRAIN 的说明与实测数据）。 */
#if SWD_SPI_TX_DRAIN
    swd_spi_drain();
#endif
}

static void swd_spi_send(uint32_t value, uint32_t frames) {
    swd_spi_send_begin(value, frames);
    swd_spi_send_end((value >> (8U * (frames - 1U))) >> 7);
}

/*
 * 请求头字节查表：索引 = request[3:0]（APnDP / RnW / A2 / A3）
 *
 *   bit0 = 1（start）
 *   bit1..4 = request[0..3]
 *   bit5 = request[3:0] 的异或（SWD 的校验位，和位时序实现里的算法一致）
 *   bit6 = 0（stop）
 *   bit7 = 1（park）
 *
 * 8 个比特完全由 4 位决定，所以整张表在编译期就定下来，热路径上只剩
 * 「取表 + 交给 SPI」，不用再做奇偶折叠。
 * 对照：读 DPIDR（request = 0x2）= 0xA5、写 DP CTRL/STAT（0x4）= 0xA9、
 *       读 DP CTRL/STAT（0x6）= 0x8D —— 都是 SWD 里最常见的几个头。
 */
static const uint8_t swd_request_byte[16] = {
    0x81, 0xA3, 0xA5, 0x87, 0xA9, 0x8B, 0x8D, 0xAF,
    0xB1, 0x93, 0x95, 0xB7, 0x99, 0xBB, 0xBD, 0x9F,
};

/* 请求头 8 拍（整字节一次发完，含 park 位）。
 * 回来时 SCK 已收回 GPIO 且为低、SWDIO 由 GPIO 驱动 1（park）。
 * 调用方紧接着 `PIN_SWDIO_INPUT()` 释放 SWDIO 做 turnaround。 */
void swd_spi_send_request(uint32_t request) {
    swd_spi_send(swd_request_byte[request & 0xFU], 1U);
}

uint32_t swd_spi_send32(uint32_t value) {
    uint32_t i = value;

    /* 这两步都不能省：
     *   - SWDIO 先摆成第一位（切 AF 后 MOSI 由 SPI 驱动，它此刻的电平是不确定的）
     *   - SCK 必须先由 GPIO 拉低：进入本相位前位时序刚把它留在高电平
     *     （swd_fast_finish_ok 结尾的 PIN_SWCLK_SET()），而 SPI 的空闲电平是低，
     *     不先拉低的话切 AF 的瞬间会在 SCK 上多出一个下降沿 = 一个假时钟。 */
    board_swd_dio_out(value & 1U);
    board_swd_clk_clr();
    board_swd_clk_dio_to_af(); /* 只切引脚，不动 SPI */

    /* 第一帧**不用**等 TXE：进入本相位时发送缓冲一定是空的（每个相位收尾都等过
     * TXE，初始化也清过），少读一次 STATR 就少一段"发送前"的空隙。 */
    SPI1->DATAR = (uint16_t)(value & 0xFFU);
    value >>= 8;
    i ^= i >> 16; /* 奇偶折叠：32 位异或成 16 位 */
    i ^= i >> 8; /* 奇偶折叠：16 位异或成 8 位 */
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    SPI1->DATAR = (uint16_t)(value & 0xFFU);
    value >>= 8;
    i ^= i >> 4; /* 奇偶折叠：8 位异或成 4 位 */
    i ^= i >> 2; /* 奇偶折叠：4 位异或成 2 位 */
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    SPI1->DATAR = (uint16_t)(value & 0xFFU);
    value >>= 8;
    i ^= i >> 1; /* 奇偶折叠：2 位异或成 1 位 = 奇偶位 */
    uint32_t parity = i & 1U;
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    SPI1->DATAR = (uint16_t)(value & 0xFFU);


    (void)swd_spi_wait_set(SPI_STATR_TXE);
    swd_spi_wait_last_frame();

    /* 只摆 SWDIO 的 ODR（切回 GPIO 后引脚电平要等于刚发出去的最后一位）。
     * SCK 的 ODR 在 begin 里已经写成 0 了，这期间没人动过，不用再写一次。
     * 摆好 ODR 再切 CFGLR，交接瞬间引脚上不会出现毛刺。 */
    board_swd_dio_out(value >> 31);
    board_swd_clk_dio_to_gpio();

    return parity;
}


/*
 * 32 位读数据相位：4 帧 = 32 个时钟（SPI 模式 3，上升沿采样）。
 *
 * 进入时 SCK 是高（ACK 第三拍结束）、SWDIO 是浮空输入（目标在驱动）；
 * 模式 3 的空闲电平也是高，所以引脚一交给外设不会产生边沿。
 *
 * 每帧「写 DATAR 造 8 个时钟 → 等 RXNE 读回」，不能先连写：接收缓冲只有 1 级，
 * 连写会溢出（OVR），读到的是旧数据。
 *
 * 只给 32 个时钟：第 33 拍是奇偶位，由调用方用 GPIO 采 —— 多打几个时钟「顺便」
 * 把奇偶位带回来是不行的，多给的时钟会把目标后面的事务带偏。
 * 退出时 SCK 回 GPIO 且保持高电平，SWDIO 仍是浮空输入。
 */
uint32_t swd_spi_recv32(void) {
    uint32_t i;
    uint32_t value = 0U;

    /* 入口：SCK 由 GPIO 拉低 —— 这就是目标往外送 bit0 的那个下降沿。
     * 之后每个 bit 由 SPI 产生「上升沿（采样）+ 下降沿（目标换数据）」，
     * 所以 32 位正好是 4 帧；SPI 模式 1 的第一个边沿是上升沿，采样点对得上。 */
    board_swd_clk_clr();
    swd_spi_set_mode(SWD_SPI_CTLR1_RX);
    board_swd_clk_to_af_dio_input();

    /* 这里**不看 TXE**：1 线接收方向（BIDIOE = 0）下 TXE 不能被当作「可以写
     * DATAR」的判据（实测进入接收相位时它常常是 0）。RXNE 是更强的条件：它
     * 置位说明上一帧已经移完、发送缓冲必然空出来了。 */
    for (i = 0; i < 4U; i++) {
        SPI1->DATAR = 0U; /* 半双工接收时写进去的数据被丢弃，只为产生 8 个时钟 */
        if (!swd_spi_wait_set(SPI_STATR_RXNE)) {
            break;
        }
        value |= (uint32_t)(SPI1->DATAR & 0xFFU) << (8U * i);
    }
    swd_spi_wait_last_frame();

    swd_spi_set_mode(SWD_SPI_CTLR1_TX); /* 恢复发送模式：写相位全程不再切 CTLR1 */
    board_swd_clk_clr(); /* 模式 1 的空闲电平是低 */
    board_swd_clk_to_gpio();
    return value;
}

/*
 * ---------------------------------------------------------------------------
 * 读数据相位（SWD_SPI_RD）为什么默认关掉 —— 实测记录，别再重复踩
 * ---------------------------------------------------------------------------
 * 1 线半双工**接收**本身是通的：SWDIO 仍配浮空输入（`board_swd_clk_to_af_dio_input`），
 * 主设备能在 PA7 上收到目标的 32 位数据 —— 但整段数据的对齐和奇偶位都不可靠：
 *
 *   SPI 时钟  CPOL/CPHA      第一次读到的 IDCODE
 *   6 MHz     0/1（模式 1）  0x2BA01477  ✅ 数据对，但第 33 拍（奇偶位）采不到
 *   6 MHz     0/0（模式 0）  0x574028EF  = (真值<<1)|1，晚 1 位
 *   12 MHz    0/1            0xFF5728EF  错位
 *   24 MHz    0/0/1          0xE8051DFF  = (真值<<6)|0x3F，晚 6 位
 *   任意      1/1（模式 3）  0x00000000  ← 主设备 1 线接收在 CPOL=1 下收不到数据
 *
 * 奇偶位的问题：位时序实现在下降沿之后采样，能采到正确的奇偶位；换成 SPI 之后
 * 无论"上升沿补一拍再采"还是"下降沿补一拍再采"，读到的都是 1（目标已释放总线），
 * 而 IDCODE 的正确奇偶位是 0 —— 说明目标把奇偶位放在我们采不到的那个边沿上。
 *
 * 结论：读方向仍是"目标驱动 SWDIO 的单线链路"，在 5 cm 跳线、没有地回路的情况下
 * 高时钟下建立时间不够。而 6 MHz 的 SPI 读（4 帧 = 5.3 µs）并不比位时序
 * （33 拍 = 5.5 µs）快，所以默认不值当。要再试的话：换短排线 + 加地回路，
 * 把 SWD_PIN_VARIANT 换成 SWDIO = PA6（MISO）—— 主设备 1 线接收在原厂例程里
 * 就是接 MISO 的，但那样写方向又得退回位时序。
 */
