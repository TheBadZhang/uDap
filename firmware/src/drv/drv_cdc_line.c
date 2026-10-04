#include "drv/drv_cdc_line.h"

// stdbool/stdint/string 必须在 CherryUSB 头文件之前：
// class/cdc/usb_cdc.h 直接用了 uint8_t / uint16_t / bool，但自己没包含
// <stdint.h> / <stdbool.h>，假定引用者已经引入过（与 src/dap/dap_main.h 里
// 记录的是同一类问题，那里踩的是 bool）。
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <usbd_cdc_acm.h>

#include "drv/drv_uart.h"

// 三种构建（--dap / --wchlink / 双模式）共用这一份行编码状态与回调，
// 见 drv_cdc_line.h 的说明。默认值与 drv_uart 的默认波特率保持一致。
#define CDC_LINE_DEFAULT_BAUDRATE 115200u

static struct cdc_line_coding s_cdc_line_coding = {
    .dwDTERate = CDC_LINE_DEFAULT_BAUDRATE,
    .bCharFormat = 0u,   /* 1 stop bit */
    .bParityType = 0u,   /* no parity  */
    .bDataBits = 8u,
};

void drv_cdc_line_init(void) {
    s_cdc_line_coding.dwDTERate = CDC_LINE_DEFAULT_BAUDRATE;
    s_cdc_line_coding.bCharFormat = 0u;
    s_cdc_line_coding.bParityType = 0u;
    s_cdc_line_coding.bDataBits = 8u;
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
    (void)intf;

    if (line_coding == NULL || line_coding->dwDTERate == 0u) {
        return;
    }
    memcpy(&s_cdc_line_coding, line_coding, sizeof(s_cdc_line_coding));
    drv_uart_set_baudrate(line_coding->dwDTERate);
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *line_coding) {
    (void)busid;
    (void)intf;

    if (line_coding == NULL) {
        return;
    }
    memcpy(line_coding, &s_cdc_line_coding, sizeof(*line_coding));
}

// DTR / RTS 本工程都不用（CDC <-> USART4 是无流控的透明桥）。
// 仍然给强定义：否则双模式下会退回 CherryUSB 的 __WEAK 空实现，
// 行为虽然一样，但显式写出来更清楚。
void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr) {
    (void)busid;
    (void)intf;
    (void)dtr;
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts) {
    (void)busid;
    (void)intf;
    (void)rts;
}
