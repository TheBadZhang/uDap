## 14. CMSIS-DAP 模式细节

### 14.1 序列号：固定值 / 芯片 ESIG UID

**默认**取芯片 ESIG 区的 **96 位出厂 UID**（出厂烧录、只读、每颗芯片唯一）作 USB
序列号，转成 24 个大写十六进制字符 —— 多块板子同时插在一台机器上时才能区分。
`-EsigSn n` 退回所有板子相同的固定串 `DEADBEEF`（VID:PID 也相同，无法区分）：

```powershell
.\build.ps1               # 默认：序列号 = 芯片 UID（-EsigSn y）
.\build.ps1 -EsigSn n     # 退回固定 DEADBEEF

xmake f -m release -y     # xmake 侧默认同样是 esig_sn
```

**寄存器定义**（代码在 `src/dap/dap_main.c`，见 CH32X035 应用手册第 19 章
「电子签名（ESIG）」）：

| 地址 | 名称 | 内容 |
| --- | --- | --- |
| `0x1FFFF7E8` | `R32_ESIG_UNIID1` | `U_ID[31:0]` |
| `0x1FFFF7EC` | `R32_ESIG_UNIID2` | `U_ID[63:32]` |
| `0x1FFFF7F0` | `R32_ESIG_UNIID3` | `U_ID[95:64]` |

拼接顺序为 UNIID1 → UNIID2 → UNIID3（与沁恒例程的打印顺序一致；顺序只是约定，
不影响唯一性）。同一份字符串同时供两处使用：

- USB 描述符的 iSerialNumber（`dap_main.c` 的 `string_descriptors[3]`）
- CMSIS-DAP 的 `DAP_GetSerNum` 命令（`src/dap/DAP_config.h` 的 `DAP_GetSerNumString()`）

两点注意：

1. **打开后会改变主机侧的设备实例**。USB 序列号参与 Windows 的设备枚举与驱动匹配，
   换了序列号等于换了一台设备，首次插入需要重新匹配一次驱动（本设备是 WinUSB，
   靠 MS OS 2.0 描述符自动匹配，无需手工安装）。原来绑定 `DEADBEEF` 实例的
   OpenOCD / pyOCD 配置需要用新序列号。
2. **UID 读不出来时会退回固定串**。若三个寄存器读回全 0 或全 FF（地址不对、
   ESIG 不可读等），固件不会把「看似唯一、其实所有板子都相同」的串当序列号发出去，
   而是退回 `DEADBEEF`，让「没生效」在主机侧可见。（本工程 `_write` 是空实现，
   没有日志可用，所以只能靠这个现象判断。）

`--esig_sn=y` 只影响 CMSIS-DAP 模式的 USB 序列号（在 `src/dap/dap_main.c` 里设置）；
双模式固件总是编入 CMSIS-DAP，所以这个选项永远有效，不存在「依赖被裁掉的模式」的问题。

### 14.2 与参考工程的两处必要差异

这两处都是参考工程 `ch32x035-usb-dap-main` 里不存在的构建陷阱，已在本工程解决：

1. **`bool` 未定义**：CherryUSB 的 `class/cdc/usbd_cdc_acm.h` 用了 `bool` 但自己没包含
   `<stdbool.h>`，而 `dap_main.h` 里 `usbd_cdc.h` 排在 `usbd_core.h`（内含 `<stdbool.h>`）
   之前。参考工程是**直接改了 cherryusb 子模块**（它的子模块处于 dirty 状态），
   本工程不改 submodule，把 `#include <stdbool.h>` 放在自己的 `src/dap/dap_main.h` 里。
   **升级 cherryusb submodule 时需重新确认这一点是否已在上下游修好。**
2. **`Delay_Init` / `Delay_Us` 未定义**：本工程不编译 `sdk/Debug/debug.c`
   （它无条件定义 `_write` / `_sbrk`，与 `src/bsp/bsp_print.c` 的 retarget 冲突），
   但本 SDK 版本的 `sdk/Peripheral/src/ch32x035_pwr.c` 里
   `PWR_VDD_SupplyVoltage()` 会调用这两个函数（参考工程用的旧版 SDK 没有这两行，
   而且它也从不调用该函数，所以没暴露问题）。已在 `src/bsp/bsp_delay.c` 末尾提供
   转发到 SysTick 延时 的等价实现。

### 14.3 状态指示灯

本板只有一颗灯（PB12，高电平点亮），接的是 CMSIS-DAP 的 **Connect LED**：主机连接到
DAP 时点亮、断开时熄灭。改脚或改极性只需动 `src/dap/DAP_config.h` 里的三个宏：

```c
#define LED_CONNECTED_PORT         GPIOB
#define LED_CONNECTED_PIN          GPIO_Pin_12
#define LED_CONNECTED_ACTIVE_HIGH  1U   // 0 = 低电平点亮
```

它由主机下发的 `ID_DAP_HostStatus(0x01)` 驱动（DAP.c 的 `DAP_HostStatus`）：

| 状态类型 | 固件回调 | 主机行为 |
| --- | --- | --- |
| `DAP_DEBUGGER_CONNECTED`(0) | `LED_CONNECTED_OUT` | pyOCD 在 `connect()` 置 1、`disconnect()` 置 0 |
| `DAP_TARGET_RUNNING`(1) | `LED_RUNNING_OUT` | pyOCD **从不置 1**（只下发 0），OpenOCD 不发该命令 |

因此 `LED_RUNNING_OUT` 保持空实现（本板也没有第二颗灯）。**这意味着灯的可见性取决于
主机工具**：pyOCD 下会亮；OpenOCD 若不支持该命令则始终不亮 —— 这种情况需要固件本地
指示（如 USB 枚举后常亮），目前未实现。

> ⚠️ **参考工程自身的 README 与代码不一致**：它的 `README.md` 引脚表写
> `PB12 = nRESET`、`PA5 = 按键`，但 `src/dap/DAP_config.h` 里是 `PA5 = nRESET`
> （它的提交记录 `将pa5作为rst引脚` 改了代码却漏改文档）。本工程照搬的是**代码**
> （PA5），不是它的 README。若本板 PA5 另有用途，必须改 `DAP_config.h`。
