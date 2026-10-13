## 3. 技术栈

### 固件

| 层 | 选型 | 说明 |
| --- | --- | --- |
| USB 协议栈 | **CherryUSB** | 上游 submodule，不改动；端口 glue 在 `src/port/` |
| 环形缓冲 | **CherryRB** | `chry_ringbuffer`，USB↔UART 两个方向的 FIFO |
| dap-link | **CherryDAP** | CMSIS-DAP 命令解析、SWD 事务层 |
| wch-link | **WCH-Link**（`src/wchlink/`） | RVSWD 目标调试 / Flash 烧录 |
| 芯片 SDK | WCH CH32X035 SDK | Core / Peripheral / Startup / Linker，位于 `sdk/` |
| 语言 | C（gnu11），无 RTOS，裸机主循环 + 中断 | |

架构约定：`third_party/` **只放上游 submodule**，针对本板的适配（glue）全部放在
`src/port/`、`src/bsp/`、`src/dap/`、`src/wchlink/`、`src/status/`、`src/drv/` 与
`src/usb_config.h`，升级依赖不会产生冲突。

### 工具链与构建

| 项目 | 选型 |
| --- | --- |
| 构建系统 | **xmake**（`firmware/xmake.lua`，工程根 = `firmware/`） |
| 编译器 | WCH RISC-V Embedded GCC 12（`riscv-wch-elf-*`，MounRiver Studio 2 自带） |
| 编译选项 | `-march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore`，`-Wall -std=gnu11`（release） |
| 优化 | LTO + `--gc-sections`；`.highcode` 段启动时从 Flash 搬到 RAM 执行。必须用 `-Os`：两套调试器代码同编，`-O3` 会让 `.highcode` 膨胀到 18.5 KiB（Flash/RAM 都装不下），`-Og`/`-O0` 又装不进 Flash —— debug 也是 `-Os` + LTO，只额外带调试符号 |
| 辅助脚本 | `firmware/build.ps1`（PowerShell，包了工具链路径与 `--esig_sn=y/n`） |
| 烧录 | `wlink`（WCH 官方）或 `wchisp` |

### 上位机 / 测试脚本

Python（pyserial）+ TCL（OpenOCD）+ PowerShell，全部在 `firmware/tools/`。

### 硬件设计

KiCad 工程：`hardware/uDap.kicad_sch` / `uDap.kicad_pcb`。
