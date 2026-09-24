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
| `--dap=y`（默认） | 25.79 KiB | 19.07 KiB |
| `--dap=n` | 12.47 KiB | 11.60 KiB |

启用 DAP 时会校验 `src/dap/DAP_config.h`、`dap_main.c`、`sw_dp.c`、`src/drv/drv_uart.c`
是否存在，缺文件直接报错并提醒可用 `--dap=n` 关闭。

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

| 项目 | 参考工程取值 | 需核对 |
| --- | --- | --- |
| SWCLK / SWDIO | PA2 / PA3 | ✓ |
| nRESET | PA5 | ✓（见下方说明） |
| 桥接串口 | USART4：PB0=TX、PB1=RX | ✓ |
| LED | 未实现（`LED_CONNECTED_OUT` / `LED_RUNNING_OUT` 为空） | — |
| USB | PC16=UDM / PC17=UDP | 固定复用，无选择余地 |

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

USB 描述符沿用 CherryDAP 示例的 VID/PID `0d28:0204`。

本工程自带 `tools/` 下的测速与串口测试脚本（与参考工程一致）：

```powershell
.\tools\speed.ps1 -Khz 12000 -Bytes 65536 -Rounds 3
```

实测基线（STM32F411 目标，64 KiB）：

| SWCLK | 写 KiB/s | 读 KiB/s |
| --- | --- | --- |
| 4000 kHz（慢路径） | ~54 | ~49 |
| 12000 kHz（快路径） | ~142 | ~132 |

**比较两版固件速度时必须确保 `-Khz` 相同**：≥12000 kHz 才走「快路径」，差 2.6 倍。
注意 PowerShell 参数要写成 `-Khz 12000` 或 `-Khz:12000`，写成 `-Khz=12000` 会直接报错
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
