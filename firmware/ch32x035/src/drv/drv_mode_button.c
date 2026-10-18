#include "drv/drv_mode_button.h"

#include "bsp/bsp_delay.h"

static bool s_raw;
static bool s_stable;
static uint32_t s_changed_at_ms;

static bool mode_button_raw_pressed(void) {
#if DRV_MODE_BUTTON_ACTIVE_LOW
    return (DRV_MODE_BUTTON_PORT->INDR & DRV_MODE_BUTTON_PIN) == 0u;
#else
    return (DRV_MODE_BUTTON_PORT->INDR & DRV_MODE_BUTTON_PIN) != 0u;
#endif
}

void drv_mode_button_init(void) {
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(DRV_MODE_BUTTON_CLK, ENABLE);

    gpio.GPIO_Pin = DRV_MODE_BUTTON_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(DRV_MODE_BUTTON_PORT, &gpio);

    // 初始就采一次真实电平：否则上电时 s_stable 的默认值会在第一次轮询里
    // 被当成"刚变化"，白白等一个去抖窗口
    s_raw = mode_button_raw_pressed();
    s_stable = s_raw;
    s_changed_at_ms = bsp_time_ms();
}

bool drv_mode_button_pressed(void) {
    bool raw = mode_button_raw_pressed();
    uint32_t now_ms = bsp_time_ms();

    if (raw != s_raw) {
        // 电平刚变化，重新开始计时（本毫秒数回绕是安全的：无符号相减）
        s_raw = raw;
        s_changed_at_ms = now_ms;
        return s_stable;
    }

    if (raw != s_stable && (now_ms - s_changed_at_ms) >= DRV_MODE_BUTTON_DEBOUNCE_MS) {
        s_stable = raw;
    }
    return s_stable;
}

void drv_mode_button_wait_release(void) {
    while (drv_mode_button_pressed()) {
    }
}
