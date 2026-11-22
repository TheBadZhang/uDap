# uDap · AT32F405 版固件

一块 AT32F405KBU7-4（QFN32，128 KiB Flash / 70 KiB RAM，12 MHz HEXT）跑出来的 uDap：
USB 一线同时提供 **CMSIS-DAP 调试器** + **一路 USB 转串口**，外加一个 **DFU + WinUSB 双通道 bootloader**。

- USB 高速：片上 OTGHS（UTMI PHY，480 Mbit/s），CherryUSB `port/dwc2` 驱动
- 调试器：CherryDAP 的 `DAP.c` + 自写的 AT32 `DAP_config.h`；SWD 物理层有两套实现（GPIO 位翻转 / SPI1 硬件移位），编译期用 `--swd_phy=` 切换；GPIO 版 ≥27 MHz 走 Flash 常驻的展开汇编快路径（写 1.75 MB/s，见 5 节），读方向默认交给上游实现
- 串口：USART2（PA2/PA3）↔ CDC ACM，波特率随上位机 `SET_LINE_CODING` 实时改
- bootloader：16 KiB，占 `0x08000000`，应用在 `0x08004000`；支持标准 DFU 1.1 与私有 WinUSB 批量协议
- 上位机：`software/udap-studio/`（C++/CLI + WinForms，.NET 4.8）

## 1. 硬件连接

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| USB | OTGHS1_D-/OTGHS1_D+ | AT32F405 专用引脚，无需 GPIO 复用；必须 12 MHz HEXT |
| SWCLK / TCK | PA5 | SPI1_SCK |
| SWDIO / TMS | PA6 + PA7 | PA6 = MISO（读）、PA7 = MOSI（写），两脚并到目标 SWDIO |
| nRESET | PA4 | 开漏，20 ms 低脉冲 |
| TDI | PB1 | 关 JTAG 时复用为 DTR |
| TDO | PB0 | 关 JTAG 时复用为 RTS |
| UART2_TX / RX | PA2 / PA3 | 桥接串口 |
| LED1 / LED2 | PB2 / PB4 | 低电平点亮 |
| BOOT0 | PF11 | 用户板上未接按键 |

## 2. 构建

```powershell
cd firmware/at32f405

# 默认：GPIO 位翻转，≥27 MHz 走 Flash 常驻展开汇编快路径（见 6.3 节），-Os + LTO
xmake f -P . -y
xmake -P . -j 8

xmake f -P . -y --swd_phy=gpio_fast   # 旧别名，等价于 gpio
xmake -P . -j 8


# SPI1 硬件移位 SWD 物理层（绝对速度最快）；SPI 版建议配 -O2
xmake f -P . -y --swd_phy=spi --opt=speed
xmake -P . -j 8
```

两个构建开关：

| 选项 | 取值 | 说明 |
| --- | --- | --- |
| `--swd_phy` | `gpio`（默认）/ `spi` | SWD 物理层实现，两者协议行为完全一致；`gpio_fast` 是 `gpio` 的旧别名 |
| `--opt` | `size`（默认，`-Os`）/ `speed`（`-O2`） | 优化目标；两个 target 都始终带 `-flto` |

- 优化级别交给 xmake 的 `set_optimize()`，不再硬写在 `gcc_flags` 里；`-flto` 同时加在
  编译和链接命令行上（只在编译期加是不会真正参与 LTO 的）。
- **`bootloader` 固定 `-Os`**：`-O2` 会让它涨到 18,864 B，超过 16 KiB 分区（115%），链接直接失败。
- **`--opt=speed` 只对 `--swd_phy=spi` 有利**（27 MHz 写 1230 → 1280 KiB/s）。GPIO 路线开 `-O2` 反而更慢：写 738 → 559 KiB/s，且 8 次里 2 次 `Unexpected ACK`（位翻转时序被压缩到没有余量）。所以默认保持 `size`。

产物：

| 文件 | 地址 | 大小 |
| --- | --- | --- |
| `build/bin/bootloader/bootloader.{bin,hex}` | `0x08000000` | 14,064 / 16,384 B（85.8%） |
| `build/bin/firmware/firmware.{bin,hex,elf,map,lst}` | `0x08004000` | 36,692 B（`-Os`）/ 38,112 B（`-O2`）+ 26.5 KB RAM |

> `xmake -P .` 不能省：仓库根目录也有一个 `xmake.lua`，父工程的配置会盖掉子工程。

## 3. 烧录

**bootloader 与应用必须同一次写入**（pyocd 会整片擦，分两次会把 bootloader 擦掉）：

```powershell
pyocd load -u <探针序列号> -t at32f405kbu7_4 -f 1000000 `
  --pack misc\ArteryTek.AT32F402_405_DFP.2.1.6.pack -e chip `
  build\bin\bootloader\bootloader.hex build\bin\firmware\firmware.hex
```

只更新应用（板子上已有 bootloader）时，最省事的是上位机：

```powershell
cd ../../software/udap-studio
.\run.ps1                      # 图形界面
.\build\udap-studio.exe --flash ..\..\firmware\at32f405\build\bin\firmware\firmware.bin
```

也可以走标准 DFU：

```powershell
dfu-util -d 0d28:0205 -a 0 -s 0x08004000:leave -D build\bin\firmware\firmware.bin
```

## 4. 使用

- **调试**：Windows 上装 WinUSB 驱动后（`0d28:0204`，MS OS 2.0 描述符自动匹配，zadig 兜底），
  `pyocd list --probes` 能看到 `uDAP CMSIS-DAP`；OpenOCD 用 `interface/cmsis-dap.cfg`。
- **串口**：设备管理器里多出两个 COM 之外的一路 CDC（VID_0D28 / PID_0204 复合设备的 MI_01）。
  默认 115200 8N1，`SET_LINE_CODING` 可改到 **12 Mbaud**（实测 1047 KiB/s 零误码，见第 5 节）。
- **模式配置**：CMSIS-DAP vendor command（`0x80` 起，报文 `[0x80, 子命令, 参数]`）：
  `GET_INFO=0x00`、`SET_UART_MODE=0x01`、`SET_LED_MODE=0x02`、`ENTER_BOOT=0x03`、
  `SET_DEBUGGER_MODE=0x04`、`RESET=0x05`、`GET_UID=0x06`。
  `firmware/tools/udap_config.py` 是命令行客户端：

  ```powershell
  python tools\udap_config.py --serial 5059EC86 list     # 多台设备时先看清序列号
  python tools\udap_config.py --serial 5059EC86 info
  python tools\udap_config.py --serial 5059EC86 led 1    # LED 模式：0 状态 1 活动 2 混合 3 关
  python tools\udap_config.py --serial 5059EC86 boot     # 进 bootloader
  ```

- **LED 含义**（模式 0，默认）：LED1 = DAP 已连接目标，LED2 = 串口有流量。

## 5. 实测数据

测试条件：AT32F405KBU7 板 + 外部 DAPLink 烧录；SWD 目标 = STM32F411RE（`pyocd` + CMSIS-DAP，
16 KiB 块，`firmware/tools/pyocd_speed.py`）；串口为 PA2→PA3 硬件回环（`tools/uart_loopback_speed.ps1`）。

### SWD 吞吐（KiB/s，写 / 读）

SPI 版（`--opt=speed`，27 MHz 已开满，见下表说明）：

| SWCLK 请求 | 实际 SWCLK | 写 | 读 | 每字（写/读） |
| --- | --- | --- | --- | --- |
| 1 MHz | 0.84 MHz（÷256） | 87.2 | 51.5 | 44.8 / 75.8 µs |
| 6 MHz | 6.75 MHz（÷32） | 331.5 | 185.7 | 11.8 / 21.1 µs |
| 12 / 18 MHz | 6.75 MHz（÷32） | 573.9 | 307.9 | 6.8 / 12.7 µs |
| **27 MHz** | **27 MHz（÷8）** | **1280.0** | **619.8** | **3.05 / 6.30 µs** |
| 36 / 54 MHz | 27 MHz（÷8，已封顶） | 与 27 MHz 相同 | 与 27 MHz 相同 | — |

> 同一份代码用默认 `-Os` 时 SPI 版是 1230 / 600 KiB/s（每字 3.20 / 6.51 µs）——
> `-O2` 在这个物理层上多出约 11%，所以 SPI 版推荐 `--opt=speed`。

- **27 MHz 是这一版的硬件上限**：SPI1 挂在 APB2（216 MHz），分频只能是 2 的幂，
  所以可用的 SWCLK 只有 27 / 13.5 / 6.75 / 3.375 … MHz 这几档。上位机要 12 MHz
  反而落到 6.75 MHz，要 27 MHz 才能吃到 ÷8 那一档 —— 这也是「27 MHz 才吃满性能」
  的真实原因，不是 12 MHz 以上的曲线饱和了。实际分频可以从 `SPI1->ctrl1` 的
  `mdiv` 位段核对，也可以打开 `swd_spi.c` 的 `SWD_SPI_STATS` 从 `.bss` 里的 `g_swd_spi_stats` 读出。
- 27 MHz 下 33 个 SWCLK 只要 1.22 µs，占每字成本的 **39%（写）/ 19%（读）**：
  剩下的 ~1.9 µs（写）/ ~5.1 µs（读）是请求/ACK/转向的位翻转、奇偶位补拍和 DAP 层调度。
  所以再往上堆频率收益有限（36 MHz、54 MHz 请求测出来和 27 MHz 一样）。
- 吞吐还对传输块大小敏感（USB 往返按包摊销）：同一个 27 MHz，16 KiB 块是
  1280/620，64 KiB 块读能到 701。`dap_raw_speed.py`
  固定 126 字/包时只有 868 KiB/s，这也是它只用来比较「固件每字成本」的原因。


GPIO 位翻转版（`--swd_phy=gpio`，默认；按 `clock_delay` 在两套实现之间分派）：

| SWCLK 请求 | 1 MHz | 3 MHz | 6 MHz | 9 MHz | 12 MHz | 15 MHz | 18 MHz | 21 MHz | 24 MHz | 27 MHz | 30 MHz |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 写 KiB/s | 85.5 | 212.0 | 418.4 | 418.6 | 1697.8 | 1708.9 | 1701.0 | 1711.9 | 1708.0 | **1747.9** | 1736.3 |
| 读 KiB/s | 48.3 | 119.5 | 235.5 | 235.1 | 391.2 | 391.8 | 392.3 | 389.9 | 390.7 | **750.5** | 745.2 |
| 每字（写/读） | 45.7/81.0 µs | 18.4/32.7 | 9.34/16.6 | 9.33/16.6 | 2.30/9.98 | 2.29/9.97 | 2.30/9.96 | 2.28/10.0 | 2.29/10.0 | **2.23/5.20** | 2.25/5.24 |

> 上表为 2026-11-23 实测（`pyocd_speed.py`，16 KiB 块，2–3 轮全部数据校验通过）。
> 同一台机器上 64 KiB 块：pyocd 写 1453.9 / 读 825.6 KiB/s（2.69 / 4.73 µs），
> OpenOCD 写 1329.6 / 读 986.1 KiB/s。原始固件（改动前）16 KiB 为 1224 / 597，
> 即写 +43%（1.75 MB/s）、读 +26%；64 KiB 读 +38%。

> **两套实现、按 `clock_delay` 分派**（`application/dap/swd_gpio_fast.c` 的 `SWD_Transfer`）：
>
> | 请求 | `DAP_Data` 状态 | 写方向 | 读方向 |
> | --- | --- | --- | --- |
> | ≥ 27 MHz | `fast_clock=1` | `swd_fast_write_hot`（8 槽/bit，33 拍 ≈8.5 周期/bit） | 上游 `SW_TransferFast`（`SWD_FAST_READ_REF 1`） |
> | 12–24 MHz | `fast_clock=0`、`clock_delay=1` | 同一套 `*_hot`（不随请求降速，实测 ≈27 MHz） | 上游 `SW_TransferSlow`（1 单位延时 ≈10 MHz） |
> | ≤ 9 MHz | `clock_delay ≥ 2` | 上游 `SWD_TransferRef` | 上游 `SWD_TransferRef` |
>
> - 分派条件就是一行：`if (!DAP_Data.fast_clock && DAP_Data.clock_delay > 1U) return SWD_TransferRef(request, data);`
> - **为什么 12–24 MHz 的写也是 1700 KiB/s 而不是 1000**：`Set_Clock_Delay()` 把
>   `clock_delay` 量化成 `(((CPU/2)/clock) - 2 + 7) / 8`，12–24 MHz 全算成 **1**（不是 >1），
>   于是落不进 `Ref` 分支；而 `*_hot` 内核对请求频率不敏感（固定 8 槽/bit，≈27 MHz），
>   所以写方向实际跑在 27 MHz。这是当前实现的已知偏差：上位机要 12 MHz 也会拿到 27 MHz
>   的写时序（读方向不受影响，它走上游、按 1 单位延时 ≈10 MHz 跑）。若要严格守约，
>   应在 `clock_delay == 1` 时改走 `swd_fast_write_mid`（`SWD_MID_PAD=3`，≈13.5 MHz）。
> - 6 MHz 与 9 MHz 持平同样是量化台阶：两者 `clock_delay` 都落在 2（≈8 MHz 实际速率）。
> - ≥27 MHz 那一档：写用展开汇编 + `scr` 直写（每 bit 4 条指令 / 2 次 AHB 写，
>   33 拍约 8.5 周期/bit，216 MHz → 27 MHz）；读走上游实现，因为自写的 32 拍采样内核
>   受「每位 1 次写时钟 + 1 次读 IDT」两次 APB 访问限制，实测 22 周期/bit（约 692 KiB/s），
>   比上游慢，所以 `SWD_FAST_READ_REF` 保持 1（内核仍在，改 0 即可启用）。
> - 低速段的台阶感来自上游 C 实现的量化延时：`(CPU/2)/clock - 2` 再整除 `DELAY_SLOW_CYCLES`(8)。
>
> 以下是 2026-11-22 那次构建的历史排查记录（数字与上表不同）：
> - **合并掉速的排查（2026-11-22）**：`gpio_fast` 单独编（只有一套实现）时 27 MHz 是
>   1316/575 KiB/s，合并成「一套实现 + 三档分派」后掉到 1125/558。逐项二分的结果：
>   ① `__builtin_parity` 在 Cortex-M4 上是 libgcc 的 `__paritysi2` 调用（Flash 里跑，
>   6 个等待周期），而每笔传输要算 2 次（请求字节 1 次 + 数据奇偶 1 次）→ 换成位折叠 +
>   `0x6996` 查表的内联实现，+30 KiB/s；
>   ② ACK 三拍里「按档位补 NOP」写成了每 bit 的 `if (!DAP_Data.fast_clock)`，三个 bit 就是
>   6 次分支 + 反复读 `DAP_Data` → 拆成快 / 中两段独立的 ACK 循环，+35 KiB/s；
>   ③ `swd_fast_finish` 由真调用改为内联，`ctx` 由「栈上结构 + 指针」改为按值传的 const
>   结构（编译器摊进寄存器），再 +30 KiB/s。
>   合计 1125 → **1224 KiB/s**。与 1316 的剩余差距（≈7%）来自合并后必定多出的
>   「档位判断 + 两套内核共存」的代码布局，实测无法再消掉，除非退回单实现单档。
> - **待办（已知未完成）**：`swd_fast_gpio.S` 里的 `*_d` 内核（每个半周期插一段运行时
>   可编程延时，本来是给 ≤6 MHz 档准备的）接上去一律 `Unexpected ACK '0'`，连延时压到最小
>   也一样，原因未定位；因此 ≤6 MHz 仍走上游 `*Ref`。`*_d` 内核留在 `.ramcode` 里（约 0.6 KB RAM）。

### 优化级别与 LTO

`-flto` 让编译器跨文件内联，收益主要在体积；`--opt` 只对 SPI 版有帮助：

| 构建 | firmware Flash | bootloader Flash | 27 MHz SWD 写 / 读（16 KiB，按物理层） |
| --- | --- | --- | --- |
| `-Os`，无 LTO（旧） | 31,612 B | 15,748 B | gpio 610/346 · spi 1224/618 |
| `-Os` + LTO（默认） | 36,692 B | 14,064 B | gpio **1748/750**（27 MHz 汇编档）· spi 1230/600 |
| `-O2` + LTO（`--opt=speed`） | 38,112 B | 14,064 B（固定 -Os） | gpio 未复测（旧内核 559/368，已废弃）· spi **1280/620** |

> gpio 版仍按 `-Os` 构建：新内核的节拍是「指令槽数 = 周期数」，`-O2` 会打乱槽位对齐，
> 只对 SPI 版有意义（+11%）。

LTO 顺带暴露出两个必须处理的点（都在代码注释里写清了原因）：

1. **`syscalls.c` 的 `_write` 不能再硬连 `bsp_uart_putc`**：LTO 先做符号解析再做
   `--gc-sections`，没人引用的 `_write` 也会被拉进链接，bootloader 因此报
   `undefined reference to 'bsp_uart_putc'`。改成 `g_sys_console_putc` 钩子，
   应用在 `main.c` 里赋值。
2. **`-T` 的链接脚本路径必须相对**：绝对路径带中文时，xmake 生成 `@args.txt`
   响应文件会按本地代码页写出，ld 读回来变成 `USB?`，报 `Invalid argument`。

### 串口回环（PA2→PA3，零误码）

16 KiB 块，`tools/uart_stress.py -p COMxx -b <baud> -n 131072 -c 4096`（加 `--full-duplex` 测管道上限）：

| 波特率 | 实际分频 | 吞吐 | 占线速 | | 波特率 | 实际分频 | 吞吐 | 占线速 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 115200 | 108 MHz/938 | 10.9 KiB/s | 97% | | 2 Mbaud | /54 | 152.3 KiB/s | 78% |
| 921600 | /117 | 78.9 KiB/s | 88% | | 4 Mbaud | /27 | 374.6 KiB/s | 96% |
| 6 Mbaud | /18 | 552.0 KiB/s | 94% | | 8 Mbaud | /14 | 694.7 KiB/s | 89% |
| **12 Mbaud** | **/9** | **1047–1049 KiB/s** | **89–90%** | | | | | |

12 Mbaud 持续 60 s 压力测试（`-t 60`）：55.5 MiB、**1047.3 KiB/s**、`err=0 timeout=0`，
块耗时 min 3.62 / avg 3.68 / max 4.68 ms，每秒报告稳定。

两个关键前提（缺一个就会丢字节）：

1. **RX 必须走 DMA**。12 Mbaud 下一个字节只有 0.83 µs，靠中断逐字节搬运时，一旦被
   USB 中断或 USB 驱动里关中断的临界区挡住就 ORE 丢字节（现象是每块末尾少 1~9 字节）。
   现在 `drv_uart.c` 用 `DMA1_CHANNEL6` + `DMAMUX_DMAREQ_ID_USART2_RX` 做 4 KiB 环形缓冲，
   `drv_uart_read()` 按 DMA 写指针拷数据，硬件握手，不再受任何中断延迟影响。
2. **UART 中断优先级 0 高于 USB 的 1**（TX 就绪中断仍需及时响应）。

`usart_init()` 的 `baudr.div = round(108 MHz / baud)` 是 16 位定点数（高 12 位整数、低 4 位小数），
所以 12 Mbaud 写 `div=9` 即 `USARTDIV=9/16=0.5625`，实际波特率正好 12.000 Mbaud。
这一档的 USARTDIV 小于常规 16 倍过采样推荐的下限 1.0，回环（收发同源）实测零误码；
接真实对端时建议先确认对方时钟精度，或退到 6.75 Mbaud（`div=16`）以求更大裕量。

**全双工模式的上限（`--full-duplex`：一边写一边读，不给 UART 留空闲）**：

| 波特率 | 分频 | 吞吐 | 占线速 | 错误 |
| --- | --- | --- | --- | --- |
| 12 Mbaud | /9 | 1141 KiB/s | 97% | 0 |
| 13.5 Mbaud | /8 | 1283 KiB/s | 97% | 0 |
| 15 Mbaud | /7（实际 15.43 M） | **1461 KiB/s** | **100%** | 0 |
| 18 Mbaud | /6 | 1285–1360 KiB/s | 73–77% | **每档都错** |

上表中的「占线速」按 8N1 的 10 bit/字节算。**USB HS 不是瓶颈**：15 Mbaud 时
1461 KiB/s 已经吃满线速（约 11.7 Mbit/s 有效载荷），只占 HS 带宽的 2% 左右。
之前那一版文档写「瓶颈是 USB 桥 ~1.05 MB/s」是错的 —— 那个数字来自 UART 还在用中断搬 RX
的版本，且半双工脚本「写一块、读一块」时读的那段时间 UART 是空的，会把占比压到 89%。
换成 `--full-duplex` 后 12 Mbaud 立刻从 89% 升到 97%。

真正的上限在 UART 自己：`div=6`（18 Mbaud）时接收端采样裕量不足，任何块大小都会错；
`div>=7`（15.43 Mbaud 及以下）全双工零误码。所以本机推荐上限是 **15 Mbaud**，
文档与自检仍按 12 Mbaud（更保险、且是精确分频）来写。

### bootloader

| 项目 | 数值 |
| --- | --- |
| WinUSB 私有协议刷写 29.1 KiB | 0.5 s，**55.3 KiB/s**（504 B/块，每块 7 ms） |
| DFU 1.1 | `wTransferSize=1024`，`bmAttributes=0x0B`，刷完自动 leave |
| bootloader 体积 | 14,064 / 16,384 B（85.8%，`-Os` + LTO） |

## 6. 两套 SWD 物理层：为什么 SPI 只搬 32 位

`application/dap/swd_spi.c` 里数据相位改成 `spi_burst()`：

```c
sck_to_spi(); mosi_to_spi();
(void)spi_burst(buf, 4U);        /* 32 个 SWCLK，正好 4 字节 */
mosi_to_gpio_out(); sck_to_gpio();
PIN_SWDIO_OUT(parity & 1U);      /* 第 33 拍奇偶位回到 GPIO */
```

踩过的坑（都体现在代码注释里）：

1. **不能发 5 个字节**。SPI 一次只能发 8 的整数倍，5 字节 = 40 个 SWCLK，多出来的
   7 拍会被目标当成下一个请求的 start bit + 前 7 位，SWD 状态机立刻失步，
   表现为「No ACK」而且降频也没用。所以奇偶位必须用 GPIO 补。
2. **`gpio_pin_mux_config()` 不改 GPIO 模式**，只写 `muxl/muxh`。必须显式
   `pin_set_mode(..., GPIO_MODE_MUX)` + `gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE5/7, GPIO_MUX_5)`
   两句都做，否则有「模式是复用但复用号还是 AF0」，数据相位一个时钟都没有。
3. **PA6 平时必须是 GPIO 上拉输入**。ACK、奇偶位、转向都靠 `PIN_SWDIO_IN()` 读 PA6，
   只有 SPI 搬运数据位的那一小段才把 PA6 切到 MUX，搬完立刻切回。
4. SPI 从设备误触发（MMERR）会把主机切成从机，所以 `SPI_CS_SOFTWARE_MODE` +
   `spi_software_cs_internal_level_set(SPI1, SPI_SWCS_INTERNAL_LEVEL_HIGHT)` 要配好。

`swd_spi.c` 里另有一个 `SWD_SPI_DEBUG_RECORD` 开关（默认 0）：打开后会把最近 8 笔传输的
ACK/请求码留在 `.bss` 的 `g_swd_spi_dbg`，可以用另一台探针在运行时读出来，是排查
「SWD 突然连不上」最有效的手段——上面第 1 个坑就是这么定位的。

### 27 MHz 下另外两处提速

数据相位交给 SPI 之后，每笔传输剩下的大头是「请求字节 + ACK」这两段位翻转，27 MHz 时
它们比数据相位还贵。为此又做了两件事（都在 `swd_spi.c` 里，可单独关掉）：

1. **请求字节也走 SPI**（`SWD_SPI_REQ_VIA_SPI`，默认 1）。请求正好 8 bit = 1 字节，
   又没有反向数据，可以直接丢给 SPI 发，省掉 8 次「两个半周期的延时循环」。
   实测写 1054 → 1150 KiB/s。
2. **位翻转的延时跟随快/慢路径**（`PIN_DELAY()`）。上位机要 ≥27 MHz 时 DAP.c 会把
   `fast_clock` 置 1、`clock_delay` 固定成 1，原来这里仍按 `PIN_DELAY_SLOW` 空转
   8 次；改成快路径插 NOP 后，ACK 3 拍 / 奇偶位 / 转向的耗时降到和 SPI 数据相位一个量级。
   实测写 1150 → 1226 KiB/s。

反过来，`swd_spi.c` 的 `spi_apply_clock()` 必须按 `DAP_Data.fast_clock` 区分反推：
慢路径的 `clock_delay` 单位是 `DELAY_SLOW_CYCLES` 个周期，不乘回去就会**超频**——
上位机特意要 6 MHz 时固件却跑 27 MHz，长线或弱目标上会直接连不上。

### 6.3 GPIO 的快路径：把位翻转内核搬进 RAM 并用汇编写

AT32F405 在 216 MHz 下访问 Flash 有等待周期，但**实测结论和直觉相反**：同一段展开的位翻转
循环，放 SRAM 跑 15.5 周期/bit、放 Flash 跑 7.3 周期/bit（ART 加速器把顺序取指的开销吃掉，
而 SRAM 侧反而被总线仲裁拖慢）。所以现在的分工是：

- **热内核放 Flash**（`.section .text.swdfast`）：`swd_fast_tx32_hot` / `swd_fast_tx8_hot` /
  `swd_fast_rx_bits`（32 拍展开）/ `swd_fast_rx_tail`（读收尾）→ `0x0800_xxxx`。
- **只能放 RAM 的**（`.section .ramcode`，链接脚本放进 `.data`，启动时 `_sidata → _sdata` 拷贝）：
  `swd_fast_tx_bits`（`SWJ_Sequence` 用的通用 32/8 bit 版）以及 `.Ltx32`/`.Ltx_loop`，
  因为**51 拍线复位**用的就是它——它在 Flash 里会跑到 ≈43 MHz，目标根本解不出复位序列
  （表现是一上来就 `Unexpected ACK '0'`），在 RAM 里 ≈21 MHz 才安全。
- 分派入口 `SWD_Transfer()` 本身也在 RAM（`SWD_RAMF`），`nm` 可以看到它落在 `0x2000_xxxx`。

**写相位：每 bit 8 个槽（8 个 CPU 周期，216 MHz → 27 MHz）**，思路与
`firmware/ch32x035/application/dap/swd_gpio_x035.c` 一致：把「取位 → 0/16 → 选半字 → 拼字」
四条 ALU 换成「两个候选字 + 进位标志二选一」，并且**一个 SCR 写同时完成「SWCLK 下降沿 + 改 SWDIO」**：

```c
/* r6 = value<<4，r8 = DIO_CLR，r9 = SCK_CLR，r12 = SCK_SET */
ands    r7, r6, #16     /* 当前位的图样 */
lsrs    r6, r6, #1      /* 下一拍预备 */
lsr     r7, r8, r7      /* 0 → DIO_CLR|SCK_CLR，16 → DIO_SET|SCK_CLR */
orrs    r7, r7, r9
str     r7, [r4]        /* 下降沿 + 数据，一次 SCR 写完成 */
nop                     /* SWD_TX_PAD_A */
str     r12, [r4]       /* 上升沿 */
nop                     /* SWD_TX_PAD_B */
```

- **槽数就是时序**：Flash 里一条指令 ≈1 个周期，`SWD_TX_PAD_A/B` 每个 NOP 精确加 1 个周期，
  所以调这些常量等于调占空比；8 个槽 = 8 周期/bit = 27 MHz（216 MHz / 8）。
- 32 位数据相位 DWT 实测 **≈324 周期（8.5 周期/bit）**，加请求 8 拍 + 3 拍 ACK + 校验位后
  整笔 **572 周期 ≈2.65 µs**；16 KiB 块实测 2.23 µs/字（1.75 MB/s）。
- 剩下的 ~130 周期/字在 DAP.c 的块循环 + USB 处理里，不在位翻转内核。
- 完全展开（`.rept 32` + `.set k`），没有循环、没有分支：**每拍指令条数一致，指令条数就是时序**。
  改任何一拍都要保证整段拍数一致，否则那一位的占空比就偏了。

**读相位：采样点必须在 SWCLK 上升沿**（ARM IHI0031：主机在下降沿写 SWDIO、在**上升沿**读
SWDIO；目标驱动的数据在下降沿后 tDVOD 才有效，主机的输入建立时间 T_is ≥4 ns）：

```c
str     r5, [r4]        /* 下降沿 */
.rept SWD_RX_LOW nop    /* 等目标把数据摆上来（≈6~7 个周期） */
ldr     r4, [idt]       /* 采样 */
ands    r4, r4, #0x80   /* PA7（板上 PA6/PA7 短接，等价于读 PA6） */
lsls    r4, r4, #24     /* 对齐到 bit31 */
lsrs    r0, r0, #1
orrs    r0, r0, r4      /* 旋转累加，32 拍后自然是 LSB 对齐 */
str     r12, [r4]       /* 上升沿 */
.rept SWD_RX_HI nop
```

- 这套内核能跑通（`SWD_FAST_READ_REF 0`），把 32 拍数据 + 校验位 + 转向 + 方向切换收进一次调用，
  但实测只有 ≈692 KiB/s：每位要 1 次写（打时钟）+ 1 次读（采样），两次 APB 往返让循环跑到
  **22 周期/bit**，这是 CPU 打 GPIO 的物理下限（≈4.7 µs/字）。
- 所以默认 `SWD_FAST_READ_REF 1`，读方向交给上游 `SW_TransferFast`（825 KiB/s / 4.73 µs）；
  要试自研读内核把它改成 0。

**两个曾经卡很久的坑**（都已修掉）：

1. 读数据相位曾**多补 1 个转向时钟**（`k_clock(turnaround + 1)`），DP 把它当成下一个包的
   start 位，表现为「读一直正常、之后每一笔都 ack=7」；改回 `turnaround` 立即恢复。
2. ACK 三拍必须**在低电平后段采样**（这三拍是目标与主机反向驱动），而数据/校验位要在上升沿
   采样，两者不能混用同一个采样点。

踩过的坑（都写在 `swd_fast_gpio.c` / `.S` 的注释里）：

1. **SCR 只影响写到的位**：bit=0 时必须显式写 `DIO_CLR`，只写 `SCK_CLR` 的话 SWDIO
   保持上一次电平，请求字节变成全 1，表现为「目标永远 No ACK」。
2. **两次写之间要留建立时间**：`.equ SWD_NOP_FALL` 现在补 1 个 NOP（占空比接近 1:1、
   吞吐无损失）；压到 0 个 NOP 会让部分位的低电平只剩一次 store 的时间，时序裕量不够。
3. **RAM 函数要 `noinline`**：否则被内联进 Flash 里的调用者就白搬了。
4. 调试这类问题最有效的手段是把最近几笔 `request/ACK` 记在 `.bss`
   （`SWD_FAST_DEBUG=1` 打开 `g_swd_fast_dbg`），用另一台探针直接读——
   上面第 1 个坑就是它定位的。
5. 改这类"同一个标识符既是包装又是内核"的代码**不要用批量正则**：曾经把
   `k_rx(){ return swd_fast_rx_bits(nbits); }` 正则成 `k_rx(){ return k_rx(nbits); }`，
   无限递归，连不上目标，查了很久。

### 6.4 一笔传输的时间都花在哪（27 MHz，DWT 实测）

当前构建（2026-11-23，写方向走自研快路径、读方向走上游）实测：

| 段 | 周期 | 说明 |
| --- | --- | --- |
| 请求 8 拍 + 转向 + ACK 3 拍 | 238 | 汇编请求内核 + C 层 ACK 采样（理论下限 ≈110） |
| 写数据 32 拍 + 奇偶位 | 324（8.5/bit） | 汇编热入口，8 槽/bit（27 MHz） |
| idle 收尾 | 9 | `n = 0`，不再补上位机要求的 2 拍 idle |
| 位翻转小计（写） | **572 = 2.65 µs** | 46 个 SWCLK 的协议下限是 368 周期 = 1.70 µs |
| DAP 块循环 + USB | ≈130/字 | 16 KiB 块实测 2.23 µs/字（1.75 MB/s） |
| 读数据 32 拍（上游 `SW_TransferFast`） | ≈700（22/bit） | 每位 1 次写时钟 + 1 次读 IDT，两次 APB 访问 |

> 下表是 2026-11-22 旧内核的分解（合计 ≈740 周期 = 1224 KiB/s），保留用于对比：

| 段 | 周期 | 说明 |
| --- | --- | --- |
| 请求字节 8 bit | 114（14.3/bit） | 起始位 + APnDP + RnW + A2/A3 + 奇偶 + 停止 + 驻留 |
| 转向 1 拍 + ACK 3 拍 | 108（≈500 ns） | 必须先释放 SWDIO（写 `cfgr`）让目标驱动，再逐拍采样 |
| ACK 结束 → 数据首拍 | ≈49（≈230 ns） | C 层：ACK 判定分支、`*data` 读、`DIO_DRIVE()`、内核调用 |
| 写数据相位 32 bit + 奇偶 | 373（11.3/bit → 19.1 MHz） | 汇编热入口，每 bit 4 条指令 / 2 次 AHB 写 |
| 合计（每字） | ≈740 = 3.43 µs | 即 1224 KiB/s |

「ACK 前后看着慢」不是 USB 慢：SWD 协议要求请求后半段把 SWDIO 交还给目标（1 个转向时钟），
目标才回 ACK，我们再给 1 拍 idle 才能接管。热入口改动前这段要多花 107 个周期（每个内核调用
push 4 个寄存器 + 8 条 `movw/movt` 装常量）；现在常量一次算好、奇偶位与转向时钟都内联，只剩 ≈49 周期。

数据相位的 SWCLK 本来就不是完美方波：① 结构上「低电平 = 1 次 store，高电平 = `lsrs`+IT+store」，
差 1–2 个周期，补 1 个 NOP 后接近 1:1 且吞吐无损失；② AHB 写延迟有 ±1–2 周期抖动，USB 中断插进
这 33 拍里会拉长某几位——要彻底消掉只能在整段外 `__disable_irq()`（≈1.7 µs），会牺牲 USB/串口
实时性，默认不开。SWD 只认建立/保持时间，占空比 45/55 不影响目标识别。

### 6.5 为什么抓到的 SWCLK 看着不齐（实测振幅）

32 位数据相位已经是**完全展开**的（`.rept 32`，每 bit 4 条指令、无循环、无分支），
但用 DWT 记录每笔 33 拍数据相位的耗时分布，仍能看到抖：

| 指标 | 周期 | 说明 |
| --- | --- | --- |
| 最小 | 372 | 每 bit 11.3 周期，正常情况 |
| 平均 | ≈430 | 与 1224 KiB/s 一致 |
| 最大 | **974** | 偶发一笔被拉长 2.6 倍（≈+600 周期） |

逐项排除的结果：

1. **不是指令/循环**：数据相位没有循环；把「两次 store」改成「高电平期算好下一拍、低电平期无条件 store」
   反而更慢（12.54 vs 11.30 周期/bit），已回退。
2. **不是中断**：把 33 拍包在 `__disable_irq()/__enable_irq()` 里，最大值仍是 982，
   而且吞吐从 1316 掉到 1209 KiB/s（每字多屏蔽 1.7 µs 中断），所以默认不开。
3. **是 AHB 总线争用**：OTGHS 设备态跑在 `device_dma_enable = true`（CherryUSB `port/dwc2/usb_glue_at.c`
   的 `param_pb14_pb15`），512 字节的 USB 包由 DMA 突发搬进 SRAM；这期间 CPU 对 `GPIOA->scr`
   的 store 要在总线上排队，正好是几百个周期量级。波形上就表现为「某几位特别宽」。

另外每个 bit 内部还有固定不对称：低电平 = 1 次 store（≈4 周期）+ 1 NOP，高电平 = `lsrs` + `IT` + store（≈6 周期），
占空比天然约 45/55。SWD 只要求最小高/低时间、没有最大周期限制，拉长的位对目标无影响
（27 MHz 下 5/5 轮、连续多轮均通过）。

**想要完全均匀的 27 MHz 时钟，只能让硬件产生时钟**：`--swd_phy=spi` 的 SCK 是 SPI 分频器输出，
不经过 CPU，也不受总线/中断抖动影响——这正是 SPI 版波形干净的原因。

### 6.6 SPI 这一轮还能抠出什么（三条否定结论 + 两处真收益）

想让 SPI 版更快，先试了两条「看起来能省 CPU 等待」的路，都被实测否掉了：

1. **改帧长一次发 16 位**（`spi_frame_bit_num_set(SPI1, SPI_FRAME_16BIT)`）→ 目标回
   `Unexpected ACK '0'`。原因是 `ctrl1.fbn` 在 SPI 使能时写入无效，所以第一次只发了
   16 个时钟就失步。就算关掉 SPI 再改帧长（`spi_enable(FALSE)` → 改 `fbn` →
   `spi_enable(TRUE)`）也一样失步 —— **16 位帧这条路在 AT32 上不通**，已经回退。
2. **「FIFO 连写、不等 TDBE」**：SPI1 没有 FIFO（寄存器只有 `sts/dt/ctrl1`），
   而且 `TDBE` 在字节一移入移位器就置位，真正要等的是移位时间本身
   （8 拍 @ 27 MHz ≈ 296 ns）。所以「连写」省不掉任何东西，反而容易写出丢字节的代码。

真正有效的是下面两处（都只影响 `--swd_phy=spi`）：

- **快路径的延时宏**：奇偶位、转向、ACK 这些 GPIO 拍原来一律用
  `PIN_DELAY_SLOW(DAP_Data.clock_delay)`。27 MHz 时 `clock_delay = 1`，慢路径延时是
  `DELAY_SLOW_CYCLES`（8）次空转 ≈ 50 周期，而每笔传输这类调用有 12 处。
  改成跟随 `DAP_Data.fast_clock` 的 `SWD_DELAY()`（快路径插 `PIN_DELAY_FAST()`）
  后，**-Os 构建从 1127 → 1230 KiB/s（写）、570 → 600（读）**。
- **转向 + ACK 内联成纯寄存器循环**（`swd_spi_turnaround_ack()`）：每拍只做
  「写 `scr` 清 SCK → 读 `idt` 采 SWDIO → 写 `scr` 置 SCK」，不再经过
  `SW_CLOCK_CYCLE()` / `SW_READ_BIT()` 宏链，再省 ~0.7%。

两处合起来（默认 `-Os`）：**1126.7 / 569.8 → 1230 / 600 KiB/s**；
`--opt=speed` 下 **1241.1 / 616.3 → 1280.0 / 619.8**。5 轮连跑 5/5 通过。

> **顺手抓到一个致命回归**：给 `swd_spi.c` / `swd_gpio_fast.c` 加调试统计数组时，
> 习惯性地把它们放进了 `.noinit`。应用的 `.noinit` 起点被链接脚本钉在 **0x20010000**，
> 而 bootloader 正好按这个固定地址读 `g_udap_boot_magic`；多占 28 字节就把 magic 顶到
> 0x20010018，于是「上位机命令进 bootloader」静默失效（设备复位后直接回到应用）。
> 是 `bootloader_flash_test.py full` 卡在「等 0D28:0205」才暴露的。
> 现在调试数组全部放 `.bss`（调试器运行时照样能读），`.noinit` 只留给跨复位握手的
> `g_udap_boot_magic`。`arm-none-eabi-nm -n firmware.elf | grep boot_magic` 必须显示
> **0x20010000**，这是自查的第一个符号。

## 7. 目录

```
firmware/at32f405/
├── application/           应用层（DAP + CDC 复合设备）
│   ├── main.c             初始化顺序 + 主循环
│   ├── board_pin_map.h    板级引脚表（全部引脚都在这里）
│   ├── usb_config.h       CherryUSB 配置
│   ├── usb_cdc_hooks.c    CDC SET/GET_LINE_CODING、DTR/RTS 钩子
│   ├── bsp/               bsp_delay / bsp_time / bsp_usb / bsp_uid / bsp_vector
│   ├── drv/               drv_led / drv_uart / drv_control / drv_mode_store
│   └── dap/               DAP_config.h、dap_main.c、swd_spi.c、udap_vendor.c
├── bootloader/            16 KiB bootloader（DFU + WinUSB）
├── sdk/                   AT32F402/405 外设库 + CMSIS + bsp_led/bsp_delay/clock
├── startup/               启动文件与链接脚本（应用偏移版 / 整片版）
├── misc/                  DFP pack 与 SVD
├── tools/                 测速与回归脚本（见下）
└── docs/bootloader-protocol.md   bootloader 与上位机的协议契约
```

`tools/` 里的脚本（都只依赖 pyusb / pyocd / .NET SerialPort）：

| 脚本 | 用途 |
| --- | --- |
| `pyocd_speed.py` | SWD 读写吞吐（唯一可信的测速脚本） |
| `dap_raw_speed.py` | 只发 `DAP_TransferBlock`，把「固件每字成本」和「pyocd 分包策略」分开 |
| `probe_swd_sweep.py` | 多频点扫描（计时靠 pyocd 日志，档位数字仅供参考） |
| `udap_config.py` | vendor command 客户端（`list/info/led/uart/mode/boot/reset/uid`） |
| `bootloader_flash_test.py` | bootloader 回归（`list/enter-boot/flash/badcrc/full/dfu/readback/bounds/reboot`） |
| `uart_stress.py` | 串口压力测试：确定性图案、写后回读逐字节校验、按秒报告；`--full-duplex` 边写边读测管道上限 |
| `uart_loopback_speed.ps1` | 串口回环测速与逐字节校验（.NET SerialPort，无 Python 依赖） |
| `uart_pattern_test.py` / `dap_probe_check.py` | 串口图案测试、探针存在性检查 |

## 8. 已知限制

- **JTAG 通路固件已通、目标侧未接**：DAP 侧 JTAG 初始化正常（OpenOCD `transport select jtag`
  能看到 `SWCLK/TCK/TDI/TDO` 电平），但板上只把 SWD 三根线接到了 STM32F411，
  TDI/TDO 没有目标驱动，扫描链读到全 1（`JTAG scan chain interrogation failed: all ones`）。
  接上五线 JTAG 后即可验证；关 JTAG 时 PB1/PB0 自动作为 DTR/RTS 使用。
- 本板单串口，`DRV_UART_MODE_COUNT = 1`，所以 `SET_UART_MODE` 只接受 0，
  上位机把双串口选项置灰（能力位 `0x0C` = LED 模式 + UF2 bootloader）。
- WCH-Link 模式未实现（`WCHLINK_ENABLE = 0`）。
- **串口推荐 12 Mbaud**（实际 12.000 Mbaud；半双工 1047 KiB/s、全双工 1141 KiB/s，均零误码）。全双工上限实测 15.43 Mbaud / 1461 KiB/s（100% 线速）；18 Mbaud 起 USART 采样裕量不足必错。USARTDIV=0.5625 低于 16 倍过采样推荐下限，接真实对端时留意对方时钟精度。USB HS 远未饱和（1.4 MB/s ≈ 带宽的 2%）。
- 全功能构建 RAM 占用约 26.5 KB / 70 KB（.data 5.2 KB 含 RAM 常驻内核 + .bss 19.8 KB +
  heap/stack 1.5 KB），Flash 36.7 KB / 128 KB；再加缓冲还有很大余量。
- **不要给 GPIO 物理层开 `-O2`**：`--opt=speed` 只适合 `--swd_phy=spi`。GPIO 版的快路径是
  「指令槽数 = 周期数」的汇编，`-O2`/LTO 会改动槽位对齐；实测旧内核在 27 MHz 从 738/422
  掉到 559/368 且 8 次里 2 次失败，所以 GPIO 版固定用 `-Os`（默认）。
- 双串口 / SWO / 时间戳未启用（`DAP_UART`、`SWO_*`、`TIMESTAMP_CLOCK` 都是 0），
  RAM 只有 70 KiB，8×512 B 的 DAP 缓冲之外的预算要留给 CDC 环形缓冲。
- GPIO 快路径的汇编内核默认启用（`SWD_FAST_PHY_ENABLE 1`、`SWD_FAST_WRITE_REF 0`），
  **写方向在 `clock_delay ≤ 1`（即 ≥10.8 MHz）时就接管**、读方向默认交给上游
  （`SWD_FAST_READ_REF 1`，自研读内核 692 KiB/s 略慢于上游 825 KiB/s）。27 MHz 下为
  **1748/750 KiB/s（16 KiB 块）/ 1454/826（64 KiB 块）**，写侧已超过 SPI 版（1230/600），
  读侧仍受「每位多一次 APB 读」限制。注意 12–24 MHz 请求时写方向实际跑在 27 MHz
  （见 5 节的档位说明），若需严格守约把 `clock_delay == 1` 改走 `swd_fast_write_mid`。

## 9. 自查命令

改完固件按这套跑一遍，几分钟就能确认没有回归（`<SN>` 换成 `5059EC86` 这类序列号前缀）：

```powershell
cd firmware/at32f405

# 1) 编译 + 烧录（bootloader 与应用同一次写入）
xmake f -P . -y --swd_phy=spi --opt=speed   # 物理层三选一；SPI 版建议 --opt=speed
xmake -P . -j 8
pyocd load -u <外部 DAPLink SN> -t at32f405kbu7_4 -f 1000000 `
  --pack misc\ArteryTek.AT32F402_405_DFP.2.1.6.pack -e chip `
  build\bin\bootloader\bootloader.hex build\bin\firmware\firmware.hex

# 2) 枚举 + vendor command
pyocd list --probes
python ..\tools\udap_config.py --serial <SN> info      # 应看到 fw 1.0.0 / caps 0x0C

# 3) SWD 吞吐（目标 STM32F411）
python ..\tools\pyocd_speed.py --probe <本板 SN> --target stm32f411re --frequency 27000000 --bytes 16384 --rounds 3
#    spi+`--opt=speed`：27 MHz 应到 ~1.28 MB/s 写 / 620 KiB/s 读；gpio 版约 1748/750（16 KiB）/ 1454/826（64 KiB）
#    注意 --frequency 的单位是 Hz（27000000 = 27 MHz），写成 27000 就成了 27 kHz，会慢到像卡死
pwsh -NoProfile -File tools\speed.ps1 -Rounds 1 -OpenOcdRoot D:\repo\closeocd\dist -Khz 27000
#    OpenOCD 实测 27 MHz：gpio 写 1329.6 / 读 986.1 KiB/s（OpenOCD 每字一条小 USB 命令，属主机侧瓶颈）
python tools\dap_raw_speed.py --serial <SN> --clock 27000000 --words 126 --packets 64
#    ⚠ 这个脚本只用来比较「固件每字成本」：协议字段有历史 bug，且固定 126 字/包，
#    绝对值 868 KiB/s 不代表真实吞吐，请以 pyocd_speed.py / speed.ps1 为准

# 4) 串口压力测试（PA2→PA3 回环）：12 Mbaud 应到 ~1.05 MB/s（--full-duplex 时 ~1.14 MB/s）、零误码
python tools\uart_stress.py -p COM39 -b 12000000 -t 60 -c 4096
#    只想快速确认：python tools\uart_stress.py -p COM39 -b 12000000 -n 65536 -c 4096
.\tools\uart_loopback_speed.ps1 -Port COM39 -Baud 12000000 -Bytes 1048576

# 5) bootloader 全流程（进 boot → 错 CRC → 正确 CRC → 回应用）
python tools\bootloader_flash_test.py --serial <SN> full build\bin\firmware\firmware.bin

# 6) 标准 DFU 通道
python tools\bootloader_flash_test.py --serial <SN> enter-boot
python tools\bootloader_flash_test.py --serial <SN> dfu-flash build\bin\firmware\firmware.bin
```

**LED 怎么验**：`udap_config.py --serial <SN> led 1` 切到活动指示，然后在串口上跑回环，
同时用外部 DAPLink 读 `0x40020414`（GPIOB 输出锁存）：`0x14` = 两颗灯都亮（PB2/PB4 低），
停掉流量变回 `0x16`/`0x17`（灭）。注意**别在跑回环时用 pyocd 长时间 halt 本板**——
那会把 USB 侧卡住，回环会报读超时（这跟固件无关）。
