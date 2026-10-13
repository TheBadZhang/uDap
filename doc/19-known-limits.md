## 19. 已知限制

| 限制 | 说明 |
| --- | --- |
| 吞吐天花板 ≈190~200 KiB/s | 桥接的极限是 USB IN 侧调度，与波特率无关（见「撤除波特率上限之后」）。**把波特率提到 2 Mbaud 以上不会更快**，反而必须缩小发送块才能不丢字节，等效速率更低。 |
| 高波特率需配分块发送 | ≥2.5 Mbaud 时只有发送块 ≤192 B 才逐字节无误（见「撤除波特率上限之后」）。持续满载收发（≥1.9 Mbaud）会偶发丢字节 —— RX 方向**没有背压手段**（UART 侧无 RTS/CTS），是并发时真正脆弱的一侧。 |
| 超规范速率 | `BRR < 16`（>3 Mbaud）超出 16 倍过采样规范，实测能用但无收益；且 2.5 Mbaud 等档位的实际速率与请求值偏差可达 1~2 %，接真实目标板时需自行留出容差。 |
| 快路径实际时钟固定 | 快路径的 SWCLK 由指令数决定（约 6 MHz），与请求的 6 ~ 24 MHz 无关；请求越高只是余量越薄。**所以推荐的请求值就是 6 MHz** —— 正好是拐点，余量最厚。 |
| 单颗状态灯 | PB12 接的是 CMSIS-DAP `LED_CONNECTED_OUT`，由主机的 `DAP_HostStatus` 驱动。**OpenOCD 不发该命令 → 灯不亮**；pyOCD 会亮。详见「状态指示灯」一节。 |
| 引脚未核对原理图 | SWD/nRESET/串口引脚取自参考工程，需按 `hardware/uDap.kicad_sch` 复核。 |
| HSI 无晶振 | 波特率/时序精度依赖内部 RC，与标称值有零点几百分比偏差。 |
| WCH-Link 功能已上板实测 | 已实测通过：USB 枚举、WCH 官方驱动识别为 `WCH-LinkRV`、WCH 官方 OpenOCD 连接并 examine 目标核、目标 ChipID/ESIG/Flash 读取、目标 RAM 读写、16 KiB 批量读（toggle 无异常）、**62 KiB 整片擦除 + 编程 + 校验**、CDC 开关与改波特率。测试前已备份目标 Flash，测后逐字节还原。详见「上板验证」一节。 |
| WCH-Link 模式无供电/指示 | 本板没有目标供电开关（参考工程的 PB12 在本板是 DAP 连通指示灯），也没有状态灯，因此 3.3V/5V 电源命令**成功但不生效**，会话状态也不可见。 |
| WCH-Link 只支持 RISC-V 模式 | WCH-Link 官方有 RISC-V 与 ARM/DAP 两套模式，且**切换方式是替换 link 固件**（进 IAP 后写另一份固件：`WCH-Link_APP_IAP_RV.bin` / `..._ARM.bin` / `WCH-DAPLink_APP_IAP.bin`）。本工程只实现 RISC-V 那套：WCH-Link 模式对模式查询恒答「RISC-V」，并实现了 IAP 钩子，但**它自己**不能变成官方 ARM 模式。本工程双模式固件里的"DAP 侧"是本工程自己的 CMSIS-DAP（`0d28:0204`），不是 WCH 的 ARM 固件；要切到它请用 PA4 按键（重新枚举 USB），不要用 WCH-LinkUtility 的「切换模式」（那会覆盖本固件）。详见「WCH-Link 模式细节」里「WCH 官方的两种 Link 模式」。 |
| WCH-Link 主机工具受限 | 只有支持 RVSWD 的主机能用。WCH 官方 OpenOCD 的 `wlinke` 适配器可正常连接（CH32X035 的 SDI 就是 DCK+DIO 两线）；**通用 OpenOCD/pyOCD 的 CMSIS-DAP 路径不适用于本模式**（此时 USB 身份是 `1a86:8010` 的 WCH-Link，不是 CMSIS-DAP）。 |
| WCH-Link 与 CMSIS-DAP 不能同时运行 | 两者都用 PA2(SWCLK) / PA3(SWDIO)，任意时刻只有一套在跑。固件固定是**双模式**（两套代码始终同编入），用外接按键（PA4 接 GND）运行期切换，切换会重新枚举 USB（主机侧等同于换设备）。 |
