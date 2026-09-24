#include "drv/drv_uart.h"

#include <stddef.h>

#include <ch32x035.h>
#include <chry_ringbuffer.h>

// USART4 挂在 APB1，波特率由 USART_Init() 通过 RCC_GetClocksFreq() 自动换算，
// 本工程 APB1 = SYSCLK = 48 MHz（见 system_ch32x035.c 的 SetSysClockTo48_HSI）。
#define UART_INSTANCE   USART4
#define UART_IRQN       USART4_IRQn
#define UART_GPIO       GPIOB
#define UART_GPIO_CLK   RCC_APB2Periph_GPIOB
#define UART_CLK        RCC_APB1Periph_USART4
#define UART_TX_PIN     GPIO_Pin_0
#define UART_RX_PIN     GPIO_Pin_1

// chry_ringbuffer 要求长度为 2 的幂（内部用掩码代替取模）。
//
// 两个方向大小相同，但要注意「谁在消耗这个 FIFO」不同，这决定了它们的处境：
//
//   TX FIFO：生产者 USB 中断（CDC OUT 回调），消费者 USART4 中断。
//            USART4 中断是真正的中断，能抢占主循环 —— SWD 在处理 DAP 命令时
//            它照样在排水。所以 TX 侧不会因为 SWD 而积压。
//
//   RX FIFO：生产者 USART4 中断，消费者**主循环**。SWD 排空命令队列期间主循环
//            回不来，RX 侧只能靠 FIFO 硬顶 —— 这是并发时真正脆弱的一侧。
//
// 曾把 RX 调到 2048 想缓解并发丢字节，**实测无改善**（1024 与 2048 都是约
// 2 次 / 2 MB）：失败形态是「停顿」而不是渐进溢出，加深缓冲治不了；
// 而且 20 KB SRAM 已接近用满（再加会与栈重叠、直接链接失败）。已改回 1024。
// 真正待办：在 chry_dap_handle() 排队循环里插一次桥接调用，或 RX 改 DMA。
#define UART_FIFO_SIZE 1024u

#define UART_DEFAULT_BAUDRATE 115200u

// TX FIFO：生产者是 USB 中断（CDC OUT 回调），消费者是 USART4 中断
static uint8_t uart_tx_pool[UART_FIFO_SIZE];
static chry_ringbuffer_t uart_tx_fifo;

// RX FIFO：生产者是 USART4 中断，消费者是主循环
static uint8_t uart_rx_pool[UART_FIFO_SIZE];
static chry_ringbuffer_t uart_rx_fifo;

static uint32_t uart_baudrate = UART_DEFAULT_BAUDRATE;
static volatile uint32_t uart_rx_overflow;
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
    (void)chry_ringbuffer_init(&uart_rx_fifo, uart_rx_pool, sizeof(uart_rx_pool));
    uart_rx_overflow = 0u;
    uart_tx_overflow = 0u;

    uart_apply_baudrate(baudrate != 0u ? baudrate : UART_DEFAULT_BAUDRATE);

    // 只常开 RXNE；TXE 仅在有数据待发时才打开，避免空 FIFO 反复进中断
    USART_ITConfig(UART_INSTANCE, USART_IT_RXNE, ENABLE);

    // USB 中断优先级是 1，这里用 2，保证 UART 中断不会打断 USB 传输
    nvic.NVIC_IRQChannel = UART_IRQN;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    USART_Cmd(UART_INSTANCE, ENABLE);
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
        // FIFO 满，超出部分被丢弃。不在这里阻塞：本函数在 USB 中断上下文里被调用
        // （CDC OUT 回调），阻塞会拖垮 USB。丢多少记多少，见
        // drv_uart_tx_overflow_count()。上层若要避免丢弃需自行限流。
        uart_tx_overflow += (length - written);
    }

    if (written != 0u) {
        // 先入队再使能 TXE，否则中断可能在数据就绪前跑空
        USART_ITConfig(UART_INSTANCE, USART_IT_TXE, ENABLE);
    }
    return written;
}

uint32_t drv_uart_read(uint8_t *data, uint32_t length) {
    if (data == NULL) {
        return 0u;
    }
    return chry_ringbuffer_read(&uart_rx_fifo, data, length);
}

uint32_t drv_uart_rx_available(void) {
    return chry_ringbuffer_get_used(&uart_rx_fifo);
}

uint32_t drv_uart_rx_overflow_count(void) {
    return uart_rx_overflow;
}

uint32_t drv_uart_tx_overflow_count(void) {
    return uart_tx_overflow;
}

void USART4_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART4_IRQHandler(void) {
    // 接收。
    // 用 USART_GetFlagStatus 而不是 USART_GetITStatus：后者会把「中断使能位」
    // 与「状态标志」相与（见 SDK 实现），使能位状态会干扰标志判读。
    // 先读 STATR 再读 DATAR 是清 RXNE / ORE 的标准时序。
    if (USART_GetFlagStatus(UART_INSTANCE, USART_FLAG_RXNE) != RESET) {
        uint8_t byte = (uint8_t)USART_ReceiveData(UART_INSTANCE);

        if (!chry_ringbuffer_write_byte(&uart_rx_fifo, byte)) {
            // FIFO 满：主循环取太慢。丢新到的字节并计数
            uart_rx_overflow++;
        }
    } else if (USART_GetFlagStatus(UART_INSTANCE, USART_FLAG_ORE) != RESET) {
        // 硬件溢出：上一字节还没被读走，新字节已到达（中断延迟太大）。
        // 必须读一次 DATAR。若 ORE 置位而 RXNE 未置位时不读 DR，
        // 接收会永久卡死。
        (void)USART_ReceiveData(UART_INSTANCE);
        uart_rx_overflow++;
    }

    // 发送
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
