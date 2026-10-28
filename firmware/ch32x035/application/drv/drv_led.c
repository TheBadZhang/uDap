#include "drv/drv_led.h"

#include <ch32x035.h>

#include "board_pin_map.h"
#include "bsp/bsp_delay.h"

/* 一次收发脉冲把灯点亮多久（ms）。大包传输会连续 pulse，看起来就是常亮。 */
#define DRV_LED_ACTIVITY_MS 30u

/* 状态尽量用最小的类型：双模式固件的 RAM 只剩几十字节余量 */
static uint8_t s_mode = DRV_LED_MODE_STATUS;
static bool s_dap_connected;
static bool s_uart_dual;

/* pulse 只置标志（可能在 USB 中断上下文里），时间戳在 service() 里更新 */
static volatile uint8_t s_pulse[2];
static uint32_t s_activity_until[2];

/* 上一次写进引脚的电平（-1 = 未知，强制下一次重写），避免每轮都写寄存器 */
static int8_t s_on[2] = {-1, -1};

static void led_write_pin(uint8_t index, bool on) {
    uint32_t pin;

    if (index >= 2u) {
        return;
    }
    pin = (index == 0u) ? BOARD_LED_DAP_PIN : BOARD_LED_UART_PIN;

    if (s_on[index] == (int8_t)on) {
        return;
    }
    s_on[index] = (int8_t)on;

#if BOARD_LED_ACTIVE_LOW
    if (on) {
        BOARD_LED_PORT->BCR = pin;
    } else {
        BOARD_LED_PORT->BSHR = pin;
    }
#else
    if (on) {
        BOARD_LED_PORT->BSHR = pin;
    } else {
        BOARD_LED_PORT->BCR = pin;
    }
#endif
}

void drv_led_init(void) {
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(BOARD_LED_PORT_CLK, ENABLE);

    // 先把输出寄存器摆成「灭」再切成推挽，避免初始化瞬间闪一下
#if BOARD_LED_ACTIVE_LOW
    BOARD_LED_PORT->BSHR = BOARD_LED_DAP_PIN | BOARD_LED_UART_PIN;
#else
    BOARD_LED_PORT->BCR = BOARD_LED_DAP_PIN | BOARD_LED_UART_PIN;
#endif

    gpio.GPIO_Pin = BOARD_LED_DAP_PIN | BOARD_LED_UART_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(BOARD_LED_PORT, &gpio);

    s_on[0] = -1;
    s_on[1] = -1;
    s_pulse[0] = 0u;
    s_pulse[1] = 0u;
    s_activity_until[0] = 0u;
    s_activity_until[1] = 0u;
}

void drv_led_set_mode(drv_led_mode_t mode) {
    if ((uint32_t)mode >= (uint32_t)DRV_LED_MODE_COUNT) {
        return;
    }
    s_mode = (uint8_t)mode;

    // 下一轮 service() 会按新模式重算；把缓存标成未知，保证切换立刻落到引脚上
    s_on[0] = -1;
    s_on[1] = -1;
    drv_led_service();
}

drv_led_mode_t drv_led_mode(void) {
    return (drv_led_mode_t)s_mode;
}

void drv_led_set_dap_connected(bool connected) {
    s_dap_connected = connected;
}

void drv_led_set_uart_dual(bool dual) {
    s_uart_dual = dual;
}

// noinline：这两个函数从 .highcode（RAM 里执行的 DAP 热路径）被调用，
// 让 LTO 内联进去会白白多吃 RAM（双模式固件的 RAM 只剩几十字节余量）。
void drv_led_pulse_uart(uint8_t uart_index) __attribute__((noinline));
void drv_led_pulse_uart(uint8_t uart_index) {
    if (uart_index >= 1u && uart_index <= 2u) {
        s_pulse[uart_index - 1u] = 1u;
    }
}

void drv_led_service(void) __attribute__((noinline));
void drv_led_service(void) {
    bool activity[2] = {false, false};
    bool led[2];
    bool need_time;
    uint32_t i;

    // 只有「活动指示」相关的模式才需要时间戳。默认（STATUS）与 OFF 模式完全
    // 不碰 bsp_time_ms() —— 那里有一次 64 位除法，主循环每轮都做不划算，
    // 而且 WCH-Link 模式的循环对 RVSWD 的时序更敏感。
    need_time = (s_mode == DRV_LED_MODE_ACTIVITY) || (s_mode == DRV_LED_MODE_MIXED) ||
                (s_pulse[0] != 0u) || (s_pulse[1] != 0u);

    if (need_time) {
        uint32_t now = bsp_time_ms();

        for (i = 0u; i < 2u; ++i) {
            if (s_pulse[i] != 0u) {
                s_pulse[i] = 0u;
                s_activity_until[i] = now + DRV_LED_ACTIVITY_MS;
            }
            // 无符号相减：截止时间回绕也不会误判成「一直亮」
            activity[i] = (int32_t)(s_activity_until[i] - now) > 0;
        }
    }

    switch (s_mode) {
    case DRV_LED_MODE_ACTIVITY:
        led[0] = activity[0];
        led[1] = activity[1];
        break;
    case DRV_LED_MODE_MIXED:
        led[0] = s_dap_connected;
        led[1] = activity[0];
        break;
    case DRV_LED_MODE_OFF:
        led[0] = false;
        led[1] = false;
        break;
    case DRV_LED_MODE_STATUS:
    default:
        led[0] = s_dap_connected;
        led[1] = s_uart_dual;
        break;
    }

    led_write_pin(0u, led[0]);
    led_write_pin(1u, led[1]);
}
