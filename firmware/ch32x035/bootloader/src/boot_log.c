#include "boot_log.h"

#include <stdarg.h>
#include <stdbool.h>

#include <ch32x035.h>

#include "board_pin_map.h"

#define BOOT_LOG_BAUDRATE 115200u

static void log_putc_raw(char c) {
    while (USART_GetFlagStatus(BOARD_UART1_INSTANCE, USART_FLAG_TXE) == RESET) {
    }
    USART_SendData(BOARD_UART1_INSTANCE, (uint8_t)c);
}

/* 把 '\n' 展开成 "\r\n"，串口终端就不用另设换行处理 */
static void log_putc(char c) {
    if (c == '\n') {
        log_putc_raw('\r');
    }
    log_putc_raw(c);
}

static void log_number(uint32_t value, uint32_t base, uint32_t min_width, char pad) {
    char buf[11];
    uint32_t n = 0u;

    if (value == 0u) {
        buf[n++] = '0';
    }
    while (value != 0u) {
        uint32_t digit = value % base;
        buf[n++] = (char)((digit < 10u) ? ('0' + digit) : ('a' + digit - 10u));
        value /= base;
    }
    while (n < min_width && n < sizeof(buf)) {
        buf[n++] = pad;
    }
    while (n != 0u) {
        log_putc(buf[--n]);
    }
}

void boot_log_init(void) {
    GPIO_InitTypeDef gpio = {0};
    USART_InitTypeDef usart = {0};

    RCC_APB2PeriphClockCmd(BOARD_UART1_PORT_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(BOARD_UART1_CLK, ENABLE);

    // PB0 = USART4_TX，复用推挽（与应用的 drv_uart_init 一致）
    gpio.GPIO_Pin = BOARD_UART1_TX_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(BOARD_UART1_PORT, &gpio);

    usart.USART_BaudRate = BOOT_LOG_BAUDRATE;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_Mode = USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(BOARD_UART1_INSTANCE, &usart);
    USART_Cmd(BOARD_UART1_INSTANCE, ENABLE);
}

void boot_log(const char *fmt, ...) {
    va_list ap;

    if (fmt == 0) {
        return;
    }

    va_start(ap, fmt);
    while (*fmt != '\0') {
        char pad = ' ';
        uint32_t width = 0u;

        if (*fmt != '%') {
            log_putc(*fmt++);
            continue;
        }
        ++fmt;

        if (*fmt == '0') {
            pad = '0';
            ++fmt;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10u + (uint32_t)(*fmt - '0');
            ++fmt;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            while (s != 0 && *s != '\0') {
                log_putc(*s++);
            }
            break;
        }
        case 'c':
            log_putc((char)va_arg(ap, int));
            break;
        case 'u':
            log_number(va_arg(ap, uint32_t), 10u, width, pad);
            break;
        case 'd': {
            int32_t value = va_arg(ap, int32_t);
            if (value < 0) {
                log_putc('-');
                value = -value;
            }
            log_number((uint32_t)value, 10u, width, pad);
            break;
        }
        case 'x':
        case 'X':
            log_number(va_arg(ap, uint32_t), 16u, width, pad);
            break;
        case '%':
            log_putc('%');
            break;
        case '\0':
            // 结尾就是 '%'：原样输出，别越界
            log_putc('%');
            va_end(ap);
            return;
        default:
            log_putc('%');
            log_putc(*fmt);
            break;
        }

        if (*fmt != '\0') {
            ++fmt;
        }
    }
    va_end(ap);
}
