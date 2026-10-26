#pragma once

#include <stdint.h>

/*
 * 硬件 SPI 位相位（SWD 的请求头 / 写数据相位改用 SPI1 打时钟）
 *
 * 只在 SWD_PIN_VARIANT_PA5_PA7 方案 + 编译期打开 SWD_SPI_DATA_PHASE 时启用
 * （SWCLK = PA5 = SPI1_SCK，SWDIO = PA7 = SPI1_MOSI，单线半双工）。
 *
 * 分工（其余相位一律留在 GPIO 位时序）：
 *   请求头 8 拍（start/APnDP/RnW/A2/A3/parity/stop/park）  → SPI 发送 1 字节
 *                                                          （**仅写事务**）
 *   写数据 32 拍                                          → SPI 发送 4 字节
 *   读数据 32 拍                                          → SPI 接收 4 字节
 *                                                          （SWD_SPI_RD，默认关）
 *   turnaround、ACK 采样、数据相位的奇偶位、idle cycles    → GPIO 位时序
 *
 * 请求头/写数据都是「纯粹的连续时钟 + 主设备驱动的逐位移位」，正好是 SPI 擅长、
 * GPIO 位时序最费指令的部分；读数据由目标驱动 SWDIO，在这块板子的跳线上余量不够
 * （实测数据会随时钟整体错位），默认仍用位时序 —— 详细数据见
 * src/dap/swd_spi_x035.c 文件尾。
 */

/* 初始化 SPI1（接一次 DAP 调一次，幂等）。调用后 SPI 处于关闭状态、引脚仍是 GPIO。 */
void swd_spi_init(void);

/* 请求头 8 拍（查表 + 整字节发，含 park 位）。返回时 SCK 已收回 GPIO 且为低、
 * SWDIO 仍由 GPIO 驱动 1（park 位），调用方紧接着释放 SWDIO 做 turnaround。
 *
 * ⚠️ 默认**不启用**（见 swd_pin_map.h 的 SWD_SPI_REQ）：实测比汇编版本慢一倍
 *    （写 279~288 → 150~162 KiB/s），原因未定位。 */
void swd_spi_send_request(uint32_t request);

uint32_t swd_spi_send32(uint32_t value);

/* 32 位读数据相位（SWD_SPI_RD=1 时才会被调用）。进入时 SWDIO 必须是浮空输入
 * （目标在驱动）；返回时 SCK 已收回 GPIO 且为低，SWDIO 仍是浮空输入
 * （奇偶位由调用方用 GPIO 采）。 */
uint32_t swd_spi_recv32(void);
