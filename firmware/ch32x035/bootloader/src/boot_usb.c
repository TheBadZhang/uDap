/*
 * UF2 bootloader 的 USB 设备：一个只含 MSC（大容量存储）接口的复合设备，
 * 主机看到的是一个 FAT16 U 盘，里面有两个文件：
 *
 *   INFO_UF2.TXT  产品/板名/进入 bootloader 的原因
 *   INDEX   HTM / JOIN HTM  指向项目主页的小网页；另有 family ID 标记文件
 *
 * 拖任意 .uf2 文件进去即可升级：MSC 的扇区写回调把数据交给 src/boot_uf2.c，
 * 由它校验 UF2 块并逐页落 Flash（每页回读校验）。
 *
 * 描述符/字符串部分照抄工程里 CMSIS-DAP 那套（CONFIG_USBDEV_ADVANCE_DESC
 * 要求的 callback 形式），只是把接口换成 MSC、序列号仍旧用芯片 UID。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <usbd_core.h>
#include <usbd_msc.h>

#include "boot_config.h"
#include "boot_log.h"
#include "boot_uf2.h"
#include "boot_usb.h"
#include "bootuf2.h"

#include "bsp/bsp_uid.h"

/* CH32X035 只有 EP0..EP7；bootloader 里没有别的接口，用 EP1 IN / EP2 OUT */
#define BOOT_MSC_IN_EP  0x81
#define BOOT_MSC_OUT_EP 0x02

#define BOOT_CONFIG_SIZE (9 + MSC_DESCRIPTOR_LEN)

#define BOOT_LANGID_STRING 1033

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00,
                               BOOT_USB_VID, BOOT_USB_PID, 0x0100, 0x01),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(BOOT_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 500),
    MSC_DESCRIPTOR_INIT(0x00, BOOT_MSC_OUT_EP, BOOT_MSC_IN_EP, 64, 0x00),
};

static char boot_serial[24] = "0123456789ABCDEF";

static char *string_descriptors[] = {
    (char[]){0x09, 0x04},                 /* Langid */
    "uDap",                               /* Manufacturer */
    "uDap UF2 Bootloader",                /* Product */
    boot_serial,                          /* Serial Number（芯片 UID） */
    "uDap UF2 update drive",              /* Interface */
};

__WEAK const uint8_t *device_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_descriptor;
}

__WEAK const uint8_t *config_descriptor_callback(uint8_t speed) {
    (void)speed;
    return config_descriptor;
}

__WEAK const char *string_descriptor_callback(uint8_t speed, uint8_t index) {
    (void)speed;
    if (index >= (sizeof(string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return string_descriptors[index];
}

/* ---------------------------------------------------------------------------
 * MSC 回调：扇区读写全部转发给 bootuf2 的虚拟 FAT16
 * ------------------------------------------------------------------------- */

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t *block_num, uint32_t *block_size) {
    (void)busid;
    (void)lun;
    *block_num = bootuf2_get_sector_count();
    *block_size = bootuf2_get_sector_size();
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length) {
    (void)busid;
    (void)lun;
    return boot2uf2_read_sector(sector, buffer, length / bootuf2_get_sector_size());
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length) {
    (void)busid;
    (void)lun;
    return boot_uf2_write_sector(sector, buffer, length / bootuf2_get_sector_size());
}

static void boot_usbd_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;

    // 这里的日志很有用：主机到底有没有枚举、有没有挂载成盘，一眼就能看出来
    switch (event) {
    case USBD_EVENT_RESET:
        boot_log("[usb] 总线复位\n");
        break;
    case USBD_EVENT_CONFIGURED:
        boot_log("[usb] 已配置（主机认到 U 盘）\n");
        break;
    case USBD_EVENT_DISCONNECTED:
        boot_log("[usb] 断开\n");
        break;
    default:
        break;
    }
}

static struct usbd_interface msc_intf;

static const struct usb_descriptor boot_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

void boot_usb_init(void) {
    bsp_get_uid_hex16(boot_serial);

    // 必须在 usbd_initialize() 之前：主机一上来就会读 0 号扇区，
    // 而 bootuf2_init() 负责把文件簇链算好。
    bootuf2_init();

    usbd_desc_register(0, &boot_descriptor);
    usbd_add_interface(0, usbd_msc_init_intf(0, &msc_intf, BOOT_MSC_OUT_EP, BOOT_MSC_IN_EP));
    usbd_initialize(0, 0, boot_usbd_event_handler);
}
