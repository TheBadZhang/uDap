/**
 * @file    boot_protocol.h
 * @brief   WinUSB 私有批量协议（bootloader 接口 1，规范第 4 节）
 */

#ifndef __BOOT_PROTOCOL_H
#define __BOOT_PROTOCOL_H

#include <stdint.h>

/**
 * @brief 处理一条完整命令（一次 OUT 传输的内容）
 * @param request        请求数据
 * @param request_length 请求字节数
 * @param response       应答缓冲（由调用者提供，至少要 512 字节）
 * @param response_size  应答缓冲大小
 * @return 应答字节数；0 表示不应答
 */
uint32_t boot_protocol_handle(const uint8_t *request, uint32_t request_length, uint8_t *response,
                              uint32_t response_size);

#endif /* __BOOT_PROTOCOL_H */
