#pragma once

#include <stdbool.h>

/*
 * 目标供电开关接口（本板为**空实现**）。
 *
 * 参考工程用 PB12 驱动目标 Type-C 的 VBUS 负载开关；本工程没有这个开关，
 * 而且 PB12 在本板是 LED_CONNECTED_OUT —— 同一个引脚、完全不同的含义。
 * 所以这里必须空实现：否则 WCH-Link 的 3.3V/5V 电源命令会去驱动本板 LED，
 * 既不会真正给目标供电，又会破坏 CMSIS-DAP 的连通指示。
 *
 * WCH-LinkUtility / MRS 会对目标下发 3.3V 电源命令；在无开关的硬件上
 * 该命令会「成功但不生效」，目标供电完全依赖板子自身的接线。
 */

void drv_power_switch_init(void);
void drv_power_switch_set_enabled(bool enabled);
