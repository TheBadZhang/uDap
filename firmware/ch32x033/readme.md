# uDap-CH32X033

事实上 CH32X033 和 CH32X035 的程序是可以互换的（只不过 CH32X035 还有 USB PD/CC 外设），
所以 uDap-CH32X033 还是使用 `firmware/ch32x035` 的编译产物，只是 port 到了不同的引脚上。

引脚差异已经做成**板级表**，不用维护第二份代码：

| 文件 | 内容 |
| --- | --- |
| `firmware/ch32x035/application/board/board_ch32x033.h` | 本板的信号 → 引脚表 |
| `firmware/ch32x035/application/board/board_ch32x035f8u6.h` | 初版板（默认） |
| `firmware/ch32x035/application/board_pin_map.h` | 按 `BOARD_VARIANT` 选表（含板级表的契约） |

## 引脚

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| LED_UART | PB1 | 低电平点亮 |
| LED_DAP | PB7 | 低电平点亮 |
| nRESET | PA9 | 目标复位，低有效（开漏模拟） |
| RX1 / TX1 | PA3 / PA2 | USART2 默认引脚 |
| SWCLK / SWDIO | PA1 / PA0 | CMSIS-DAP 与 WCH-Link 共用 |
| TX2 / RX2 | PA10 / PA11 | USART1 + `USART1_RM = 01`（部分重映射 1） |
| 按键 | PC3 | 内部上拉，按下 = 低 |

本板没有 DTR/RTS 输出脚，所以默认串口模式是**双串口**（两路 CDC 都能直接当桥用）；
双击按键可以切回单串口。DTR/RTS 仍然可以由上位机设置，只是不下发到引脚。

## 构建 / 烧录

```powershell
cd firmware\ch32x035

.\build.ps1 -Board ch32x033             # CMSIS-DAP + UF2 bootloader（默认布局）
.\build.ps1 -Board ch32x033 -Wchlink y # 双模式（CMSIS-DAP + WCH-Link），无 UF2
```

产物与默认板完全一样（`build\release\firmware.uf2` / `firmware_full.bin` 等），
烧录流程也相同（`flash.ps1` / `flash_isp.ps1`，或首次烧录后拖 UF2）。

> 烧录验证要点：PA9 = nRESET、PA0/PA1 = SWD、PA2/PA3 = COM1、PA10/PA11 = COM2、
> PB1/PB7 = 两颗灯、PC3 = 按键。板子上没有 DTR/RTS，所以「单串口」模式下这两个
> 控制线不会有任何电平变化。
