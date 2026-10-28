/*
 * CH32X035F8U USBFS 硬件初始化（必选）。
 *
 * CherryUSB 的端口层把 usb_dc_low_level_init()/usb_dc_low_level_deinit() 声明为
 * __WEAK 空实现，这里给出本芯片的实际实现。
 *
 * 寄存器依据 WCH 官方例程（sdk/EXAM/USB/USBFS/DEVICE/SimulateCDC/User/
 * ch32x035_usbfs_device.c 的 GPIO_USB_INIT + USBFS_Device_Init）：
 *
 *   1. AFIO / GPIOC 时钟、USBFS 时钟；
 *   2. PC16 = UDM（浮空输入）、PC17 = UDP（上拉输入）；
 *   3. AFIO->CTLR 配置 PHY：USB_IOEN 使能 USB 复用引脚，UDP 内部上拉的阻值
 *      与 PHY 是否使能 V33 稳压器必须跟随实际 VDD：
 *        5V   供电：关闭 PHY_V33，UDP 用 10K 上拉
 *        3.3V 供电：使能 PHY_V33，UDP 用 1.5K 上拉（USB 2.0 规范要求的全速上拉）
 *      参考工程 ch32x035-usb-dap-main 直接按 3.3V 写死（PHY_V33 + 1.5K），
 *      这里改为读 PWR_VDD_SupplyVoltage() 自动选择，两种供电都正确。
 *   4. 使能 USBFS 中断。
 */

#include <ch32x035.h>
#include <ch32x035_usb.h>

#include "bsp/bsp_usb.h"

/* CH32X035 的 USBFS D-/D+ 固定复用为 PC16(UDM) / PC17(UDP) */
#define BSP_USB_DM_PIN GPIO_Pin_16
#define BSP_USB_DP_PIN GPIO_Pin_17

void usb_dc_low_level_init(void) {
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBFS, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Pin = BSP_USB_DM_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &gpio);
    gpio.GPIO_Pin = BSP_USB_DP_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOC, &gpio);

    if (PWR_VDD_SupplyVoltage() == PWR_VDD_5V) {
        /* 5V：不使能 PHY_V33，UDP 内部上拉 10K */
        AFIO->CTLR = (AFIO->CTLR & ~(AFIO_CTLR_UDP_PUE | AFIO_CTLR_UDM_PUE | AFIO_CTLR_USB_PHY_V33)) |
                     AFIO_CTLR_UDP_PUE_1 | AFIO_CTLR_USB_IOEN;
    } else {
        /* 3.3V：使能 PHY_V33，UDP 内部上拉 1.5K */
        AFIO->CTLR = (AFIO->CTLR & ~(AFIO_CTLR_UDP_PUE | AFIO_CTLR_UDM_PUE)) |
                     AFIO_CTLR_USB_PHY_V33 | AFIO_CTLR_UDP_PUE | AFIO_CTLR_USB_IOEN;
    }

    NVIC_InitTypeDef nvic = {0};
    nvic.NVIC_IRQChannel = USBFS_IRQn;
    /* 优先级与 drv_uart.c 的约定保持一致：USB = 1，USART4 = 2，
     * 保证 UART 中断不会打断 USB 传输。
     * 本芯片 startup 写入 CSR(0x804) = 0x3（中断嵌套使能），与 SDK V1.0.3
     * misc.h 的默认值 INTSYSCR_INEST_EN 一致，因此 4 个 bit 全是抢占优先级。 */
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

void usb_dc_low_level_deinit(void) {
    /* 断开 UDP 上拉并释放 USB 复用引脚，主机侧看到设备拔出 */
    AFIO->CTLR &= ~(AFIO_CTLR_UDP_PUE | AFIO_CTLR_UDM_PUE | AFIO_CTLR_USB_IOEN);

    NVIC_InitTypeDef nvic = {0};
    nvic.NVIC_IRQChannel = USBFS_IRQn;
    nvic.NVIC_IRQChannelCmd = DISABLE;
    NVIC_Init(&nvic);
}
