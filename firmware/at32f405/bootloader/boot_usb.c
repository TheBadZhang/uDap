/**
 * @file    boot_usb.c
 * @brief   bootloader 的 USB 设备栈（接口 0 = DFU 1.1，接口 1 = WinUSB 私有协议）
 *
 * 描述符写法照抄应用侧 application/dap/dap_main.c：
 *   - 设备描述符 18 字节，bDeviceClass = 0xEF（IAD 复合设备）；
 *   - 配置描述符里接口 0 是 CherryUSB 的 DFU 类（DFU_DESCRIPTOR_INIT 展开成
 *     interface + functional 两个描述符，共 18 字节），接口 1 是我们自己的
 *     vendor 接口（两个 bulk 端点 0x06/0x85）；
 *   - MS OS 2.0：接口 0 与接口 1 都标记成 WinUSB（各 160 字节功能子集），
 *     这样 Windows 会把两个接口都绑到 WinUSB 驱动上；
 *   - BOS 里带一个 WinUSB platform capability（vendor code 0x20）。
 *
 * 序列号复用芯片 UID（0x1FFFF7E8/0x1FFFF7EC）派生 16 个十六进制字符，
 * 与应用的 udap_serial_number() 完全一致，两个模式（0204/0205）序列号相同。
 */

#include "boot_usb.h"

#include "at32f402_405.h"

#include "usbd_core.h"
#include "usbd_dfu.h"

#include "boot_config.h"
#include "boot_protocol.h"

/* WinUSB 私有协议的功能子集用「一个接口」还是「两个接口」：
 * 本 bootloader 两个接口都要被 WinUSB 绑定，因此用 MULTI 版本（带子集头）。 */
#define BOOT_WINUSB_VENDOR_CODE 0x20u

/* MS OS 2.0 描述符集合总长 = 10 字节头 + 2 个 160 字节功能子集 */
#define BOOT_MSOSV2_DESC_SET_LEN                                                  \
    (WINUSB_DESCRIPTOR_SET_HEADER_SIZE +                                         \
     2u * USB_MSOSV2_COMP_ID_FUNCTION_WINUSB_MULTI_DESCRIPTOR_LEN)

/* BOS 总长 = 5 字节头 + 28 字节 WinUSB platform capability */
#define BOOT_BOS_TOTAL_LEN (0x05u + USB_BOS_CAP_PLATFORM_WINUSB_DESCRIPTOR_LEN)

/* 与 UID 相关的 eFlash 签名区地址（应用侧 dap_main.c 用的同一对地址） */
#define BOOT_ESIG_UNIID1_ADDR 0x1FFFF7E8u
#define BOOT_ESIG_UNIID2_ADDR 0x1FFFF7ECu

/* --------------------------------------------------------------- 描述符 */

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_1, 0xEF, 0x02, 0x01, BOOT_USBD_VID, BOOT_USBD_PID, 0x0100, 0x01),
};

/*
 * 配置描述符（HS）：总长 50 字节
 *   9 配置 + 18 DFU(interface+functional) + 9 vendor interface + 7 + 7 两个 bulk 端点
 */
#define BOOT_CONFIG_TOTAL_LEN (9u + DFU_DESCRIPTOR_LEN + 9u + 7u + 7u)

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(BOOT_CONFIG_TOTAL_LEN, 0x02, 0x01, USB_CONFIG_BUS_POWERED, BOOT_MAX_POWER),
    /* 接口 0：标准 DFU 1.1，alt 0 对应 0x08004000 起的应用区，wTransferSize=1024 */
    DFU_DESCRIPTOR_INIT(4),
    /* 接口 1：WinUSB 私有批量协议 */
    USB_INTERFACE_DESCRIPTOR_INIT(0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(BOOT_OUT_EP, USB_ENDPOINT_TYPE_BULK, BOOT_PACKET_SIZE, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(BOOT_IN_EP, USB_ENDPOINT_TYPE_BULK, BOOT_PACKET_SIZE, 0x00),
};

/* 全速「其它速度」配置：结构一致，bulk 包长 64 */
static const uint8_t other_speed_config_descriptor[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(BOOT_CONFIG_TOTAL_LEN, 0x02, 0x01, USB_CONFIG_BUS_POWERED,
                                           BOOT_MAX_POWER),
    DFU_DESCRIPTOR_INIT(4),
    USB_INTERFACE_DESCRIPTOR_INIT(0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(BOOT_OUT_EP, USB_ENDPOINT_TYPE_BULK, 64u, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(BOOT_IN_EP, USB_ENDPOINT_TYPE_BULK, 64u, 0x00),
};

/* 设备限定符：HS 设备必须提供，全速配置同上 */
static const uint8_t device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_1, 0x00, 0x00, 0x00, 0x01),
};

/* MS OS 2.0 描述符集合：接口 0（DFU）与接口 1（vendor）都标成 WINUSB */
static const uint8_t msosv2_descriptor_set[] = {
    USB_MSOSV2_COMP_ID_SET_HEADER_DESCRIPTOR_INIT(BOOT_MSOSV2_DESC_SET_LEN),
    USB_MSOSV2_COMP_ID_FUNCTION_WINUSB_MULTI_DESCRIPTOR_INIT(0x00),
    USB_MSOSV2_COMP_ID_FUNCTION_WINUSB_MULTI_DESCRIPTOR_INIT(0x01),
};

/* BOS：WinUSB platform capability，指向上面的 MS OS 2.0 描述符集合 */
static const uint8_t bos_descriptor[] = {
    USB_BOS_HEADER_DESCRIPTOR_INIT(BOOT_BOS_TOTAL_LEN, 0x01),
    USB_BOS_CAP_PLATFORM_WINUSB_DESCRIPTOR_INIT(BOOT_WINUSB_VENDOR_CODE, BOOT_MSOSV2_DESC_SET_LEN),
};

/* 字符串描述符：0 = LangID，1 = 厂商，2 = 产品，3 = 序列号，4 = DFU 接口 */
static char s_serial_number[17] = "DEADBEEF";

static char *string_descriptors[] = {
    (char[]){0x09, 0x04}, /* LangID（en-US = 1033），先用占位，见下面的回调 */
    "uDAP",
    "uDAP Bootloader",
    s_serial_number,
    "DFU 1.1",
};

/* 编译期校验描述符长度，写错立刻在编译期报出来（照抄应用侧的做法） */
_Static_assert(sizeof(device_descriptor) == 18u, "设备描述符长度必须是 18 字节");
_Static_assert(sizeof(config_descriptor) == BOOT_CONFIG_TOTAL_LEN, "配置描述符长度不对");
_Static_assert(sizeof(other_speed_config_descriptor) == BOOT_CONFIG_TOTAL_LEN, "其它速度描述符长度不对");
_Static_assert(sizeof(device_quality_descriptor) == 10u, "设备限定符长度必须是 10 字节");
_Static_assert(sizeof(msosv2_descriptor_set) == BOOT_MSOSV2_DESC_SET_LEN, "MS OS 2.0 描述符集合长度不对");
_Static_assert(sizeof(bos_descriptor) == BOOT_BOS_TOTAL_LEN, "BOS 描述符长度不对");

/* ------------------------------------------------------------ 描述符回调 */

static const uint8_t *boot_device_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_descriptor;
}

static const uint8_t *boot_config_descriptor_callback(uint8_t speed) {
    return (speed == USB_SPEED_HIGH) ? config_descriptor : other_speed_config_descriptor;
}

static const uint8_t *boot_device_quality_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_quality_descriptor;
}

static const uint8_t *boot_other_speed_descriptor_callback(uint8_t speed) {
    (void)speed;
    return other_speed_config_descriptor;
}

static const char *boot_string_descriptor_callback(uint8_t speed, uint8_t index) {
    (void)speed;
    if (index >= (sizeof(string_descriptors) / sizeof(string_descriptors[0]))) {
        return NULL;
    }
    return string_descriptors[index];
}

/* ------------------------------------------------------ 序列号（芯片 UID） */

static bool uid_word_plausible(uint32_t value) {
    return (value != 0u) && (value != 0xFFFFFFFFu);
}

/* 8 个十六进制字符，高位在前（与应用侧 esig_sn_put_word 一致） */
static void serial_put_word(uint32_t value, char *out) {
    static const char hex[] = "0123456789ABCDEF";

    for (uint32_t i = 0u; i < 8u; i++) {
        out[i] = hex[(value >> (28u - 4u * i)) & 0x0Fu];
    }
}

static void boot_serial_apply(void) {
    uint32_t uid_low = *(const volatile uint32_t *)BOOT_ESIG_UNIID1_ADDR;
    uint32_t uid_high = *(const volatile uint32_t *)BOOT_ESIG_UNIID2_ADDR;

    /* UID 被擦成全 0 或全 F（未烧过选项字节/测试芯片）时回退到固定串号 */
    if (!uid_word_plausible(uid_low) && !uid_word_plausible(uid_high)) {
        return;
    }
    serial_put_word(uid_low, &s_serial_number[0]);
    serial_put_word(uid_high, &s_serial_number[8]);
    s_serial_number[16] = '\0';
}

/* ------------------------------------------------------------- USB 设备栈 */

static struct usbd_interface s_dfu_intf;
static struct usbd_interface s_vendor_intf;

static struct usbd_endpoint s_out_ep = {
    .ep_addr = BOOT_OUT_EP,
    .ep_cb = NULL,
};
static struct usbd_endpoint s_in_ep = {
    .ep_addr = BOOT_IN_EP,
    .ep_cb = NULL,
};

/* 批量端点缓冲：OUT 收命令，IN 发应答（READ 最长 2+504 = 506 字节） */
static uint8_t s_out_buffer[BOOT_PACKET_SIZE] __attribute__((aligned(4)));
static uint8_t s_in_buffer[BOOT_PACKET_SIZE + 8u] __attribute__((aligned(4)));

static volatile bool s_out_ready;
static volatile uint32_t s_out_length;

static void boot_ep_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;

    /* 中断里只记下长度，命令交给主循环处理（擦 Flash 要 1 秒以上，不能在 ISR 里做） */
    s_out_length = nbytes;
    s_out_ready = true;
}

static void boot_ep_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;
    (void)nbytes;
}

static void boot_usbd_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;

    switch (event) {
        case USBD_EVENT_CONFIGURED:
            /* 主机配置完成后才开始收命令 */
            s_out_ready = false;
            usbd_ep_start_read(0, BOOT_OUT_EP, s_out_buffer, BOOT_PACKET_SIZE);
            break;
        case USBD_EVENT_RESET:
            s_out_ready = false;
            break;
        default:
            break;
    }
}

/* OTGHS 中断入口（startup_at32f405.s 里向量表用的就是这个名字） */
void OTGHS_IRQHandler(void) {
    USBD_IRQHandler(0);
}

/* CherryUSB 的 dwc2 glue 只提供 FIFO/PHY 参数，低层钩子要平台自己补 */
void usb_dc_low_level_init(uint8_t busid) {
    (void)busid;
    nvic_irq_enable(OTGHS_IRQn, 0u, 0u);
}

void usb_dc_low_level_deinit(uint8_t busid) {
    (void)busid;
    nvic_irq_disable(OTGHS_IRQn);
}

/* USB 时钟：OTGHS 控制器 + 内置 HS PHY（12 MHz 来自 HEXT/1），内核时钟 48 MHz 来自 PLLU。
 * 与 application/bsp/bsp_usb.c 的 bsp_usb_clock_init() 完全相同。 */
static void boot_usb_clock_init(void) {
    crm_periph_clock_enable(CRM_OTGHS_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_OTGHSPHY_PERIPH_CLOCK, TRUE);

    crm_usb_phy12_clock_select(CRM_USB_PHY12_CLOCK_HEXT_DIV_1);

    crm_pllu_output_set(TRUE);
    while (crm_flag_get(CRM_PLLU_STABLE_FLAG) != SET) {
    }
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_PLLU);

    nvic_irq_enable(OTGHS_IRQn, 0u, 0u);
}

void boot_usb_init(void) {
    static const struct usb_msosv2_descriptor msosv2_desc = {
        .vendor_code = BOOT_WINUSB_VENDOR_CODE,
        .compat_id = msosv2_descriptor_set,
        .compat_id_len = BOOT_MSOSV2_DESC_SET_LEN,
    };
    static const struct usb_bos_descriptor bos_desc = {
        .string = bos_descriptor,
        .string_len = BOOT_BOS_TOTAL_LEN,
    };
    static const struct usb_descriptor boot_usb_descriptor = {
        .device_descriptor_callback = boot_device_descriptor_callback,
        .config_descriptor_callback = boot_config_descriptor_callback,
        .device_quality_descriptor_callback = boot_device_quality_descriptor_callback,
        .other_speed_descriptor_callback = boot_other_speed_descriptor_callback,
        .string_descriptor_callback = boot_string_descriptor_callback,
        .bos_descriptor = &bos_desc,
        .msosv2_descriptor = &msosv2_desc,
    };

    boot_usb_clock_init();
    boot_serial_apply();

    s_out_ep.ep_cb = boot_ep_out_callback;
    s_in_ep.ep_cb = boot_ep_in_callback;

    usbd_desc_register(0, &boot_usb_descriptor);

    /* 注册顺序决定 bInterfaceNumber：DFU = 0、vendor = 1，必须与配置描述符一致 */
    usbd_add_interface(0, usbd_dfu_init_intf(&s_dfu_intf));
    usbd_add_interface(0, &s_vendor_intf);
    usbd_add_endpoint(0, &s_out_ep);
    usbd_add_endpoint(0, &s_in_ep);

    usbd_initialize(0, OTGHS_BASE, boot_usbd_event_handler);
}

void boot_usb_poll(void) {
    uint32_t request_length;
    uint32_t response_length;

    if (!s_out_ready) {
        return;
    }
    s_out_ready = false;
    request_length = s_out_length;

    response_length = boot_protocol_handle(s_out_buffer, request_length, s_in_buffer, sizeof(s_in_buffer));
    if (response_length > 0u) {
        usbd_ep_start_write(0, BOOT_IN_EP, s_in_buffer, response_length);
    }

    /* 重新挂上 OUT 接收，等主机下一条命令 */
    usbd_ep_start_read(0, BOOT_OUT_EP, s_out_buffer, BOOT_PACKET_SIZE);
}
