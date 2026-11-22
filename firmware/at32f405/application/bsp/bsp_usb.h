#pragma once

/*
 * USB（OTGHS，480 Mbps 高速口）的时钟与中断接线。
 *
 * AT32F405 有两个 USB 控制器：
 *   OTGHS  带片上 HS PHY，D-/D+ 是芯片专用引脚（QFN32 的 18/19 脚），
 *          必须由 12 MHz HEXT 提供 PHY 参考时钟，USB 内核时钟 48 MHz 来自 PLLU；
 *   OTGFS1 全速口，D-/D+ 复用 PA11/PA12，可以不用晶振。
 *
 * 本板用 OTGHS：板上有 12 MHz 晶振，CherryUSB 的 dwc2 端口也已经带好了
 * AT32F402/405 的 HS 参数（`port/dwc2/usb_glue_at.c` 的 param_pb14_pb15，
 * `dwc2_get_user_params()` 里用 `reg_base == OTGHS_BASE` 选择）。
 *
 * 调用顺序：system_clock_config() -> bsp_usb_clock_init() -> usbd_initialize()。
 */

/* 打开 OTGHS / PHY 时钟，选 48 MHz 与 12 MHz 时钟源，使能 OTGHS 中断 */
void bsp_usb_clock_init(void);

/* 48 MHz 时钟已经就绪（调试用） */
int bsp_usb_clock_ready(void);
