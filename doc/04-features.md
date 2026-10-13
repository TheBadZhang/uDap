## 4. 功能特性

- **CMSIS-DAP**：SWD 传输，支持 `DAP_Transfer` / `DAP_TransferBlock` / `DAP_ExecuteCommands`；
  nRESET 可控；已实测 OpenOCD 能读写 STM32F411 的 Flash 与 RAM。
- **快/慢双路径 SWD**：请求时钟 < 6 MHz 走慢路径（带可调延时，兼容长线/慢目标）；
  ≥ 6 MHz 走快路径（固定指令数时序，约 6 MHz 实际 SWCLK）。阈值由 CherryDAP 的
  `MAX_SWJ_CLOCK(DELAY_FAST_CYCLES) = (48M/2)/(2+DELAY_FAST_CYCLES)` 决定，
  本工程在 `DAP_config.h` 里把 `DELAY_FAST_CYCLES` 设为 2 → 阈值 **6 MHz**
  （上游默认 0，即 12 MHz）。
- **USB↔串口桥**：CDC ACM 与 USART4 双向搬运，波特率由主机 `SET_LINE_CODING` 实时设定。
- **不限制波特率**：主机下什么速率就设什么（不拒绝、不截断）。USART 是 16 倍过采样，
  `BRR = 48 MHz / 波特率`，标称上限 3 Mbaud；实测 6 Mbaud（BRR=8，USARTDIV<1）也能字节无误。
  详见「撤除波特率上限之后」。
- **CDC 侧背压**：UART TX FIFO 余量不足时不重挂 OUT 端点，主机 bulk 写被 NAK 挡住，
  主机侧自然阻塞在串口波特率上，**不丢字节**（见「COM 口环回吞吐与利用率」）。
- **状态指示灯**：PB12 显示主机是否已连接（由 `DAP_HostStatus` 驱动）。
- **唯一序列号（默认）**：用芯片 ESIG 区的 96 位出厂 UID 作 CMSIS-DAP 的
  USB 序列号（同一份字符串也用于 CMSIS-DAP 的 `DAP_GetSerNum`），多块板子可区分；
  `--esig_sn=n` 可退回固定的 `DEADBEEF`。见「序列号」一节。
- **WCH-Link（RVSWD）模式**：随固件一起提供，切到该模式后主机侧识别为 `1a86:8010` /
  `WCH-Link`（Windows 自动绑定 WCH 官方 `WCHLink_A64` 驱动，设备名 `WCH-LinkRV`），
  支持 6 个目标族（CH32X03X / CH32L10X / CH32V20X / CH32V30X / CH58X / CH59X）的连接、
  读写、擦除、下载与校验，代码移植自 `firmware/ch32x035-usb-wchlink-main/`。
  **已上板实测**：WCH 官方 OpenOCD 连接并 examine 目标核，62 KB 整片擦除 + 编程 +
  校验通过。CDC 串口行为与 CMSIS-DAP 模式完全一致；本板没有状态灯 / 目标电源开关，
  相关命令成功但不生效，详见「WCH-Link 模式细节」一节。
- **双模式固件**：CMSIS-DAP 与 WCH-Link 两套调试器代码**始终**编进同一份固件
  （Flash 59.8 KiB / RAM 18.5 KiB），**没有裁剪选项**。根据不同的用户配置会进 CMSIS-DAP或者WCH-Link，
  通过按键（**PA4 接 GND**）可互相切换，并记录当前的模式（切换会重新枚举 USB，主机侧等同
  于换设备）。两者不同时运行，所以各自私有的代码与数据用链接脚本 `OVERLAY` 复用同一
  段 RAM（RAM 从 25.3 KiB 降到 17.2 KiB）。不需要按键时用镊子把 PA4 短到 GND 也行。
  见「双模式：模式切换按键与持久化」一节。
