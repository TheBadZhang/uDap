# 第二部分：架构与代码结构

## 6. 仓库结构

```text
uDap/
├── firmware/                    xmake 工程根
│   ├── src/                     本工程代码（应用 + 移植 glue）
│   │   ├── main.c               入口：双模式轮转 / overlay 初始化 / USB 重枚举
│   │   ├── mode_overlay.h       overlay 段属性宏（.highcode.dap / .overlay.wchlink …）
│   │   ├── system_ch32x035.c    时钟与系统初始化
│   │   ├── ch32x035_it.c        中断向量实现
│   │   ├── usb_config.h         CherryUSB 应用侧配置（必选）
│   │   ├── bsp/                 板级：延时/打印/系统复位/USB 硬件/UID
│   │   ├── port/                CherryUSB CH32X035 USBFS 设备端口 glue（必选）
│   │   ├── dap/                 CMSIS-DAP glue（描述符 + SWD 位翻转）
│   │   ├── wchlink/             WCH-Link glue（transport/protocol/rvswd/target/flash/session/usb）
│   │   ├── status/              状态灯接口（本板空实现）
│   │   └── drv/                 UART 桥 / 模式按键 / 模式持久化 / 电源开关（空实现）
│   ├── sdk/                     WCH CH32X035 SDK（Core/Peripheral/Startup/Ld）
│   ├── third_party/             仅上游 submodule：cherryusb / cherryrb / cherrydap
│   ├── toolchains/wch-riscv/    工具链探测
│   ├── tools/                   测速 / 烧录 / 串口压力测试脚本（Python + PowerShell + Tcl）
│   ├── xmake.lua                构建脚本（唯一构建：双模式）
│   └── build.ps1                便捷构建脚本
├── hardware/                    KiCad 工程（uDap.kicad_sch / .kicad_pcb）+ ch552 变体
└── software/                    上位机等（当前为空）
```
