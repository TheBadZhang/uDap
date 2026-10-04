# WCH-Link 实现

本目录承载 WCH-Link USB 命令到目标调试操作的实现，按协议、传输、调试原语、目标、会话和 USB 适配分目录组织

依赖方向从 `usb` 进入 `session`，再进入 `target`、`rvswd` 和 `transport`，底层模块不反向依赖上层命令。芯片族差异集中在 `target` 及其调用的专用 backend，通用模块不保存芯片寄存器表

目录下的 README 说明各模块职责和入口，具体协议字段、目标参数和实验结论以源码与项目级资料为准

## 相对来源工程（ch32x035-usb-wchlink-main）的改动

本目录整体移植自 `firmware/ch32x035-usb-wchlink-main/src/wchlink/`，字段、时序和目标参数
均保持原样，只做了下面四类适配：

1. `usb/wchlink_usb.c` 的 CDC 通道：由来源工程的**原样回环**改为与 CMSIS-DAP 模式一致的
   **USART4（PB0=TX / PB1=RX）双向桥**，含 TX FIFO 背压，波特率跟随主机 `SET_LINE_CODING`。
2. `wchlink_usb_init()` 不再初始化状态灯 / 目标电源开关 —— 本板没有这些硬件，
   `src/status/status_led.c` 与 `src/drv/drv_power_switch.c` 都是空实现。
   特别是**本板的 PB12 是 `LED_CONNECTED_OUT`，与来源工程的目标电源开关同引脚不同义**，
   所以电源命令必须保持空实现。
3. `usb/wchlink_usb.c` 调用的 `ch32x035_usbd_ep_abort_in()` 由本工程自己的端口层
   `src/port/usb_ch32x035_dc_usbfs.c`（声明在 `src/port/usb_ch32x035_dc_usbfs.h`）提供 ——
   本工程不编译来源工程的 `third_party/cherryusb_port/`。
4. 序列号改用本工程 `src/bsp/bsp_uid.c` 的 `bsp_get_uid_hex16()`。

其余（`transport/`、`protocol/`、`rvswd/`、`target/`、`flash/`、`session/`）为原样拷贝。
另外本工程用 `-Os` 而不是 `-O3` 编译 WCH-Link（原因见 `firmware/README.md` 的尺寸表）。

## 上板实测结果

已在 uDap2 硬件上（PA2→目标 DCK / PA3→目标 DIO，目标为 CH32X035）实测通过：

- WCH 官方驱动把它识别为 `WCH-LinkRV`；WCH-LinkUtility 报「已连接的 RISC-V 模式 WCH-Link 计数：1」
- WCH 官方 OpenOCD（`wlinke` 适配器）`wlink_init ok`，并成功 examine 目标核
  （`datacount=2 progbufsize=8`、`XLEN=32 misa=0x40901105`）
- 目标 ChipID / ESIG / Flash 读取、目标 RAM 读写、16 KiB 批量读（无 toggle 失配）
- 目标 Flash 擦除 + 编程 + 校验（62 KB 整片 `program … verify` → `Verified OK`）
- CDC（CDC ↔ USART4 桥）可打开并接受主机改波特率

细节（命令与原始输出）见 `firmware/README.md` 的「验证 → WCH-Link 模式」。
