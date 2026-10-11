# CH32X035F8U DAP（uDap 固件）

CH32X035F8U 上的调试器固件，xmake + WCH RISC-V GCC 构建。固件固定为**双模式**：
**CMSIS-DAP** 与 **WCH-Link**（RVSWD 目标调试 / Flash 烧录）两套调试器代码都编入
同一份固件，运行期用外接按键切换（两者都用 PA2/PA3，不可能同时运行）。COM 口
（CDC <-> USART4 桥）在两种模式下行为一致。

## 目录结构

本仓库根目录为 `uDap2/`，固件工程根是 **`firmware/`**（`xmake.lua` 所在层级）。

```
uDap2/
├── firmware/               ← 固件工程根（xmake 工程）
│   ├── src/                本工程代码（应用 + 移植 glue）
│   │   ├── main.c          应用入口（双模式：CMSIS-DAP / WCH-Link 运行期切换）
│   │   ├── system_ch32x035.c  时钟与系统初始化
│   │   ├── ch32x035_it.c   中断实现
│   │   ├── ch32x035_conf.h SDK 配置
│   │   ├── usb_config.h    CherryUSB 应用侧配置（必选）
│   │   ├── bsp/
│   │   │   ├── bsp_usb.c       CH32X035 USBFS 硬件初始化（必选）
│   │   │   ├── bsp_delay.c     SysTick 延时/时间 + Delay_Init/Us/Ms 兼容层
│   │   │   ├── bsp_print.c     newlib retarget（_write / _sbrk）
│   │   │   ├── bsp_system.c    进 ISP 复位
│   │   │   └── bsp_uid.c       读芯片 UID（WCH-Link 序列号用）
│   │   ├── port/
│   │   │   ├── usb_ch32x035_dc_usbfs.c   CherryUSB 的 CH32X035 USBFS 设备端口 glue（必选）
│   │   │   └── usb_ch32x035_dc_usbfs.h   端口扩展（IN 传输取消，WCH-Link 用）
│   │   ├── dap/            CMSIS-DAP glue
│   │   │   ├── DAP_config.h    SWD / nRESET 引脚与时序配置
│   │   │   ├── dap_main.c      USB 描述符、WinUSB/MSOS、DAP 与 CDC 端点分发
│   │   │   └── sw_dp.c         SWD 位翻转实现（CherryDAP 的传输后端）
│   │   ├── wchlink/        WCH-Link glue（与 dap 共存于同一份固件，见「双模式」）
│   │   │   ├── transport/      RVSWD 物理层（PA2/PA3 位翻转）与帧收发
│   │   │   ├── protocol/       WCH-Link USB 命令帧
│   │   │   ├── rvswd/          RISC-V DM 调试原语（寄存器/内存/复位）
│   │   │   ├── target/         目标芯片族适配（X03X/L103/V20X/V30X/CH58X/CH59X）
│   │   │   ├── flash/          各族 Flash 烧录与 Option Bytes
│   │   │   ├── session/        命令会话与数据传输状态机
│   │   │   └── usb/            厂商接口 + CDC 的 USB 适配
│   │   ├── status/
│   │   │   └── status_led.c    状态灯接口（本板无灯，空实现）
│   │   └── drv/
│   │       ├── drv_uart.c      CDC <-> USART4 桥（CherryRB FIFO）
│   │       ├── drv_mode_button.c  模式切换按键（PA4）
│   │       ├── drv_mode_store.c   模式持久化（用户选择字 Data0）
│   │       └── drv_power_switch.c  目标电源开关接口（本板无开关，空实现）
│   ├── sdk/                WCH CH32X035 SDK（Core / Peripheral / Debug / Startup / Ld / EXAM）
│   ├── third_party/        仅上游依赖，全部为 submodule
│   │   ├── cherryusb       https://github.com/cherry-embedded/CherryUSB.git
│   │   ├── cherryrb        https://github.com/cherry-embedded/CherryRB.git
│   │   └── cherrydap       https://github.com/cherry-embedded/CherryDAP.git
│   ├── toolchains/wch-riscv/  工具链探测（WCH_TOOLCHAIN_ROOT 或 MounRiver 默认路径）
│   ├── docs/               性能文档
│   ├── tools/              测速 / 串口测试脚本
│   ├── xmake.lua           构建脚本
│   └── build.ps1           便捷构建脚本
├── hardware/               KiCad 工程（uDap.kicad_sch / uDap.kicad_pcb）
└── software/               上位机等
```

`firmware/third_party/` 只放上游 submodule（**不改动 submodule 内容**）；针对本工程的
移植层（glue）放在 `firmware/src/port/`、`src/bsp/`、`src/dap/`、`src/wchlink/`、
`src/status/`、`src/drv/` 与 `src/usb_config.h`。

`src/wchlink/` 移植自参考工程 `firmware/ch32x035-usb-wchlink-main/`（仅作移植来源，
不参与本工程编译）。

submodule 的 git 管理数据在 `.git/modules/firmware/third_party/`，与工作区路径一一对应；
若以后再次整体移动工程目录，记得同步修正各 submodule 的 `.git` 文件与
`.git/modules/.../config` 里的 `core.worktree`。

## USB（必选）

本芯片必然使用 USB，所以以下三部分在 `xmake.lua` 中**无条件编译**，缺文件直接报错
（`USB is mandatory: missing ...`），不会默默编出一个没有 USB 的固件：

| 部分 | 位置 |
| --- | --- |
| CherryUSB 初始化配置 | `src/usb_config.h` |
| CherryUSB 核心 + CDC ACM 类 | `third_party/cherryusb/`（submodule） |
| CH32X035 USBFS 设备端口 glue | `src/port/usb_ch32x035_dc_usbfs.c` |
| CH32X035 USB 硬件初始化 | `src/bsp/bsp_usb.c` |

注意：CherryUSB 的端口层把 `usb_dc_low_level_init()` / `usb_dc_low_level_deinit()` 声明为
`__WEAK` 空实现，**USB 时钟、PC16/PC17、AFIO PHY、NVIC 必须由应用侧提供**，否则
`usbd_initialize()` 之后 USB 既不枚举也不进中断。`src/bsp/bsp_usb.c` 覆盖了这两个弱函数，
并按 WCH 官方例程区分供电电压：

- 5V 供电：关闭 `PHY_V33`，UDP 内部上拉 10K；
- 3.3V 供电：使能 `PHY_V33`，UDP 内部上拉 1.5K（USB 2.0 全速规范要求）。

调用链是 `usbd_initialize()` → `usb_dc_init()` → `usb_dc_low_level_init()`，因此应用侧必须先
调用 `NVIC_PriorityGroupConfig()` 设定优先级分组。

## CMSIS-DAP + WCH-Link（双模式）

`xmake.lua` **不提供裁剪选项**：CMSIS-DAP（CherryDAP + `src/dap`、`src/drv` glue）与
WCH-Link（`src/wchlink/`，RVSWD 目标调试 / Flash 烧录）两套代码强制同时编入，
产出的一律是运行期按键切换的双模式固件。两者都要占用 PA2=SWCLK / PA3=SWDIO，
USB 身份也不同（见下面「双模式」一节），所以不存在「只编其中一个」的构建。

`src/main.c` 在两种模式间轮转；两种模式**各自私有**的代码与数据被链接到同一段
RAM（overlay），这是两套代码能同时装进 20 KiB RAM 的原因。

实测尺寸（release）：

| 配置 | Flash | RAM |
| --- | --- | --- |
| 默认（`-EsigSn y`，序列号用芯片 ESIG UID） | 58.67 KiB | 17.23 KiB |
| `-EsigSn n`（序列号固定 `DEADBEEF`） | 58.53 KiB | 17.23 KiB |

上限：Flash 63232 B（`62K - 256`，末尾 256 B 留给 NV 配置），RAM 20480 B。
Flash 余量约 3.2 KiB，RAM 余量约 2.8 KiB。

构建开始时会校验两套模式的关键文件（`src/dap/DAP_config.h`、`dap_main.c`、`sw_dp.c`、
`src/wchlink/` 的若干文件、`drv_mode_button.c` 等）是否齐全，缺文件直接报错，
不会默默编出一个少了一半功能的固件。

> **必须用 `-Os`**，这是能装进 62 KiB / 20 KiB 的前提。
> `sdk/Ld/Link_highcode_nv_256B_dual.ld` 的 `.highcode` 是 `> RAM AT > FLASH`（启动时
> 从 Flash 搬到 RAM 执行），而 USB 端口层把 `usbd_ep_start_read/write` 与
> `USBFS_IRQHandler` 放进了 `.highcode`。用 `-O3` + LTO 时整条 USB 调用链会被内联进
> 中断处理函数，`.highcode` 从 5.4 KiB 膨胀到 18.5 KiB，同时吃光 Flash（溢出
> 11100 字节）和 RAM（溢出约 10.6 KiB）—— 实测链接直接失败。参考工程
> `ch32x035-usb-wchlink-main` 同样使用 smallest。
>
> **debug 模式（`-Mode debug`）同样是 `-Os` + LTO**：`-Og` / `-O0` 下两套代码 +
> overlay 装不进 Flash（`.data` 直接报 region overflow），所以 debug 只额外保留调试
> 符号，不降优化。

链接脚本的 `OVERLAY` 段 LMA 由 ld 单独排布，**不受 `FLASH` 区域长度约束**，超出也不会
报链接错误，只会静默生成一个装不进芯片的 `.bin`。因此 `xmake.lua` 在 `after_build`
里按 `.bin` 实际长度兜底检查，超过 63232 B 直接报错并终止构建。

### 序列号：固定值 / 芯片 ESIG UID

**默认**取芯片 ESIG 区的 **96 位出厂 UID**（出厂烧录、只读、每颗芯片唯一）作 USB
序列号，转成 24 个大写十六进制字符 —— 多块板子同时插在一台机器上时才能区分。
`-EsigSn n` 退回所有板子相同的固定串 `DEADBEEF`（VID:PID 也相同，无法区分）：

```powershell
.\build.ps1               # 默认：序列号 = 芯片 UID（-EsigSn y）
.\build.ps1 -EsigSn n     # 退回固定 DEADBEEF

xmake f -m release -y     # xmake 侧默认同样是 esig_sn
```

**寄存器定义**（代码在 `src/dap/dap_main.c`，见 CH32X035 应用手册第 19 章
「电子签名（ESIG）」）：

| 地址 | 名称 | 内容 |
| --- | --- | --- |
| `0x1FFFF7E8` | `R32_ESIG_UNIID1` | `U_ID[31:0]` |
| `0x1FFFF7EC` | `R32_ESIG_UNIID2` | `U_ID[63:32]` |
| `0x1FFFF7F0` | `R32_ESIG_UNIID3` | `U_ID[95:64]` |

拼接顺序为 UNIID1 → UNIID2 → UNIID3（与沁恒例程的打印顺序一致；顺序只是约定，
不影响唯一性）。同一份字符串同时供两处使用：

- USB 描述符的 iSerialNumber（`dap_main.c` 的 `string_descriptors[3]`）
- CMSIS-DAP 的 `DAP_GetSerNum` 命令（`src/dap/DAP_config.h` 的 `DAP_GetSerNumString()`）

两点注意：

1. **打开后会改变主机侧的设备实例**。USB 序列号参与 Windows 的设备枚举与驱动匹配，
   换了序列号等于换了一台设备，首次插入需要重新匹配一次驱动（本设备是 WinUSB，
   靠 MS OS 2.0 描述符自动匹配，无需手工安装）。原来绑定 `DEADBEEF` 实例的
   OpenOCD / pyOCD 配置需要用新序列号。
2. **UID 读不出来时会退回固定串**。若三个寄存器读回全 0 或全 FF（地址不对、
   ESIG 不可读等），固件不会把「看似唯一、其实所有板子都相同」的串当序列号发出去，
   而是退回 `DEADBEEF`，让「没生效」在主机侧可见。（本工程 `_write` 是空实现，
   没有日志可用，所以只能靠这个现象判断。）

`--esig_sn=y` 只影响 CMSIS-DAP 模式的 USB 序列号（在 `src/dap/dap_main.c` 里设置）；
双模式固件总是编入 CMSIS-DAP，所以这个选项永远有效，不存在「依赖被裁掉的模式」的问题。

### 与参考工程的两处必要差异

这两处都是参考工程 `ch32x035-usb-dap-main` 里不存在的构建陷阱，已在本工程解决：

1. **`bool` 未定义**：CherryUSB 的 `class/cdc/usbd_cdc_acm.h` 用了 `bool` 但自己没包含
   `<stdbool.h>`，而 `dap_main.h` 里 `usbd_cdc.h` 排在 `usbd_core.h`（内含 `<stdbool.h>`）
   之前。参考工程是**直接改了 cherryusb 子模块**（它的子模块处于 dirty 状态），
   本工程不改 submodule，把 `#include <stdbool.h>` 放在自己的 `src/dap/dap_main.h` 里。
   **升级 cherryusb submodule 时需重新确认这一点是否已在上下游修好。**
2. **`Delay_Init` / `Delay_Us` 未定义**：本工程不编译 `sdk/Debug/debug.c`
   （它无条件定义 `_write` / `_sbrk`，与 `src/bsp/bsp_print.c` 的 retarget 冲突），
   但本 SDK 版本的 `sdk/Peripheral/src/ch32x035_pwr.c` 里
   `PWR_VDD_SupplyVoltage()` 会调用这两个函数（参考工程用的旧版 SDK 没有这两行，
   而且它也从不调用该函数，所以没暴露问题）。已在 `src/bsp/bsp_delay.c` 末尾提供
   转发到 SysTick 延时 的等价实现。

## WCH-Link（RVSWD）

WCH-Link 的实现移植自参考工程 `ch32x035-usb-wchlink-main` 的 `src/wchlink/**`
（含 6 个目标族：CH32X03X / CH32L10X / CH32V20X / CH32V30X / CH58X / CH59X）。

它与 CMSIS-DAP 都占用 PA2=SWCLK / PA3=SWDIO，所以**不能同时运行**；本固件把两套
代码都编进来，运行期用外接按键在两者间切换（见下面「双模式」一节）。

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

### 本板没有的硬件（都已替换为空实现）

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

### COM 口（CDC）在 WCH-Link 模式下的行为

参考工程的 CDC 是「USB 进什么就原样回什么」的自测回环；本工程把它换成了与 CMSIS-DAP
模式**完全相同**的 USART4（PB0=TX / PB1=RX）双向桥，含 TX FIFO 背压，波特率跟随主机的
CDC 行编码。改动集中在 `src/wchlink/usb/wchlink_usb.c` 的
`wchlink_cdc_out_callback()`（中断里写 UART，按 `drv_uart_tx_free()` 做背压）、
`wchlink_cdc_service()`（主循环恢复背压并取 UART RX 送 CDC IN）与
`usbd_cdc_acm_set_line_coding()`（`dwDTERate` → `drv_uart_set_baudrate()`）。

### EP 的 data toggle（已实测通过，保留说明备查）

WCH-Link 的厂商接口会在同一个端点上传 256 B 的多包传输（4 × 64 B），这依赖设备侧
DATA0/DATA1 切换与主机完全同步。本工程的 USB 端口层（`src/port/`）是**手动**翻转
toggle，而来源工程的端口层对 EP1~EP3 改用了硬件自动翻转
（`USBFS_UEP_R_AUTO_TOG` / `USBFS_UEP_T_AUTO_TOG`），并额外提供
`ch32x035_usbd_set_ep_out_toggle_resync()` 用于「跨主机会话重同步」——
两者的注释都指向「主机中途放弃传输后 toggle 失配」这一类场景。

**实测结论：这个风险没有出现。** 连续两次 dump 目标 Flash 16 KiB
（= 64 次 256 B 传输 = 256 个 64 B 包）得到**完全相同的 SHA256**
（`DEC4A1CF9DA27C56605B27FB9221557A20100F40FE2E5CF0ADCC4B9F1070BF73`），
且读写都无重试、无报错，见下方「验证 → WCH-Link 模式」。
所以本工程**没有**改动端口层的端点时序（避免影响已经实测通过的 CMSIS-DAP 路径）。

若将来在别的目标/主机上遇到多包传输偶发卡死或丢包，第一个要查的仍是这里：
可以参照 `firmware/ch32x035-usb-wchlink-main/third_party/cherryusb_port/usb_ch32x035_dc_usbfs.c`
把 EP1~EP3 换成硬件自动翻转，并补上 toggle 重同步接口。

### WCH 官方的两种 Link 模式：切换 = 换固件（与本固件的按键切换无关）

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

## 双模式（CMSIS-DAP + WCH-Link，运行期按键切换）

本固件**固定**把两套调试器代码都编进同一份固件，运行期用外接按键在两种模式间切换。
当前模式会写进芯片的**用户选择字**，下次上电自动恢复；全新芯片（或记录无效时）默认进
CMSIS-DAP。

```powershell
.\build.ps1        # 直接构建；没有「只编一个模式」的选项
```

**两种模式不可能同时运行**：它们都要占用 PA2=SWCLK/DCK、PA3=SWDIO/DIO（同一对物理
引脚），而且 USB 身份完全不同（`0d28:0204` WinUSB vs `1a86:8010` 厂商接口），主机侧
本来就当成两台不同设备。所以任意时刻只有一套在跑，切换 = 反初始化 USB + 用另一套
描述符重新枚举（`usbd_deinitialize` → `usbd_desc_register` → `usbd_initialize`；
`usbd_desc_register` 内部会 `memset` 整个 `g_usbd_core`，反复切换不会累积状态）。

### 模式切换按键

本板（uDap2）**没有按键**，需要外接一个：一端接 **PA4**，另一端接 **GND**。
内部上拉已使能，所以按下 = 低电平。没有按键时用镊子/杜邦线把 PA4 短到 GND 也可以。

PA4 的选择依据（CH32X035F8U 是 QFN-20，全部引脚已核对）：

| 状态 | 引脚 |
| --- | --- |
| 已占用 | PA2/PA3 = SWCLK/SWDIO，PA5 = nRESET，PB0/PB1 = USART4，PB12 = 状态灯，PC16/PC17（即 UDM/UDP）= USB |
| **空闲可选** | **PA4**、PA0、PA1、PA6、PA7、PB3、PB11、PC18、PC19 |

选 PA4 是因为它与 SWD/nRESET 同在 GPIOA，便于在同一排就近引线；参考工程里 PA4 是
SBU MUX 的交换控制，而本板没有 MUX，所以确定空闲。**要改引脚只改
`src/drv/drv_mode_button.h` 里的 4 个宏。**

去抖 20 ms，且切换后会等按键释放，同一次按住不会来回抖动切换。

### 模式持久化（用户选择字 Data0）

每次按键切换后，新选的模式会写进 CH32X035 的**用户选择字**，下次上电由
`src/drv/drv_mode_store.c` 读回并直接进入该模式。没有有效记录（擦除态 / 反码校验
失败 / 取值非法）时回退到默认的 CMSIS-DAP。

依据 CH32X035 应用手册 20.5 节「用户选择字」：信息块共 8 字节，其中 `Data0`/`Data1`
是 2 字节用户自定义数据，地址 `0x1FFFF804` 的 32 位字排布为
`[31:24]=nData1 [23:16]=Data1 [15:8]=nData0 [7:0]=Data0`（每个字节都带反码用于装载
校验）。本工程只用 `Data0`：`0x00 = CMSIS-DAP`、`0x01 = WCH-Link`，`Data1` 及
RDPR/USER/WRPR 全部原样保留。模式枚举 `firmware_mode_t` 定义在
`src/drv/drv_mode_store.h`，`main.c` 与持久化共用同一套取值。**再增加模式时改两处**：
在该枚举末尾追加成员（已有取值会写进 Flash，属于 ABI，不能重排；取值须从 0 起连续），
再在 `main.c` 的 `mode_run()` 里加一个 `case` —— 主循环用 `(mode + 1) % FIRMWARE_MODE_COUNT`
轮转，有效性判断也自动落在新增成员上，无需再改别处。

> ⚠️ 注意：用户选择字与读保护 `RDPR`、`USER` 配置字、写保护 `WRPR0..3` 同处
> `0x1FFFF800` 开始的同一个 256 B 擦除页。编程前必须整区擦除，所以保存时会把
> RDPR/USER/WRPR 原样回写 —— 否则会改变读保护状态（可能导致整片擦除甚至读保护）。
> 选项字节的擦写寿命有限，且擦/写过程中掉电可能损坏选择字，所以驱动只在值真的变化时
> 才擦写，并在编程后回读校验。若要频繁切换模式又担心寿命，可改用链接脚本预留的
> 末尾 256 B NV 页（见下文）而不是选项字节。

### RAM overlay：两种模式复用同一段 RAM

两套代码全编进来时 Flash 够用，但 RAM 需要约 25.3 KiB 而只有 20 KiB。解决办法是把
**只有某一个模式用得到**的私有**数据**放进同一段 RAM 地址（它们不同时运行），用
ld 的 `OVERLAY` 命令实现（见 `sdk/Ld/Link_highcode_nv_256B_dual.ld`）：

| 段 | 内容 | 实测大小 | 位置 |
| --- | --- | --- | --- |
| `.highcode` | **共用**：USB 端口层热路径 | 约 3.1 KiB | RAM（启动时拷入）|
| `.highcode.dap` | 仅 CMSIS-DAP：`sw_dp.c`/`dap_main.c` 热路径 | 约 2.4 KiB | RAM（并进 `.highcode`）|
| `.overlay.dap` | 仅 CMSIS-DAP：USB 收发缓冲 | 约 1.6 KiB | RAM，overlay |
| `.text` 里的 `.highcode.wchlink` | 仅 WCH-Link：`rvswd_phy_gpio.c` 位翻转 | 约 2.0 KiB | **Flash（XIP）** |
| `.overlay.wchlink` | 仅 WCH-Link：会话/传输状态与大缓冲 | 约 6.0 KiB | RAM，overlay |
| `.bss` | 共用：USB 核心、端口状态、UART FIFO 等 | 约 4.9 KiB | RAM |

**只有数据还参与 overlay**：两个数据段的 VMA **相同**（取较大的 6.0 KiB 作窗口），
因为数据不能放 Flash（要写），而两模式的数据又互斥 —— 重叠才省 RAM。

代码不再要求运行期拷贝：

- CMSIS-DAP 的热路径（`.highcode.dap`）并进常驻 `.highcode`，由
  `startup_ch32x035_highcode.S` 在**启动时**拷一次；
- WCH-Link 的热路径（`.highcode.wchlink`）直接编在 Flash 上执行（XIP）；
- 于是 `main()` 进模式时**只剩清零数据段**（`_xxx_overlay_bss_*`），没有代码拷贝。

> ⚠️ 代价：WCH-Link 的位翻转代码在 Flash 上执行，而 48 MHz 下 Flash 需要
> **2 个等待周期**（`FLASH_ACTLR_LATENCY_2`，见 `src/system_ch32x035.c` 的
> `SetSysClockTo48_HSI`），所以 RVSWD 的时钟会比放在 RAM 时低。这是刻意的取舍 ——
> 换来省掉运行期代码拷贝、并让 RAM 完全用不着为 WCH-Link 的代码留窗口。

注意事项（改代码时要留意）：

- 数据段的清零**必须由 `main()` 完成**，不能放进 `cherrydap_process()` 之类的
  函数里 —— 那个函数自己就在被覆盖的地址范围内，调用即等于跳进未初始化的地址。
- 模式退出时先返回 `main()`（并在那里 `usbd_deinitialize`），确认不再执行该段里的
  代码后，才允许覆盖那段 RAM。
- 往任一模式里新加**较大的静态缓冲**时，如果想让它也参与复用，需要给它加上
  `DAP_PRIVATE_SECTION` / `WCHLINK_PRIVATE_SECTION`（见 `src/mode_overlay.h`）。
  不加也能工作，只是不省 RAM。
- `.highcode` 里新增共用热路径代码会同时占用两种模式的 RAM（这是应该的 —— 它被共用）。
- **不要把 `.text.DAP_ProcessCommand`（约 2.9 KiB）收进 `.highcode`**：它的名字会被
  `.text` 的 `*(.text.*)` 收走而留在 Flash。收进 RAM 会把 RAM 顶到 20.5 KiB
  （> 20 KiB 上限），并且 `.bss` 会压到 `.stack` 上。

本工程**只有这一种构建**：`sdk/Ld/Link_highcode_nv_256B_dual.ld` 是本目录下唯一的
工程链接脚本（`mode_overlay.h` 的段宏也没有「退化成 `.highcode`」的分支）。

## 第三方依赖

```sh
git submodule update --init
```

三个 submodule 固定在以下版本：

| submodule | commit |
| --- | --- |
| cherryusb | `0e40349` (v1.6.1-82) |
| cherryrb | `19ea7c6` (v1.0.0) |
| cherrydap | `c40bb5a` (HSLinkPro-2.4.2-41) |

## 构建

需要 WCH RISC-V GCC 工具链：设置 `WCH_TOOLCHAIN_ROOT`，或安装 MounRiver Studio 2
由 `toolchains/wch-riscv/xmake.lua` 自动探测默认路径。

```powershell
# 便捷脚本（release）。产物固定是 CMSIS-DAP + WCH-Link 双模式固件
.\build.ps1
.\build.ps1 -EsigSn n       # 退回固定串 DEADBEEF（默认：芯片 ESIG UID，每颗唯一）
.\build.ps1 -Clean          # 先清理
.\build.ps1 -Mode debug     # debug 构建
.\build.ps1 -Reconfigure    # 工具链路径变化后强制重新配置

# 或直接用 xmake
$env:WCH_TOOLCHAIN_ROOT = "C:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC12"
xmake f -m release -y
xmake -r

# 用固定串 DEADBEEF 作序列号（xmake 默认也是 esig_sn）
xmake f -m release --esig_sn=n -y
xmake -r
```

> ⚠️ `--esig_sn` 是唯一的应用侧选项，xmake 会把它**持久化**到本地配置；
> `build.ps1` 每次都显式传值，保证构建结果只由参数决定。直接敲 xmake 时也请显式指明。
> 调试器功能的裁剪选项（`--dap` / `--wchlink`）已经**删除** —— 传了 xmake 会报
> `error: Invalid option: --dap=n`，因为不存在「只编一个模式」的构建。

产物：`build/<mode>/firmware.elf`、`firmware.bin`、`firmware.map`。构建结束会打印
Flash / RAM 占用统计。

链接脚本为 `sdk/Ld/Link_highcode_nv_256B_dual.ld`：Flash 62K（末尾 256B 留给 NV 配置），
RAM 20K；`.highcode` 段由 `startup_ch32x035_highcode.S` 在启动时从 Flash 搬到 RAM 执行，
USB 设备端口等热路径代码通过 `section(".highcode")` 放在其中。

## 当前状态

USB（CherryUSB + CH32X035 端口 glue + 硬件初始化）与 CMSIS-DAP（CherryDAP + 本板
DAP glue）都已编译链接成功，并经实测确认：USB 正常枚举（CMSIS-DAP 调试器 + CDC 串口
同时出现），DAP 能通过 OpenOCD 对目标读写（140 KiB/s 量级，见下方「验证」）。

WCH-Link 模式已移植完成，**编译链接通过并已上板完整实测**。实测覆盖 USB 枚举、
WCH 官方驱动识别（`WCH-LinkRV`）、RVSWD 连接与目标核 examine、目标 ChipID/ESIG/Flash
读取、目标 RAM 读写、整片擦除 + 编程 + 校验（62 KB）以及 CDC，详见下方
「验证 → WCH-Link 模式」。

**注意：`src/dap/DAP_config.h` 的 SWD / nRESET 引脚是按参考工程
`ch32x035-usb-dap-main` 直接搬过来的（SWCLK=PA2、SWDIO=PA3、nRESET=PA5），
尚未按 uDap2 原理图核对，上板前必须确认。WCH-Link 只用 PA2/PA3，不使用 nRESET。**

| 项目 | 取值 | 需核对 |
| --- | --- | --- |
| SWCLK / SWDIO | PA2 / PA3 | ✓（两种模式共用，互斥） |
| nRESET | PA5 | ✓（仅 CMSIS-DAP 用；见下方说明） |
| 桥接串口 | USART4：PB0=TX、PB1=RX | ✓（两种模式一致） |
| 状态灯 | PB12，高电平点亮，接 `LED_CONNECTED_OUT` | ✓（仅 CMSIS-DAP 用；WCH-Link 模式下不驱动） |
| USB | PC16=UDM / PC17=UDP | 固定复用，无选择余地 |

### 状态指示灯

本板只有一颗灯（PB12，高电平点亮），接的是 CMSIS-DAP 的 **Connect LED**：主机连接到
DAP 时点亮、断开时熄灭。改脚或改极性只需动 `src/dap/DAP_config.h` 里的三个宏：

```c
#define LED_CONNECTED_PORT         GPIOB
#define LED_CONNECTED_PIN          GPIO_Pin_12
#define LED_CONNECTED_ACTIVE_HIGH  1U   // 0 = 低电平点亮
```

它由主机下发的 `ID_DAP_HostStatus(0x01)` 驱动（DAP.c 的 `DAP_HostStatus`）：

| 状态类型 | 固件回调 | 主机行为 |
| --- | --- | --- |
| `DAP_DEBUGGER_CONNECTED`(0) | `LED_CONNECTED_OUT` | pyOCD 在 `connect()` 置 1、`disconnect()` 置 0 |
| `DAP_TARGET_RUNNING`(1) | `LED_RUNNING_OUT` | pyOCD **从不置 1**（只下发 0），OpenOCD 不发该命令 |

因此 `LED_RUNNING_OUT` 保持空实现（本板也没有第二颗灯）。**这意味着灯的可见性取决于
主机工具**：pyOCD 下会亮；OpenOCD 若不支持该命令则始终不亮 —— 这种情况需要固件本地
指示（如 USB 枚举后常亮），目前未实现。

> ⚠️ **参考工程自身的 README 与代码不一致**：它的 `README.md` 引脚表写
> `PB12 = nRESET`、`PA5 = 按键`，但 `src/dap/DAP_config.h` 里是 `PA5 = nRESET`
> （它的提交记录 `将pa5作为rst引脚` 改了代码却漏改文档）。本工程照搬的是**代码**
> （PA5），不是它的 README。若本板 PA5 另有用途，必须改 `DAP_config.h`。

## 验证

固件烧录后，主机应能看到一个 CMSIS-DAP 调试器和一个 CDC 串口。可用：

```sh
probe-rs list
probe-rs info --chip <目标型号> --probe <CMSIS-DAP探针序列号>
```

USB 描述符沿用 CherryDAP 示例的 VID/PID `0d28:0204`。序列号默认取芯片出厂 UID，
可用 `--esig_sn=n` 退回固定的 `DEADBEEF`（见上方「序列号」一节）。

本工程自带 `tools/` 下的测速与串口测试脚本（与参考工程一致）：

```powershell
.\tools\speed.ps1 -Khz 6000 -Bytes 65536 -Rounds 3

# 频率扫描（一次跑 100 kHz ~ 12 MHz 并汇总）
.\tools\openocd_speed_sweep.ps1 -Rounds 5 -Warmup 1

# 串口环回吞吐 / 利用率（需 PB0-PB1 短接）
python .\tools\com_loopback_speed.py --seconds 2 --repeats 3

# 分块 / 间隔发送（撤除波特率上限后新增）
python .\tools\com_loopback_paced.py --baud 2000000 --chunk 4096 --total 262144
python .\tools\com_loopback_matrix.py --baud 2000000,3000000 --chunk 192,4096
```

实测基线（STM32F411 目标，OpenOCD + `tools/openocd_speed_test.tcl`，2026-09-26，
16 KiB 块、预热 1 轮 + 5 轮平均）：

| SWCLK | 写 KiB/s | 读 KiB/s | 路径 |
| --- | --- | --- | --- |
| 4000 kHz | 54.2 | 45.3 | 慢（已饱和） |
| 5750 kHz | 54.1 | 45.5 | 慢（饱和，最后一档） |
| **6000 kHz** | **125.4** | **121.0** | **快（拐点）** |
| 12000 kHz | 126.3 | 119.0 | 快（与 6 MHz 同速） |

慢路径自约 2.5 MHz 起完全饱和（2.5 → 5.75 MHz 都在 54 / 45 KiB/s），
6 MHz 跨过 `MAX_SWJ_CLOCK(2)` 阈值切到快路径后约 2.3 倍。
**快路径的实际 SWCLK 由固定指令数决定（约 6 MHz），所以填 6 MHz 和填 12 MHz 速度相同 ——
推荐就填 6 MHz**（阈值处余量最厚）。

阈值可在 `src/dap/DAP_config.h` 里用 `DELAY_FAST_CYCLES` 调整
（阈值 = `24 MHz / (2 + DELAY_FAST_CYCLES)`，本工程 2 → 6 MHz；上游默认 0 → 12 MHz）。
`tools/speed.ps1` 与 `openocd_speed_sweep.ps1` 的 `-FastMinKhz`（默认 6000）
只影响打印的路径标注，**改固件阈值时需同步**。

### WCH-Link 模式

已实测通过。主机侧枚举与驱动：

| 接口 | 设备名 | 驱动 | 说明 |
| --- | --- | --- | --- |
| 复合设备 | `USB Composite Device` | `usbccgp` | `VID_1A86&PID_8010`，序列号 `035CDAB1F937` |
| MI_00 | **`WCH-LinkRV`** | `WCHLink_A64`（wch.cn） | 厂商接口；Windows 用 WCH 官方驱动把它识别为 **RV 模式的 WCH-Link** |
| MI_01 | `WCH-Link SERIAL (COM26)` | `usbser` | CDC 串口 |

序列号 `035CDAB1F937` = `"035"` + 芯片 UID 前 9 位，与 `src/bsp/bsp_uid.c` 的设计一致
（同时验证了 UID 读取有效、不是全 0/全 F）。

WCH 官方的 **WCH-LinkUtility**（`MounRiver_Studio2\...\Others\SWDTool\default\WCH-LinkUtility.exe`）
启动后也会识别到本机：

```
已连接的RISC-V模式WCH-Link计数：1
```

即官方上位机把本固件当成**正牌 RISC-V 模式的 WCH-Link**（该 exe 是 GUI，只在 stdout 打日志，
没有可脚本化的烧录 CLI；本节的其它实测用 WCH 官方 OpenOCD 完成）。

用 WCH 官方 OpenOCD（`wlinke` 适配器驱动，`sdi` 传输 —— 对 CH32X035 就是 DCK+DIO 两线）
实测：

```powershell
$bin = "D:\...\WCH\OpenOCD\OpenOCD\bin"   # MounRiver Studio 2 自带
& "$bin\openocd.exe" -f "$bin\wch-riscv.cfg" -c "init" -c "targets" -c "shutdown"
```

结果（目标是一颗 CH32X035，PA2→DCK、PA3→DIO）：

```
Info : WCH-LinkE  mode:RV version 3.3
Info : wlink_init ok
Info : clock speed 6000 kHz
Info : [wch_riscv.cpu.0] datacount=2 progbufsize=8
Info : [wch_riscv.cpu.0] Examined RISC-V core; found 1 harts
Info : [wch_riscv.cpu.0]  XLEN=32, misa=0x40901105
[wch_riscv.cpu.0] Target successfully examined.
```

`misa=0x40901105` 解出 RV32 + M/A/I/C + U + X，与 CH32X035 的 `rv32imacxw` 一致。

读写实测：

| 操作 | 命令 | 结果 |
| --- | --- | --- |
| 读目标 ChipID | `mdw 0x1FFFF704 1` | `0x035e0631` → `0x035` 开头，确认是 CH32X03X 族 |
| 读目标 ESIG UID | `mdw 0x1FFFF7E8 3` | `923cabcd fb83bd7a ffffffff` |
| 读目标 Flash 大小 | `mdw 0x1FFFF7E0 1` | `ffff003e` → 低半字 `0x3E` = **62 KB** |
| 读目标 Flash | `mdw 0x00000000 4` | `7b10206f 00000000 00000000 00005920` |
| **写目标 RAM** | `mww 0x20001000 0xdeadbeef` 后回读 | `0x20001000: deadbeef 12345678` ✅ |
| **批量读（toggle 压力）** | `dump_image tgt.bin 0 0x4000` 连做两次 | 两次 SHA256 全同 `DEC4A1CF…070BF73`，16384 B @ ~13 KiB/s |
| **Flash 编程 + 校验** | `flash write_image patch.bin 0x8000`（256 B 的 `0xA5`） | `verified 256 bytes`；随后 `mdw 0x8000 4` = `a5a5a5a5 a5a5a5a5 a5a5a5a5 a5a5a5a5` ✅ |
| **整片擦除 + 编程 + 校验** | `program backup.bin 0 verify reset`（63488 B） | `** Programming Started/Finished **` → `** Verify Started **` `** Verified OK **` → `** Resetting Target **`，无错误，@ ~12.8 KiB/s |

批量读那一项是 64 次 256 B 传输（256 个 64 B 包），直接压到了 data 端点的多包路径，
**没有出现 toggle 失配**（见上方「EP 的 data toggle」）。

CDC（虚拟串口）实测：`COM26` 可打开（115200 8N1），且主机改波特率（→ 921600）被接受，
说明 `SET_LINE_CODING` → `drv_uart_set_baudrate()` 这段联动生效。

> **上板测试对目标芯片的影响已归零**：测试前先把目标 62 KB Flash 完整备份，写入
> `0xA5` 标记验证「擦除 + 编程 + 读回」后，再用备份整片回写并 `verify`；最后重新
> dump 比对 SHA256，与备份**逐字节一致**（`6E061247…C05922C`）。目标保持原样。
>
> **本板无供电开关，所以 WCH-LinkUtility 的 3.3V/5V 电源命令成功但不生效**，
> 目标供电须由板子自身接线提供（本板已通过目标芯片的 DCK/DIO 上拉得到供电即可通信）。

COM 口波特率不再有上限（原先 `> 2 Mbaud` 会被 STALL，已撤除，见 `src/dap/dap_main.c`
里「关于 COM 口波特率」的说明）。实测桥接吞吐天花板约 **190~200 KiB/s**，
2 Mbaud 就处在天花板上；再提高波特率换不来吞吐，反而要求把发送块压到 ≤192 B
才能不丢字节。**完整频率表、串口环回利用率、波特率 × 块大小矩阵与可靠性边界，
见仓库根目录的 `readme.md` 第 6 节。**

**比较两版固件速度时必须确保 `-Khz` 相同**：≥6000 kHz 才走「快路径」，差 2.3 倍。
注意 PowerShell 参数要写成 `-Khz 6000` 或 `-Khz:6000`，写成 `-Khz=6000` 会直接报错
（已加 `[CmdletBinding()]` 拦截，避免静默回落到默认的 4000 kHz）。

## 其他可参考的实现

本仓库运行期**不依赖**任何参考工程；以下按名称记录，需要时自行取用。

| 工程 | 技术路线 | 可参考部分 |
| --- | --- | --- |
| `ch32x035-usb-wchlink-main` | CherryUSB + 自研 RVSWD 协议栈 | `src/wchlink` 的整体来源（transport / protocol / rvswd / target / flash / session / usb）|
| `ch32x035-usb-dap-main` | CherryUSB + CherryRB + CherryDAP | 本工程 glue（`src/dap`、`src/drv`、`src/bsp`）的来源 |
| `CH32X035F8U`（CheapLink_X035） | WCH USBFS 驱动 + FreeRTOS | PIOC 加速（SWD 提速）、AIRCR 复位序列、TIM3 时间戳、三色 LED 状态机 |

> `ch32x035-usb-wchlink-main/` 目前仍留在 `firmware/` 下（**未跟踪**，也不参与编译）。
> 按本工程惯例（参考工程用完移出）可以删掉；若要留着，注意它没有被 `.gitignore`
> 覆盖，`git add -A` 会把它一起提交。

`CH32X035F8U`（CheapLink_X035）的 SWD 引脚是 PA5/PA6/PA7 加 PIOC 的 PC18/PC19，
与本工程当前取值不同；若最终采用 PIOC 方案，需按它重写 `DAP_config.h` 的引脚宏与
`PORT_SWD_SETUP()`。以 uDap2 原理图 `hardware/uDap.kicad_sch` 为准。
