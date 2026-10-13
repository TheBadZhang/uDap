## 12. 模式二：WCH-Link

### 12.1 分层与依赖方向

USB 身份：`1a86:8010`（厂商接口 + CDC），序列号 = `"035"` + UID 前 9 位十六进制。
依赖从 `usb` 进入 `session`，再进入 `target` / `rvswd` / `transport`，**底层不反向依赖上层**。

```mermaid
flowchart TB
    subgraph L_USB["usb/ — USB 适配层"]
        WUSB["wchlink_usb.c<br/>枚举 · 端点挂载 · 异步完成 · 超时回收"]
    end
    subgraph L_SESSION["session/ — 会话状态机"]
        SESS["wchlink_session.c<br/>command 分派 + data IN/OUT 挂载"]
        CMD["wchlink_command.c<br/>命令语义"]
        XFER["wchlink_transfer.c<br/>Flash/内存数据阶段 + loader 生命周期"]
    end
    subgraph L_TARGET["target/ — 目标适配 seam"]
        REG["rvswd_target_registry.c<br/>按 family / ChipID 分派"]
        CONN["rvswd_target_connect.c"]
        PORTS["wchlink_target_ports.c<br/>连接后锁定 module"]
        FAM["rvswd_target_x03x / l103 / v20x / v30x / ch58x / ch59x"]
        RES["rvswd_target_result.c"]
    end
    subgraph L_RVSWD["rvswd/ — RISC-V DM 调试原语"]
        DBG["rvswd_debug.c"]
        MEM["rvswd_memory.c"]
        RESET["rvswd_reset.c"]
        OP["rvswd_operation.c"]
    end
    subgraph L_TRANSPORT["transport/ — RVSWD 物理层"]
        FRAME["rvswd_frame.c<br/>帧编解码"]
        PHY["rvswd_phy_gpio.c<br/>PA2/PA3 位翻转"]
        TRANS["rvswd_transport.c<br/>DMI 收发 + 重试 + 状态分类"]
    end
    subgraph L_FLASH["flash/ — Flash 事务 backend"]
        FL1["rvswd_flash_ch32.c"]
        FL2["rvswd_flash_ch58x_59x.c + .S stub"]
        FLO["rvswd_flash_option.c"]
    end
    subgraph L_PROTO["protocol/ — wire 编码"]
        WIRE["wchlink_wire.c<br/>0x81 命令 / 0x82 回复帧"]
        FAMH["wchlink_family.h"]
    end

    WUSB --> SESS
    SESS --> CMD
    SESS --> XFER
    CMD --> REG
    XFER --> PORTS
    REG --> FAM
    PORTS --> FAM
    FAM --> DBG
    FAM --> MEM
    FAM --> RESET
    FAM --> FL1
    FAM --> FL2
    FAM --> FLO
    DBG --> OP
    MEM --> OP
    RESET --> OP
    OP --> TRANS
    TRANS --> FRAME
    TRANS --> PHY
    CMD --> WIRE
    XFER --> WIRE
    FL1 --> OP
    FL2 --> OP
```

各层职责（详见各目录 `README.md`）：

| 层 | 职责 | 不做 |
| --- | --- | --- |
| `usb/` | 枚举、端点挂载、异步完成标志、超时回收、主循环调度 | 不解释命令字段，不调 target/rvswd/flash |
| `session/` | 把 wire command 组织成有序目标操作，映射错误 | 不实现物理传输 |
| `target/` | 目标身份、ChipID→module 解析、target ports、loader 契约、族专用行为 | 不让 backend 反向识别族 |
| `rvswd/` | DM 调试原语（寄存器/内存/复位），失败诊断按次调用保存 | — |
| `transport/` | 帧编解码、GPIO 物理收发、DMI 传输、时钟/方向/重试 | 不保存芯片 Flash 参数 |
| `flash/` | 无族选择的 Flash 事务 backend 与共享擦除 stub | 不按 family 选 backend |
| `protocol/` | WCH-Link USB 命令帧（0x81 命令 / 0x82 回复） | — |

---

### 12.2 命令处理时序

USB 回调只更新端点状态，完整 command/data 阶段由**主循环**交给 session 处理。

```mermaid
sequenceDiagram
    autonumber
    participant H as 主机 (WCH-LinkUtility / OpenOCD)
    participant U as wchlink_usb (USB 回调/主循环)
    participant S as wchlink_session
    participant C as wchlink_command
    participant T as wchlink_transfer → wchlink_target_ports
    participant M as 族 module (X03X/V20X/…)
    participant R as rvswd_* / rvswd_transport
    participant P as rvswd_phy_gpio (PA2/PA3)
    participant G as 目标芯片

    H->>U: OUT 端点写入命令帧 (0x81 …)
    U->>S: 主循环 wchlink_session_process(request)
    S->>C: wchlink_command_process()
    C->>T: 组织目标操作（连接/读 ID/读内存…）
    T->>M: 通过 module 的 ops 调用族专用实现
    M->>R: read_dmi / write_dmi / loader 执行
    R->>P: drive_range / sample_range（位翻转）
    P->>G: DCK + DIO 物理帧
    G-->>P: 目标回复
    P-->>R: 采样值
    R-->>M: DMI 结果（status / ok / retryable）
    M-->>T: 目标结果（rvswd_target_result）
    T-->>C: 值 / 状态
    C-->>S: command_result（status/action/policy/len）
    S-->>U: response bytes
    U->>H: IN 端点回写回复帧 (0x82 …)

    Note over S,U: Flash 数据阶段：session 声明 next_data_io，<br/>USB 主循环按方向挂载一次端点，<br/>callback 只复制数据并置 pending
```

- `WCHLINK_FAMILY_DEVICE_MODE = 0x0f`：`0x02 Query` 恒回“RISC-V 模式”；`0x01 IAP` 不回复，
  改为 `bsp_system_enter_isp()` 复位进 ISP bootloader（官方工具“切换模式”的入口）。
- 厂商接口会在同一端点上传 256 B 多包（4×64 B），依赖 DATA0/DATA1 toggle 同步；
  实测无 toggle 失配，故本工程未改端口层时序。

---

### 12.3 目标芯片族分派

`registry` 只做索引/分派，族差异集中在各 `rvswd_target_xxx.c`；profile 只存稳定数据与能力，
寄存器访问顺序、前置配置、错误恢复、loader 时序由族模块的 C 函数实现。

```mermaid
flowchart TB
    CID["读 ChipID / family"] --> REG{{rvswd_target_registry<br/>module_from_chip_id / module_from_family}}
    REG --> X03X["CH32X03X<br/>官方抓包独立实现"]
    REG --> L103["CH32L103<br/>官方抓包独立实现"]
    REG --> V20X["CH32V20X<br/>已验证：ChipID/ESIG/loader/擦写/读保护"]
    REG --> V30X["CH32V30X<br/>占位（TODO）"]
    REG --> CH58X["CH58X<br/>占位 + 共享擦除 stub"]
    REG --> CH59X["CH59X<br/>占位 + 共享擦除 stub"]

    X03X --> MOD["struct rvswd_target_module"]
    L103 --> MOD
    V20X --> MOD
    V30X --> MOD
    CH58X --> MOD
    CH59X --> MOD

    MOD --> OPS["ops 集合：<br/>probe / memory / loader / flash / control"]
    MOD --> PROF["profile：identity · option · loader"]
    MOD --> CAP["capabilities：packet_mode · chip_info_layout · streaming"]
    OPS --> PORTS["wchlink_target_ports（连接后锁定 module）"]
```

`struct rvswd_target_module`（`src/wchlink/target/rvswd_target_module.h`）把
**行为**（ops 函数指针）与**数据**（profile / capabilities）分开：USB、transport、
通用 RVSWD 只依赖中立类型与原语，不读取族模块的私有数据。
