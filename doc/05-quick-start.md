## 5. 快速开始

### 构建

```powershell
cd firmware

# 便捷脚本（release）。产物固定是 CMSIS-DAP + WCH-Link 双模式固件
.\build.ps1
.\build.ps1 -EsigSn y     # 用芯片 ESIG UID 作序列号（每颗芯片唯一）
.\build.ps1 -Clean        # 先清理

# 或直接用 xmake
$env:WCH_TOOLCHAIN_ROOT = "D:/program/wch/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC12"
xmake f -m release -y
xmake -r

# 用芯片 ESIG UID 作序列号（每颗芯片唯一，多块板子可区分）
xmake f -m release --esig_sn=y -y
xmake -r
```

产物：`firmware/build/release/firmware.elf` / `.bin` / `.map`。

构建结束后除打印 Flash / RAM 占用外，还会按 `.bin` 实际长度兜底检查是否超过 63232 B
（链接脚本的 `OVERLAY` 段 LMA 不受 `FLASH` 区域约束，超了 ld 不会报错）。

> ⚠️ xmake 会把 option 值持久化到本地配置，构建脚本每次都显式传 `--esig_sn=...`；
> 直接敲 xmake 时也请显式指定，否则结果会被上一次的配置决定。
>
> ⚠️ 调试器功能的裁剪选项（`--dap` / `--wchlink`）已经**删除**：两套调试器代码始终
> 都编入，不存在「只编一个模式」的构建（传这两个选项会报 `Invalid option`）。
> 两种模式都占用 PA2/PA3，不能同时运行，运行期用按键切换。
>
> ⚠️ `--esig_sn=y` 打开后 CMSIS-DAP 模式的 USB 序列号从 `DEADBEEF` 变成芯片 UID，
> 主机侧会当成**另一台设备**，需要重新匹配一次驱动（WinUSB 自动匹配，无需手工装）。

### 烧录

```powershell
wlink flash firmware/build/release/firmware.elf
```

### 使用

```sh
# 调试器（会同时看到 uDAP CMSIS-DAP 探针和一个 CDC 串口）
openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg
probe-rs info --chip stm32f411ceu --probe DEADBEEF

# 串口
python -m serial.tools.miniterm COM21 115200
```

> 上面的 `--probe DEADBEEF` 是**默认**序列号。若用 `--esig_sn=y` 构建过，
> 探针标识会变成芯片 UID（24 位十六进制），用 `probe-rs list` 或
> `Get-PnpDevice` 查实际值。
