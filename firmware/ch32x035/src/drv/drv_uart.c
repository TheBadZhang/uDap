#include "drv/drv_uart.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ch32x035.h>
#include <chry_ringbuffer.h>

// CherryUSB 的 class/cdc 头文件用了 bool / uint8_t 却没自己包含 <stdbool.h> /
// <stdint.h>（见 src/dap/dap_main.h 里的同类说明），所以上面几个标准头必须排在前面。
#include <usbd_cdc_acm.h>

#include "board_pin_map.h"
#include "drv/drv_mode_store.h"

// ===========================================================================
// 本文件 = 全部串口相关驱动（原来分散在 6 对文件里）：
//
//   RX 的 DMA 环形引擎（两路共用，本文件 internal）
//   PA2 = DTR / PA3 = RTS（低有效输出）
//   drv_uart.c       COM1：USART4（PB0 / PB1）
//   COM2：USART2（PA2 / PA3，仅双串口模式）
//   串口模式切换 + PB12 指示灯 + 选项字持久化
//   CDC ACM 行编码 / DTR / RTS 回调
//
// 合并的理由：这几块共用同一组引脚（PA2/PA3）、同一套 DMA 引擎、同一份
// 「谁在占用引脚」的约定（drv_uart_mode），拆开时改动一处往往要同步改三四处；
// 合成一对文件后，引脚归属与模式切换的因果链在同一个 TU 里可见。
//
// 分节顺序即依赖顺序：DMA 引擎 -> 流控脚 -> COM1 -> COM2 -> 模式 -> CDC 回调。
// ===========================================================================


// ===========================================================================
// 第 1 节：RX 的 DMA 引擎（两路 UART 共用）
// ===========================================================================

// DMA 中断优先级：与原来的 UART 中断一致（USB 是 1，这里 2），
// 保证 DMA 的中断处理不会打断 USB 传输。
#define UART_DMA_IRQ_PRIORITY 2u

// ---------------------------------------------------------------------------
// 引擎的数据结构：合并后只在**本文件内部**使用（原来放在 drv_uart_dma.h）
// ---------------------------------------------------------------------------

/* 一个 UART 的 RX DMA 通道 + 对应的中断/标志位（不同通道的位不同，必须显式给） */
typedef struct {
    DMA_Channel_TypeDef *rx_ch;
    IRQn_Type rx_irqn;
    uint32_t rx_ht_it;   /* DMA1_IT_HTn */
    uint32_t rx_tc_it;   /* DMA1_IT_TCn */
    uint32_t rx_te_it;   /* DMA1_IT_TEn */
    uint32_t rx_ht_flag; /* DMA1_FLAG_HTn */
    uint32_t rx_tc_flag; /* DMA1_FLAG_TCn */
    uint32_t rx_te_flag; /* DMA1_FLAG_TEn */
} uart_dma_hw_t;

typedef struct {
    const uart_dma_hw_t *hw;
    USART_TypeDef *usart;
    uint8_t *rx_buf;
    uint32_t rx_size;              /* 必须是 2 的幂 */
    volatile uint32_t rx_base;     /* 上次半/全传输时的累计接收字节数 */
    volatile uint32_t rx_consumed; /* 软件已取走的累计字节数 */
    volatile uint32_t rx_lost;     /* 软件来不及取、被 DMA 覆盖掉的字节数 */
} uart_dma_t;

// DMA 当前已写入的累计字节数 = 上次半/全传输的基准 + 半缓冲内的增量。
//
// CH32X035 的 DMA 在 Circular 模式下 CNTR 从 buffer_size 递减到 0 后自动重装，
// 所以「已写位置」= buffer_size - CNTR 对 buffer_size 取模。base 与 base_pos 的
// 差值取模即可得到自上次边界以来写入的字节数 —— 无论此刻 CNTR 是重装前还是
// 重装后都成立（两种情况下增量都等于半个缓冲）。
//
// 读 base 与 CNTR 之间可能被 HT/TC 中断插入（它也会改 base），所以重读 base
// 直到稳定；否则会算出「倒退」的生产者位置，让读侧看到的数据少一截。
//
// ⚠️ 还有一个**更危险的窗口**：DMA 在 Circular 模式下走完一整圈时 CNTR 会重装成
// rx_size，而 HT/TC 中断可能还没跑（中断延迟几个微秒、或恰好被更高优先级中断
// 挡住）。此时 CNTR 读到的仍是满值 → pos = 0、delta = 0 → 算出的 prod 比真实值
// **小一整圈**，甚至小于已经消费到的位置。之后的溢出判断会把
// 「prod - consumed」当成下溢出的巨大数，判定为溢出 —— 结果是一次性地把环形
// 缓冲里的一大段旧数据当成新数据送给主机，也就是实板上看到的
// 「偶发性大量打印」。
//
// 真实的生产者位置不可能落后于已被读走的位置，所以这里按整圈补齐。
static uint32_t uart_dma_rx_produced(const uart_dma_t *u) {
    uint32_t mask = u->rx_size - 1u;
    const uint32_t *base_ref = (const uint32_t *)&u->rx_base;
    const uint32_t *consumed_ref = (const uint32_t *)&u->rx_consumed;
    uint32_t base;
    uint32_t remain;
    uint32_t prod;

    do {
        base = *base_ref;
        remain = DMA_GetCurrDataCounter(u->hw->rx_ch);
    } while (base != *base_ref);

    prod = base + (((u->rx_size - remain) - (base & mask)) & mask);

    // 落后于已消费位置 = 少算了一圈（见上面的说明），补齐到不小于 consumed
    while ((int32_t)(prod - *consumed_ref) < 0) {
        prod += u->rx_size;
    }

    return prod;
}

static void uart_dma_rx_start(uart_dma_t *u, const uart_dma_hw_t *hw, USART_TypeDef *usart,
                       uint8_t *rx_buf, uint32_t rx_size) {
    DMA_InitTypeDef dma = {0};
    NVIC_InitTypeDef nvic = {0};

    u->hw = hw;
    u->usart = usart;
    u->rx_buf = rx_buf;
    u->rx_size = rx_size;
    u->rx_base = 0u;
    u->rx_consumed = 0u;
    u->rx_lost = 0u;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(hw->rx_ch);
    dma.DMA_PeripheralBaseAddr = (uint32_t)(uintptr_t)&usart->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)(uintptr_t)rx_buf;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = (uint32_t)rx_size;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(hw->rx_ch, &dma);

    // 半传输 + 全传输：用来推进 rx_base 并检查溢出。每半个缓冲一次中断，
    // 1 KB 缓冲在 1 Mbaud 下是每 5 ms 一次，开销可以忽略。
    DMA_ITConfig(hw->rx_ch, hw->rx_ht_it | hw->rx_tc_it | hw->rx_te_it, ENABLE);
    DMA_ClearFlag(hw->rx_ht_flag | hw->rx_tc_flag | hw->rx_te_flag);

    nvic.NVIC_IRQChannel = hw->rx_irqn;
    nvic.NVIC_IRQChannelPreemptionPriority = UART_DMA_IRQ_PRIORITY;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    // 只开 RX 的 DMA 请求；TX 仍由 USART 的 TXE 中断驱动（见 drv_uart.c）
    USART_DMACmd(usart, USART_DMAReq_Rx, ENABLE);
    DMA_Cmd(hw->rx_ch, ENABLE);
}

static void uart_dma_rx_stop(uart_dma_t *u) {
    if (u->hw == NULL) {
        return;
    }

    USART_DMACmd(u->usart, USART_DMAReq_Rx, DISABLE);
    DMA_Cmd(u->hw->rx_ch, DISABLE);
    DMA_DeInit(u->hw->rx_ch);
    u->hw = NULL;
}

static uint32_t uart_dma_rx_available(uart_dma_t *u) {
    if (u->hw == NULL) {
        return 0u;
    }
    return uart_dma_rx_produced(u) - u->rx_consumed;
}

static uint32_t uart_dma_rx_read(uart_dma_t *u, uint8_t *dst, uint32_t len) {
    uint32_t mask = u->rx_size - 1u;
    uint32_t consumed = u->rx_consumed;
    uint32_t prod;
    uint32_t avail;
    uint32_t copied;
    uint32_t idx;

    if (u->hw == NULL || dst == NULL || len == 0u) {
        return 0u;
    }

    prod = uart_dma_rx_produced(u);

    // 溢出（软件太久没取，DMA 已经绕圈覆盖）：丢掉最旧的字节并记账。
    // consumed 只在这一处推进（主循环），所以不需要关中断。
    if ((prod - consumed) > u->rx_size) {
        uint32_t lost = (prod - consumed) - u->rx_size;

        u->rx_lost += lost;
        consumed = prod - u->rx_size;
        u->rx_consumed = consumed;
    }

    avail = prod - consumed;
    if (avail > u->rx_size) {
        // 双保险：正常路径下 prod 已保证不超过 consumed + rx_size
        avail = u->rx_size;
    }
    if (avail > len) {
        avail = len;
    }

    // 环形缓冲最多两段（到回卷点 + 剩余部分）
    idx = consumed & mask;
    copied = u->rx_size - idx;
    if (copied > avail) {
        copied = avail;
    }
    memcpy(dst, &u->rx_buf[idx], copied);
    if (copied < avail) {
        memcpy(&dst[copied], u->rx_buf, avail - copied);
    }

    u->rx_consumed = consumed + avail;
    return avail;
}

static uint32_t uart_dma_rx_lost(uart_dma_t *u) {
    return u->rx_lost;
}

static void uart_dma_rx_restart(uart_dma_t *u) {
    uint32_t pending;

    if (u->hw == NULL) {
        return;
    }

    // 已经收进缓冲但还没读走的数据，重启后就读不到了 —— 如实计入丢失
    pending = uart_dma_rx_produced(u) - u->rx_consumed;
    u->rx_lost += pending;

    DMA_Cmd(u->hw->rx_ch, DISABLE);
    DMA_ClearFlag(u->hw->rx_ht_flag | u->hw->rx_tc_flag | u->hw->rx_te_flag);
    DMA_SetCurrDataCounter(u->hw->rx_ch, (uint16_t)u->rx_size);
    u->rx_base = 0u;
    u->rx_consumed = 0u;
    DMA_Cmd(u->hw->rx_ch, ENABLE);
}

static void uart_dma_rx_isr(uart_dma_t *u) {
    // 到半/全传输边界了：CNTR 此时正好在边界上，直接把累计值推到这里。
    // 溢出检测放在 uart_dma_rx_read()（consumed 只有它推进），这里只做基数维护。
    if (u->hw == NULL) {
        return;
    }

    if (DMA_GetFlagStatus(u->hw->rx_te_flag) != RESET) {
        DMA_ClearFlag(u->hw->rx_te_flag);
    }

    if (DMA_GetFlagStatus(u->hw->rx_ht_flag) != RESET ||
        DMA_GetFlagStatus(u->hw->rx_tc_flag) != RESET) {
        u->rx_base = uart_dma_rx_produced(u);
        DMA_ClearFlag(u->hw->rx_ht_flag | u->hw->rx_tc_flag);
    }

    // 硬件溢出（DMA 没能及时搬走）：读一次 DATAR 清 ORE，并记一次丢失
    if (USART_GetFlagStatus(u->usart, USART_FLAG_ORE) != RESET) {
        (void)USART_ReceiveData(u->usart);
        u->rx_lost++;
    }
}


// ===========================================================================
// 第 2 节：PA2 / PA3 的 DTR + RTS 输出
// ===========================================================================

// DTR/RTS 是「低有效」输出（FT232 的 DTR#/RTS# 习惯），所以：
//   断言 -> BCR 拉低      释放 -> BSHR 置高
static bool s_dtr;
static bool s_rts;
static bool s_enabled;

static void flow_pin_drive(uint32_t pin, bool asserted) {
    if (asserted) {
        BOARD_AUX_PORT->BCR = pin;
    } else {
        BOARD_AUX_PORT->BSHR = pin;
    }
}

static void flow_pins_apply(void) {
    flow_pin_drive(BOARD_AUX_DTR_PIN, s_dtr);
    flow_pin_drive(BOARD_AUX_RTS_PIN, s_rts);
}

void drv_flow_pins_release(void) {
    // 每个 PA0..PA7 在 CFGLR 里占 4 位；0x4 = 浮空输入
    BOARD_AUX_PORT->CFGLR =
        (BOARD_AUX_PORT->CFGLR &
         ~((0xFUL << BOARD_AUX_DTR_CFGLR_SHIFT) | (0xFUL << BOARD_AUX_RTS_CFGLR_SHIFT))) |
        (0x4UL << BOARD_AUX_DTR_CFGLR_SHIFT) | (0x4UL << BOARD_AUX_RTS_CFGLR_SHIFT);
    s_enabled = false;
}

void drv_flow_pins_enable(void) {
    RCC_APB2PeriphClockCmd(BOARD_AUX_PORT_CLK, ENABLE);

    // 先把输出数据摆成「未断言」（高电平），再切成推挽输出，
    // 避免切换瞬间把两根线短暂拉低（目标板可能把它当复位/下载请求）
    BOARD_AUX_PORT->BSHR = BOARD_AUX_DTR_PIN | BOARD_AUX_RTS_PIN;
    BOARD_AUX_PORT->CFGLR =
        (BOARD_AUX_PORT->CFGLR &
         ~((0xFUL << BOARD_AUX_DTR_CFGLR_SHIFT) | (0xFUL << BOARD_AUX_RTS_CFGLR_SHIFT))) |
        (0x1UL << BOARD_AUX_DTR_CFGLR_SHIFT) | (0x1UL << BOARD_AUX_RTS_CFGLR_SHIFT);
    s_enabled = true;

    flow_pins_apply();
}

void drv_flow_pins_set_dtr(bool asserted) {
    s_dtr = asserted;
    if (s_enabled) {
        flow_pin_drive(BOARD_AUX_DTR_PIN, asserted);
    }
}

void drv_flow_pins_set_rts(bool asserted) {
    s_rts = asserted;
    if (s_enabled) {
        flow_pin_drive(BOARD_AUX_RTS_PIN, asserted);
    }
}

bool drv_flow_pins_dtr(void) {
    return s_enabled && s_dtr;
}

bool drv_flow_pins_rts(void) {
    return s_enabled && s_rts;
}


// ===========================================================================
// COM1：USART4（PB0 = TX / PB1 = RX）  (原 drv_uart.c)
// ===========================================================================

// USART4 挂在 APB1，波特率由 USART_Init() 通过 RCC_GetClocksFreq() 自动换算，
// 本工程 APB1 = SYSCLK = 48 MHz（见 system_ch32x035.c 的 SetSysClockTo48_HSI）。
// 引脚定义见 src/board_pin_map.h（PB0 = TX、PB1 = RX）。
#define UART_INSTANCE   BOARD_UART1_INSTANCE
#define UART_IRQN       BOARD_UART1_IRQN
#define UART_GPIO       BOARD_UART1_PORT
#define UART_GPIO_CLK   BOARD_UART1_PORT_CLK
#define UART_CLK        BOARD_UART1_CLK
#define UART_TX_PIN     BOARD_UART1_TX_PIN
#define UART_RX_PIN     BOARD_UART1_RX_PIN

// ===========================================================================
// RX 走 DMA（USART4_RX = DMA1_Channel8，见本文件开头的 DMA 引擎小节），TX 仍用 TXE 中断
//
// RX 是丢数据的那一侧：1 Mbaud 下逐字节中断每秒 10 万次，又会被 USB 中断抢占，
// 来不及读 DATAR 时硬件置 ORE 直接丢字节 —— 这部分交给 DMA 之后只剩「每半个
// 缓冲一次」的中断。
//
// TX 保持中断：实测在这颗芯片上给 TX 挂 DMA（Ch1 / Ch7 都试过）会让对应串口
// 卡死（原因未定位）。TX 逐字节中断约 10% CPU，可靠性优先。
// ===========================================================================

// RX 用 DMA 环形缓冲，长度必须是 2 的幂（引擎用掩码取模）。
// 1 KB 在 1 Mbaud 下是约 10 ms 的容错窗口；这是**唯一的** RX 缓冲，
// 没有第二级 FIFO。
#define UART_RX_DMA_SIZE 1024u

// TX 是软件 ring（USB 侧按它做背压），由 TXE 中断逐字节排空。
// 256 B = 4 个 USB 包，配合背压足够。
#define UART_TX_FIFO_SIZE 256u

#define UART_DEFAULT_BAUDRATE 115200u

static uint8_t uart_rx_dma_buf[UART_RX_DMA_SIZE];

// TX FIFO：生产者是 USB 中断（CDC OUT 回调），消费者是 USART4 中断
static uint8_t uart_tx_pool[UART_TX_FIFO_SIZE];
static chry_ringbuffer_t uart_tx_fifo;

static uart_dma_t uart_dma;

static const uart_dma_hw_t uart_dma_hw = {
    .rx_ch = DMA1_Channel8,
    .rx_irqn = DMA1_Channel8_IRQn,
    .rx_ht_it = DMA1_IT_HT8,
    .rx_tc_it = DMA1_IT_TC8,
    .rx_te_it = DMA1_IT_TE8,
    .rx_ht_flag = DMA1_FLAG_HT8,
    .rx_tc_flag = DMA1_FLAG_TC8,
    .rx_te_flag = DMA1_FLAG_TE8,
};

static uint32_t uart_baudrate = UART_DEFAULT_BAUDRATE;
static volatile uint32_t uart_tx_overflow;
static bool uart_initialized;

static void uart_apply_baudrate(uint32_t baudrate) {
    USART_InitTypeDef usart = {0};

    usart.USART_BaudRate = baudrate;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(UART_INSTANCE, &usart);

    uart_baudrate = baudrate;
}

void drv_uart_init(uint32_t baudrate) {
    GPIO_InitTypeDef gpio = {0};
    NVIC_InitTypeDef nvic = {0};

    RCC_APB2PeriphClockCmd(UART_GPIO_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(UART_CLK, ENABLE);

    // PB0 = USART4_TX，复用推挽
    gpio.GPIO_Pin = UART_TX_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(UART_GPIO, &gpio);

    // PB1 = USART4_RX，上拉输入：不接目标时也不会收到悬空噪声
    gpio.GPIO_Pin = UART_RX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(UART_GPIO, &gpio);

    (void)chry_ringbuffer_init(&uart_tx_fifo, uart_tx_pool, sizeof(uart_tx_pool));
    uart_tx_overflow = 0u;

    uart_apply_baudrate(baudrate != 0u ? baudrate : UART_DEFAULT_BAUDRATE);

    // RX 由 DMA 接管，所以**不**开 RXNE 中断；TXE 仅在有数据待发时才打开，
    // 避免空 FIFO 反复进中断。
    USART_Cmd(UART_INSTANCE, ENABLE);
    uart_dma_rx_start(&uart_dma, &uart_dma_hw, UART_INSTANCE, uart_rx_dma_buf,
                      UART_RX_DMA_SIZE);

    // TXE 中断：USB 中断优先级 1，这里 2，保证 UART 中断不会打断 USB
    nvic.NVIC_IRQChannel = UART_IRQN;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    uart_initialized = true;
}

bool drv_uart_tx_idle(void) {
    return chry_ringbuffer_check_empty(&uart_tx_fifo) &&
           (USART_GetFlagStatus(UART_INSTANCE, USART_FLAG_TC) != RESET);
}

void drv_uart_set_baudrate(uint32_t baudrate) {
    if (!uart_initialized || baudrate == 0u || baudrate == uart_baudrate) {
        return;
    }

    // 等当前待发数据发完，避免换速率时把在途字节截断
    uint32_t guard = 0u;
    while (!drv_uart_tx_idle() && guard++ < 1000000u) {
    }

    uart_apply_baudrate(baudrate);
}

uint32_t drv_uart_baudrate(void) {
    return uart_baudrate;
}

uint32_t drv_uart_write(const uint8_t *data, uint32_t length) {
    uint32_t written;

    if (!uart_initialized || data == NULL) {
        return 0u;
    }

    // chry_ringbuffer_write 的参数是 void*（只读语义），这里做个 const 转换
    written = chry_ringbuffer_write(&uart_tx_fifo, (void *)(uintptr_t)data, length);

    if (written != length) {
        // ring 满，超出部分被丢弃。正常路径不会走到这里 —— CDC OUT 回调会先查
        // drv_uart_tx_free()，确认放得下整包才重挂端点（背压）。
        uart_tx_overflow += (length - written);
    }

    if (written != 0u) {
        // 先入队再使能 TXE，否则中断可能在数据就绪前跑空
        USART_ITConfig(UART_INSTANCE, USART_IT_TXE, ENABLE);
    }
    return written;
}

uint32_t drv_uart_read(uint8_t *data, uint32_t length) {
    return uart_dma_rx_read(&uart_dma, data, length);
}

uint32_t drv_uart_rx_available(void) {
    return uart_dma_rx_available(&uart_dma);
}

uint32_t drv_uart_tx_free(void) {
    return chry_ringbuffer_get_free(&uart_tx_fifo);
}

uint32_t drv_uart_rx_overflow_count(void) {
    return uart_dma_rx_lost(&uart_dma);
}

uint32_t drv_uart_tx_overflow_count(void) {
    return uart_tx_overflow;
}

// 长时间关中断（擦写选项字）之后重新对齐 RX 环形 DMA 的计数基准，
// 见本文件 uart_dma_rx_restart() 的说明。
void drv_uart_rx_resync(void) {
    uart_dma_rx_restart(&uart_dma);
}

// USART4 中断：只处理 TXE（RX 由 DMA 负责）。
// 用 USART_GetFlagStatus 而不是 USART_GetITStatus：后者会把「中断使能位」与
// 「状态标志」相与（见 SDK 实现），使能位状态会干扰标志判读。
void USART4_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART4_IRQHandler(void) {
    if (USART_GetFlagStatus(UART_INSTANCE, USART_FLAG_TXE) != RESET) {
        uint8_t byte;

        if (chry_ringbuffer_read_byte(&uart_tx_fifo, &byte)) {
            USART_SendData(UART_INSTANCE, byte);
        } else {
            USART_ITConfig(UART_INSTANCE, USART_IT_TXE, DISABLE);

            // 关键：关掉 TXE 之后再确认一次 FIFO 是否真的空。
            // 本中断优先级为 2，而 USB 中断是 1（更高）。USB 中断可能在
            // 上面「判定为空」与这行「关闭 TXE」之间插入 drv_uart_write()，
            // 写入新数据并重新使能 TXE。若不复查，这次关闭就会把 TXE 永久关掉，
            // 数据卡在 FIFO 里直到下次 drv_uart_write 才被唤醒 —— 现象就是
            // 高波特率下串口随机「卡住几秒」。
            if (!chry_ringbuffer_check_empty(&uart_tx_fifo)) {
                USART_ITConfig(UART_INSTANCE, USART_IT_TXE, ENABLE);
            }
        }
    }
}

// USART4 的 RX DMA 中断（半/全传输、传输错误）
void DMA1_Channel8_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel8_IRQHandler(void) {
    uart_dma_rx_isr(&uart_dma);
}


// ===========================================================================
// 第 4 节：COM2 —— USART2（PA2 = TX2 / PA3 = RX2，仅双串口模式）
// ===========================================================================

// USART2 挂在 APB1，波特率由 USART_Init() 通过 RCC_GetClocksFreq() 自动换算
// （本工程 APB1 = SYSCLK = 48 MHz，与 USART4 相同）。
// 引脚见 src/board_pin_map.h：PA2 = USART2_TX（TX2）、PA3 = USART2_RX（RX2）。
//
// ===========================================================================
// RX 走 DMA（USART2_RX = DMA1_Channel6），TX 用 TXE 中断
// ===========================================================================
// 与 COM1（drv_uart.c）的分工一致：
//   RX —— DMA 环形缓冲（1 KB，零拷贝），只有「每半个缓冲一次」的中断，
//         高波特率下不会再因为 ISR 延迟来不及读 DATAR 而触发 ORE 丢字节。
//   TX —— 逐字节 TXE 中断。TX DMA 在这颗芯片上实测会让串口卡死（Ch1 / Ch7 都
//         试过），所以两个串口的 TX 都保持中断。
#define UART2_RX_DMA_SIZE 1024u
#define UART2_TX_FIFO_SIZE 256u
#define UART2_DEFAULT_BAUDRATE 115200u

static uint8_t uart2_rx_dma_buf[UART2_RX_DMA_SIZE];

// TX FIFO：生产者是 USB 中断（CDC OUT 回调），消费者是 USART2 中断
static uint8_t uart2_tx_pool[UART2_TX_FIFO_SIZE];
static chry_ringbuffer_t uart2_tx_fifo;

static uart_dma_t uart2_dma;

static const uart_dma_hw_t uart2_dma_hw = {
    .rx_ch = DMA1_Channel6,
    .rx_irqn = DMA1_Channel6_IRQn,
    .rx_ht_it = DMA1_IT_HT6,
    .rx_tc_it = DMA1_IT_TC6,
    .rx_te_it = DMA1_IT_TE6,
    .rx_ht_flag = DMA1_FLAG_HT6,
    .rx_tc_flag = DMA1_FLAG_TC6,
    .rx_te_flag = DMA1_FLAG_TE6,
};

static uint32_t uart2_baudrate = UART2_DEFAULT_BAUDRATE;
static volatile uint32_t uart2_tx_overflow;
static bool uart2_initialized;

static void uart2_apply_baudrate(uint32_t baudrate) {
    USART_InitTypeDef usart = {0};

    usart.USART_BaudRate = baudrate;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(BOARD_AUX_UART_INSTANCE, &usart);

    uart2_baudrate = baudrate;
}

void drv_uart2_reset(void) {
    (void)chry_ringbuffer_init(&uart2_tx_fifo, uart2_tx_pool, sizeof(uart2_tx_pool));
    uart2_tx_overflow = 0u;
}

void drv_uart2_init(uint32_t baudrate) {
    GPIO_InitTypeDef gpio = {0};
    NVIC_InitTypeDef nvic = {0};

    if (uart2_initialized) {
        drv_uart2_deinit();
    }

    RCC_APB2PeriphClockCmd(BOARD_AUX_PORT_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(BOARD_AUX_UART_CLK, ENABLE);

    // PA2 = USART2_TX，复用推挽
    gpio.GPIO_Pin = BOARD_AUX_UART_TX_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(BOARD_AUX_PORT, &gpio);

    // PA3 = USART2_RX，上拉输入：不接目标时也不会收到悬空噪声
    gpio.GPIO_Pin = BOARD_AUX_UART_RX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(BOARD_AUX_PORT, &gpio);

    drv_uart2_reset();

    uart2_apply_baudrate(baudrate != 0u ? baudrate : UART2_DEFAULT_BAUDRATE);

    // RX 交给 DMA（引擎内部会开 RX 的 DMA 请求并挂 Ch6 中断），这里只留 TXE 中断。
    USART_Cmd(BOARD_AUX_UART_INSTANCE, ENABLE);
    uart_dma_rx_start(&uart2_dma, &uart2_dma_hw, BOARD_AUX_UART_INSTANCE,
                      uart2_rx_dma_buf, UART2_RX_DMA_SIZE);

    // 与 USART4 同级：USB 中断优先级 1，这里 2，保证 UART 中断不会打断 USB
    nvic.NVIC_IRQChannel = BOARD_AUX_UART_IRQN;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    uart2_initialized = true;
}

void drv_uart2_deinit(void) {
    NVIC_InitTypeDef nvic = {0};

    if (uart2_initialized) {
        uart_dma_rx_stop(&uart2_dma);

        USART_ITConfig(BOARD_AUX_UART_INSTANCE, USART_IT_TXE, DISABLE);
        USART_Cmd(BOARD_AUX_UART_INSTANCE, DISABLE);

        nvic.NVIC_IRQChannel = BOARD_AUX_UART_IRQN;
        nvic.NVIC_IRQChannelPreemptionPriority = 2;
        nvic.NVIC_IRQChannelSubPriority = 0;
        nvic.NVIC_IRQChannelCmd = DISABLE;
        NVIC_Init(&nvic);

        RCC_APB1PeriphClockCmd(BOARD_AUX_UART_CLK, DISABLE);

        uart2_initialized = false;
        drv_uart2_reset();
    }

    // 无论是否初始化过都把 PA2/PA3 交出去：调用方指望本函数之后这两个脚
    // 不再由 USART2 驱动（随后由 drv_flow_pins_enable() 接管成 DTR/RTS）
    drv_flow_pins_release();
}

bool drv_uart2_ready(void) {
    return uart2_initialized;
}

void drv_uart2_set_baudrate(uint32_t baudrate) {
    if (!uart2_initialized || baudrate == 0u || baudrate == uart2_baudrate) {
        return;
    }
    uart2_apply_baudrate(baudrate);
}

uint32_t drv_uart2_baudrate(void) {
    return uart2_baudrate;
}

uint32_t drv_uart2_write(const uint8_t *data, uint32_t length) {
    uint32_t written;

    if (!uart2_initialized || data == NULL || length == 0u) {
        return 0u;
    }

    // chry_ringbuffer_write 的参数是 void*（只读语义），这里做个 const 转换
    written = chry_ringbuffer_write(&uart2_tx_fifo, (void *)(uintptr_t)data, length);

    if (written != length) {
        // USB 侧已做背压（先查 drv_uart2_tx_free() 再重挂端点），
        // 这里不该丢；一旦增长说明背压失效或有非预期重入
        uart2_tx_overflow += (length - written);
    }

    if (written != 0u) {
        // 先入队再使能 TXE，否则中断可能在数据就绪前跑空
        USART_ITConfig(BOARD_AUX_UART_INSTANCE, USART_IT_TXE, ENABLE);
    }
    return written;
}

uint32_t drv_uart2_read(uint8_t *data, uint32_t length) {
    return uart_dma_rx_read(&uart2_dma, data, length);
}

uint32_t drv_uart2_rx_available(void) {
    return uart_dma_rx_available(&uart2_dma);
}

uint32_t drv_uart2_tx_free(void) {
    return chry_ringbuffer_get_free(&uart2_tx_fifo);
}

uint32_t drv_uart2_rx_overflow_count(void) {
    return uart_dma_rx_lost(&uart2_dma);
}

uint32_t drv_uart2_tx_overflow_count(void) {
    return uart2_tx_overflow;
}

// 长时间关中断（擦写选项字）之后重新对齐 RX 环形 DMA 的计数基准，
// 见本文件 uart_dma_rx_restart() 的说明。
void drv_uart2_rx_resync(void) {
    uart_dma_rx_restart(&uart2_dma);
}

// USART2 中断：只处理 TXE（RX 由 DMA 负责）
void USART2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART2_IRQHandler(void) {
    // 用 USART_GetFlagStatus 而不是 USART_GetITStatus：后者会把「中断使能位」
    // 与「状态标志」相与（见 SDK 实现），使能位状态会干扰标志判读。
    if (USART_GetFlagStatus(BOARD_AUX_UART_INSTANCE, USART_FLAG_TXE) != RESET) {
        uint8_t byte;

        if (chry_ringbuffer_read_byte(&uart2_tx_fifo, &byte)) {
            USART_SendData(BOARD_AUX_UART_INSTANCE, byte);
        } else {
            USART_ITConfig(BOARD_AUX_UART_INSTANCE, USART_IT_TXE, DISABLE);

            // 关 TXE 后再复查一次：USB 中断（优先级 1）可能刚好在中间写入新数据
            if (!chry_ringbuffer_check_empty(&uart2_tx_fifo)) {
                USART_ITConfig(BOARD_AUX_UART_INSTANCE, USART_IT_TXE, ENABLE);
            }
        }
    }
}

// USART2 的 RX DMA 中断（半/全传输、传输错误）
void DMA1_Channel6_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel6_IRQHandler(void) {
    uart_dma_rx_isr(&uart2_dma);
}


// ===========================================================================
// 第 5 节：串口工作模式（单串口+DTR/RTS <-> 双串口）+ PB12 指示
// ===========================================================================

static drv_uart_mode_t s_mode = DRV_UART_MODE_FLOW;

// PB12 是 pin12 -> CFGHR，走 GPIO_Init（SDK 对 CFGHR 有静态缓存，裸写会让缓存
// 失步，详见 board_pin_map.h 的说明）
static void uart_mode_led_init(void) {
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(BOARD_LED_PORT_CLK, ENABLE);

    // 先把输出数据摆成「灭」再切输出，避免初始化瞬间闪一下
#if BOARD_LED_ACTIVE_LOW
    BOARD_LED_PORT->BSHR = BOARD_LED_UART_PIN;
#else
    BOARD_LED_PORT->BCR = BOARD_LED_UART_PIN;
#endif

    gpio.GPIO_Pin = BOARD_LED_UART_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(BOARD_LED_PORT, &gpio);
}

static void uart_mode_led_set(bool on) {
#if BOARD_LED_ACTIVE_LOW
    if (on) {
        BOARD_LED_PORT->BCR = BOARD_LED_UART_PIN;
    } else {
        BOARD_LED_PORT->BSHR = BOARD_LED_UART_PIN;
    }
#else
    if (on) {
        BOARD_LED_PORT->BSHR = BOARD_LED_UART_PIN;
    } else {
        BOARD_LED_PORT->BCR = BOARD_LED_UART_PIN;
    }
#endif
}

// 把 PA2/PA3 交给对应功能，并同步指示灯。
// 两个分支都先让「下一个使用者」之外的功能退场，避免两边抢同一组 CFGLR 位。
static void uart_mode_apply(drv_uart_mode_t mode) {
    if (mode == DRV_UART_MODE_DUAL) {
        drv_flow_pins_release();                    // 先交出 GPIO
        drv_uart2_init(drv_uart2_baudrate());       // 保留上次主机设过的波特率
    } else {
        drv_uart2_deinit();                         // 内部会释放引脚
        drv_flow_pins_enable();                     // 再接管成 DTR/RTS
    }

    s_mode = mode;
    uart_mode_led_set(mode == DRV_UART_MODE_DUAL);
}

void drv_uart_mode_init(void) {
    drv_uart_mode_t mode = DRV_UART_MODE_FLOW;

    uart_mode_led_init();

    // 选项字里的记录无效（全新芯片 / 擦除态 / 校验失败）时保持默认的 DTR/RTS 模式
    (void)drv_mode_store_load_uart_mode(&mode);

    // uart_mode_apply 内部保证：接管 DTR/RTS 时先摆成「未断言」（高）再切输出，
    // 所以上电不会把目标摁进复位
    uart_mode_apply(mode);
}

drv_uart_mode_t drv_uart_mode_get(void) {
    return s_mode;
}

bool drv_uart_mode_is_dual(void) {
    return s_mode == DRV_UART_MODE_DUAL;
}

void drv_uart_mode_toggle(void) {
    drv_uart_mode_t next = (s_mode == DRV_UART_MODE_DUAL) ? DRV_UART_MODE_FLOW : DRV_UART_MODE_DUAL;

    uart_mode_apply(next);

    // 只影响持久化，失败也保持刚切过去的模式
    (void)drv_mode_store_save_uart_mode(next);

    // 必须在写选项字**之后**：擦整页要几十毫秒，期间 CPU 停在 Flash 编程上，
    // 两路 RX 的 DMA 都在自己绕圈，计数基准会失真。这里重新对齐
    // （在途字节会计入丢失），否则之后读到的数据位置会错。
    drv_uart_rx_resync();
    drv_uart2_rx_resync();
}


// ===========================================================================
// 第 6 节：CDC ACM 行编码 / DTR / RTS 回调（两种调试器模式共用）
// ===========================================================================

// stdbool/stdint/string 必须在 CherryUSB 头文件之前：
// class/cdc/usb_cdc.h 直接用了 uint8_t / uint16_t / bool，但自己没包含
// <stdint.h> / <stdbool.h>，假定引用者已经引入过（与 src/dap/dap_main.h 里
// 记录的是同一类问题，那里踩的是 bool）。



// 两个 CDC 控制接口的编号：
//   COM1 = 接口 1（+ 数据接口 2）—— CMSIS-DAP 与 WCH-Link 两种设备一致
//   COM2 = 接口 3（+ 数据接口 4）—— 只有 CMSIS-DAP 设备有（双串口模式的第二路）
#define CDC_INTF_COM1 1u
#define CDC_INTF_COM2 3u

// 默认值与 COM1 / COM2 的默认波特率一致。
#define CDC_LINE_DEFAULT_BAUDRATE 115200u

static struct cdc_line_coding s_cdc_line_coding_com1 = {
    .dwDTERate = CDC_LINE_DEFAULT_BAUDRATE,
    .bCharFormat = 0u,   /* 1 stop bit */
    .bParityType = 0u,   /* no parity  */
    .bDataBits = 8u,
};

static struct cdc_line_coding s_cdc_line_coding_com2 = {
    .dwDTERate = CDC_LINE_DEFAULT_BAUDRATE,
    .bCharFormat = 0u,   /* 1 stop bit */
    .bParityType = 0u,   /* no parity  */
    .bDataBits = 8u,
};

static struct cdc_line_coding *cdc_line_coding_for(uint8_t intf) {
    return (intf == CDC_INTF_COM2) ? &s_cdc_line_coding_com2 : &s_cdc_line_coding_com1;
}

void drv_cdc_line_init(void) {
    s_cdc_line_coding_com1.dwDTERate = CDC_LINE_DEFAULT_BAUDRATE;
    s_cdc_line_coding_com1.bCharFormat = 0u;
    s_cdc_line_coding_com1.bParityType = 0u;
    s_cdc_line_coding_com1.bDataBits = 8u;

    s_cdc_line_coding_com2.dwDTERate = CDC_LINE_DEFAULT_BAUDRATE;
    s_cdc_line_coding_com2.bCharFormat = 0u;
    s_cdc_line_coding_com2.bParityType = 0u;
    s_cdc_line_coding_com2.bDataBits = 8u;
}

// 主机下发行编码：既回存（主机打开串口时要能读回自己的配置），也用来设真实波特率。
//
// 不对速率做上限检查（见 src/dap/dap_main.c 里「关于 COM 口波特率」的说明）：
// 主机下什么就设什么，UART 硬件能产生多少就是多少。USART 是 16 倍过采样，
// BRR = 48 MHz / 波特率，标称上限 3 Mbaud；超过后 BRR 整数部分为 0，
// 实际速率不再受控（≈48 MHz / BRR）。
void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *line_coding) {
    (void)busid;

    if (line_coding == NULL || line_coding->dwDTERate == 0u) {
        return;
    }

    memcpy(cdc_line_coding_for(intf), line_coding, sizeof(struct cdc_line_coding));

    // COM2 在非双串口模式下 USART2 是关的，drv_uart2_set_baudrate 会直接返回；
    // 等切到双串口模式时 drv_uart2_init 会带上这里存下的速率
    if (intf == CDC_INTF_COM2) {
        drv_uart2_set_baudrate(line_coding->dwDTERate);
    } else {
        drv_uart_set_baudrate(line_coding->dwDTERate);
    }
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *line_coding) {
    (void)busid;

    if (line_coding == NULL) {
        return;
    }
    memcpy(line_coding, cdc_line_coding_for(intf), sizeof(*line_coding));
}

// DTR / RTS 只来自 COM1：COM2 是给第二路串口用的，它的控制线没有对应引脚。
//
// 电平是**低有效**（FT232 的 DTR#/RTS# 习惯）：主机断言时 PA2/PA3 输出低。
// 只有「串口 1 + DTR/RTS 流控」模式下这两个脚才是 GPIO；双串口模式下它们
// 已经交给 USART2，drv_flow_pins_set_* 会自动变成空操作（引脚已释放）。
void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr) {
    (void)busid;

    if (intf != CDC_INTF_COM1) {
        return;
    }
    drv_flow_pins_set_dtr(dtr);
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts) {
    (void)busid;

    if (intf != CDC_INTF_COM1) {
        return;
    }
    drv_flow_pins_set_rts(rts);
}
