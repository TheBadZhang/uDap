#pragma once

/*
 * 状态灯接口（本板为**空实现**）。
 *
 * 参考工程 ch32x035-usb-wchlink-main 用一颗 WS2816C 做状态指示，本工程
 * （uDap2）没有这颗灯，但 src/wchlink 下的代码大量调用 status_led_set_state()
 * 表示会话进展。这里保留完全一致的接口，实现为空操作，
 * 这样 src/wchlink 可以原样复用而不必改动调用点。
 *
 * 注意：本板的 PB12 是 LED_CONNECTED_OUT（CMSIS-DAP 的连通指示），
 * 与参考工程的「目标电源开关」同引脚但含义不同，因此这里**不能**去驱动它。
 */

enum status_led_state {
    STATUS_LED_IDLE,
    STATUS_LED_ACTIVE,
    STATUS_LED_SUCCESS,
    STATUS_LED_ERROR,
};

void status_led_init(void);
void status_led_set_state(enum status_led_state state);
