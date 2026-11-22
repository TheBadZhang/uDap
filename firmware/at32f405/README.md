# uDap · AT32F405 版固件

一块 AT32F405KBU7-4（QFN32，128 KiB Flash / 70 KiB RAM，12 MHz HEXT）跑出来的 uDap：
USB 一线同时提供 **CMSIS-DAP 调试器** + **一路 USB 转串口**，外加一个 **DFU + WinUSB 双通道 bootloader**。

- USB 高速：片上 OTGHS（UTMI PHY，480 Mbit/s），CherryUSB `port/dwc2` 驱动
- 调试器：CherryDAP 的 `DAP.c` + 自写的 AT32 `DAP_config.h`；SWD 物理层有两套实现（GPIO 位翻转 / SPI1 硬件移位），编译期用 `--swd_phy=` 切换；GPIO 版在 12–24 MHz 与 ≥27 MHz 两档都跑 RAM 常驻汇编内核（只是节拍不同）
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

# 默认：GPIO 位翻转，27 MHz 档自动切到 RAM 常驻汇编快路径（见 6.3 节），-Os + LTO
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
| `build/bin/firmware/firmware.{bin,hex,elf,map,lst}` | `0x08004000` | 27,324 B（`-Os`）/ 38,112 B（`-O2`）+ 67 KB RAM |

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


GPIO 位翻转版（`--swd_phy=gpio`，默认；按请求频率自动在三套实现之间切换）：

| SWCLK 请求 | 1 MHz | 3 MHz | 6 MHz | 12 MHz | 18 MHz | 24 MHz | 27 MHz | 30 MHz |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 写 KiB/s | 57.8 | 143.4 | 283.9 | 1006.6 | 1003.0 | 1002.9 | **1223.9** | 1221.4 |
| 读 KiB/s | 35.7 | 87.4 | 169.1 | 555.0 | 553.2 | 552.4 | **597.1** | 597.5 |

> **三套实现、按请求频率分派**（`application/dap/swd_gpio_fast.c` 的 `SWD_Transfer`）：
>
> | 请求 | `DAP_Data` 状态 | 走哪套 | 说明 |
> | --- | --- | --- | --- |
> | ≥ 27 MHz | `fast_clock=1` | `swd_fast_gpio.S` 的 `*_hot` | 每 bit 4 条指令 / 2 次 AHB 写，33 拍 10.4 周期/bit |
> | 12–24 MHz | `fast_clock=0`、`clock_delay<=1` | 同文件的 `*_mid` | 同结构，但每个半周期插 `SWD_MID_PAD=3` 个 NOP（≈13.5 MHz） |
> | ≤ 6 MHz | 更慢的档位 | cherrydap 原版 `SW_DP.c` | 链接时 `-D` 改名成 `SWD_TransferRef`，与上游逐字一致 |
>
> - 分派条件就是一行：`if (!DAP_Data.fast_clock && DAP_Data.clock_delay > 1U) return SWD_TransferRef(request, data);`
> - **12–24 MHz 这一档原来直接落到 Flash 里的 C 实现（442/258 KiB/s），加中间档后是 941/529（+113% 写 / +105% 读）**；
>   三档都是「RAM 常驻 + 完全展开」，只有节拍不同。
> - `SWD_MID_PAD` 实测：3 → 12 MHz 942 KiB/s（≈13.5 MHz，与请求相当）；2 → 1012 KiB/s，但实际时钟约 20 MHz、超出请求 50%，所以不取；4 → 886 KiB/s。
> - ≥27 MHz 那一档是换内核而不是「快一点」：写 441 → 1125 KiB/s。热入口（`swd_fast_tx32_hot`）
>   省掉常量加载与 push/pop，配合 `DAP_config.h` 的 `PIN_SET/PIN_CLR` 走 `scr` 直写，
>   才把 14.6 周期/bit 压到 10.4（216 MHz → ≈20.8 MHz，这是软件位翻转的下限）。
> - 低速段的台阶感来自上游 C 实现的量化延时：`(CPU/2)/clock - 2` 再整除 `DELAY_SLOW_CYCLES`(8)。
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

| 构建 | firmware Flash | bootloader Flash | 27 MHz SWD 写 / 读（按物理层） |
| --- | --- | --- | --- |
| `-Os`，无 LTO（旧） | 31,612 B | 15,748 B | gpio 610/346 · spi 1224/618 |
| `-Os` + LTO（默认） | **32,772 B** | **14,064 B** | gpio **1224/597**（27 MHz 汇编档）· spi **1230/600** |
| `-O2` + LTO（`--opt=speed`） | 38,112 B | 14,064 B（固定 -Os） | gpio 559/368（不稳定）· spi **1280/620** |

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

AT32F405 的 Flash 在 216 MHz 下有 6 个等待周期，位翻转这种「每拍都要取指」的循环跑在
Flash 里明显吃亏。`--swd_phy=gpio`（旧名 `gpio_fast`）在**请求到 27 MHz 档时**自动切到这条路线的做法：

- `application/dap/swd_fast_gpio.S`：三个内核 `swd_fast_tx_bits` / `swd_fast_rx_bits` /
  `swd_fast_clock_n`，整段 `.section .ramcode`。
- 链接脚本把 `*(.ramcode) *(.ramcode*)` 放进 `.data` 输出段（`>RAM AT>FLASH`），
  于是启动文件现有的 `_sidata → _sdata.._edata` 拷贝**自动**把内核装进 RAM，
  不用改 startup、也不用第二份加载镜像。`nm` 可以看到它们落在 `0x2000_00xx`。

**32 位数据相位是完全展开的流水线，实测 10.4 个 CPU 周期/bit（216 MHz → 20.8 MHz）**，
思路与 `firmware/ch32x035/application/dap/swd_gpio_x035.c` 一致，但把「取位 → 0/16 →
选半字 → 拼字」这四条 ALU 整个省掉了：

```c
/* 两个候选字预先放在 r4 / r7 里，用进位标志二选一，不再逐拍拼字 */
lsrs    r0, r0, #1      /* C = 当前数据位（LSB 先发） */
ite     cc
strcc   r4, [r2]        /* bit=0：SWDIO 清零 + SWCLK 下降沿，一次 SCR 写完成 */
strcs   r7, [r2]        /* bit=1：SWDIO 置位 + SWCLK 下降沿，一次 SCR 写完成 */
str     r3, [r2]        /* SWCLK 上升沿 */
```

- **为什么那 4 条 ALU 可以省**：`ubfx`（取第 k 位）、`lsls`（0/1 → 0/16 当移位量）、
  `lsr`（在 `DIO_CLR` / `DIO_SET` 两个半字间做无分支选择，因为 `DIO_CLR >> 16 == DIO_SET`）、
  `orr`（或上 `SCK_CLR` 合成"数据 + 下降沿"的整字）——它们本质上都是在**算下一次要写的那个字**。
  既然候选值只有两个，就把两个候选字预先放进寄存器，用 `lsrs` 把数据位移进进位标志，
  再用 IT 块里的两条条件存储二选一：**每 bit 4 条指令、2 次 AHB 写**。
- 两次 AHB 写是协议本身决定的（数据 + 下降沿一次、上升沿一次）。DWT 实测单次
  `GPIOA->scr` 写约 4 个周期，两次写就是 8 个周期，加上 `lsrs` + IT 块刚好 10.4 个周期。
  **这基本就是 216 MHz 软件位翻转的下限**：`swd_fast_tx_bits(…, 32)` 实测 333 周期
  （10.41 周期/bit = 20.76 MHz），整条数据相位 12.8 周期/bit（含 1 bit 奇偶位的调用开销）。
  要真的 27 MHz 只能让 SPI 硬件移位（见 6.2 节）。
- 读相位每 bit 是「2 次写 + 1 次读」（`ldr [IDT]` 采样），实测 21 周期/bit ≈ 10.3 MHz；
  采样点放在**下降沿后 1 个周期**（`.equ SWD_NOP_RX`，等目标 `tDVOD`），采样结果按常量
  直接 or 进第 k 位；3 bit 的 ACK 放在 bit29..31（调用方 `>>29`），1 bit 奇偶位左对齐到 bit31。
- 完全展开（`.rept 32` + `.set k`），没有循环、没有分支：**每拍指令条数一致，指令条数就是时序**。
  改任何一拍都要保证整段拍数一致，否则那一位的占空比就偏了。

比「4 条 ALU 拼字」那一版（14.6 周期/bit）再省 29%：27 MHz 下写 **900 → 1224 KiB/s**、读 **481 → 597 KiB/s**，
与 SPI 版的写侧（1241 KiB/s）相比已经反超，读侧仍受「每位多一次 AHB 读」限制（575 vs 616）。
（读相位此前一直是坏的，新内核一次就通了：根因是旧内核把 3 bit ACK 放到 bit0..2，调用方却按 `>>29` 取，永远读到 ACK=0。）

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
- 全功能构建 RAM 占用 67 KB / 70 KB（93.6%），再加缓冲要先腾 RAM。
- **不要给 GPIO 物理层开 `-O2`**：`--opt=speed` 只适合 `--swd_phy=spi`。`gpio` 版在 27 MHz
  会从 738/422 掉到 559/368 且 8 次里 2 次失败；汇编快路径开 `-O2` 后连不上目标。原因是 `-O2`
  会把位翻转循环压到没有余量（LTO 下尤其明显，所以纯 C 物理层的 `PIN_SET/PIN_CLR` 特意包了
  `application/dap/swd_pin_slow.c` 的 `noinline` 包装）。默认 `-Os` 是唯一三套 PHY 全绿的配置。
- 双串口 / SWO / 时间戳未启用（`DAP_UART`、`SWO_*`、`TIMESTAMP_CLOCK` 都是 0），
  RAM 只有 70 KiB，8×512 B 的 DAP 缓冲之外的预算要留给 CDC 环形缓冲。
- GPIO 快路径的汇编读写内核都已可用（`SWD_FAST_TX_ASM` / `SWD_FAST_RX_ASM` 默认 1），
  但**只在 `DAP_Data.fast_clock`（上位机请求 ≥27 MHz）时启用**；低速档回落 C 内核，
  保证请求 12–24 MHz 走中间档、≤6 MHz 转给上游实现、真的跑慢。27 MHz 下为 1224/597 KiB/s，
  写侧已与 SPI 版（1230/600）接近、读侧略低——位翻转占用 CPU 是路线性差异。

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
#    spi+`--opt=speed`：27 MHz 应到 ~1.28 MB/s 写 / 620 KiB/s 读；gpio 版约 1224/597
python tools\dap_raw_speed.py --serial <SN> --clock 27000000 --words 126 --packets 64
#    这里能把「固件每字成本」和 USB 按包摊销的固定开销分开看

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
