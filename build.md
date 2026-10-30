
### 工具链与构建

| 项目 | 选型 |
| --- | --- |
| 构建系统 | **xmake**（应用与 bootloader 是**同一个工程里的两个 target**） |
| WCH 编译器 | WCH RISC-V Embedded GCC 15（MounRiver Studio 2 自带；没装时自动回退 GCC 12。GCC15 的 `-Os` 更省：bootloader 14.5 KiB vs 15.3 KiB，应用 25.8 KiB vs 26.7 KiB） |
| 编译选项 | `-march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore`，`-Wall -std=gnu11` |
| 优化 | LTO + `--gc-sections`；`.highcode` 段启动时从 Flash 搬到 RAM 执行。必须用 `-Os`：`-O3` 会让 `.highcode` 膨胀到 18.5 KiB（Flash/RAM 都装不下），`-Og`/`-O0` 又装不进 Flash —— debug 也是 `-Os` + LTO，只额外带调试符号 |
| UF2 打包 | `third_party/uf2`（microsoft/uf2 子模块）里的 `utils/uf2conv.py` |
| 辅助脚本 | `firmware/ch32x035/build.ps1`（包了工具链路径与所有 option）、`flash.ps1`（WCH-Link / SDI 烧录） |
| 烧录 | `firmware/ch32x035/flash.ps1`（WCH 官方 OpenOCD）或 UF2 拖拽升级 |


### 构建

```powershell
cd firmware\ch32x035

# 一键构建（默认）：bootloader + 应用 + firmware.uf2 + firmware_full.bin
.\build.ps1
.\build.ps1 -EsigSn n              # 固定序列号 DEADBEEF（所有板子相同）
.\build.ps1 -Wchlink y             # 双模式（+ WCH-Link）；这个构建不支持 UF2
.\build.ps1 -Bootloader n          # 不要 bootloader，应用从 0x0 起
.\build.ps1 -Clean                 # 先清理再构建（换编译器版本时会自动做一次）

# 也可以走 cmd 包装（参数原样透传，优先 pwsh，只有 5.1 时也能跑）
build.bat
build.bat -Wchlink y

# 或直接用 xmake（记得显式传选项，xmake 会记住上次的值）
# 工具链优先 GCC15，其次是 GCC12；也可以自己指定任意版本/安装路径
$env:WCH_TOOLCHAIN_ROOT = "C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC15"
xmake f -m release --esig_sn=y --wchlink=n --swd_pins=pa5_pa6 --bootloader=y -y
xmake -P . -r              # 两个 target 都编：bootloader → firmware
xmake -P . -r firmware     # 只编应用
xmake -P . -r bootloader   # 只编 bootloader（默认布局下才存在）
```

> `-P .` 不能省：仓库根目录也有一个只含 task 的 `xmake.lua`，不给 `-P` 时 xmake
> 会认成那个工程，报 `not a valid target name`。
>
> 换编译器版本（GCC12 ↔ GCC15）后**必须重编**：xmake 的对象依赖里没有编译器版本，
> 只改 `WCH_TOOLCHAIN_ROOT` 不清缓存，会继续拿旧编译器编出来的 `.o` 去链接
> （现象：打印的工具链路径变了，尺寸却一点没变）。`build.ps1` 把上次用的工具链
> 记在 `build\.toolchain-root`，发现变了会自动 `xmake clean --all`；直接敲 xmake
> 的话请自己先 `xmake clean --all`。

产物：

| 文件 | 说明 |
| --- | --- |
| `build\<mode>\firmware.elf` / `.bin` / `.map` | 应用固件（默认链接到 `0x4000`） |
| `build\<mode>\firmware.uf2` | 拖进 UF2 bootloader 的升级包（family `0x75446170`） |
| `build\<mode>\firmware_full.bin` | **bootloader + 应用的合并镜像**，从 `0x0` 整片烧录（没有调试器时用） |
| `build\<mode>\bootloader.elf` / `.bin` / `.map` | bootloader（链接到 `0x0`，16 KiB 上限，GCC15 下实际约 14.5 KiB） |

构建结束后除打印 Flash / RAM 占用外，还会按 `.bin` 实际长度兜底检查应用区是否超过
46 848 B（= 63 232 - 16 384；链接脚本的 `OVERLAY` 段 LMA 不受 `FLASH` 区域约束，
超了 ld 不会报错）。UF2 打包依赖 `python`，找不到时只打印一行提示并跳过（`.bin`
仍然可用）。

### 烧录

```powershell
cd firmware\ch32x035
.\flash.ps1                   # 有 WCH-Link/SDI：bootloader@0x0 + 应用@0x4000
.\flash.ps1 -SkipBootloader    # 只更新应用
.\flash.ps1 -NoBootloader      # 老布局：只写应用@0x0（-Wchlink y 或 -Bootloader n 的构建）

.\flash_isp.ps1                # 没有调试器：整片烧 firmware_full.bin（芯片自带 USB ISP）
.\flash_isp.ps1 -Info          # 只看芯片信息
```

`flash_isp.ps1` 用芯片自带的 3 KB 系统 BOOT 区：用户区为空/无效（或应用把下次复位
切到 BOOT 区）时芯片从那里启动，枚举成 USB ISP 设备（`4348:55e0`），因此空片也能
烧第一版固件。命令行依赖 [wchisp](https://github.com/ch32-rs/wchisp)
（`cargo install wchisp` 或下载 nightly 预编译；Windows 可能还需要
`CH375DLL64.dll`，或用 Zadig 换 WinUSB 驱动）；没有它时脚本会打印 WCHISPTool 的
手动步骤。

之后升级应用就不用接线了：上电长按 PB3 → 出现 U 盘 → 拖 `firmware.uf2`。
也可以由上位机发 vendor command `ENTER_UF2` 直接进 bootloader。

> ⚠️ xmake 会把 option 值持久化到本地配置，构建脚本每次都显式传全部选项；
> 直接敲 xmake 时也请显式指定，否则结果会被上一次的配置决定。
>
> ⚠️ `--esig_sn=y` 打开后 CMSIS-DAP 模式的 USB 序列号从 `DEADBEEF` 变成芯片 UID，
> 主机侧会当成**另一台设备**，需要重新匹配一次驱动（WinUSB 自动匹配，无需手工装）。
>
> ⚠️ **UF2 拖拽升级只适用于单模式（CMSIS-DAP）构建**：双模式固件约 57 KiB，
> 62 KiB Flash 放不下 16 KiB 的 bootloader，`--wchlink=y` 会自动关掉
> `--bootloader` 的布局（打印里能看到 `Layout:` 一行）。
