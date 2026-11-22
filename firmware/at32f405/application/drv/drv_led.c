#include "drv/drv_led.h"

#include "at32f402_405.h"

#include "board_pin_map.h"
#include "bsp/bsp_time.h"

/* 串口活动指示的亮灯时长（毫秒） */
#define LED_PULSE_MS 40u

static drv_led_mode_t s_mode = DRV_LED_MODE_STATUS;
static bool s_dap_connected;
static bool s_uart_dual;
static uint32_t s_pulse_until[2]; /* [0]=COM1, [1]=COM2 */
static bool s_led1_on;
static bool s_led2_on;

/* 低电平点亮：on=1 -> 输出低 */
static void led_write(gpio_type *port, uint16_t pin, bool on) {
    if (on) {
        gpio_bits_reset(port, pin);
    } else {
        gpio_bits_set(port, pin);
    }
}

static void led_apply(bool led1_on, bool led2_on) {
    if (led1_on == s_led1_on && led2_on == s_led2_on) {
        return;
    }
    s_led1_on = led1_on;
    s_led2_on = led2_on;
    led_write(BOARD_LED1_PORT, BOARD_LED1_PIN, led1_on);
    led_write(BOARD_LED2_PORT, BOARD_LED2_PIN, led2_on);
}

void drv_led_init(void) {
    gpio_init_type gpio_init_struct;

    crm_periph_clock_enable(BOARD_LED_GPIO_CLK, TRUE);

    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = BOARD_LED1_PIN | BOARD_LED2_PIN;
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;

    /* 先把输出锁存器写成“灭”，再切成输出，避免上电瞬间灯闪一下 */
    led_write(BOARD_LED1_PORT, BOARD_LED1_PIN, false);
    led_write(BOARD_LED2_PORT, BOARD_LED2_PIN, false);
    gpio_init(BOARD_LED1_PORT, &gpio_init_struct);

    s_led1_on = false;
    s_led2_on = false;
    s_mode = DRV_LED_MODE_STATUS;
    s_dap_connected = false;
    s_uart_dual = false;
    s_pulse_until[0] = 0u;
    s_pulse_until[1] = 0u;
}

void drv_led_set_mode(drv_led_mode_t mode) {
    if ((uint32_t)mode >= (uint32_t)DRV_LED_MODE_COUNT) {
        return;
    }
    s_mode = mode;
    drv_led_service();
}

drv_led_mode_t drv_led_mode(void) {
    return s_mode;
}

void drv_led_set_dap_connected(bool connected) {
    s_dap_connected = connected;
}

void drv_led_set_uart_dual(bool dual) {
    s_uart_dual = dual;
}

void drv_led_pulse_uart(uint8_t uart_index) {
    if (uart_index == 0u || uart_index > 2u) {
        return;
    }
    s_pulse_until[uart_index - 1u] = bsp_time_ms() + LED_PULSE_MS;
}

void drv_led_force(bool led1_on, bool led2_on) {
    led_apply(led1_on, led2_on);
}

void drv_led_service(void) {
    const uint32_t now = bsp_time_ms();
    bool active1 = !bsp_time_reached(now, s_pulse_until[0]);
    bool active2 = !bsp_time_reached(now, s_pulse_until[1]);

    switch (s_mode) {
    case DRV_LED_MODE_STATUS:
        led_apply(s_dap_connected, s_uart_dual);
        break;
    case DRV_LED_MODE_ACTIVITY:
        led_apply(active1, active2);
        break;
    case DRV_LED_MODE_MIXED:
        led_apply(s_dap_connected, active1);
        break;
    case DRV_LED_MODE_OFF:
    default:
        led_apply(false, false);
        break;
    }
}
