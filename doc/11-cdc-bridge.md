## 11. CDC ↔ USART4 串口桥（两模式共用）

```mermaid
flowchart LR
    HOST["主机 CDC（ACM）<br/>SET_LINE_CODING"] -->|dwDTERate| LINE["drv_cdc_line<br/>存行编码 + drv_uart_set_baudrate"]
    subgraph BUF["drv_uart 双 FIFO（CherryRB）"]
        direction LR
        TXF["TX FIFO<br/>生产=USB 中断<br/>消费=USART4 中断"]
        RXF["RX FIFO<br/>生产=USART4 中断<br/>消费=主循环"]
    end
    CB_OUT["CDC OUT 回调<br/>中断上下文，非阻塞写"] -->|drv_uart_tx_free 背压| TXF
    TXF --> UTX["USART4 TX PB0"]
    URX["USART4 RX PB1"] --> RXF
    RXF --> CB_IN["主循环 chry_dap_cdc_bridge /<br/>wchlink_cdc_service"] --> CDCIN["CDC IN 端点"]
```

- **背压**：OUT 回调重挂端点前先查 `drv_uart_tx_free()`；余量不足就不重挂，主机收到 NAK 自然阻塞。
- `usbd_ep_start_write` **非中断安全**，故 IN 方向（UART RX → CDC IN）必须在主循环推进。
- 测量统计：`drv_uart_rx_overflow_count()` / `drv_uart_tx_overflow_count()`，正常收发下
  `tx_overflow` 应恒为 0（否则说明背压失效）。
