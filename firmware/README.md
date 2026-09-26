# CH32X035F8U DAP（uDap2 固件）

CH32X035F8U 上的 CMSIS-DAP 调试器固件，xmake + WCH RISC-V GCC 构建。

## 目录结构

本仓库根目录为 `uDap2/`，固件工程根是 **`firmware/`**（`xmake.lua` 所在层级）。

```
uDap2/
├── firmware/               ← 固件工程根（xmake 工程）
│   ├── src/                本工程代码（应用 + 移植 glue）
│   │   ├── main.c          应用入口（用 ENABLE_DAP 区分 DAP / 仅 USB 两条路径）
│   │   ├── system_ch32x035.c  时钟与系统初始化
│   │   ├── ch32x035_it.c   中断实现
│   │   ├── ch32x035_conf.h SDK 配置
│   │   ├── usb_config.h    CherryUSB 应用侧配置（必选）
│   │   ├── bsp/
│   │   │   ├── bsp_usb.c       CH32X035 USBFS 硬件初始化（必选）
│   │   │   ├── bsp_delay.c     SysTick 延时/时间 + Delay_Init/Us/Ms 兼容层
│   │   │   ├── bsp_print.c     newlib retarget（_write / _sbrk）
│   │   │   └── bsp_system.c    进 ISP 复位
│   │   ├── port/
│   │   │   └── usb_ch32x035_dc_usbfs.c   CherryUSB 的 CH32X035 USBFS 设备端口 glue（必选）
│   │   ├── dap/            CMSIS-DAP glue（可选）
│   │   │   ├── DAP_config.h    SWD / nRESET 引脚与时序配置
│   │   │   ├── dap_main.c      USB 描述符、WinUSB/MSOS、DAP 与 CDC 端点分发
│   │   │   └── sw_dp.c         SWD 位翻转实现（CherryDAP 的传输后端）
│   │   └── drv/
│   │       └── drv_uart.c      CDC <-> USART4 桥（CherryRB FIFO）
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
移植层（glue）放在 `firmware/src/port/`、`src/bsp/`、`src/dap/`、`src/drv/` 与
`src/usb_config.h`。

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

## CMSIS-DAP（可选）

调试器功能（CherryDAP + `src/dap`、`src/drv` glue）是可选的，默认启用，可以关闭：

```powershell
.\build.ps1 -Dap n      # 便捷脚本：关闭
.\build.ps1             # 便捷脚本：启用（-Dap y 是默认值）

xmake f --dap=n -y      # 直接用 xmake：关闭
xmake f --dap=y -y      # 直接用 xmake：启用
```

> ⚠️ **xmake 会把选项值持久化**到本地配置：执行过 `xmake f --dap=n` 之后，再运行
> 不带 `--dap` 的 `xmake f` 仍然保持关闭（实测：关闭后只跑 `xmake f -m release -y`
> 再构建，产物仍是 12.47 KiB）。因此 `build.ps1` 每次都显式传 `--dap=...`，
> 保证构建结果只由参数决定；直接敲 xmake 时也请显式指明。

`xmake.lua` 里的 `option("dap")` 控制它，并传递 `-DENABLE_DAP=0/1` 给 C 代码；
`src/main.c` 用该宏区分两条初始化路径（关闭时不再调 `chry_dap_init()`）。
两个实测尺寸：

| 配置 | Flash | RAM |
| --- | --- | --- |
| `--dap=y`（默认，`--esig_sn=n`） | 26.09 KiB | 19.16 KiB |
| `--dap=y --esig_sn=y` | 26.71 KiB | 19.16 KiB |
| `--dap=n` | 12.47 KiB | 11.60 KiB |

启用 DAP 时会校验 `src/dap/DAP_config.h`、`dap_main.c`、`sw_dp.c`、`src/drv/drv_uart.c`
是否存在，缺文件直接报错并提醒可用 `--dap=n` 关闭。

### 序列号：固定值 / 芯片 ESIG UID

默认所有板子的 USB 序列号都是固定的 `DEADBEEF` —— 多块板子同时插在一台机器上时
无法区分（VID:PID 也相同）。`option("esig_sn")` 打开后改用芯片 ESIG 区的
**96 位出厂 UID**（出厂烧录、只读、每颗芯片唯一），转成 24 个大写十六进制字符：

```powershell
.\build.ps1 -EsigSn y     # 便捷脚本：用芯片 UID
.\build.ps1               # 默认：固定 DEADBEEF（-EsigSn n）

xmake f --dap=y --esig_sn=y -y
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

`--esig_sn=y` 需要 `--dap=y`：序列号在 `src/dap/dap_main.c` 里设置，而该文件只在
启用 CMSIS-DAP 时编译。这个组合会被 `xmake.lua` 与 `build.ps1` 直接拒绝，
不会默默编出一个用不上它的固件。

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
# 便捷脚本（release，DAP 默认启用）
.\build.ps1
.\build.ps1 -Dap n          # 关闭 CMSIS-DAP，只编 USB 骨架
.\build.ps1 -EsigSn y       # 用芯片 ESIG UID 作序列号（每颗芯片唯一）
.\build.ps1 -Clean          # 先清理
.\build.ps1 -Mode debug     # debug 构建
.\build.ps1 -Reconfigure    # 工具链路径变化后强制重新配置

# 或直接用 xmake
$env:WCH_TOOLCHAIN_ROOT = "D:/program/wch/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC12"
xmake f -m release -y
xmake -r

# 关闭 CMSIS-DAP，只编 USB 骨架
xmake f -m release --dap=n -y
xmake -r

# 用芯片 UID 作序列号
xmake f -m release --dap=y --esig_sn=y -y
xmake -r
```

产物：`build/<mode>/firmware.elf`、`firmware.bin`、`firmware.map`。构建结束会打印
Flash / RAM 占用统计。

链接脚本为 `sdk/Ld/Link_highcode_nv_256B.ld`：Flash 62K（末尾 256B 留给 NV 配置），
RAM 20K；`.highcode` 段由 `startup_ch32x035_highcode.S` 在启动时从 Flash 搬到 RAM 执行，
USB 设备端口等热路径代码通过 `section(".highcode")` 放在其中。

## 当前状态

USB（CherryUSB + CH32X035 端口 glue + 硬件初始化）与 CMSIS-DAP（CherryDAP + 本板
DAP glue）都已编译链接成功，并经实测确认：USB 正常枚举（CMSIS-DAP 调试器 + CDC 串口
同时出现），DAP 能通过 OpenOCD 对目标读写（140 KiB/s 量级，见下方「验证」）。

**注意：`src/dap/DAP_config.h` 的 SWD / nRESET 引脚是按参考工程
`ch32x035-usb-dap-main` 直接搬过来的（SWCLK=PA2、SWDIO=PA3、nRESET=PA5），
尚未按 uDap2 原理图核对，上板前必须确认。**

| 项目 | 取值 | 需核对 |
| --- | --- | --- |
| SWCLK / SWDIO | PA2 / PA3 | ✓ |
| nRESET | PA5 | ✓（见下方说明） |
| 桥接串口 | USART4：PB0=TX、PB1=RX | ✓ |
| 状态灯 | PB12，高电平点亮，接 `LED_CONNECTED_OUT` | ✓ |
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

USB 描述符沿用 CherryDAP 示例的 VID/PID `0d28:0204`。序列号默认固定为 `DEADBEEF`，
可用 `--esig_sn=y` 改成芯片出厂 UID（见上方「序列号」一节）。

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

COM 口波特率不再有上限（原先 `> 2 Mbaud` 会被 STALL，已撤除，见 `src/dap/dap_main.c`
里「关于 COM 口波特率」的说明）。实测桥接吞吐天花板约 **190~200 KiB/s**，
2 Mbaud 就处在天花板上；再提高波特率换不来吞吐，反而要求把发送块压到 ≤192 B
才能不丢字节。**完整频率表、串口环回利用率、波特率 × 块大小矩阵与可靠性边界，
见仓库根目录的 `readme.md` 第 6 节。**

**比较两版固件速度时必须确保 `-Khz` 相同**：≥6000 kHz 才走「快路径」，差 2.3 倍。
注意 PowerShell 参数要写成 `-Khz 6000` 或 `-Khz:6000`，写成 `-Khz=6000` 会直接报错
（已加 `[CmdletBinding()]` 拦截，避免静默回落到默认的 4000 kHz）。

## 其他可参考的实现

本仓库内**不再包含**这两个参考工程（用完后已移出）；以下按名称记录，需要时自行取用。

| 工程 | 技术路线 | 可参考部分 |
| --- | --- | --- |
| `ch32x035-usb-dap-main` | CherryUSB + CherryRB + CherryDAP | 本工程 glue（`src/dap`、`src/drv`、`src/bsp`）的来源 |
| `CH32X035F8U`（CheapLink_X035） | WCH USBFS 驱动 + FreeRTOS | PIOC 加速（SWD 提速）、AIRCR 复位序列、TIM3 时间戳、三色 LED 状态机 |

`CH32X035F8U`（CheapLink_X035）的 SWD 引脚是 PA5/PA6/PA7 加 PIOC 的 PC18/PC19，
与本工程当前取值不同；若最终采用 PIOC 方案，需按它重写 `DAP_config.h` 的引脚宏与
`PORT_SWD_SETUP()`。以 uDap2 原理图 `hardware/uDap.kicad_sch` 为准。
