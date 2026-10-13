## 8. 双模式运行期状态机

两种模式都要占用 `PA2=SWCLK/DCK`、`PA3=SWDIO/DIO`，且 USB 身份不同，
**不可能同时运行**。切换 = 交出 USB → 用另一套描述符重新枚举。

```mermaid
stateDiagram-v2
    [*] --> Boot: 上电 / 复位
    Boot --> LoadMode: SystemInit · 延时 · drv_uart_init<br/>drv_mode_button_init · drv_mode_store_load
    LoadMode --> DAP_Run: 用户选择字 = 0（或无效 → 默认）
    LoadMode --> WCH_Run: 用户选择字 = 1

    state DAP_Run {
        [*] --> DAPOverlay: dap_overlay_init()<br/>清零 DAP 数据段（无代码拷贝）
        DAPOverlay --> DAPLoop: cherrydap_process()<br/>chry_dap_init / handle / cdc_bridge
    }

    state WCH_Run {
        [*] --> WCHOverlay: wchlink_overlay_init()<br/>清零 WCH-Link 数据段（无代码拷贝）
        WCHOverlay --> WCHLoop: wchlink_process()<br/>rvswd_phy_gpio_init / usb_init / usb_process
    }

    DAP_Run --> SwitchOut: PA4 按下（去抖 20ms）
    WCH_Run --> SwitchOut: PA4 按下（去抖 20ms）

    SwitchOut --> Deinit: 退出 overlay 代码<br/>usbd_deinitialize(0)
    Deinit --> Persist: mode_next() 轮转<br/>drv_mode_store_save()
    Persist --> WaitRel: 等待按键释放
    WaitRel --> LoadMode: 重新进入下一模式
```

关键约束（改代码时必须遵守）：

- 数据段的清零**只能由 `main()` 完成**，不能放进 `cherrydap_process()` / `wchlink_process()`
  ——那两个函数自身就在被覆盖的地址范围内，调用即跳进未初始化的地址。
- 覆盖数据段 RAM 之前，必须确认已不在执行那段地址里的代码（先返回 `main()`）。
- 新增模式：在 `firmware_mode_t` 末尾**追加**成员 + 在 `mode_run()` 加一个 `case`
  （已有取值写进 Flash，属 ABI，不可重排；取值须从 0 起连续）。
