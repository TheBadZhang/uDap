## 22. 其他可参考的实现

本仓库运行期**不依赖**任何参考工程；以下按名称记录，需要时自行取用。

| 工程 | 技术路线 | 可参考部分 |
| --- | --- | --- |
| `ch32x035-usb-wchlink-main` | CherryUSB + 自研 RVSWD 协议栈 | `src/wchlink` 的整体来源（transport / protocol / rvswd / target / flash / session / usb）|
| `ch32x035-usb-dap-main` | CherryUSB + CherryRB + CherryDAP | 本工程 glue（`src/dap`、`src/drv`、`src/bsp`）的来源 |
| `CH32X035F8U`（CheapLink_X035） | WCH USBFS 驱动 + FreeRTOS | PIOC 加速（SWD 提速）、AIRCR 复位序列、TIM3 时间戳、三色 LED 状态机 |

> `ch32x035-usb-wchlink-main/` 目前仍留在 `firmware/` 下（**未跟踪**，也不参与编译）。
> 按本工程惯例（参考工程用完移出）可以删掉；若要留着，注意它没有被 `.gitignore`
> 覆盖，`git add -A` 会把它一起提交。

`CH32X035F8U`（CheapLink_X035）的 SWD 引脚是 PA5/PA6/PA7 加 PIOC 的 PC18/PC19，
与本工程当前取值不同；若最终采用 PIOC 方案，需按它重写 `DAP_config.h` 的引脚宏与
`PORT_SWD_SETUP()`。以 uDap2 原理图 `hardware/uDap.kicad_sch` 为准。
