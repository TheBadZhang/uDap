#pragma once

/*
 * CDC ACM 行编码 / 控制线的**共用**实现。
 *
 * CherryUSB 的 class/cdc/usbd_cdc_acm.c 把这 4 个回调声明成 __WEAK 空实现，
 * 应用侧覆盖它们。原先 CMSIS-DAP（src/dap/dap_main.c）与 WCH-Link
 * （src/wchlink/usb/wchlink_usb.c）各写了一份 —— 两者行为本来就一致
 * （存行编码 + 把 dwDTERate 下发给 drv_uart），但没有共用。
 *
 */

// 默认行编码：115200 8N1（与 drv_uart 的默认波特率一致）
void drv_cdc_line_init(void);
