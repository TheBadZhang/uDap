#include "drv/drv_mode_button.h"

#include "bsp/bsp_delay.h"

static bool s_raw;
static bool s_stable;
static bool s_prev_stable;
static uint32_t s_changed_at_ms;

// 双击检测：记一次「已按下过」，在窗口内再按下一次即判为双击
static bool s_click_pending;
static uint32_t s_click_at_ms;
static bool s_double_click;

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
    s_prev_stable = s_stable;
    s_changed_at_ms = bsp_time_ms();

    s_click_pending = false;
    s_click_at_ms = s_changed_at_ms;
    s_double_click = false;
}

bool drv_mode_button_pressed(void) {
    bool raw = mode_button_raw_pressed();
    uint32_t now_ms = bsp_time_ms();

    if (raw != s_raw) {
        // 电平刚变化，重新开始计时（毫秒数回绕是安全的：无符号相减）
        s_raw = raw;
        s_changed_at_ms = now_ms;
    } else if (raw != s_stable && (now_ms - s_changed_at_ms) >= DRV_MODE_BUTTON_DEBOUNCE_MS) {
        s_stable = raw;
    }

    // 双击判定：两次「稳定按下沿」的间隔不超过 DRV_MODE_BUTTON_DOUBLE_MS。
    // 用按下沿而不是释放沿，这样手上按得久一点（比如 200 ms）也不会破坏双击。
    if (s_stable != s_prev_stable) {
        s_prev_stable = s_stable;
        if (s_stable) {
            if (s_click_pending) {
                s_double_click = true;
                s_click_pending = false;
            } else {
                s_click_pending = true;
                s_click_at_ms = now_ms;
            }
        }
    }

    // 等不到第二次按下就放弃这一轮，避免「单击 -> 很久以后再单击」被凑成双击
    if (s_click_pending && (now_ms - s_click_at_ms) > DRV_MODE_BUTTON_DOUBLE_MS) {
        s_click_pending = false;
    }

    return s_stable;
}

bool drv_mode_button_take_double_click(void) {
    bool click = s_double_click;

    s_double_click = false;
    return click;
}
