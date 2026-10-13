## 10. 模式一：CMSIS-DAP 数据通路

USB 身份：`0d28:0204`（WinUSB，MS OS 2.0 描述符自动匹配），同时提供一个 CDC 串口。

```mermaid
flowchart LR
    subgraph HOST["PC 主机"]
        OCD["OpenOCD / pyOCD / probe-rs"]
        TERM["串口终端"]
    end

    subgraph DEV["uDap2 · CMSIS-DAP 模式"]
        EP_DAP["DAP 端点<br/>IN 0x85 / OUT 0x06"]
        EP_CDC["CDC 端点<br/>IN 0x81 / OUT 0x02 / INT 0x83"]
        DAPMAIN["dap_main.c<br/>描述符 + 端点分发"]
        DAPC["CherryDAP DAP.c<br/>DAP_ProcessCommand"]
        SWDP["sw_dp.c<br/>SWD 位翻转后端"]
        CDCL["drv_cdc_line<br/>行编码 + 波特率"]
        UART["drv_uart<br/>CDC ↔ USART4 桥"]
    end

    subgraph TGT["目标板"]
        TS["SWCLK PA2 / SWDIO PA3 / nRESET PA5"]
        TU["USART4：PB0=TX / PB1=RX"]
    end

    OCD -- "USB bulk" --> EP_DAP
    TERM -- "USB CDC ACM" --> EP_CDC
    EP_DAP --> DAPMAIN --> DAPC --> SWDP --> TS
    EP_CDC --> DAPMAIN --> CDCL --> UART --> TU
```

- 主循环：`chry_dap_handle()`（推进 DAP 收发）+ `chry_dap_cdc_bridge()`（推进串口桥）。
- CDC OUT 方向做**背压**：`drv_uart_tx_free()` 余量足够才重挂端点，USB bulk OUT 天然流控，
  主机 `write()` 自然阻塞在波特率上，不丢字节。
- SWD 快/慢路径阈值由 `DELAY_FAST_CYCLES` 决定（阈值 = `24MHz / (2 + DELAY_FAST_CYCLES)`，
  本工程 = 2 → 6 MHz）。
