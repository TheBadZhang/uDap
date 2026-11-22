# uDap（AT32F405）Bootloader 与上位机协议规范

本文件是「bootloader 固件」与「PC 上位机」两侧的共同契约。修改任一侧都必须先改这里。

## 1. Flash 布局

| 区域 | 起始地址 | 大小 | 说明 |
| --- | --- | --- | --- |
| bootloader | `0x08000000` | 16 KiB（`0x4000`） | 上电先跑它；自带 USB 设备栈（DFU + WinUSB） |
| application | `0x08004000` | 112 KiB（`0x1C000`） | uDap 主固件（CMSIS-DAP + CDC） |
| USD（选项字节） | `0x1FFFF800` | 2 KiB | data0/data1 存串口/LED 模式，data2 存 boot 请求标记 |

- 应用工程的链接脚本必须用 `0x08004000` 起始、`_Min_Stack_Size` 等照旧；启动文件里的向量表偏移通过 `SCB->VTOR = 0x08004000` 解决（AT32 的 `system_at32f402_405.c` 里 `VECT_TAB_OFFSET` 就是干这个的）。
- RAM 布局不变（128 KiB 档：RAM = 0x20000000, 70 KiB）。
- `.noinit` 段里放 `g_udap_boot_magic`（`0x55444642` = 'UDFB'），应用在 `NVIC_SystemReset()` 之前写它，bootloader 上电/复位后读到就**不跳应用**，直接停在 USB 升级模式。bootloader 自己启动时必须把它清零。

## 2. 进入 bootloader 的三种方式

1. **应用请求**：应用收到 vendor command `UDAP_VC_ENTER_BOOT`（0x03）→ 写 `g_udap_boot_magic` → `NVIC_SystemReset()`。bootloader 看到 magic 就停住。
2. **应用无效**：bootloader 读 `0x08004000` 的初始 SP 与 Reset 向量，若 `SP` 不在 `0x20000000..0x20011800` 范围内或 Reset 向量不在 `0x08004000..0x08020000` 范围内，判定无应用，停在升级模式（板子第一次烧、或擦除后常见）。
3. **手动**：目前板上没有按键，留空。将来接入按键时，上电按住即可强制进入。

bootloader 跳应用前：`__disable_irq()`、关 SysTick、把 `SCB->VTOR` 设成 `0x08004000`、读初始 MSP、跳到 Reset 向量。跳之前把 `g_udap_boot_magic` 清零。

## 3. USB 身份

| 模式 | VID | PID | 接口 |
| --- | --- | --- | --- |
| 应用（CMSIS-DAP） | `0x0D28` | `0x0204` | 0 = WinUSB DAP，1/2 = CDC |
| bootloader | `0x0D28` | `0x0205` | 0 = DFU，1 = WinUSB（私有批量协议） |

- 序列号统一用芯片 UID 派生的 16 字符十六进制串（应用里已经实现 `udap_serial_number()`，bootloader 复用同一算法：`0x1FFFF7E8/0x1FFFF7EC` 两个 32 位按小端拼成 16 个十六进制字符）。
- 两个接口都走 WinUSB：MS OS 2.0 描述符里把接口 1 标记为 WinUSB（`DeviceInterfaceGUID` 用 `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`，和 Newlib/WinUSB 模板一致即可，上位机会按 VID/PID + GUID 打开）。
- DFU 用标准 DFU 1.1（`class/dfu`），`wTransferSize = 1024`，alt 0 对应「内部 Flash 应用区」`0x08004000` 起。这样 `dfu-util -d 0d28:0205 -a 0 -s 0x08004000:leave -D app.bin` 能直接刷。

## 4. WinUSB 私有批量协议（bootloader，接口 1）

端点沿用应用侧的编号习惯：OUT = `0x06`，IN = `0x85`（各自 512 字节 HS 包）。

一个命令 = 一次 OUT 传输；应答 = 一次 IN 传输。命令第 1 字节是命令码，第 2 字节保留（填 0）。

| 命令 | 码 | 请求负载 | 应答 |
| --- | --- | --- | --- |
| `GET_INFO` | `0x01` | 无 | 见下（28 字节） |
| `ERASE_APP` | `0x02` | 无 | `[0x01, status]` |
| `WRITE` | `0x03` | `[0x03, 0, addr u32 LE, len u16 LE, data...]` | `[0x03, status]` |
| `READ` | `0x04` | `[0x04, 0, addr u32 LE, len u16 LE]` | `[0x04, status, data...]` |
| `FINISH` | `0x05` | `[0x05, 0, total_len u32 LE, crc32 u32 LE]` | `[0x05, status]` |
| `REBOOT` | `0x06` | 无 | `[0x06, 0]`，随后设备复位进应用 |

- `status`：`0x00` 成功；`0x01` 参数错（地址越界/长度过大）；`0x02` Flash 操作失败；`0x03` CRC 不匹配；`0x04` 未擦除就写。
- `GET_INFO` 应答：`[0x01, 0, proto_ver u8, fw_major u8, fw_minor u8, fw_patch u8, app_base u32 LE, app_size u32 LE, page_size u32 LE, max_chunk u16 LE, caps u16 LE]`，共 28 字节（含前两字节）。`caps` bit0 = 支持 DFU，bit1 = 支持 WinUSB 写入。
- `WRITE` 的 `len` 上限 = `max_chunk`（512 - 8 = 504，取 504）。写入必须 4 字节对齐；bootloader 内部按页（AT32F405 为 2 KiB）补齐。
- `FINISH` 校验整片 CRC32（IEEE，初值 `0xFFFFFFFF`，末尾异或 `0xFFFFFFFF`，与 `zlib.crc32` 一致）；校验通过后跳应用。
- 协议版本 `proto_ver = 1`。

## 5. 上位机（`software/udap-studio/`）

- C++/CLI + WinForms，目标 .NET Framework 4.8（兼容 Win7）。若 C++/CLI 工具链缺 `MSCOREE.lib`，构建脚本先用 `dumpbin /exports C:\Windows\System32\mscoree.dll` + `lib /def` 现场生成导入库；实在不行退化为同目录的 C# 版本（只有一个 `.cs`，用 `csc.exe` 编译），功能一致。
- 功能：
  1. 设备列表：枚举 `0d28:0204`（应用）与 `0d28:0205`（bootloader），显示序列号与固件版本。
  2. 应用设置：LED 模式（0..3）、串口模式（只读，本板固定单串口）、调试器模式（CMSIS-DAP）、软复位、读芯片 UID —— 全部走第 4 节的 vendor command（应用侧 `0x80` 前缀，见 `application/dap/udap_vendor.h`）。
  3. 烧录：选 `.bin`/`.hex` → 若当前是应用则先发 `ENTER_BOOT` → 等 `0d28:0205` 出现 → `ERASE_APP` → 分块 `WRITE` → `FINISH` → `REBOOT`。进度条 + 日志。
  4. DFU 可选页签：提示可用 `dfu-util` 命令行。
- 打包：`software/udap-studio/build.ps1` 一条命令产出 `udap-studio.exe`（含图标可省）。

## 6. 应用侧 vendor command 现状（供上位机对照）

见 `application/dap/udap_vendor.h`：`0x80` 前缀，子命令 `GET_INFO=0x00`、`SET_UART_MODE=0x01`、`SET_LED_MODE=0x02`、`ENTER_UF2=0x03`（本板语义改为「进 bootloader」）、`SET_DEBUGGER_MODE=0x04`、`RESET=0x05`、`GET_UID=0x06`。应答首字节回显 `0x80`。
