#include "drv/drv_power_switch.h"

// 本板没有目标供电开关，接口保留为空操作（见 drv_power_switch.h 的说明）。
void drv_power_switch_init(void) {
}

void drv_power_switch_set_enabled(bool enabled) {
    (void)enabled;
}
