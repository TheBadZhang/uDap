## 2. 硬件

### 主控芯片

| 项目 | 参数 |
| --- | --- |
| 型号 | **CH32X035F8U6**（沁恒 WCH，TSSOP-20） |
| 内核 | RISC-V，`rv32imacxw`（RV32IMAC + WCH 扩展，沁恒 QingKe V4 系列） |
| 主频 | 48 MHz，来自内部 HSI RC（无外部晶振），工程内 `CPU_CLOCK = 48000000` |
| Flash | 62 KiB 可用（链接脚本按「62K − 256B」划分，末尾 256B 留给 NV 配置） |
| SRAM | 20 KiB |
| USB | 内置 USB 全速（12 Mbps）设备控制器，DP/DM 固定在 PC16/PC17 |
| 固件体积 | Flash ≈ 59.8 KiB（`.bin` 61244 B）、RAM ≈ 18.5 KiB（release，`-EsigSn y`；`.bin` 上限 63232 B） |

芯片外设在本工程中用到的部分：USBFS、USART4、GPIOA/GPIOB（含 BSHR/BCR 原子置位复位，
SWD 位翻转靠它做到单指令翻转）、SysTick、Flash。

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| SWCLK | PA2 | SWD 时钟 |
| SWDIO | PA3 | SWD 双向数据（CFGLR 动态切输入/输出） |
| nRESET | PA5 | 低有效，复位目标 |
| 桥接串口 | PB0 = TX / PB1 = RX | USART4 |
| 状态灯 | PB12 | 高电平点亮，接 CMSIS-DAP `LED_CONNECTED_OUT` |
| USB | PC16 = DPDM / PC17 = DP | 固定复用 |

> ⚠️ 引脚取自参考工程，**尚未逐条对照 `hardware/uDap.kicad_sch` 核对**，上板前请确认。

合并两种模式的完整引脚分配（改引脚时的权威表）：

| 信号 | 引脚 | 使用方 | 备注 |
| --- | --- | --- | --- |
| SWCLK / DCK | PA2 | CMSIS-DAP + WCH-Link（互斥） | 两模式共用 |
| SWDIO / DIO | PA3 | CMSIS-DAP + WCH-Link（互斥） | 两模式共用 |
| nRESET | PA5 | 仅 CMSIS-DAP | 待按原理图核对 |
| 桥接串口 | PB0=TX / PB1=RX | USART4（两模式一致） | 网络名 `PB0_USART4_TX` / `PB1_USART4_RX` |
| 状态灯 | PB12 | 仅 CMSIS-DAP（`LED_CONNECTED_OUT`） | 高电平点亮；WCH-Link 不驱动 |
| 模式按键 | PA4 | 双模式 | 外接，一端 PA4 一端 GND，内部上拉 |
| USB | PC16=UDM / PC17=UDP | 两模式 | 固定复用 |

### USB 描述符

USB 描述符（`src/dap/dap_main.c`）：

| 项目 | 值 |
| --- | --- |
| VID:PID | `0d28:0204`（与部分 DAPLink 相同，同插时需按序列号区分） |
| 制造商 / 产品 | `uDAP` / `uDAP CMSIS-DAP` |
| 序列号 | **默认**取芯片 ESIG 区的 96 位出厂 UID（24 位十六进制，每颗芯片唯一，多块板子可区分）；`--esig_sn=n` 退回固定串 `DEADBEEF` |
| 复合接口 | 接口 0 = CMSIS-DAP 厂商自定义 bulk（MS OS 2.0 描述符自动装 WinUSB）<br/>接口 1/2 = CDC ACM |
| DAP 固件版本 | `0.1.0` |
