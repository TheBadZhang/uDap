#pragma once

#include <stdbool.h>

#include "drv/drv_led.h"
#include "drv/drv_uart.h"

/*
 * 用 AT32F405 的「用户系统数据区」（USD，0x1FFFF800）持久化运行期模式。
 *
 * USD 的 16 位字布局（`sdk/drivers/inc/at32f402_405_flash.h` 里的 usd_type）：
 *   0x1FFFF800  fap     FLASH 访问保护
 *   0x1FFFF802  ssb     系统设置字节（WDT 自动启动 / boot1 等）
 *   0x1FFFF804  data0   用户数据 0   <- 本驱动用
 *   0x1FFFF806  data1   用户数据 1   <- 本驱动用
 *   0x1FFFF808  epp0..3 擦写保护位
 * 每个字节硬件都会自动存成「值 + 反码」两个字节，所以读回时高字节等于低字节
 * 的反码才是有效数据；擦除态读回 0xFFFF，校验天然失败。
 *
 * 存两个字节：
 *   data0 = 调试器模式（firmware_mode_t）
 *   data1 = bit0 串口模式 | bit2:1 LED 模式 | bit7:3 保留（必须为 0）
 *
 * 注意：USD 与 fap / ssb / epp 同处一个擦除页，编程前必须整页擦除，所以
 * 每次保存都要「擦 + 重写两个字节」。选项字节擦写寿命有限（数据手册标
 * 1000 次量级），因此只在值真的变化时才写，并在编程后回读校验。
 */

typedef enum {
    FIRMWARE_MODE_CMSIS_DAP = 0,
    /* 参考工程的第二形态（WCH-Link 兼容调试器）。本固件没有编入这一形态
     * （WCHLINK_ENABLE = 0），保留编号是为了和 vendor 协议 / 选项字节布局
     * 兼容：收到这个值时上层会回 UDAP_STATUS_UNSUPPORTED，也不会被写进 Flash。 */
    FIRMWARE_MODE_WCHLINK = 1,
    // 哨兵，不是真实模式：仅供有效性上界使用，永不写进 Flash。
    FIRMWARE_MODE_COUNT,
} firmware_mode_t;

/* 从 USD data0 读取已保存的调试器模式。无效时返回 false，调用方用默认值。 */
bool drv_mode_store_load(firmware_mode_t *out_mode);

/* 把调试器模式写入 USD data0（保留 data1 的内容）。 */
bool drv_mode_store_save(firmware_mode_t mode);

/* 从 USD data1 读取已保存的串口模式。 */
bool drv_mode_store_load_uart_mode(drv_uart_mode_t *out_mode);

/* 把串口模式写入 USD data1（保留同一字节里的 LED 模式位）。 */
bool drv_mode_store_save_uart_mode(drv_uart_mode_t mode);

/* 从 USD data1 的 bit2:1 读取 LED 模式。 */
bool drv_mode_store_load_led_mode(drv_led_mode_t *out_mode);

/* 把 LED 模式写入 USD data1（保留同一字节里的串口模式位）。 */
bool drv_mode_store_save_led_mode(drv_led_mode_t mode);
