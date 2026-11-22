/**
 * @file    boot_main.c
 * @brief   bootloader 主流程：时钟 -> LED 自检 -> 判断进升级模式还是跳应用 -> USB 升级
 *
 * 上电/复位后的决策顺序（docs/bootloader-protocol.md 第 2 节）：
 *   1. 读 .noinit 里的 g_udap_boot_magic：等于 0x55444642（'UDFB'）就清掉、
 *      留在升级模式（应用通过 vendor command 写 magic 后软复位进来的路径）；
 *   2. 否则检查 0x08004000 处的初始 MSP 与 Reset 向量是否合法，合法就跳应用；
 *   3. 都不满足就留在升级模式等上位机刷固件（新板/擦除后）。
 *
 * bootloader 只驱动两路 LED（PB2/PB4，低电平点亮），其余外部引脚一律不碰。
 */

#include "boot_main.h"

#include "at32f402_405.h"
#include "at32f405_clock.h"
#include "bsp_delay.h"

#include "boot_config.h"
#include "boot_flash.h"
#include "boot_usb.h"

/* LED 引脚：与应用侧 board_pin_map.h 一致，但 bootloader 自包含、不 include 应用目录 */
#define BOOT_LED_PORT GPIOB
#define BOOT_LED_PINS (GPIO_PINS_2 | GPIO_PINS_4)

/* 发完应答到真正跳应用之间的等待，让 USB IN 传输有机会真正发出去 */
#define BOOT_JUMP_DELAY_MS 300u

/* 延时动作的轮询粒度（主循环里每轮延时这么多） */
#define BOOT_SERVICE_TICK_MS 2u

/* ---------------------------------------------------------------- 魔术字 */

/*
 * 跨软复位传递「请进升级模式」的标记，放在 .noinit（复位不清零）。
 * 应用侧 application/drv/drv_control.c 里的定义必须与本行完全一致
 * （名字、类型、段名），两个固件的链接脚本把 .noinit 固定在同一 RAM 地址
 * 0x20010000，bootloader 与应用看到的是同一个 4 字节。
 */
__attribute__((section(".noinit"), used)) volatile uint32_t g_udap_boot_magic;

/* ---------------------------------------------------------------- 延时 */

void boot_delay_ms(uint32_t ms) {
    bsp_delay_ms(ms);
}

/* ------------------------------------------------------------------ LED */

static void boot_leds_write(bool on) {
    if (on) {
        /* 低电平点亮 */
        gpio_bits_reset(BOOT_LED_PORT, BOOT_LED_PINS);
    } else {
        gpio_bits_set(BOOT_LED_PORT, BOOT_LED_PINS);
    }
}

static void boot_led_init(void) {
    gpio_init_type gpio_init_struct;

    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);

    /* 先把输出锁存器写成「灭」，再切成输出，避免上电瞬间灯闪一下 */
    boot_leds_write(false);

    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = BOOT_LED_PINS;
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(BOOT_LED_PORT, &gpio_init_struct);
}

/* 上电自检：两个灯一起快闪两下，然后保持灭（不常亮，省电） */
static void boot_led_self_test(void) {
    for (uint32_t i = 0u; i < BOOT_LED_BLINK_COUNT; i++) {
        boot_leds_write(true);
        boot_delay_ms(BOOT_LED_BLINK_MS);
        boot_leds_write(false);
        boot_delay_ms(BOOT_LED_BLINK_MS);
    }
    boot_leds_write(false);
}

/* ------------------------------------------------------------ 跳应用 */

void boot_jump_to_app(void) {
    uint32_t app_msp = *(const volatile uint32_t *)BOOT_APP_BASE;
    uint32_t app_reset = *(const volatile uint32_t *)(BOOT_APP_BASE + 4u);

    /* 跳之前清 magic：应用下次软复位不会再被当成「请求进升级模式」 */
    g_udap_boot_magic = 0u;

    /* 断掉 bootloader 自己的一切：中断、SysTick、向量表 */
    __disable_irq();
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL = 0u;
    SCB->VTOR = BOOT_APP_BASE;

    /* 切到应用的栈，恢复中断使能（__disable_irq 之后 PRIMASK = 1，
     * 不恢复的话应用一开 USB 中断就会进 HardFault） */
    __set_MSP(app_msp);
    __enable_irq();

    ((void (*)(void))app_reset)();

    /* 理论上不会到这里；万一 Reset 向量是死循环也保持可预期行为 */
    for (;;) {
    }
}

/* --------------------------------------------------------- 延时动作调度 */

#define BOOT_ACTION_NONE   0u
#define BOOT_ACTION_JUMP   1u
#define BOOT_ACTION_REBOOT 2u

static volatile uint8_t s_action = BOOT_ACTION_NONE;
static volatile uint32_t s_action_remaining_ms;

void boot_request_jump_to_app(void) {
    s_action_remaining_ms = BOOT_JUMP_DELAY_MS;
    s_action = BOOT_ACTION_JUMP;
}

void boot_request_reboot(void) {
    s_action_remaining_ms = BOOT_REBOOT_DELAY_MS;
    s_action = BOOT_ACTION_REBOOT;
}

void boot_service(void) {
    uint8_t action = s_action;

    if (action == BOOT_ACTION_NONE) {
        return;
    }

    if (s_action_remaining_ms > BOOT_SERVICE_TICK_MS) {
        s_action_remaining_ms -= BOOT_SERVICE_TICK_MS;
        return;
    }
    s_action = BOOT_ACTION_NONE;

    if (action == BOOT_ACTION_JUMP) {
        boot_jump_to_app();
    }
    NVIC_SystemReset();
}

/* ----------------------------------------------------------------- main */

int main(void) {
    bool force_upgrade;

    /* 216 MHz / USB 48 MHz（复用 SDK 的 AT32 Clock Configuration，12 MHz HEXT） */
    system_clock_config();
    bsp_delay_init(system_core_clock);

    boot_led_init();
    boot_led_self_test();

    /* 应用通过 vendor command 写 magic 再软复位 -> 这里读到就停在升级模式 */
    force_upgrade = (g_udap_boot_magic == BOOT_MAGIC_ENTER);
    g_udap_boot_magic = 0u; /* bootloader 负责清 magic，应用不清 */

    if (!force_upgrade && boot_app_vector_valid()) {
        boot_jump_to_app();
    }

    /* 升级模式：USB（接口 0 = DFU，接口 1 = WinUSB 私有协议） */
    boot_flash_init();
    boot_usb_init();

    for (;;) {
        boot_usb_poll();
        boot_service();
        boot_delay_ms(BOOT_SERVICE_TICK_MS);
    }
}
