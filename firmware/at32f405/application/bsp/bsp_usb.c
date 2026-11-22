#include "bsp/bsp_usb.h"

#include "at32f402_405.h"

#include "board_pin_map.h"
#include "usbd_core.h"

static volatile int s_clock_ready;

void bsp_usb_clock_init(void) {
    s_clock_ready = 0;

    /* OTGHS 控制器与它自己的 PHY 是两个独立时钟门 */
    crm_periph_clock_enable(CRM_OTGHS_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_OTGHSPHY_PERIPH_CLOCK, TRUE);

    /*
     * PHY 的 12 MHz 参考时钟取 HEXT/1。AT32F405 的 HS PHY 只支持 12 MHz
     * 晶振直连（16 MHz 等其它频率会不稳），本板正好是 12 MHz。
     */
    crm_usb_phy12_clock_select(CRM_USB_PHY12_CLOCK_HEXT_DIV_1);

    /* USB 内核时钟 48 MHz 来自 PLLU：先使能 PLLU 输出并等它稳定 */
    crm_pllu_output_set(TRUE);
    while (crm_flag_get(CRM_PLLU_STABLE_FLAG) != SET) {
    }
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_PLLU);

    /* OTGHS 中断：优先级 0（USB 是实时性最高的外设，不能被打断太久） */
    nvic_irq_enable(BOARD_USB_IRQ, 1u, 0u);

    s_clock_ready = 1;
}

int bsp_usb_clock_ready(void) {
    return s_clock_ready;
}

/* CherryUSB 的 dwc2 端口只提供 USBD_IRQHandler(busid)，向量入口在这里接上 */
void BOARD_USB_IRQHandler(void) {
    USBD_IRQHandler(0);
}

/*
 * CherryUSB 的 port/dwc2 把「平台相关的最后一步」留给 glue：它调用
 * usb_dc_low_level_init()/deinit()，但 AT32 的 glue（usb_glue_at.c）只提供
 * FIFO/PHY 参数与时钟查询，没有实现这两个钩子（其它厂商的 glue 会实现）。
 * 我们的时钟已经在 bsp_usb_clock_init() 里配好，这里只补中断使能/关闭。
 */
void usb_dc_low_level_init(uint8_t busid) {
    (void)busid;
    nvic_irq_enable(BOARD_USB_IRQ, 1u, 0u);
}

void usb_dc_low_level_deinit(uint8_t busid) {
    (void)busid;
    nvic_irq_disable(BOARD_USB_IRQ);
}
