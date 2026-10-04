#include "status/status_led.h"

// 本板没有状态灯，接口保留为空操作（见 status_led.h 的说明）。
void status_led_init(void) {
}

void status_led_set_state(enum status_led_state state) {
    (void)state;
}
