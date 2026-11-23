/**
 * @file    main.c
 * @brief   application2：USB Audio Class 2.0 speaker -> I2S1 -> PCM5102
 *
 * 电脑上会出现一个名为 "uDAP UAC2 PCM5102" 的扬声器；主机通过 USB ISO OUT
 * 把 PCM 送到 AT32F405，固件再以 I2S 主机方式从 PA4/PA5/PA7 输出给 PCM5102：
 *
 *   PA4 -> LCK (I2S1_WS)
 *   PA5 -> BCK (I2S1_CK)
 *   PA6 -> DIN (板上与 PA7 短接，实际由 PA7 的 I2S1_SD 驱动)
 *
 * 启动顺序：
 *   1. system_clock_config()：216 MHz，PLLU=48 MHz，OTGHS PHY 用 12 MHz HEXT
 *   2. 时基 + LED
 *   3. audio_i2s_init()：PA4/PA5/PA7 复用、SPI1/I2S1、DMA1
 *   4. bsp_usb_clock_init() + audio_usb_init()：注册 UAC2 描述符并启动 OTGHS
 *   5. 主循环只做状态灯，音频由 USB/DMA 中断驱动
 */

#include "at32f402_405.h"

#include "board_pin_map.h"

#include "bsp/bsp_time.h"
#include "bsp/bsp_usb.h"
#include "drv/drv_led.h"

#include "audio_eq.h"
#include "audio_i2s.h"
#include "audio_usb.h"

extern void system_clock_config(void);
extern void bsp_delay_init(uint32_t hclk);

int main(void) {
    system_clock_config();
    bsp_delay_init(system_core_clock);
    bsp_time_init();

    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

    drv_led_init();
    drv_led_force(true, false); /* LED1 常亮：固件已启动 */

    audio_eq_init();
    audio_i2s_init();

    bsp_usb_clock_init();
    audio_usb_init(0u, (uint32_t)BOARD_USB_BASE);

    bool streaming = false;
    for (;;) {
        bool now = audio_i2s_is_streaming();
        if (now != streaming) {
            /* LED2：USB 主机正在播放 */
            drv_led_force(true, now);
            streaming = now;
        }
        __WFI();
    }
}
