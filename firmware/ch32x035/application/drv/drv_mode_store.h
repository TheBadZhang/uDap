#pragma once

#include <stdbool.h>

#include "drv/drv_led.h"
#include "drv/drv_uart.h"

/*
 * 用 CH32X035「用户选择字」（选项字节）里的 Data0 / Data1 持久化两个运行期模式。
 *
 * 依据 CH32X035 应用手册 20.5 节「用户选择字」：用户选择字信息块共 8 字节，
 * 其中 Data0/Data1 是 2 字节用户自定义数据，每个字节都配有反码字节用于装载校验。
 * 存放位置（32 位字的排布）：
 *
 *   0x1FFFF804  [31:24]=nData1 [23:16]=Data1 [15:8]=nData0 [7:0]=Data0
 *
 * 两个字节的用途：
 *
 *   Data0 = 调试器模式（firmware_mode_t）
 *           0x00 = CMSIS-DAP，0x01 = WCH-Link
 *           （WCH-Link 不参与编译时该字节仍然保留，不影响已烧录板子的记录）
 *   Data1 = 桥接串口模式 + LED 模式（位域）
 *           bit0    = 串口模式：0 = 串口 1 + DTR/RTS，1 = 双串口
 *           bit2:1  = LED 模式：0..3（见 drv/drv_led.h 的 drv_led_mode_t）
 *           bit7:3  = 保留，必须为 0；不满足（例如擦除态 0xFF）就整体当作
 *                     「从未保存过」，由调用方回退到默认值
 *
 * 为什么把 LED 模式和串口模式挤在同一个字节里：用户选择字只有 Data0/Data1
 * 两个字节可用（其余是读保护 / 写保护 / USER 配置字，动它们风险太大）。
 * 旧固件写进去的 0x00/0x01 恰好等于「LED 模式 0 + 对应串口模式」，
 * 所以升级后老板子的记录仍然有效。
 *
 * 读回时要求反码匹配且取值合法，否则视为「从未保存过」，由调用方回退到默认值。
 * 擦除态读回 0xFFFF，反码校验天然失败。
 *
 * 这里的 firmware_mode_t 是**两种模式共用的唯一模式枚举**（main.c 也用它），
 * 避免出现两套取值凑巧相同、各改各的枚举。要再加一种模式时：
 *   1. 在下面的枚举末尾**追加**新成员（不要插入/改动已有取值）；
 *   2. 在 main.c 的 mode_run() 里加一个 case。
 * 已有取值的数值会持久化到 Flash，属于对已烧录板子的 ABI，不能重排；
 * 取值还必须从 0 起连续（本驱动用 `< xxx_COUNT` 判断有效性）。
 *
 * ---------------------------------------------------------------------------
 * 注意（改这里之前先读）
 * ---------------------------------------------------------------------------
 * 用户选择字与「读保护 RDPR」「USER 配置字」「写保护 WRPR0..3」同处 0x1FFFF800
 * 开始的同一个 256 B 擦除页。编程前必须整区擦除，所以保存时要把这两个字**原样
 * 回写** —— 否则会改掉读保护：把读保护从「保护」改成「非保护」会触发整片擦除，
 * 而写坏 RDPR 可能让芯片进入读保护状态。
 *
 * 另外选项字节的擦写寿命有限，掉电恰好落在擦/写过程中也可能损坏选择字，
 * 所以本驱动只在值真的变化时才擦写，并在编程后回读校验。
 */

typedef enum {
    FIRMWARE_MODE_CMSIS_DAP = 0,
    FIRMWARE_MODE_WCHLINK = 1,
    // 哨兵，不是真实模式：仅供轮转取模与有效性上界使用，永不写进 Flash。
    FIRMWARE_MODE_COUNT,
} firmware_mode_t;

// 从用户选择字 Data0 读取已保存的调试器模式。
// 返回 true 时 *out_mode 有效；false 表示没有有效记录（擦除态 / 反码校验失败 /
// 取值非法 / out_mode 为空），调用方应使用默认模式。
bool drv_mode_store_load(firmware_mode_t *out_mode);

// 把调试器模式写入用户选择字 Data0，其余选择字（含 Data1）保持不变。
// 已保存相同值时直接返回 true（不擦写）；读保护已开启或擦/写失败时返回 false。
bool drv_mode_store_save(firmware_mode_t mode);

// 从用户选择字 Data1 读取已保存的桥接串口模式。无效（擦除态 / 校验失败 / 取值
// 非法）时返回 false，调用方回退到 DRV_UART_MODE_FLOW。
bool drv_mode_store_load_uart_mode(drv_uart_mode_t *out_mode);

// 把桥接串口模式写入 Data1（保留同一字节里的 LED 模式位）。
// 已保存相同值时直接返回 true（不擦写）。
bool drv_mode_store_save_uart_mode(drv_uart_mode_t mode);

// 从 Data1 的 bit2:1 读取 LED 模式。无效时返回 false（调用方用 DRV_LED_MODE_STATUS）。
bool drv_mode_store_load_led_mode(drv_led_mode_t *out_mode);

// 把 LED 模式写入 Data1（保留同一字节里的串口模式位）。
bool drv_mode_store_save_led_mode(drv_led_mode_t mode);
