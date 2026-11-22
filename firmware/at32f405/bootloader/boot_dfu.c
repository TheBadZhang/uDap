/**
 * @file    boot_dfu.c
 * @brief   CherryUSB DFU 1.1 类的弱函数实现（接口 0）
 *
 * CherryUSB 的 class/dfu/usbd_dfu.c 只实现 DFU 状态机，真正的 Flash 访问通过
 * 下面 5 个 __WEAK 函数交给应用实现（这里就是我们的实现）。
 *
 * 下载寻址：wValue = 块号，地址 = 0x08004000 + 块号 * 1024（wTransferSize）。
 * 这样 dfu-util 用标准 DFU 1.1 从块 0 开始发 1024 字节包即可直接写入应用区。
 * 擦除是「懒擦除」：写到哪一页就先擦哪一页，避免一开始就擦 112 KiB。
 */

#include "at32f402_405.h"

#include "usbd_core.h"
#include "usbd_dfu.h"

#include "boot_config.h"
#include "boot_flash.h"
#include "boot_main.h"

static bool s_dfu_active;

/* 块号 -> 绝对地址 */
static uint32_t dfu_block_address(uint16_t block) {
    return BOOT_APP_BASE + ((uint32_t)block * BOOT_DFU_TRANSFER_SIZE);
}

/* 开始一次下载/上传（DFU 状态机第一次收到非空 DNLOAD 时调用） */
void usbd_dfu_begin_load(void) {
    if (!s_dfu_active) {
        s_dfu_active = true;
        boot_flash_init();
    }
}

/* 下载结束（主机发 wLength == 0 的 DNLOAD） */
void usbd_dfu_end_load(void) {
    s_dfu_active = false;
}

/**
 * @brief DFU 下载的一块数据
 * @return 0 成功；-1 失败（DFU 类会进 DFU_ERROR 状态）
 */
int usbd_dfu_write(uint16_t value, const uint8_t *data, uint16_t length) {
    if ((data == NULL) || (length == 0u)) {
        return 0;
    }

    if (boot_flash_write(dfu_block_address(value), data, (uint32_t)length, true) != BOOT_FLASH_OK) {
        return -1;
    }
    return 0;
}

/**
 * @brief DFU 上传的一块数据（本 bootloader 支持读回应用区，便于工具校验）
 */
int usbd_dfu_read(uint16_t value, uint8_t *data, uint16_t length, uint16_t *actual_length) {
    if ((data == NULL) || (actual_length == NULL) || (length == 0u)) {
        return -1;
    }

    if (!boot_flash_read(dfu_block_address(value), data, (uint32_t)length)) {
        return -1;
    }

    *actual_length = length;
    return 0;
}

/* 主机在 MANIFEST_WAIT_RESET 状态下读 GETSTATUS 时调用：复位进应用。
 * 这里只置「延时复位」标志，真正复位在主循环里做（USB 中断里不做重活）。 */
void usbd_dfu_reset(void) {
    s_dfu_active = false;
    boot_request_reboot();
}
