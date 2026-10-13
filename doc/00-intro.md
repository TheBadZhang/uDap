# uDap

基于 **CH32X035F8U** 的 **CMSIS-DAP 调试器 + WCH-Link 调试器 + USB↔串口桥**。
单颗 USB 全速设备口同时提供 SWD 调试和一路虚拟串口；仓库包含固件（`firmware/`）、
KiCad 硬件设计（`hardware/`）与上位机脚本（`software/`）。

本文件是**唯一的项目文档**：架构、内存布局、双模式行为、性能实测、已知限制都在这。

> ⚠️ **本文件由 `doc/` 下的分节文档合并生成**（`python doc/build_readme.py`）。
> 要改内容请改 `doc/` 下的对应小节，**不要直接改这里** —— 下次合并会覆盖。
> 参与合并的文件、命名与编辑约定见 [`doc/README.md`](doc/README.md)。

- 调试器 ①：**CMSIS-DAP**（bulk，WinUSB）——OpenOCD / pyOCD / probe-rs 可直接使用
- 调试器 ②：**WCH-Link**（RVSWD，`1a86:8010`）——支持 CH32X03X / CH32L10X / CH32V20X /
  CH32V30X / CH58X / CH59X 六个目标族
- 两个调试器**同编入一份固件**（无裁剪选项），运行期按键（PA4 接 GND）切换，见「双模式：模式切换按键与持久化」
- 桥接串口：CDC ACM ↔ 目标板 USART4（PB0=TX / PB1=RX），**不限制波特率**
- SWD 时钟：请求 **≥6 MHz** 自动切「快路径」，**6 MHz 与更高同速**，推荐就填 6 MHz
- SWD 吞吐（STM32F411 目标，64 KiB 块）：**pyocd 262 写 / 232 读 KiB/s**、
  OpenOCD 149 / 148、probe-rs 155 / 140 —— 见「性能实测」一节
- 串口环回：**2 Mbaud 持续满载零丢字节**（96~98 % 利用率）；分块发送时 6 Mbaud 亦字节无误
- 桥接吞吐天花板：约 **190~200 KiB/s**（≈2 Mbaud），受 USB IN 侧调度限制，与 USB 带宽无关

---

[TOC]
