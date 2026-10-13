## 15. WCH-Link 模式细节

WCH-Link 的实现移植自参考工程 `ch32x035-usb-wchlink-main` 的 `src/wchlink/**`
（含 6 个目标族：CH32X03X / CH32L10X / CH32V20X / CH32V30X / CH58X / CH59X）。

它与 CMSIS-DAP 都占用 PA2=SWCLK / PA3=SWDIO，所以**不能同时运行**；本固件把两套
代码都编进来，运行期用外接按键在两者间切换（见「双模式：模式切换按键与持久化」一节）。

没有任何构建选项能把它单独编出来 —— 编译即双模式。

WCH-Link 模式下的 USB 身份与 CMSIS-DAP 完全不同（这是主机端工具识别的**前提**，不是笔误）：

| 项 | WCH-Link | CMSIS-DAP |
| --- | --- | --- |
| VID / PID | `0x1a86` / `0x8010` | `0x0d28` / `0x0204` |
| 产品名 | `WCH-Link` | `uDAP CMSIS-DAP` |
| 序列号 | `035` + UID 前 9 个十六进制字符（`src/bsp/bsp_uid.c`） | `DEADBEEF` 或 ESIG UID |
| 接口 | 厂商接口（4 端点）+ CDC | WinUSB + CDC（+ MS OS 2.0） |
| 描述符 | `src/wchlink/usb/wchlink_usb.c` | `src/dap/dap_main.c` |

RVSWD 引脚与 USB 端口扩展：

| 项 | 取值 | 说明 |
| --- | --- | --- |
| SWCLK | PA2 | 与 CMSIS-DAP 同引脚，两种模式互斥使用 |
| SWDIO | PA3 | 同上 |
| 桥接串口 | USART4：PB0=TX、PB1=RX | 与 CMSIS-DAP 模式完全一致 |
| 端口扩展 | `src/port/usb_ch32x035_dc_usbfs.h` 的 `ch32x035_usbd_ep_abort_in()` | CherryUSB 没有「取消未被主机读取的 IN 传输」的语义，WCH-Link 需要它来放弃残留回复，否则主机会话会永久阻塞 |

#### 15.1 本板没有的硬件（都已替换为空实现）

参考工程的 link 板上有按键、SBU MUX、WS2816C 状态灯和目标供电开关，本板都没有。
**特别是 PB12：在本板是 `LED_CONNECTED_OUT`，在参考工程里却是目标电源开关** ——
同引脚、完全不同的含义。所以 `src/drv/drv_power_switch.c` 必须是空实现，
否则 WCH-Link 的 3.3V/5V 电源命令会去驱动本板的 DAP 连通指示灯：

| 参考工程硬件 | 本工程处理 |
| --- | --- |
| 按键（PA5）+ SBU MUX（PA0/PA4） | 未移植（`src/wchlink/` 不依赖它们） |
| WS2816C 状态灯（PA7） | `src/status/status_led.c` 空实现 |
| 目标供电开关（PB12） | `src/drv/drv_power_switch.c` 空实现 |
| D+ 外部上拉控制（PB11） | 未移植 |

因此 WCH-LinkUtility / MRS 下发的 3.3V/5V 电源命令会**成功但不生效**，
目标供电完全取决于板子自身的接线。

#### 15.2 COM 口（CDC）在 WCH-Link 模式下的行为

参考工程的 CDC 是「USB 进什么就原样回什么」的自测回环；本工程把它换成了与 CMSIS-DAP
模式**完全相同**的 USART4（PB0=TX / PB1=RX）双向桥，含 TX FIFO 背压，波特率跟随主机的
CDC 行编码。改动集中在 `src/wchlink/usb/wchlink_usb.c` 的
`wchlink_cdc_out_callback()`（中断里写 UART，按 `drv_uart_tx_free()` 做背压）、
`wchlink_cdc_service()`（主循环恢复背压并取 UART RX 送 CDC IN）与
`usbd_cdc_acm_set_line_coding()`（`dwDTERate` → `drv_uart_set_baudrate()`）。

#### 15.3 EP 的 data toggle（已实测通过，保留说明备查）

WCH-Link 的厂商接口会在同一个端点上传 256 B 的多包传输（4 × 64 B），这依赖设备侧
DATA0/DATA1 切换与主机完全同步。本工程的 USB 端口层（`src/port/`）是**手动**翻转
toggle，而来源工程的端口层对 EP1~EP3 改用了硬件自动翻转
（`USBFS_UEP_R_AUTO_TOG` / `USBFS_UEP_T_AUTO_TOG`），并额外提供
`ch32x035_usbd_set_ep_out_toggle_resync()` 用于「跨主机会话重同步」——
两者的注释都指向「主机中途放弃传输后 toggle 失配」这一类场景。

**实测结论：这个风险没有出现。** 连续两次 dump 目标 Flash 16 KiB
（= 64 次 256 B 传输 = 256 个 64 B 包）得到**完全相同的 SHA256**
（`DEC4A1CF9DA27C56605B27FB9221557A20100F40FE2E5CF0ADCC4B9F1070BF73`），
且读写都无重试、无报错，见「上板验证」一节。
所以本工程**没有**改动端口层的端点时序（避免影响已经实测通过的 CMSIS-DAP 路径）。

若将来在别的目标/主机上遇到多包传输偶发卡死或丢包，第一个要查的仍是这里：
可以参照 `firmware/ch32x035-usb-wchlink-main/third_party/cherryusb_port/usb_ch32x035_dc_usbfs.c`
把 EP1~EP3 换成硬件自动翻转，并补上 toggle 重同步接口。

#### 15.4 WCH 官方的两种 Link 模式：切换 = 换固件（与本固件的按键切换无关）

WCH 官方 Link 有两套互斥的"模式"，**本工程只实现了 RISC-V（RVSWD）这一套**。
原因是官方那套切换并不是改一个状态位，而是**替换 link 自己的固件**：

| | 官方 Link 做法 | 本固件对应 |
| --- | --- | --- |
| **RISC-V 模式** | 往 link 写 `WCH-Link_APP_IAP_RV.bin` | WCH-Link 模式（USB 身份 `1a86:8010` 厂商协议）|
| **ARM / DAP 模式** | 往 link 写 `WCH-Link_APP_IAP_ARM.bin`、`WCH-DAPLink_APP_IAP.bin` 或 `FIRMWARE_DAP_CH549.bin` | CMSIS-DAP 模式（USB 身份 `0d28:0204` **CMSIS-DAP**）|
| **切换动作** | 进 IAP → 写另一份固件 → 重启（换固件） | PA4 按键 → 反初始化 USB → 换描述符重新枚举（**同一份固件**，见下节）|

证据：

- OpenOCD 的配置文件本身就分成两套（MounRiver Studio 2 的 `OpenOCD/bin/`）：
  - `wch-riscv.cfg` → `adapter driver wlinke` + `transport select sdi`（RV 模式，DCK+DIO 两线）
  - `wch-arm.cfg` → **`adapter driver cmsis-dap`** + `transport select swd`
    即 **ARM/DAP 模式的 link 对主机就是一个 CMSIS-DAP 设备** —— 正好等于本固件的 CMSIS-DAP 模式（双模式固件默认进入的那一套）。
- WCH-LinkUtility 自带的固件与字符串（`Others/SWDTool/default/`）：
  - `Firmware_Link/WCH-Link_APP_IAP_RV.bin`、`WCH-Link_APP_IAP_ARM.bin`、`WCH-DAPLink_APP_IAP.bin`、`FIRMWARE_DAP_CH549.bin`
  - `wchlink.wcfg`：`CH549Ver_RV=32` / `CH549Ver_ARM=31`（两种模式是两份固件、两个版本号）
  - exe 内字符串：`切换WCH-Link模式...`、`切换WCH-Link到IAP升级模式...`、
    `成功:当前WCH-Link已处于RISC-V模式!` / `...ARM-WINUSB模式!` / `...ARM-HID模式!`、`当前WCH-Link模式:`

本工程在协议层实现的正是这个流程的**入口**（`src/wchlink/protocol/wchlink_wire.h` 的
`WCHLINK_FAMILY_DEVICE_MODE = 0x0f`，处理在 `src/wchlink/session/wchlink_command.c:29`）：

| 子命令 | 本固件行为 |
| --- | --- |
| `0x02` Query（`WCHLINK_DEVICE_MODE_QUERY`）| 恒回 `0x02` ＝「当前处于 RISC-V 调试模式」。OpenOCD 打印的 `mode:RV` 与 MRS 的识别都来自这里 |
| `0x01` IAP（`WCHLINK_DEVICE_MODE_IAP`）| 不回复协议帧，改为 `bsp_system_enter_isp()`（`src/wchlink/usb/wchlink_usb.c:562`）→ 复位进 CH32X035 的 ISP bootloader |
| 其它 | 回 `unsupported` |

也就是说：**WCH 官方"切模式"动作的钩子（IAP）是有的，真正换固件是主机侧完成的**，
收到 IAP 命令时本固件只会把自己复位进 ISP。本固件自己的模式切换不走这条路
（见下节：按键 + 重新枚举 USB，不经过 IAP）。

> ⚠️ **注意一个真实风险**：烧了本固件之后，如果在 WCH-LinkUtility 里点「切换模式」并选
> ARM/DAP，官方工具会先走 IAP（本固件会配合复位进 bootloader），然后**把 WCH 官方的
> ARM/DAP 固件写进去，覆盖本固件**。这是符合官方设计的行为，不是 bug；想回到本固件
> 就重新烧一次 `firmware.bin` 即可。要在本固件内部切到 CMSIS-DAP 模式，用 PA4 按键，
> 不要用官方工具的「切换模式」。
