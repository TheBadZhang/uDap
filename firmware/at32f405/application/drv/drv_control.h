#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "drv/drv_mode_store.h"

/*
 * 「需要等 USB 应答发完再执行」的动作。
 *
 * vendor command 是同步处理的：处理函数返回后，CMSIS-DAP 的应答才被挂到 USB IN
 * 端点上发走。而「切串口模式」要重新枚举 USB，「进 bootloader」要复位，如果在
 * 处理函数里立刻做，主机永远收不到那条应答。所以这里只登记动作，由主循环在
 * 几十毫秒后执行。
 */

typedef enum {
    DRV_CONTROL_NONE = 0,
    DRV_CONTROL_APPLY_UART_MODE, /* arg = drv_uart_mode_t，切模式并重新枚举 */
    DRV_CONTROL_SOFT_RESET,      /* 软复位 */
    DRV_CONTROL_ENTER_UF2,       /* 写握手标记 + 软复位，进 bootloader */
    DRV_CONTROL_APPLY_DEBUGGER,  /* arg = firmware_mode_t，持久化后软复位 */
} drv_control_action_t;

/* 上电时调用一次：记录当前运行模式 */
void drv_control_init(firmware_mode_t runtime_mode);

/* 当前正在运行的调试器模式 */
firmware_mode_t drv_control_runtime_mode(void);

/* 登记一个延迟动作。已有动作在排队时返回 false（不会覆盖） */
bool drv_control_request(drv_control_action_t action, uint32_t arg);

/* 主循环每轮调用：到时间就执行排队的动作 */
void drv_control_service(void);

/* 复位进 bootloader 用的握手标记（放在备份寄存器里，软复位不清） */
void drv_control_set_boot_request(void);
bool drv_control_take_boot_request(void);
