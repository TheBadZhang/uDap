#include "drv/drv_control.h"

#include "at32f402_405.h"

#include "bsp/bsp_time.h"

/* 延迟执行的时间：给 USB 主机留出「收完应答包」的时间（一帧 125us，几帧足够） */
#define CONTROL_DELAY_MS 30u

/*
 * 「跳过 bootloader」/「进 bootloader」的握手标记。
 *
 * 放在 .noinit 段：启动代码不会清它（既不在 .data 也不在 .bss），所以它能
 * 跨「软件跳转」和「软复位」存活；应用和 bootloader 共用同一个地址（两边
 * 的链接脚本都把 .noinit 放在 RAM 末尾附近，见 startup/linker 下的 ld 文件）。
 */
__attribute__((section(".noinit"), used)) volatile uint32_t g_udap_boot_magic;

#define UDAP_BOOT_MAGIC_ENTER 0x55444642u /* 'UDFB'：请求进入 bootloader */

static firmware_mode_t s_runtime_mode = FIRMWARE_MODE_CMSIS_DAP;
static drv_control_action_t s_pending_action = DRV_CONTROL_NONE;
static uint32_t s_pending_arg;
static uint32_t s_pending_deadline;
static bool s_pending;

void drv_control_init(firmware_mode_t runtime_mode) {
    s_runtime_mode = runtime_mode;
    s_pending = false;
    s_pending_action = DRV_CONTROL_NONE;
    /*
     * 这里故意不清 g_udap_boot_magic：
     *   本标记的写-读-清三方都在「bootloader 在 0x08000000、应用在其后」的
     *   启动链里完成 —— 应用写标记 -> 软复位 -> 先跑 bootloader（读并清标记）
     *   -> bootloader 跳回应用。应用启动时 magic 已经被 bootloader 清掉了；
     *   如果这里再清一次，反而会把「应用还没来得及被 bootloader 读到」的窗口
     *   里可能残留的值抹掉，破坏 ENTER_BOOT 语义。
     */
}

firmware_mode_t drv_control_runtime_mode(void) {
    return s_runtime_mode;
}

bool drv_control_request(drv_control_action_t action, uint32_t arg) {
    if (action == DRV_CONTROL_NONE || s_pending) {
        return false;
    }
    s_pending = true;
    s_pending_action = action;
    s_pending_arg = arg;
    s_pending_deadline = bsp_time_ms() + CONTROL_DELAY_MS;
    return true;
}

void drv_control_set_boot_request(void) {
    g_udap_boot_magic = UDAP_BOOT_MAGIC_ENTER;
}

bool drv_control_take_boot_request(void) {
    if (g_udap_boot_magic != UDAP_BOOT_MAGIC_ENTER) {
        return false;
    }
    g_udap_boot_magic = 0u;
    return true;
}

void drv_control_service(void) {
    if (!s_pending || !bsp_time_reached(bsp_time_ms(), s_pending_deadline)) {
        return;
    }

    const drv_control_action_t action = s_pending_action;
    const uint32_t arg = s_pending_arg;
    s_pending = false;
    s_pending_action = DRV_CONTROL_NONE;

    switch (action) {
    case DRV_CONTROL_SOFT_RESET:
        NVIC_SystemReset();
        break;

    case DRV_CONTROL_ENTER_UF2:
        /*
         * 写握手标记后软复位。bootloader 在 0x08000000，应用在它后面；
         * 复位后由 bootloader 读标记决定是「进下载模式」还是「直接跳应用」。
         * 没有编 bootloader 的构建里，复位后仍然回到本应用（无害）。
         */
        drv_control_set_boot_request();
        NVIC_SystemReset();
        break;

    case DRV_CONTROL_APPLY_DEBUGGER:
        (void)drv_mode_store_save((firmware_mode_t)arg);
        NVIC_SystemReset();
        break;

    case DRV_CONTROL_APPLY_UART_MODE:
    default:
        /* 本板只有一路串口，切模式不需要重新枚举 */
        break;
    }
}
