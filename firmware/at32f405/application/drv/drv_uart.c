#include "drv/drv_uart.h"

#include <string.h>

#include "at32f402_405.h"

#include "board_pin_map.h"
#include "chry_ringbuffer.h"

#define UART_RX_BUFFER_SIZE 4096u
#define UART_TX_BUFFER_SIZE 4096u

/* RX 走 DMA 环形缓冲：12 Mbaud 下一个字节只有 0.83 us，靠中断逐字节搬运
 * 一旦被 USB 中断（或在 USB 驱动里关中断的临界区）挡住超过一个字节时间就会
 * ORE 丢字节。DMA 由硬件握手，不再受任何中断延迟影响。 */
#define UART_RX_DMA_CHANNEL  DMA1_CHANNEL6
#define UART_RX_DMA_MUX      DMA1MUX_CHANNEL6
#define UART_RX_DMA_REQ      DMAMUX_DMAREQ_ID_USART2_RX

static chry_ringbuffer_t s_tx_rb;
static uint8_t s_rx_pool[UART_RX_BUFFER_SIZE];
static uint8_t s_tx_pool[UART_TX_BUFFER_SIZE];

/* DMA 写完的位置（= 已写总量 % UART_RX_BUFFER_SIZE），由计数器反推 */
static uint32_t s_rx_read_pos;

static volatile uint32_t s_rx_overflow;
static volatile uint32_t s_tx_overflow;
static uint32_t s_baudrate = 115200u;

static bool s_flow_pins_enabled;
static bool s_dtr;
static bool s_rts;

/* ------------------------------------------------------------------ 内部 */

static void uart_gpio_config(void) {
    gpio_init_type gpio_init_struct;

    crm_periph_clock_enable(BOARD_UART_GPIO_CLK, TRUE);

    /* PA2 = USART2_TX，PA3 = USART2_RX，AF7 */
    gpio_pin_mux_config(GPIOA, BOARD_UART_TX_SOURCE, BOARD_UART_MUX);
    gpio_pin_mux_config(GPIOA, BOARD_UART_RX_SOURCE, BOARD_UART_MUX);

    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = BOARD_UART_TX_PIN | BOARD_UART_RX_PIN;
    gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_pull = GPIO_PULL_UP; /* RX 空闲时保持高电平 */
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOA, &gpio_init_struct);
}

static void uart_rx_dma_config(void) {
    dma_init_type dma_init_struct;

    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);

    dma_reset(UART_RX_DMA_CHANNEL);
    dmamux_enable(DMA1, TRUE);
    dma_flexible_config(DMA1, UART_RX_DMA_MUX, UART_RX_DMA_REQ);

    dma_default_para_init(&dma_init_struct);
    dma_init_struct.peripheral_base_addr = (uint32_t)&BOARD_UART->dt;
    dma_init_struct.memory_base_addr = (uint32_t)s_rx_pool;
    dma_init_struct.direction = DMA_DIR_PERIPHERAL_TO_MEMORY;
    dma_init_struct.buffer_size = (uint16_t)UART_RX_BUFFER_SIZE;
    dma_init_struct.peripheral_inc_enable = FALSE;
    dma_init_struct.memory_inc_enable = TRUE;
    dma_init_struct.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
    dma_init_struct.memory_data_width = DMA_MEMORY_DATA_WIDTH_BYTE;
    dma_init_struct.loop_mode_enable = TRUE;
    dma_init_struct.priority = DMA_PRIORITY_MEDIUM;
    dma_init(UART_RX_DMA_CHANNEL, &dma_init_struct);

    dma_channel_enable(UART_RX_DMA_CHANNEL, TRUE);

    /* 环形缓冲每次重开都从 0 开始，读指针同步复位 */
    s_rx_read_pos = 0u;
}

/* DMA 已写位置（0..size-1） */
static uint32_t uart_rx_dma_write_pos(void) {
    return UART_RX_BUFFER_SIZE - (uint32_t)dma_data_number_get(UART_RX_DMA_CHANNEL);
}

void drv_uart_init(uint32_t baudrate) {
    chry_ringbuffer_init(&s_tx_rb, s_tx_pool, UART_TX_BUFFER_SIZE);
    s_rx_overflow = 0u;
    s_tx_overflow = 0u;

    crm_periph_clock_enable(BOARD_UART_CLK, TRUE);
    uart_gpio_config();

    usart_init(BOARD_UART, baudrate, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_parity_selection_config(BOARD_UART, USART_PARITY_NONE);
    usart_transmitter_enable(BOARD_UART, TRUE);
    usart_receiver_enable(BOARD_UART, TRUE);

    uart_rx_dma_config();
    usart_dma_receiver_enable(BOARD_UART, TRUE);

    usart_flag_clear(BOARD_UART, USART_RDBF_FLAG);
    usart_interrupt_enable(BOARD_UART, USART_RDBF_INT, FALSE); /* RX 交给 DMA */
    usart_interrupt_enable(BOARD_UART, USART_TDBE_INT, FALSE);

    nvic_irq_enable(BOARD_UART_IRQ, 0u, 0u); /* TX 就绪中断尽量快，避免 TX 环积压 */
    usart_enable(BOARD_UART, TRUE);

    s_baudrate = baudrate;
}

void drv_uart_set_baudrate(uint32_t baudrate) {
    usart_enable(BOARD_UART, FALSE);
    usart_init(BOARD_UART, baudrate, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_transmitter_enable(BOARD_UART, TRUE);
    usart_receiver_enable(BOARD_UART, TRUE);
    usart_enable(BOARD_UART, TRUE);

    /* 换波特率时丢掉旧数据，避免半帧混进来 */
    s_rx_read_pos = uart_rx_dma_write_pos();

    s_baudrate = baudrate;
}

uint32_t drv_uart_baudrate(void) {
    return s_baudrate;
}

void drv_uart_putc(uint8_t byte)
{
    (void)drv_uart_write(&byte, 1u);
}
uint32_t drv_uart_write(const uint8_t *data, uint32_t length) {
    if (data == 0 || length == 0u) {
        return 0u;
    }

    const uint32_t written = chry_ringbuffer_write(&s_tx_rb, (void *)data, length);
    if (written < length) {
        s_tx_overflow += length - written;
    }

    if (written > 0u) {
        usart_interrupt_enable(BOARD_UART, USART_TDBE_INT, TRUE);
    }
    return written;
}

uint32_t drv_uart_read(uint8_t *data, uint32_t length) {
    if (data == 0 || length == 0u) {
        return 0u;
    }

    uint32_t available = drv_uart_rx_available();
    if (available == 0u) {
        return 0u;
    }
    if (length > available) {
        length = available;
    }

    uint32_t first = UART_RX_BUFFER_SIZE - s_rx_read_pos;
    if (first > length) {
        first = length;
    }

    memcpy(data, &s_rx_pool[s_rx_read_pos], first);
    if (length > first) {
        memcpy(&data[first], s_rx_pool, length - first);
    }

    s_rx_read_pos = (s_rx_read_pos + length) & (UART_RX_BUFFER_SIZE - 1u);
    return length;
}

uint32_t drv_uart_rx_available(void) {
    const uint32_t write_pos = uart_rx_dma_write_pos();
    return (write_pos + UART_RX_BUFFER_SIZE - s_rx_read_pos) & (UART_RX_BUFFER_SIZE - 1u);
}

uint32_t drv_uart_tx_free(void) {
    return chry_ringbuffer_get_free(&s_tx_rb);
}

bool drv_uart_tx_idle(void) {
    return chry_ringbuffer_check_empty(&s_tx_rb) && usart_flag_get(BOARD_UART, USART_TDC_FLAG) == SET;
}

uint32_t drv_uart_rx_overflow_count(void) {
    return s_rx_overflow;
}

uint32_t drv_uart_tx_overflow_count(void) {
    return s_tx_overflow;
}

void drv_uart_rx_resync(void) {
    /* 丢弃环形缓冲里的残余数据（DMA 继续跑，只把读指针追上写指针） */
    s_rx_read_pos = uart_rx_dma_write_pos();
}

/* -------------------------------------------------- 第二路串口（未实现） */

void drv_uart2_init(uint32_t baudrate) {
    (void)baudrate;
}
void drv_uart2_deinit(void) {}
bool drv_uart2_ready(void) {
    return false;
}
void drv_uart2_reset(void) {}
void drv_uart2_set_baudrate(uint32_t baudrate) {
    (void)baudrate;
}
uint32_t drv_uart2_baudrate(void) {
    return 0u;
}
uint32_t drv_uart2_write(const uint8_t *data, uint32_t length) {
    (void)data;
    (void)length;
    return 0u;
}
uint32_t drv_uart2_read(uint8_t *data, uint32_t length) {
    (void)data;
    (void)length;
    return 0u;
}
uint32_t drv_uart2_rx_available(void) {
    return 0u;
}
uint32_t drv_uart2_tx_free(void) {
    return 0u;
}
uint32_t drv_uart2_rx_overflow_count(void) {
    return 0u;
}
uint32_t drv_uart2_tx_overflow_count(void) {
    return 0u;
}
void drv_uart2_rx_resync(void) {}

/* ---------------------------------------------------------- DTR/RTS 引脚 */

static void flow_pins_write(void) {
    /* 低电平有效（DTR/RTS 语义与 RS232 一致：asserted = 逻辑 1 = 引脚低） */
    if (s_dtr) {
        gpio_bits_reset(BOARD_DTR_PORT, BOARD_DTR_PIN);
    } else {
        gpio_bits_set(BOARD_DTR_PORT, BOARD_DTR_PIN);
    }
    if (s_rts) {
        gpio_bits_reset(BOARD_RTS_PORT, BOARD_RTS_PIN);
    } else {
        gpio_bits_set(BOARD_RTS_PORT, BOARD_RTS_PIN);
    }
}

void drv_flow_pins_enable(void) {
    gpio_init_type gpio_init_struct;

    crm_periph_clock_enable(BOARD_JTAG_GPIO_CLK, TRUE);

    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = BOARD_DTR_PIN | BOARD_RTS_PIN;
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;

    /* 先写锁存器再切输出：DTR/RTS 默认“未 assert”= 高电平 */
    gpio_bits_set(BOARD_DTR_PORT, BOARD_DTR_PIN | BOARD_RTS_PIN);
    gpio_init(BOARD_DTR_PORT, &gpio_init_struct);

    s_flow_pins_enabled = true;
    flow_pins_write();
}

void drv_flow_pins_release(void) {
    gpio_init_type gpio_init_struct;

    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = BOARD_DTR_PIN | BOARD_RTS_PIN;
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init(BOARD_DTR_PORT, &gpio_init_struct);

    s_flow_pins_enabled = false;
}

void drv_flow_pins_set_dtr(bool asserted) {
    s_dtr = asserted;
    if (s_flow_pins_enabled) {
        flow_pins_write();
    }
}

void drv_flow_pins_set_rts(bool asserted) {
    s_rts = asserted;
    if (s_flow_pins_enabled) {
        flow_pins_write();
    }
}

bool drv_flow_pins_dtr(void) {
    return s_dtr;
}

bool drv_flow_pins_rts(void) {
    return s_rts;
}

/* -------------------------------------------------------------- 串口模式 */

void drv_uart_mode_init(void) {
    /* 本板只有一路串口，模式固定。
     * DTR/RTS（PB1/PB0）在单串口模式下作为流控脚使用，默认释放（高 = 无效）；
     * 只有主机真的发 SET_CONTROL_LINE_STATE 时 drv_flow_pins_set_* 才会拉动。 */
    drv_flow_pins_enable();
}

drv_uart_mode_t drv_uart_mode_get(void) {
    return DRV_UART_MODE_SINGLE;
}

bool drv_uart_mode_is_dual(void) {
    return false;
}

bool drv_uart_mode_set(drv_uart_mode_t mode) {
    return mode == DRV_UART_MODE_SINGLE;
}

void drv_uart_mode_toggle(void) {
    /* 没有可切换的第二路串口 */
}

/* ------------------------------------------------------------- 中断服务 */

void BOARD_UART_IRQHandler(void) {
    /* RX 由 DMA 环形缓冲接管（见 uart_rx_dma_config），这里只剩 TX 就绪 */
    if (BOARD_UART->ctrl1_bit.tdbeien != 0u && usart_flag_get(BOARD_UART, USART_TDBE_FLAG) != RESET) {
        uint8_t byte;
        if (chry_ringbuffer_read_byte(&s_tx_rb, &byte)) {
            usart_data_transmit(BOARD_UART, byte);
        } else {
            usart_interrupt_enable(BOARD_UART, USART_TDBE_INT, FALSE);
        }
    }
}
