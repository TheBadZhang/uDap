#include "drv/drv_control.h"

#include <ch32x035.h>

#include <usbd_core.h>

#include "boot_info.h"
#include "bsp/bsp_delay.h"
#include "dap/dap_main.h"
#include "drv/drv_uart.h"

/*
 * 延迟动作的执行时机。
 *
 * 30 ms 足够让 CMSIS-DAP 的应答（64 B 全速批量传输）在主机侧收完；再长会让
 * 用户觉得切换「卡了一下」。这个值只影响主观延迟，不影响正确性。
 */
#define DRV_CONTROL_DELAY_MS 30u

static firmware_mode_t s_runtime_mode = FIRMWARE_MODE_CMSIS_DAP;
static drv_control_action_t s_action = DRV_CONTROL_NONE;
static uint32_t s_arg;
static uint32_t s_requested_at;
static bool s_scheduled;

void drv_control_init(firmware_mode_t runtime_mode) {
    s_runtime_mode = runtime_mode;
    s_action = DRV_CONTROL_NONE;
    s_scheduled = false;
}

firmware_mode_t drv_control_runtime_mode(void) {
    return s_runtime_mode;
}

bool drv_control_request(drv_control_action_t action, uint32_t arg) {
    if (action == DRV_CONTROL_NONE) {
        return false;
    }
    if (s_scheduled) {
        return false;
    }

    s_action = action;
    s_arg = arg;
    s_requested_at = bsp_time_ms();
    s_scheduled = true;
    return true;
}

static void control_reset_now(void) {
    // 软复位不会清 SRAM，握手区（boot_info）因此能跨复位传给 bootloader
    __disable_irq();
    NVIC_SystemReset();
}

static void control_execute(void) {
    switch (s_action) {
#if UART_ENABLE
    case DRV_CONTROL_APPLY_UART_MODE:
        // 接口数变了，必须重新枚举：先断开，再按新模式注册描述符/接口/端点
        usbd_deinitialize(0u);
        (void)drv_uart_mode_set((drv_uart_mode_t)s_arg);
        chry_dap_usb_start();
        break;
#endif

    case DRV_CONTROL_SOFT_RESET:
        control_reset_now();
        break;

    case DRV_CONTROL_ENTER_UF2:
        boot_info_write(BOOT_INFO_REASON_REQUEST_UF2, 0u);
        control_reset_now();
        break;

    case DRV_CONTROL_APPLY_DEBUGGER:
        // 模式写进选项字后复位：USB 身份会变（0d28:0204 <-> 1a86:8010），
        // 只有重新启动才能干净地换一套描述符与私有数据
        (void)drv_mode_store_save((firmware_mode_t)s_arg);
        control_reset_now();
        break;

    case DRV_CONTROL_NONE:
    default:
        break;
    }

    s_action = DRV_CONTROL_NONE;
    s_scheduled = false;
}

void drv_control_service(void) __attribute__((noinline));
void drv_control_service(void) {
    if (!s_scheduled) {
        return;
    }
    if ((bsp_time_ms() - s_requested_at) < DRV_CONTROL_DELAY_MS) {
        return;
    }
    control_execute();
}
