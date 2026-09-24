#include "bsp/bsp_delay.h"
#include "bsp/bsp_system.h"

#include <ch32x035.h>

// CMSIS-DAP 是可选功能，由 xmake.lua 的 dap 选项控制：
//   xmake f --dap=n   # 关闭，只保留 USB 骨架
// 未定义时默认启用，便于 MRS 等不传该宏的构建方式。
#ifndef ENABLE_DAP
#define ENABLE_DAP 1
#endif

#if ENABLE_DAP
#include "dap_main.h"
#include "drv/drv_uart.h"

static void __attribute__((section(".highcode"), noinline, optimize("O2"))) cherrydap_process(void) {

    for (;;) {
        chry_dap_handle();
        chry_dap_cdc_bridge();
    }
}
#endif

int main(void) {
    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    bsp_delay_ms(10u);

#if ENABLE_DAP
    // COM 口 <-> USART4 (PB0=TX, PB1=RX)，默认 115200；
    // 主机改 CDC 行编码时波特率会跟着变
    drv_uart_init(0u);

    chry_dap_init(0u, 0u);

    cherrydap_process();
#else
    for (;;) {
        bsp_delay_ms(1000u);
    }
#endif
}
