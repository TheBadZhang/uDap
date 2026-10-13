## 17. 上板验证

USB（CherryUSB + CH32X035 端口 glue + 硬件初始化）与 CMSIS-DAP（CherryDAP + 本板
DAP glue）都已编译链接成功，并经实测确认：USB 正常枚举（CMSIS-DAP 调试器 + CDC 串口
同时出现），DAP 能通过 OpenOCD 对目标读写（140 KiB/s 量级，见「上板验证」一节）。

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

固件烧录后，主机应能看到一个 CMSIS-DAP 调试器和一个 CDC 串口。可用：

```sh
probe-rs list
probe-rs info --chip <目标型号> --probe <CMSIS-DAP探针序列号>
```

USB 描述符沿用 CherryDAP 示例的 VID/PID `0d28:0204`。序列号默认取芯片出厂 UID，
可用 `--esig_sn=n` 退回固定的 `DEADBEEF`（见「序列号」一节）。

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

同一台目标、同一块固件、同一会话下横向对比三个主机工具（64 KiB 块、6 MHz）：
**pyocd 明显最快，「命令间空转」几乎为零，最接近固件的真实天花板** ——
所以判断固件还剩多少余量要看 pyocd 那两列。

| 主 机 工 具 | 版本 | 写 KiB/s | 读 KiB/s | 写 µs/字 |
| --- | --- | --- | --- | --- |
| **pyocd**（`tools/pyocd_speed.py`） | 0.45.1 | **252.2** | **202.9** | **15.49** |
| probe-rs（`probe-rs benchmark`） | 0.32.0 | 155.1 | 140.0 | 25.19 |
| OpenOCD（`tools/speed.ps1`） | 0.12.0 | 139.0 | 129.0 | 28.11 |

```powershell
# pyocd 测速（与 speed.ps1 同地址 / 同块大小 / 同 pattern / 同回读校验，可直接比数）
python .\tools\pyocd_speed.py --frequency 6000000 --bytes 65536 --rounds 10
```

用 pyocd 前要先装 F411 的 Keil pack（pyocd 内置目标表**没有** stm32f411）：

```powershell
pyocd pack -i stm32f411
```

命令级空转的差别很大：OpenOCD 在 `init` 时打印
`Allocating FIFO for 3 pending packets`，因为它把在飞命令数夹在写死的常量
`MAX_PENDING_REQUESTS = 3` 上（`cmsis_dap.c`），尽管固件通过 `DAP_Info`
advertise 了 `DAP_PACKET_COUNT = 8`。probe-rs 也低于 pyocd。

实测基线（STM32F411 目标，OpenOCD + `tools/openocd_speed_test.tcl`，2026-09-26，
16 KiB 块、预热 1 轮 + 5 轮平均）：
（块大小与上表不同 —— 这里是 16 KiB，上表是 64 KiB，所以数值偏低，别混着比）

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

#### 17.1 WCH-Link 模式验证

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
**没有出现 toggle 失配**（见「EP 的 data toggle」）。

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
见「性能实测」一节。**

**比较两版固件速度时必须确保 `-Khz` 相同**：≥6000 kHz 才走「快路径」，差 2.3 倍。
注意 PowerShell 参数要写成 `-Khz 6000` 或 `-Khz:6000`，写成 `-Khz=6000` 会直接报错
（已加 `[CmdletBinding()]` 拦截，避免静默回落到默认的 4000 kHz）。
