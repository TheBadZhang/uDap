/**
 * @file    usb_cdc_hooks.c
 * @brief   CherryUSB CDC ACM 的弱函数钩子 —— 把 USB 侧的行设置绑定到物理串口
 *
 * CherryUSB 的 class/cdc/usbd_cdc_acm.c 里所有「主机设置了什么」都是 __WEAK
 * 空实现，由应用覆盖。参考工程把这些写在 dap_main.c 里；这里单独一个文件，
 * 让 dap_main.c 保持「可上游同步」的纯 USB 描述符/端点实现。
 *
 * 接口编号（单串口模式）：
 *   0 = DAP（WinUSB）  1 = CDC1 控制  2 = CDC1 数据
 * 双串口模式会多出 3/4，但本固件不提供双串口（DRV_UART_MODE_COUNT == 1），
 * 所以只处理 1。
 */

#include "at32f402_405.h"

#include <usbd_cdc.h>

#include "drv/drv_uart.h"

/* host 发下来的串口参数不一定是物理串口能跑的，做一个上下限保护 */
#define CDC_BAUDRATE_MIN 1200u
#define CDC_BAUDRATE_MAX 40000000u

static uint32_t clamp_baudrate(uint32_t baudrate) {
    if (baudrate < CDC_BAUDRATE_MIN) {
        return CDC_BAUDRATE_MIN;
    }
    if (baudrate > CDC_BAUDRATE_MAX) {
        return CDC_BAUDRATE_MAX;
    }
    return baudrate;
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding) {
    (void)busid;
    if (line_coding == 0) {
        return;
    }

    /*
     * 本桥只支持 8N1（与固件里 usart_init 的配置一致），其它组合直接按 8N1
     * 处理：AT32 的 usart_init 是按参数配置的，这里不重配，免得运行中改帧格式
     * 导致收发错位。波特率是唯一真正生效的参数。
     */
    if (intf == 1u) {
        drv_uart_set_baudrate(clamp_baudrate(line_coding->dwDTERate));
    } else if (intf == 3u) {
        drv_uart2_set_baudrate(clamp_baudrate(line_coding->dwDTERate));
    }
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding) {
    (void)busid;
    if (line_coding == 0) {
        return;
    }

    line_coding->dwDTERate = (intf == 3u) ? drv_uart2_baudrate() : drv_uart_baudrate();
    line_coding->bCharFormat = 0u; /* 1 stop bit */
    line_coding->bParityType = 0u; /* no parity */
    line_coding->bDataBits = 8u;
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr) {
    (void)busid;
    if (intf == 1u) {
        drv_flow_pins_set_dtr(dtr);
    }
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts) {
    (void)busid;
    if (intf == 1u) {
        drv_flow_pins_set_rts(rts);
    }
}

void usbd_cdc_acm_send_break(uint8_t busid, uint8_t intf) {
    /* 不做 break 发送：目标板的 break 语义由上位机自己用 DTR/RTS 组合实现 */
    (void)busid;
    (void)intf;
}
