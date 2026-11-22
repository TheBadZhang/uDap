/**
 * @file    boot_protocol.c
 * @brief   WinUSB 私有批量协议（bootloader 接口 1）
 *
 * 协议定义见 docs/bootloader-protocol.md 第 4 节，要点：
 *   一个命令 = 一次 OUT 传输，应答 = 一次 IN 传输；
 *   命令第 1 字节是命令码，第 2 字节保留填 0。
 * 状态码：0x00 成功 / 0x01 参数错 / 0x02 Flash 失败 / 0x03 CRC 错 / 0x04 未擦除就写。
 *
 * 注意 ERASE_APP 的应答首字节按规范字面是 0x01（不是 0x02 回显），
 * 这里严格照文档实现，改动前先改规范。
 */

#include "boot_protocol.h"

#include <stdbool.h>
#include <stddef.h>

#include "boot_config.h"
#include "boot_flash.h"
#include "boot_main.h"

/* 命令码 */
#define BOOT_CMD_GET_INFO  0x01u
#define BOOT_CMD_ERASE_APP 0x02u
#define BOOT_CMD_WRITE     0x03u
#define BOOT_CMD_READ      0x04u
#define BOOT_CMD_FINISH    0x05u
#define BOOT_CMD_REBOOT    0x06u

/* 状态码 */
#define BOOT_STATUS_OK        0x00u
#define BOOT_STATUS_PARAM     0x01u
#define BOOT_STATUS_FLASH     0x02u
#define BOOT_STATUS_CRC       0x03u
#define BOOT_STATUS_NOT_ERASED 0x04u

/* GET_INFO 应答长度：规范写 28 字节；字段只占前 22 字节，后面 6 字节保留填 0 */
#define BOOT_GET_INFO_LEN 28u

/* caps 位：bit0 = 支持 DFU，bit1 = 支持 WinUSB 写入 */
#define BOOT_CAPS (0x0001u | 0x0002u)

/* ------------------------------------------------------------ 小工具 */

static void put_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_le32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint16_t get_le16(const uint8_t *in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_le32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

/* boot_flash 的结果码映射成协议状态码 */
static uint8_t status_from_flash(boot_flash_result_t result) {
    switch (result) {
        case BOOT_FLASH_OK:
            return BOOT_STATUS_OK;
        case BOOT_FLASH_ERR_RANGE:
            return BOOT_STATUS_PARAM;
        case BOOT_FLASH_ERR_NOT_ERASED:
            return BOOT_STATUS_NOT_ERASED;
        case BOOT_FLASH_ERR_OPERATION:
        default:
            return BOOT_STATUS_FLASH;
    }
}

/* ------------------------------------------------------------ 各命令 */

static uint32_t protocol_get_info(uint8_t *response, uint32_t response_size) {
    if (response_size < BOOT_GET_INFO_LEN) {
        return 0u;
    }

    response[0] = BOOT_CMD_GET_INFO;
    response[1] = BOOT_STATUS_OK;
    response[2] = BOOT_PROTO_VER;
    response[3] = BOOT_FW_MAJOR;
    response[4] = BOOT_FW_MINOR;
    response[5] = BOOT_FW_PATCH;
    put_le32(&response[6], BOOT_APP_BASE);
    put_le32(&response[10], BOOT_APP_SIZE);
    put_le32(&response[14], BOOT_PAGE_SIZE);
    put_le16(&response[18], BOOT_MAX_CHUNK);
    put_le16(&response[20], BOOT_CAPS);

    /* 规范说应答 28 字节，字段之后补 0（保留位） */
    for (uint32_t i = 22u; i < BOOT_GET_INFO_LEN; i++) {
        response[i] = 0u;
    }
    return BOOT_GET_INFO_LEN;
}

static uint32_t protocol_erase_app(uint8_t *response) {
    /* 规范第 4 节：ERASE_APP 应答字面是 [0x01, status] */
    response[0] = BOOT_CMD_GET_INFO;

    if (!boot_flash_erase_app()) {
        response[1] = BOOT_STATUS_FLASH;
    } else {
        response[1] = BOOT_STATUS_OK;
    }
    return 2u;
}

static uint32_t protocol_write(const uint8_t *request, uint32_t request_length, uint8_t *response) {
    uint32_t address;
    uint16_t length;

    response[0] = BOOT_CMD_WRITE;
    response[1] = BOOT_STATUS_OK;

    if (request_length < 8u) {
        response[1] = BOOT_STATUS_PARAM;
        return 2u;
    }

    address = get_le32(&request[2]);
    length = get_le16(&request[6]);

    /* 地址必须落在应用区且 4 字节对齐；len 不能超过一次 IN 传输能带的上限 */
    if ((length == 0u) || (length > BOOT_MAX_CHUNK) || ((address & 0x03u) != 0u) ||
        (request_length < (8u + (uint32_t)length))) {
        response[1] = BOOT_STATUS_PARAM;
        return 2u;
    }

    response[1] = status_from_flash(boot_flash_write(address, &request[8], (uint32_t)length, false));
    return 2u;
}

static uint32_t protocol_read(const uint8_t *request, uint32_t request_length, uint8_t *response,
                              uint32_t response_size) {
    uint32_t address;
    uint16_t length;

    response[0] = BOOT_CMD_READ;
    response[1] = BOOT_STATUS_OK;

    if (request_length < 8u) {
        response[1] = BOOT_STATUS_PARAM;
        return 2u;
    }

    address = get_le32(&request[2]);
    length = get_le16(&request[6]);

    if ((length == 0u) || (length > BOOT_MAX_CHUNK) || (response_size < (2u + (uint32_t)length))) {
        response[1] = BOOT_STATUS_PARAM;
        return 2u;
    }

    if (!boot_flash_read(address, &response[2], (uint32_t)length)) {
        response[1] = BOOT_STATUS_PARAM;
        return 2u;
    }
    return 2u + (uint32_t)length;
}

static uint32_t protocol_finish(const uint8_t *request, uint32_t request_length, uint8_t *response) {
    uint32_t total_length;
    uint32_t expected_crc;
    uint32_t actual_crc;

    response[0] = BOOT_CMD_FINISH;
    response[1] = BOOT_STATUS_OK;

    if (request_length < 10u) {
        response[1] = BOOT_STATUS_PARAM;
        return 2u;
    }

    total_length = get_le32(&request[2]);
    expected_crc = get_le32(&request[6]);

    if ((total_length == 0u) || (total_length > BOOT_APP_SIZE)) {
        response[1] = BOOT_STATUS_PARAM;
        return 2u;
    }

    /* 校验整片镜像：从应用区起始读 total_length 字节，CRC32 与 zlib.crc32 一致 */
    actual_crc = boot_crc32((const uint8_t *)BOOT_APP_BASE, total_length);
    if (actual_crc != expected_crc) {
        response[1] = BOOT_STATUS_CRC;
        return 2u;
    }

    /* CRC 对了还要确认向量表可用，避免跳进空片上电跑飞 */
    if (!boot_app_vector_valid()) {
        response[1] = BOOT_STATUS_FLASH;
        return 2u;
    }

    /* 校验通过：发完应答后稍等片刻再跳应用（让 IN 传输真正上线） */
    boot_request_jump_to_app();
    return 2u;
}

static uint32_t protocol_reboot(uint8_t *response) {
    response[0] = BOOT_CMD_REBOOT;
    response[1] = BOOT_STATUS_OK;

    /* 发完应答后复位；复位后 bootloader 重新判断 magic/向量，正常会跳进应用 */
    boot_request_reboot();
    return 2u;
}

/* ------------------------------------------------------------ 入口 */

uint32_t boot_protocol_handle(const uint8_t *request, uint32_t request_length, uint8_t *response,
                              uint32_t response_size) {
    if ((request == NULL) || (response == NULL) || (request_length < 1u) || (response_size < 2u)) {
        return 0u;
    }

    switch (request[0]) {
        case BOOT_CMD_GET_INFO:
            return protocol_get_info(response, response_size);
        case BOOT_CMD_ERASE_APP:
            return protocol_erase_app(response);
        case BOOT_CMD_WRITE:
            return protocol_write(request, request_length, response);
        case BOOT_CMD_READ:
            return protocol_read(request, request_length, response, response_size);
        case BOOT_CMD_FINISH:
            return protocol_finish(request, request_length, response);
        case BOOT_CMD_REBOOT:
            return protocol_reboot(response);
        default:
            /* 未知命令：回显命令码 + 参数错 */
            response[0] = request[0];
            response[1] = BOOT_STATUS_PARAM;
            return 2u;
    }
}
