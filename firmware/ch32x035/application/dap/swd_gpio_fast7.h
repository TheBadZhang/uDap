#pragma once

#include <stdint.h>

#include "swd_gpio.h"

/*
 * SWD 物理层的**另一种实现**：每比特 7 个 SWCLK（现役实现是 8 个）。
 *
 * 与 swd_gpio_x035.c 的区别只在位时序，接口相同；符号带 `swd7_` 前缀，
 * 所以两套实现可以同时编进固件，用哪套由调用点决定
 * （xmake 的 option("swd7") -> SWD7_ENABLE，见 sw_dp.c）。
 *
 * 为什么不是 6 —— 见 swd_gpio_x035_fast7.c 文件头的槽位预算推导。
 */

#ifndef SWD7_ENABLE
#define SWD7_ENABLE 0
#endif

/* 复用 SWD_GPIO_IMPL 的「加引脚方案后缀」规则，把 base 名换成 swd7_* 即可，
 * 例如 SWD_GPIO_IMPL(swd7_fast_write_data) -> swd7_fast_write_data_pa5_pa6 */
void SWD_GPIO_IMPL(swd7_fast_packet_request)(uint32_t request);
void SWD_GPIO_IMPL(swd7_fast_read_data)(uint32_t *value, uint32_t *parity);
void SWD_GPIO_IMPL(swd7_fast_write_data)(uint32_t value);

/* 端口生命周期与现役实现完全相同（属于引脚表那一层），直接复用 */
#define swd7_gpio_port_setup()        swd_gpio_port_setup()
#define swd7_gpio_port_off()          swd_gpio_port_off()
#define swd7_gpio_packet_request(req) SWD_GPIO_IMPL(swd7_fast_packet_request)((req))
#define swd7_gpio_read_data(v, p)     SWD_GPIO_IMPL(swd7_fast_read_data)((v), (p))
#define swd7_gpio_write_data(val)     SWD_GPIO_IMPL(swd7_fast_write_data)((val))
