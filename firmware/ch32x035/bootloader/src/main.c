/*
 * uDap UF2 bootloader。
 *
 * ============================ 启动判定 ============================
 *
 *  1. RAM 里有「应用请求进 UF2」的握手记录（应用发 vendor command 后软复位时
 *     写的，见 src/boot_info.h）     -> 留在 UF2
 *  2. 上电时**一直按住**板上按键超过 1.2 s -> 留在 UF2（「按住按键上电」的用法）
 *  3. 应用区无效（空片 / 中途断开的升级）-> 留在 UF2，至少还能当 U 盘用
 *  4. 其余                             -> 立刻跳应用
 *
 * 注意这里**没有**「短按」这种手势：调试器模式（CMSIS-DAP <-> WCH-Link）改由
 * 运行期双击按键切换，和串口模式复用同一个手势（双模式固件里串口固定单串口，
 * 见应用侧的 src/main.c）。bootloader 只区分「按住到超时」与「没按/提前松开」。
 *
 * ============================ 升级流程 ============================
 *
 * 留在 UF2 后把 USB 初始化成 MSC 设备，主机出现一个 U 盘；把 firmware.uf2
 * 拖进去，扇区写回调会解析 UF2 块并逐页落 Flash（见 src/boot_uf2.c）。
 *
 * 断点安全：开始一轮拷贝时先擦掉应用入口页，整份文件都落盘后才最后写它。
 * 所以任何中途断开都不会跳进写了一半的固件 —— 下次上电还会回到 bootloader。
 *
 * 全部写完 -> 常亮 300 ms -> 软复位 -> 重新走一遍启动判定（这次应用有效，
 * 于是进应用）。
 *
 * ============================ LED ============================
 *
 *   两颗灯同时 2 Hz 慢闪  —— 已进入 UF2，等主机拖文件
 *   常亮               —— 正在写 Flash
 *   快闪 N 次 + 长停    —— 出错，N = 错误码（见 boot_uf2.h）：
 *                          2 = Flash 擦写/校验失败
 *                          3 = 同一页被两个不同的块覆盖（畸形的 UF2）
 *                          4 = 最后提交入口页失败
 *                        出错后主机安静 2 s 会自动复位会话，可以直接再拖一次。
 */

#include <stdbool.h>
#include <stdint.h>

#include <ch32x035.h>

#include "board_pin_map.h"

#include "boot_config.h"
#include "boot_flash.h"
#include "boot_info.h"
#include "boot_time.h"
#include "boot_uf2.h"
#include "boot_usb.h"

#include "bsp/bsp_delay.h"

#define SYSTICK_CTLR_REG (*(volatile uint32_t *)0xE000F000UL)

/* ---------------------------------------------------------------------------
 * LED：引脚见 application/board_pin_map.h 的 BOARD_LED_*，低电平点亮
 * ------------------------------------------------------------------------- */

static void led_gpio_init(void) {
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(BOARD_LED_PORT_CLK, ENABLE);

#if BOARD_LED_ACTIVE_LOW
    BOARD_LED_PORT->BSHR = BOARD_LED_DAP_PIN | BOARD_LED_UART_PIN;
#else
    BOARD_LED_PORT->BCR = BOARD_LED_DAP_PIN | BOARD_LED_UART_PIN;
#endif

    gpio.GPIO_Pin = BOARD_LED_DAP_PIN | BOARD_LED_UART_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(BOARD_LED_PORT, &gpio);
}

static void led_write(bool left_on, bool right_on) {
#if BOARD_LED_ACTIVE_LOW
    BOARD_LED_PORT->BCR = (left_on ? BOARD_LED_DAP_PIN : 0u) | (right_on ? BOARD_LED_UART_PIN : 0u);
    BOARD_LED_PORT->BSHR = (left_on ? 0u : BOARD_LED_DAP_PIN) | (right_on ? 0u : BOARD_LED_UART_PIN);
#else
    BOARD_LED_PORT->BSHR = (left_on ? BOARD_LED_DAP_PIN : 0u) | (right_on ? BOARD_LED_UART_PIN : 0u);
    BOARD_LED_PORT->BCR = (left_on ? 0u : BOARD_LED_DAP_PIN) | (right_on ? 0u : BOARD_LED_UART_PIN);
#endif
}

/* 错误提示：快闪 code 次（80 ms）+ 长停 800 ms，循环。
 * 非阻塞 —— USB 必须继续被服务，否则主机看不到任何后续动作。 */
static void led_error_service(uint8_t code) {
    static uint32_t last_toggle;
    static uint8_t step;
    static uint8_t last_code;

    if (code != last_code) {
        last_code = code;
        step = 0u;
        last_toggle = BOOT_SYSTICK_CNTL;
        led_write(false, false);
        return;
    }

    {
        uint32_t interval = (step < (uint8_t)(code * 2u)) ? 80u : 800u;

        if (boot_ticks_since(last_toggle) < boot_ticks_from_ms(interval)) {
            return;
        }
    }

    last_toggle = BOOT_SYSTICK_CNTL;
    step++;

    if (step > (uint8_t)(code * 2u)) {
        step = 0u;
        led_write(false, false);
        return;
    }
    led_write((step & 1u) != 0u, (step & 1u) != 0u);
}

/* ---------------------------------------------------------------------------
 * 按键：引脚见 board_pin_map.h 的 BOARD_BTN_*，内部上拉，低电平 = 按下
 * ------------------------------------------------------------------------- */

static void button_gpio_init(void) {
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(BOARD_BTN_PORT_CLK, ENABLE);

    gpio.GPIO_Pin = BOARD_BTN_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(BOARD_BTN_PORT, &gpio);
}

static bool button_pressed(void) {
#if BOARD_BTN_ACTIVE_LOW
    return (BOARD_BTN_PORT->INDR & BOARD_BTN_PIN) == 0u;
#else
    return (BOARD_BTN_PORT->INDR & BOARD_BTN_PIN) != 0u;
#endif
}

/* 上电时按住并**一直保持**到超时才算「要进 bootloader」；提前松开就是普通上电。
 * 前 20 ms 的连续采样是去抖：滤掉接触抖动与电源爬升。 */
static bool button_held_at_boot(void) {
    uint32_t elapsed;

    for (elapsed = 0u; elapsed < 20u; elapsed += 5u) {
        bsp_delay_ms(5u);
        if (!button_pressed()) {
            return false;
        }
    }

    // 已确认按住：等到松开或者超时。窗口内每 50 ms 闪一次灯，告诉用户
    // 「再按住一会儿就进 bootloader」。
    for (elapsed = 0u; elapsed < BOOT_BUTTON_HOLD_MS; elapsed += 10u) {
        if (!button_pressed()) {
            return false;
        }
        led_write(((elapsed / 50u) & 1u) != 0u, ((elapsed / 50u) & 1u) == 0u);
        bsp_delay_ms(10u);
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * 跳转到应用
 * ------------------------------------------------------------------------- */

typedef void (*boot_entry_t)(void);

static void jump_to_app(void) {
    boot_entry_t entry = (boot_entry_t)BOOT_APP_START;

    // 走这条路径时 USB 从未初始化（升级完只会软复位，不会跳转），所以只需要把
    // 中断和 SysTick 停干净：应用的启动代码会重新配置 gp/sp/mtvec、清 bss、
    // 调 SystemInit()。
    //
    // ⚠️ 本函数**必须在机器模式**下调用（见 sdk/Startup/*.S 的
    // START_KEEP_MACHINE_MODE）：应用入口会重跑 `csrw 0xbc0`，那是机器模式专用
    // CSR（0xB00~0xBFF）。如果 bootloader 已经按 WCH 默认掉进用户模式，跳过去
    // 的第一条 CSR 写就是非法指令 —— 实测 mcause=2 @handle_reset+0x80，表现为
    // 「应用一进去就复位」的死循环。
    SYSTICK_CTLR_REG = 0u;
    __disable_irq();

    entry();

    for (;;) {
    }
}

/* ---------------------------------------------------------------------------
 * UF2 主循环
 * ------------------------------------------------------------------------- */

static void uf2_run(void) {
    uint32_t blink_start = BOOT_SYSTICK_CNTL;
    bool blink_on = false;
    uint8_t error;

    boot_flash_init();
    boot_uf2_init();
    boot_usb_init();

    for (;;) {
        error = boot_uf2_error();
        boot_uf2_service();

        if (error == BOOT_UF2_OK && boot_uf2_is_done()) {
            // 整份固件已落盘（含最后提交的入口页）：常亮一下再软复位，
            // 重新走一遍启动判定，这样「升级完是否进应用」只有 main() 一处逻辑。
            led_write(true, true);
            bsp_delay_ms(300u);
            NVIC_SystemReset();
        }

        // is_done() 的提交动作可能刚刚置位错误，重新取一次
        error = boot_uf2_error();
        if (error != BOOT_UF2_OK) {
            led_error_service(error);
            blink_start = BOOT_SYSTICK_CNTL;
            blink_on = false;
            continue;
        }

        {
            uint32_t written = boot_flash_last_write_tick();

            if (written != 0xFFFFFFFFu &&
                boot_ticks_since(written) < boot_ticks_from_ms(200u)) {
                led_write(true, true); /* 正在写 Flash：常亮 */
                blink_start = BOOT_SYSTICK_CNTL;
                blink_on = true;
            } else if (boot_ticks_since(blink_start) >= boot_ticks_from_ms(250u)) {
                blink_start = BOOT_SYSTICK_CNTL;
                blink_on = !blink_on;
                led_write(blink_on, blink_on);
            }
        }
    }
}

int main(void) {
    uint32_t reason = BOOT_INFO_REASON_NONE;
    uint32_t arg = 0u;
    bool need_uf2 = false;

    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    led_gpio_init();
    button_gpio_init();

    // 应用通过软复位传过来的记录（读走即清，见 src/boot_info.h）：
    //   REQUEST_UF2 -> 留在 UF2
    //   APP_STAGE / APP_PANIC -> 只清掉记录，不再输出诊断信息
    if (boot_info_take(&reason, &arg) && reason == BOOT_INFO_REASON_REQUEST_UF2) {
        need_uf2 = true;
    }
    (void)arg;

    if (button_held_at_boot()) {
        need_uf2 = true;
    }

    if (need_uf2) {
        uf2_run(); /* 不返回 */
    }

    if (!boot_app_is_valid()) {
        // 空片 / 上一次升级没写完：留在 UF2，至少可以拖固件进去
        uf2_run(); /* 不返回 */
    }

    jump_to_app();
    return 0;
}
