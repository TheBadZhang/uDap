#include "dap_main.h"

#include "drv/drv_uart.h"

#define CMSIS_DAP_INTERFACE_SIZE (9 + 7 + 7)
#define CUSTOM_HID_LEN           (9 + 9 + 7 + 7)

#define HIDRAW_INTERVAL 4

#define HID_CUSTOM_REPORT_DESC_SIZE 53

#define USBD_WINUSB_VENDOR_CODE 0x20
#define USBD_WEBUSB_VENDOR_CODE 0x21

#define USBD_WEBUSB_ENABLE 0
#define USBD_BULK_ENABLE   1
#define USBD_WINUSB_ENABLE 1

/* WinUSB Microsoft OS 2.0 descriptor sizes */
#define WINUSB_DESCRIPTOR_SET_HEADER_SIZE  10
#define WINUSB_FUNCTION_SUBSET_HEADER_SIZE 8
#define WINUSB_FEATURE_COMPATIBLE_ID_SIZE  20

#define FUNCTION_SUBSET_LEN                160
#define DEVICE_INTERFACE_GUIDS_FEATURE_LEN 132

#define USBD_WINUSB_DESC_SET_LEN (WINUSB_DESCRIPTOR_SET_HEADER_SIZE + USBD_WEBUSB_ENABLE * FUNCTION_SUBSET_LEN + USBD_BULK_ENABLE * FUNCTION_SUBSET_LEN)

#define USBD_NUM_DEV_CAPABILITIES (USBD_WEBUSB_ENABLE + USBD_WINUSB_ENABLE)

#define USBD_WEBUSB_DESC_LEN 24
#define USBD_WINUSB_DESC_LEN 28

#define USBD_BOS_WTOTALLENGTH (0x05 +                                      \
                               USBD_WEBUSB_DESC_LEN * USBD_WEBUSB_ENABLE + \
                               USBD_WINUSB_DESC_LEN * USBD_WINUSB_ENABLE)

#define USB_CONFIG_SIZE (9 + CMSIS_DAP_INTERFACE_SIZE + CDC_ACM_DESCRIPTOR_LEN + \
                         CONFIG_CHERRYDAP_USE_CUSTOM_HID * CUSTOM_HID_LEN +      \
                         CONFIG_CHERRYDAP_USE_MSC * MSC_DESCRIPTOR_LEN + USBD_WEBUSB_ENABLE * 9)

#define INTF_NUM (1 + 2 + CONFIG_CHERRYDAP_USE_CUSTOM_HID + CONFIG_CHERRYDAP_USE_MSC + USBD_WEBUSB_ENABLE)

#define MSC_INTF_NUM (3 + CONFIG_CHERRYDAP_USE_CUSTOM_HID)

#define WEBUSB_INTF_NUM (3 + CONFIG_CHERRYDAP_USE_CUSTOM_HID + CONFIG_CHERRYDAP_USE_MSC)

#define WEBUSB_URL_STRINGS \
    'c', 'h', 'e', 'r', 'r', 'y', 'd', 'a', 'p', '.', 'c', 'h', 'e', 'r', 'r', 'y', '-', 'e', 'm', 'b', 'e', 'd', 'd', 'e', 'd', '.', 'o', 'r', 'g',

__ALIGN_BEGIN const uint8_t USBD_WinUSBDescriptorSetDescriptor[] = {
    WBVAL(WINUSB_DESCRIPTOR_SET_HEADER_SIZE), /* wLength */
    WBVAL(WINUSB_SET_HEADER_DESCRIPTOR_TYPE), /* wDescriptorType */
    0x00, 0x00, 0x03, 0x06, /* >= Win 8.1 */  /* dwWindowsVersion*/
    WBVAL(USBD_WINUSB_DESC_SET_LEN),          /* wDescriptorSetTotalLength */
#if (USBD_WEBUSB_ENABLE)
    WBVAL(WINUSB_FUNCTION_SUBSET_HEADER_SIZE),  // wLength
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE),  // wDescriptorType
    WEBUSB_INTF_NUM,                            // bFirstInterface USBD_WINUSB_IF_NUM
    0,                                          // bReserved
    WBVAL(FUNCTION_SUBSET_LEN),                 // wSubsetLength
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_SIZE),   // wLength
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_TYPE),   // wDescriptorType
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,         // CompatibleId
    0, 0, 0, 0, 0, 0, 0, 0,                     // SubCompatibleId
    WBVAL(DEVICE_INTERFACE_GUIDS_FEATURE_LEN),  // wLength
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),    // wDescriptorType
    WBVAL(WINUSB_PROP_DATA_TYPE_REG_MULTI_SZ),  // wPropertyDataType
    WBVAL(42),                                  // wPropertyNameLength
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80),  // wPropertyDataLength
    '{', 0,
    '9', 0, '2', 0, 'C', 0, 'E', 0, '6', 0, '4', 0, '6', 0, '2', 0, '-', 0,
    '9', 0, 'C', 0, '7', 0, '7', 0, '-', 0,
    '4', 0, '6', 0, 'F', 0, 'E', 0, '-', 0,
    '9', 0, '3', 0, '3', 0, 'B', 0, '-',
    0, '3', 0, '1', 0, 'C', 0, 'B', 0, '9', 0, 'C', 0, '5', 0, 'A', 0, 'A', 0, '3', 0, 'B', 0, '9', 0,
    '}', 0, 0, 0, 0, 0,
#endif
#if USBD_BULK_ENABLE
    WBVAL(WINUSB_FUNCTION_SUBSET_HEADER_SIZE), /* wLength */
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE), /* wDescriptorType */
    0,                                         /* bFirstInterface USBD_BULK_IF_NUM*/
    0,                                         /* bReserved */
    WBVAL(FUNCTION_SUBSET_LEN),                /* wSubsetLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_SIZE),  /* wLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_TYPE),  /* wDescriptorType */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,        /* CompatibleId*/
    0, 0, 0, 0, 0, 0, 0, 0,                    /* SubCompatibleId*/
    WBVAL(DEVICE_INTERFACE_GUIDS_FEATURE_LEN), /* wLength */
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),   /* wDescriptorType */
    WBVAL(WINUSB_PROP_DATA_TYPE_REG_MULTI_SZ), /* wPropertyDataType */
    WBVAL(42),                                 /* wPropertyNameLength */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80), /* wPropertyDataLength */
    '{', 0,
    'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0, '5', 0, 'A', 0, 'D', 0, '-', 0,
    '2', 0, '9', 0, '3', 0, 'B', 0, '-', 0,
    '4', 0, '6', 0, '6', 0, '3', 0, '-', 0,
    'A', 0, 'A', 0, '3', 0, '6', 0, '-',
    0, '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0, '6', 0, '4', 0, '6', 0, '3', 0, '7', 0, '7', 0, '6', 0,
    '}', 0, 0, 0, 0, 0
#endif
};

__ALIGN_BEGIN const uint8_t USBD_BinaryObjectStoreDescriptor[] = {
    0x05,                         /* bLength */
    0x0f,                         /* bDescriptorType */
    WBVAL(USBD_BOS_WTOTALLENGTH), /* wTotalLength */
    USBD_NUM_DEV_CAPABILITIES,    /* bNumDeviceCaps */
#if (USBD_WEBUSB_ENABLE)
    USBD_WEBUSB_DESC_LEN,           /* bLength */
    0x10,                           /* bDescriptorType */
    USB_DEVICE_CAPABILITY_PLATFORM, /* bDevCapabilityType */
    0x00,                           /* bReserved */
    0x38,
    0xB6,
    0x08,
    0x34, /* PlatformCapabilityUUID */
    0xA9,
    0x09,
    0xA0,
    0x47,
    0x8B,
    0xFD,
    0xA0,
    0x76,
    0x88,
    0x15,
    0xB6,
    0x65,
    WBVAL(0x0100),
    /* 1.00 */               /* bcdVersion */
    USBD_WEBUSB_VENDOR_CODE, /* bVendorCode */
    1,                       /* iLandingPage */
#endif
#if (USBD_WINUSB_ENABLE)
    USBD_WINUSB_DESC_LEN,           /* bLength */
    0x10,                           /* bDescriptorType */
    USB_DEVICE_CAPABILITY_PLATFORM, /* bDevCapabilityType */
    0x00,                           /* bReserved */
    0xDF,
    0x60,
    0xDD,
    0xD8, /* PlatformCapabilityUUID */
    0x89,
    0x45,
    0xC7,
    0x4C,
    0x9C,
    0xD2,
    0x65,
    0x9D,
    0x9E,
    0x64,
    0x8A,
    0x9F,
    0x00,
    0x00,
    0x03,
    0x06,
    /* >= Win 8.1 */                 /* dwWindowsVersion*/
    WBVAL(USBD_WINUSB_DESC_SET_LEN), /* wDescriptorSetTotalLength */
    USBD_WINUSB_VENDOR_CODE,         /* bVendorCode */
    0,                               /* bAltEnumCode */
#endif
};

#define URL_DESCRIPTOR_LENGTH (3 + 29)

const uint8_t USBD_WebUSBURLDescriptor[URL_DESCRIPTOR_LENGTH] = {
    URL_DESCRIPTOR_LENGTH,
    WEBUSB_URL_TYPE,
    WEBUSB_URL_SCHEME_HTTPS,
    WEBUSB_URL_STRINGS};

// clang-format off
#define HID_DESC()                                                                                                                       \
    /************** Descriptor of Custom interface *****************/                                                                    \
    0x09,                                               /* bLength: Interface Descriptor size */                                         \
    USB_DESCRIPTOR_TYPE_INTERFACE,                  /* bDescriptorType: Interface descriptor type */                                 \
    0X03,                                           /* bInterfaceNumber: Number of Interface */                                      \
    0x00,                                           /* bAlternateSetting: Alternate setting */                                       \
    0x02,                                           /* bNumEndpoints */                                                              \
    0x03,                                           /* bInterfaceClass: HID */                                                       \
    0x01,                                           /* bInterfaceSubClass : 1=BOOT, 0=no boot */                                     \
    0x00,                                           /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */                           \
    0, /* iInterface: Index of string descriptor */ /******************** Descriptor of Custom HID ********************/             \
    0x09,                                           /* bLength: HID Descriptor size */                                               \
    HID_DESCRIPTOR_TYPE_HID,                        /* bDescriptorType: HID */                                                       \
    0x11,                                           /* bcdHID: HID Class Spec release number */                                      \
    0x01,                                                                                                                            \
    0x00,                                              /* bCountryCode: Hardware target country */                                   \
    0x01,                                              /* bNumDescriptors: Number of HID class descriptors to follow */              \
    0x22,                                              /* bDescriptorType */                                                         \
    HID_CUSTOM_REPORT_DESC_SIZE,                       /* wItemLength: Total length of Report descriptor */                          \
    0x00,                                              /******************** Descriptor of Custom in endpoint ********************/  \
    0x07,                                              /* bLength: Endpoint Descriptor size */                                       \
    USB_DESCRIPTOR_TYPE_ENDPOINT,                      /* bDescriptorType: */                                                        \
    HID_IN_EP,                                         /* bEndpointAddress: Endpoint Address (IN) */                                 \
    0x03,                                              /* bmAttributes: Interrupt endpoint */                                        \
    WBVAL(HID_PACKET_SIZE),                            /* wMaxPacketSize: 4 Byte max */                                              \
    HIDRAW_INTERVAL, /* bInterval: Polling Interval */ /******************** Descriptor of Custom out endpoint ********************/ \
    0x07,                                              /* bLength: Endpoint Descriptor size */                                       \
    USB_DESCRIPTOR_TYPE_ENDPOINT,                      /* bDescriptorType: */                                                        \
    HID_OUT_EP,                                        /* bEndpointAddress: Endpoint Address (IN) */                                 \
    0x03,                                              /* bmAttributes: Interrupt endpoint */                                        \
    WBVAL(HID_PACKET_SIZE),                            /* wMaxPacketSize: 4 Byte max */                                              \
    HIDRAW_INTERVAL                                    /* bInterval: Polling Interval */
// clang-format on

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_1, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* Interface 0 */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x02),
    /* Endpoint OUT 2 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_OUT_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    /* Endpoint IN 1 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, DAP_PACKET_SIZE, 0x00),
#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
    HID_DESC(),
#endif
#if CONFIG_CHERRYDAP_USE_MSC
    MSC_DESCRIPTOR_INIT(MSC_INTF_NUM, MSC_OUT_EP, MSC_IN_EP, DAP_PACKET_SIZE, 0x00),
#endif
#if USBD_WEBUSB_ENABLE
    USB_INTERFACE_DESCRIPTOR_INIT(WEBUSB_INTF_NUM, 0x00, 0x00, 0xff, 0x00, 0x00, 0x04),
#endif
};

static const uint8_t other_speed_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* Interface 0 */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x02),
    /* Endpoint OUT 2 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_OUT_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    /* Endpoint IN 1 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, DAP_PACKET_SIZE, 0x00),
#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
    HID_DESC(),
#endif
#if CONFIG_CHERRYDAP_USE_MSC
    MSC_DESCRIPTOR_INIT(0x04, MSC_OUT_EP, MSC_IN_EP, DAP_PACKET_SIZE, 0x00),
#endif
#if USBD_WEBUSB_ENABLE
    USB_INTERFACE_DESCRIPTOR_INIT(WEBUSB_INTF_NUM, 0x00, 0x00, 0xff, 0x00, 0x00, 0x04),
#endif
};

/*!< custom hid report descriptor */
const uint8_t hid_custom_report_desc[HID_CUSTOM_REPORT_DESC_SIZE] = {
    /* USER CODE BEGIN 0 */
    0x06, 0x00, 0xff, /* USAGE_PAGE (Vendor Defined Page 1) */
    0x09, 0x01,       /* USAGE (Vendor Usage 1) */
    0xa1, 0x01,       /* COLLECTION (Application) */
    0x85, 0x02,       /*   REPORT ID (0x02) */
    0x09, 0x02,       /*   USAGE (Vendor Usage 1) */
    0x15, 0x00,       /*   LOGICAL_MINIMUM (0) */
    0x25, 0xff,       /*LOGICAL_MAXIMUM (255) */
    0x75, 0x08,       /*   REPORT_SIZE (8) */
    0x96, 0xff, 0x03, /*   REPORT_COUNT (1023) */
    0x81, 0x02,       /*   INPUT (Data,Var,Abs) */
    /* <___________________________________________________> */
    0x85, 0x01,       /*   REPORT ID (0x01) */
    0x09, 0x03,       /*   USAGE (Vendor Usage 1) */
    0x15, 0x00,       /*   LOGICAL_MINIMUM (0) */
    0x25, 0xff,       /*   LOGICAL_MAXIMUM (255) */
    0x75, 0x08,       /*   REPORT_SIZE (8) */
    0x96, 0xff, 0x03, /*   REPORT_COUNT (1023) */
    0x91, 0x02,       /*   OUTPUT (Data,Var,Abs) */

    /* <___________________________________________________> */
    0x85, 0x03,       /*   REPORT ID (0x03) */
    0x09, 0x04,       /*   USAGE (Vendor Usage 1) */
    0x15, 0x00,       /*   LOGICAL_MINIMUM (0) */
    0x25, 0xff,       /*   LOGICAL_MAXIMUM (255) */
    0x75, 0x08,       /*   REPORT_SIZE (8) */
    0x96, 0xff, 0x03, /*   REPORT_COUNT (1023) */
    0xb1, 0x02,       /*   FEATURE (Data,Var,Abs) */
    /* USER CODE END 0 */
    0xC0 /*     END_COLLECTION	             */
};

char serial_number_dynamic[36] = "DEADBEEF";  // Dynamic serial number

char *string_descriptors[] = {
    (char[]){0x09, 0x04},               /* Langid */
    "uDAP",                        /* Manufacturer */
    "uDAP CMSIS-DAP",              /* Product */
    "DEADBEEF", /* Serial Number */
    "uDAP WebUSB",
};

static const uint8_t device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_1, 0x00, 0x00, 0x00, 0x01),
};

__WEAK const uint8_t *device_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_descriptor;
}

__WEAK const uint8_t *config_descriptor_callback(uint8_t speed) {
    (void)speed;
    return config_descriptor;
}

__WEAK const uint8_t *device_quality_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_quality_descriptor;
}

__WEAK const uint8_t *other_speed_config_descriptor_callback(uint8_t speed) {
    (void)speed;
    return other_speed_config_descriptor;
}

__WEAK const char *string_descriptor_callback(uint8_t speed, uint8_t index) {
    (void)speed;

    if (index == 3) {
        return serial_number_dynamic;
    }

    if (index >= (sizeof(string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return string_descriptors[index];
}

static volatile uint16_t USB_RequestIndexI = 0;  // Request  Index In
static volatile uint16_t USB_RequestIndexO = 0;  // Request  Index Out
static volatile uint16_t USB_RequestCountI = 0;  // Request  Count In
static volatile uint16_t USB_RequestCountO = 0;  // Request  Count Out
static volatile uint8_t USB_RequestIdle = 1;     // Request  Idle  Flag

static volatile uint16_t USB_ResponseIndexI = 0;  // Response Index In
static volatile uint16_t USB_ResponseIndexO = 0;  // Response Index Out
static volatile uint16_t USB_ResponseCountI = 0;  // Response Count In
static volatile uint16_t USB_ResponseCountO = 0;  // Response Count Out
static volatile uint8_t USB_ResponseIdle = 1;     // Response Idle  Flag

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t USB_Request[DAP_PACKET_COUNT][DAP_PACKET_SIZE];   // Request  Buffer
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t USB_Response[DAP_PACKET_COUNT][DAP_PACKET_SIZE];  // Response Buffer
static uint16_t USB_RespSize[DAP_PACKET_COUNT];                                                         // Response Size

// COM 口现在是 USART4 (PB0=TX, PB1=RX) 的双向桥。
//
// 两个方向的缓冲大小取值不同，原因在 USB 端口层的完成判据
// （x035_usb_complete_noncontrol_out）：
//     complete = packet_len < mps || remaining == 0 || copy_len != packet_len
// 即 OUT 方向只有「收满请求长度」或「收到短包」才算完成。而 USB CDC 批量 OUT
// 没有长度字段，主机写 64 字节（正好等于端点 MPS）时既不是短包、也填不满更长的
// 请求长度，端点就会一直等下去 —— 回调不触发、数据不回环。
// 所以 OUT 的请求长度**必须**等于 MPS：这样每个包都让 remaining 归零立即完成，
// 任意主机写长度都会自然拆成 64 + 余数（余数 < MPS 同样是短包）。
//
// IN 方向相反，长度由设备决定，可以一次带多个包，所以用大缓冲减少 USB 往返。
#define CDC_OUT_BUFFER_SIZE DAP_PACKET_SIZE
#define CDC_IN_BUFFER_SIZE  512u

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_out_buffer[CDC_OUT_BUFFER_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_in_buffer[CDC_IN_BUFFER_SIZE];
static volatile struct cdc_line_coding g_cdc_lincoding;
static volatile uint8_t cdc_tx_busy;

// CDC OUT 背压标志：UART TX FIFO 余量不足时置位，表示 OUT 端点已停止重挂。
// 排空到能再容一整包后，由 chry_dap_cdc_bridge() 重新挂起。
static volatile uint8_t cdc_out_stalled;

// ---------------------------------------------------------------------------
// 关于 COM 口波特率：不再设上限
// ---------------------------------------------------------------------------
// 曾经这里有一个 2 Mbaud 的硬上限（用包装 CherryUSB 的类请求处理函数实现，
// 超限就 STALL）。撤掉的原因是两件事：
//
//   1. 拒绝在主机侧**不可观测**。实测（pyserial 3.5 / Windows 11）请求
//      2.5M / 3M / 4M 都能成功打开、不抛异常；usbser.sys 并没有把 STALL
//      变成 SetCommState 失败。设备保持上一次的速率，主机却以为自己设成了。
//      这种「静默不生效」比「跑一个可能丢数据的速率」更难排查。
//   2. 高波特率的实际可用性取决于**使用模式**而非波特率本身：只要发送是
//      「小载荷」或「分批、中间留空隙」，就不会触发 RX FIFO 溢出，
//      2 Mbaud 以上同样能零丢字节（实测 512 KiB @ 3M/6Mbaud、192 B 块全对）。
//
// 所以现在完全接受主机下发的速率。**但注意：并非「波特率越高越快」** ——
// 实测桥接的吞吐天花板约 190~200 KiB/s，正好落在 2 Mbaud，再往上提没有收益：
//   - 2 Mbaud 配大块（4 KiB）发送：~191 KiB/s，利用率 98%（已到顶）。
//   - 更高波特率必须把块压到 <=192 B 才不丢字节，等效速率反而降到 ~165 KiB/s。
// 天花板在本文件的 chry_dap_cdc_bridge()：IN 方向每次主循环只发起一次
// CDC IN 传输并等它完成，所以每秒能搬的字节数有上限（不是 USB 带宽不够）。
// 详见仓库根 readme.md 第 6.4 节。
//
// 精度与上限交给 UART 硬件与使用方把关：
//   - USART 是 16 倍过采样，BRR = 48 MHz / 波特率。BRR = 16（3 Mbaud）是
//     16 倍过采样的标称下限；BRR < 16 即 USARTDIV < 1，超出规范但实测仍可用。
//   - 请求的速率往往无法被 BRR 整除（如 2.5 M 实际 2.526 M，+1.05%），
//     接真实目标板时会带来累积偏差 —— 这是正常现象，设备侧无法检测。

// COM 口 <-> 目标串口的周期性服务，两个方向都在这一个函数里推进：
//   OUT 方向：CDC OUT 背压的恢复侧 —— TX FIFO 重新有一整包余量后重挂 OUT 端点
//   IN  方向：从 UART RX FIFO 取数据送 CDC IN
//
// 必须由主循环反复调用。OUT 那一支尤其不能省：USB 中断只在传输完成时触发，
// 端点既然没挂就永远不会再有中断，「FIFO 已排空」只能在主循环里察觉 ——
// 少了这一半，背压会把 OUT 端点永久停摆（比丢字节更糟）。
// IN 那一支不能在 UART 中断里做，因为 usbd_ep_start_write 不是中断安全的。
void chry_dap_cdc_bridge(void) {
    uint32_t length;

    // ---- OUT 方向：背压恢复 ----
    // 阀值取「一整包」而不是「>0」：OUT 端点每次完成最多交付
    // CDC_OUT_BUFFER_SIZE 字节，若只剩几字节空间就重挂，那一包仍会被丢掉一截。
    // 取整包余量可保证每次重挂后那一包必然完整入队，也即零丢字节。
    //
    // 说明：TXE 中断（优先级 2，低于 USB 的 1）只从 FIFO 取数据，因此从这里
    // 检查到回调真正写入之间，余量只会变大不会变小，该保证成立。
    if (cdc_out_stalled != 0U && drv_uart_tx_free() >= CDC_OUT_BUFFER_SIZE) {
        cdc_out_stalled = 0U;
        usbd_ep_start_read(0, CDC_OUT_EP, cdc_out_buffer, sizeof(cdc_out_buffer));
    }

    // ---- IN 方向：目标串口 -> COM 口 ----
    // 上一包还没发完就等下一轮，usbd_ep_start_write 不能重入
    if (cdc_tx_busy) {
        return;
    }

    // 批量取，一次 IN 传输尽量塞满，减少 USB 往返开销
    length = drv_uart_read(cdc_in_buffer, sizeof(cdc_in_buffer));
    if (length == 0U) {
        return;
    }

    cdc_tx_busy = 1U;
    if (usbd_ep_start_write(0, CDC_IN_EP, cdc_in_buffer, length) < 0) {
        cdc_tx_busy = 0U;
    }
}

void usbd_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;
    switch (event) {
        case USBD_EVENT_RESET:
            // 复位后端点重新枚举，发送状态一并清零
            cdc_tx_busy = 0U;
            break;
        case USBD_EVENT_CONNECTED:
            break;
        case USBD_EVENT_DISCONNECTED:
            break;
        case USBD_EVENT_RESUME:
            break;
        case USBD_EVENT_SUSPEND:
            break;
        case USBD_EVENT_CONFIGURED:
            /* setup first out ep read transfer */
            USB_RequestIdle = 0U;
            cdc_tx_busy = 0U;
            cdc_out_stalled = 0U;

            usbd_ep_start_read(0, DAP_OUT_EP, USB_Request[0], DAP_PACKET_SIZE);
            usbd_ep_start_read(0, CDC_OUT_EP, cdc_out_buffer, sizeof(cdc_out_buffer));

            break;
        case USBD_EVENT_SET_REMOTE_WAKEUP:
            break;
        case USBD_EVENT_CLR_REMOTE_WAKEUP:
            break;

        default:
            break;
    }
}

void dap_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) __attribute__((section(".highcode")));
void dap_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    if (USB_Request[USB_RequestIndexI][0] == ID_DAP_TransferAbort) {
        DAP_TransferAbort = 1U;
    } else {
        USB_RequestIndexI++;
        if (USB_RequestIndexI == DAP_PACKET_COUNT) {
            USB_RequestIndexI = 0U;
        }
        USB_RequestCountI++;
    }

    // Start reception of next request packet
    if ((uint16_t)(USB_RequestCountI - USB_RequestCountO) != DAP_PACKET_COUNT) {
        usbd_ep_start_read(0, DAP_OUT_EP, USB_Request[USB_RequestIndexI], DAP_PACKET_SIZE);
    } else {
        USB_RequestIdle = 1U;
    }
}

void dap_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) __attribute__((section(".highcode")));
void dap_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    if (USB_ResponseCountI != USB_ResponseCountO) {
        // Load data from response buffer to be sent back
        usbd_ep_start_write(0, DAP_IN_EP, USB_Response[USB_ResponseIndexO], USB_RespSize[USB_ResponseIndexO]);
        USB_ResponseIndexO++;
        if (USB_ResponseIndexO == DAP_PACKET_COUNT) {
            USB_ResponseIndexO = 0U;
        }
        USB_ResponseCountO++;
    } else {
        USB_ResponseIdle = 1U;
    }
}

void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes) __attribute__((section(".highcode"), noinline));
void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;

    // 主机 -> 目标串口：先写入 UART TX FIFO。
    // 走背压路径时本包必然放得下，故不会丢；若仍发生部分写入，
    // 说明有非预期的重入，会被 drv_uart_tx_overflow_count() 记下来。
    drv_uart_write(cdc_out_buffer, nbytes);

    // **背压**：只有 FIFO 除本包外还能再容一整包时才立刻重挂端点。
    // 余量不足就不重挂 —— 主机侧 bulk 写会一直被 NAK，于是它自然阻塞在
    // 串口波特率上，数据一个都不丢。旧行为是无条件重挂，USB 会以 MB/s 级
    // 速度灌满 FIFO，超出部分直接丢弃（实测：115200 下写 8192 B 丢 85%），
    // 主机侧等待等长回读的脚本因此永远等不齐 —— 表现为「一直跑不结束」。
    //
    // 代价：主机的一次大 write() 现在会阻塞到串口把数据排出去为止
    // （115200 下写 8 KiB 约 711 ms）。这是零丢字节换来的必然结果。
    if (drv_uart_tx_free() >= CDC_OUT_BUFFER_SIZE) {
        usbd_ep_start_read(0, CDC_OUT_EP, cdc_out_buffer, sizeof(cdc_out_buffer));
    } else {
        cdc_out_stalled = 1U;
    }
}

void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes) __attribute__((section(".highcode"), noinline));
void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;
    (void)nbytes;

    // 上一包发完，允许主循环取下一批串口数据
    cdc_tx_busy = 0U;
}

struct usbd_endpoint dap_out_ep = {
    .ep_addr = DAP_OUT_EP,
    .ep_cb = dap_out_callback};

struct usbd_endpoint dap_in_ep = {
    .ep_addr = DAP_IN_EP,
    .ep_cb = dap_in_callback};

struct usbd_endpoint cdc_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out};

struct usbd_endpoint cdc_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in};

#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
struct usbd_endpoint hid_custom_in_ep = {
    .ep_addr = HID_IN_EP,
    .ep_cb = usbd_hid_custom_in_callback,
};

struct usbd_endpoint hid_custom_out_ep = {
    .ep_addr = HID_OUT_EP,
    .ep_cb = usbd_hid_custom_out_callback,
};
#endif

struct usbd_interface dap_intf;
struct usbd_interface intf1;
struct usbd_interface intf2;
#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
struct usbd_interface hid_intf;
#endif

#if CONFIG_CHERRYDAP_USE_MSC
struct usbd_interface intf3;
#endif

struct usb_msosv2_descriptor msosv2_desc = {
    .vendor_code = USBD_WINUSB_VENDOR_CODE,
    .compat_id = USBD_WinUSBDescriptorSetDescriptor,
    .compat_id_len = USBD_WINUSB_DESC_SET_LEN,
};

struct usb_bos_descriptor bos_desc = {
    .string = USBD_BinaryObjectStoreDescriptor,
    .string_len = USBD_BOS_WTOTALLENGTH};

struct usb_webusb_descriptor webusb_url_desc = {
    .vendor_code = USBD_WEBUSB_VENDOR_CODE,
    .string = USBD_WebUSBURLDescriptor,
    .string_len = URL_DESCRIPTOR_LENGTH};

const struct usb_descriptor cmsisdap_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
    .bos_descriptor = &bos_desc,
    .msosv2_descriptor = &msosv2_desc,
    .webusb_url_descriptor = &webusb_url_desc};

void chry_dap_init(uint8_t busid, uint32_t reg_base) {
    DAP_Setup();

    usbd_desc_register(0, &cmsisdap_descriptor);

    /*!< winusb */
    usbd_add_interface(0, &dap_intf);
    usbd_add_endpoint(0, &dap_out_ep);
    usbd_add_endpoint(0, &dap_in_ep);

    /*!< cdc acm */
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf1));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf2));
    usbd_add_endpoint(0, &cdc_out_ep);
    usbd_add_endpoint(0, &cdc_in_ep);

#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
    /*!< hid */
    usbd_add_interface(0, usbd_hid_init_intf(0, &hid_intf, hid_custom_report_desc, HID_CUSTOM_REPORT_DESC_SIZE));
    hid_intf.notify_handler = hid_custom_notify_handler;
    usbd_add_endpoint(0, &hid_custom_in_ep);
    usbd_add_endpoint(0, &hid_custom_out_ep);
#endif

#if CONFIG_CHERRYDAP_USE_MSC
    usbd_add_interface(0, usbd_msc_init_intf(0, &intf3, MSC_OUT_EP, MSC_IN_EP));
#endif
    usbd_initialize(busid, reg_base, usbd_event_handler);
}

void chry_dap_handle(void) {
    uint32_t n;

    // Process pending requests
    while (USB_RequestCountI != USB_RequestCountO) {
        // Handle Queue Commands
        n = USB_RequestIndexO;
        while (USB_Request[n][0] == ID_DAP_QueueCommands) {
            USB_Request[n][0] = ID_DAP_ExecuteCommands;
            n++;
            if (n == DAP_PACKET_COUNT) {
                n = 0U;
            }
            if (n == USB_RequestIndexI) {
                // flags = osThreadFlagsWait(0x81U, osFlagsWaitAny, osWaitForever);
                // if (flags & 0x80U) {
                //     break;
                // }
            }
        }

        // Execute DAP Command (process request and prepare response)
        USB_RespSize[USB_ResponseIndexI] =
            (uint16_t)DAP_ExecuteCommand(USB_Request[USB_RequestIndexO], USB_Response[USB_ResponseIndexI]);

        // Update Request Index and Count
        USB_RequestIndexO++;
        if (USB_RequestIndexO == DAP_PACKET_COUNT) {
            USB_RequestIndexO = 0U;
        }
        USB_RequestCountO++;

        if (USB_RequestIdle) {
            if ((uint16_t)(USB_RequestCountI - USB_RequestCountO) != DAP_PACKET_COUNT) {
                USB_RequestIdle = 0U;
                usbd_ep_start_read(0, DAP_OUT_EP, USB_Request[USB_RequestIndexI], DAP_PACKET_SIZE);
            }
        }

        // Update Response Index and Count
        USB_ResponseIndexI++;
        if (USB_ResponseIndexI == DAP_PACKET_COUNT) {
            USB_ResponseIndexI = 0U;
        }
        USB_ResponseCountI++;

        if (USB_ResponseIdle) {
            if (USB_ResponseCountI != USB_ResponseCountO) {
                // Load data from response buffer to be sent back
                n = USB_ResponseIndexO++;
                if (USB_ResponseIndexO == DAP_PACKET_COUNT) {
                    USB_ResponseIndexO = 0U;
                }
                USB_ResponseCountO++;
                USB_ResponseIdle = 0U;
                usbd_ep_start_write(0, DAP_IN_EP, USB_Response[n], USB_RespSize[n]);
            }
        }

        // 长 DAP 命令（例如大块内存读写）会把主循环占住几百毫秒，期间 COM 口的
        // OUT 端点是停止重挂状态的话，主机的串口写就一直在被 NAK。这里每处理完
        // 一条命令就补一次桥接服务，让串口在大块 SWD 操作期间也能持续推进。
        chry_dap_cdc_bridge();
    }
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding) {
    (void)busid;
    (void)intf;

    // 行编码除了回存（主机打开串口时要能读回自己的配置），还用来设真实波特率。
    //
    // 不再对速率做上限检查（见文件上方「关于 COM 口波特率」的说明）：主机下什么
    // 就设什么，UART 硬件能产生多少就是多少。注意 USART 是 16 倍过采样，
    // BRR = 48 MHz / 波特率，标称上限 3 Mbaud；超过后 BRR 整数部分为 0，
    // 实际速率不再受控（≈48 MHz / BRR）。
    if (line_coding != NULL && line_coding->dwDTERate != 0U) {
        memcpy((uint8_t *)&g_cdc_lincoding, line_coding, sizeof(struct cdc_line_coding));
        drv_uart_set_baudrate(line_coding->dwDTERate);
    }
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding) {
    (void)busid;
    memcpy(line_coding, (uint8_t *)&g_cdc_lincoding, sizeof(struct cdc_line_coding));
}

#if CONFIG_CHERRYDAP_USE_MSC
#define BLOCK_SIZE  512
#define BLOCK_COUNT 10

typedef struct
{
    uint8_t BlockSpace[BLOCK_SIZE];
} BLOCK_TYPE;

BLOCK_TYPE mass_block[BLOCK_COUNT];

void usbd_msc_get_cap(uint8_t lun, uint32_t *block_num, uint16_t *block_size) {
    *block_num = 1000;  // Pretend having so many buffer,not has actually.
    *block_size = BLOCK_SIZE;
}
int usbd_msc_sector_read(uint32_t sector, uint8_t *buffer, uint32_t length) {
    if (sector < 10)
        memcpy(buffer, mass_block[sector].BlockSpace, length);
    return 0;
}

int usbd_msc_sector_write(uint32_t sector, uint8_t *buffer, uint32_t length) {
    if (sector < 10)
        memcpy(mass_block[sector].BlockSpace, buffer, length);
    return 0;
}
#endif
