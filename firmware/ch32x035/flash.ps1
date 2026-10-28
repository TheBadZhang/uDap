# 用 WCH-Link + WCH 官方 OpenOCD 烧录 CH32X035（本板 = DAP 自身）
#
# 走 SDI 传输（adapter driver wlinke），接在板子的 DIO/DCK 两脚上。
# 与 SWD 测速用的那个 CMSIS-DAP 探针**不能同时**接：同一对引脚。
#
# **没有调试器时**用 .\flash_isp.ps1：走芯片自带的 USB ISP（系统 BOOT 区），
# 烧 build\release\firmware_full.bin（bootloader + 应用的合并镜像）。
#
# 用法:
#   .\flash.ps1                          # 烧 bootloader@0x0 + firmware@0x5000（UF2 布局）
#   .\flash.ps1 -SkipBootloader          # 只更新应用（bootloader 已经在板子上）
#   .\flash.ps1 -NoBootloader            # 老布局：只烧 firmware@0x0（-Wchlink y 或 -Bootloader n 的构建）
#   .\flash.ps1 -File ..\..\x.bin        # 烧指定文件
#   .\flash.ps1 -NoVerify                # 跳过校验（更快，不建议）
#
# 地址约定（见 bootloader/src/boot_config.h 与 sdk/Ld/Link_highcode_nv_256B_uf2.ld）：
#   0x0000..0x4FFF  bootloader（20 KiB）
#   0x5000..0xF6FF  应用固件
#   0xF700..0xF7FF  预留（NV 配置页）
#
# 烧完 bootloader 后就可以不再用 SWD：上电按住 PB3 进 UF2，把 firmware.uf2
# 拖进出现的 U 盘即可升级应用。

[CmdletBinding()]
param(
    [string]$File = 'build\release\firmware.bin',

    # bootloader 的 bin（与应用在同一份 xmake 工程里，产物同目录）
    [string]$BootloaderFile = 'build\release\bootloader.bin',

    # 应用固件的烧录地址。带 bootloader 的构建是 0x5000。
    [string]$AppBase = '0x00005000',

    # 只烧应用，不动 bootloader（bootloader 已在板子上、且没有改动时用）
    [switch]$SkipBootloader,

    # 老布局：应用在 0x0，且不烧 bootloader（对应 -Wchlink y / -Bootloader n 的构建）
    [switch]$NoBootloader,

    # WCH OpenOCD 安装根目录（含 bin\openocd.exe 与 bin\wch-riscv.cfg）
    [string]$OpenOcdRoot = '',

    [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'

# 常见安装位置，找不到再报错
if (-not $OpenOcdRoot) {
    foreach ($candidate in @(
            'C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD',
            'D:\program\wch\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD')) {
        if (Test-Path (Join-Path $candidate 'bin\openocd.exe')) {
            $OpenOcdRoot = $candidate
            break
        }
    }
}
if (-not $OpenOcdRoot -or -not (Test-Path (Join-Path $OpenOcdRoot 'bin\openocd.exe'))) {
    throw "找不到 WCH OpenOCD（bin\openocd.exe）。请用 -OpenOcdRoot 指定安装根目录。"
}

$exe = Join-Path $OpenOcdRoot 'bin\openocd.exe'
$cfg = Join-Path $OpenOcdRoot 'bin\wch-riscv.cfg'

if (-not (Test-Path $File)) {
    throw "找不到要烧录的文件: $File"
}
$bin = (Resolve-Path $File).Path

if ($NoBootloader) {
    $AppBase = '0x00000000'
    $SkipBootloader = $true
}

$verify = if ($NoVerify) { '' } else { 'verify ' }

# 注意：**不能**用 `program ... reset exit` —— WCH 的 SDI 复位会把核留在 HALT 状态，
# 结果 USB 不枚举（主机侧看不到设备），必须显式 `reset run` + `resume` 才真正跑起来。
# 所以烧写与复位分成不同的 -c：先烧写+校验，最后再复位让核跑起来。
$programCmds = @()

if (-not $SkipBootloader) {
    if (-not (Test-Path $BootloaderFile)) {
        throw "找不到 bootloader: $BootloaderFile（先跑 .\build.ps1，或用 -SkipBootloader / -NoBootloader）"
    }
    $boot = (Resolve-Path $BootloaderFile).Path
    $programCmds += "program {$boot} 0x00000000 $verify"
    Write-Host "bootloader: $boot（$((Get-Item $boot).Length) 字节）-> 0x00000000" -ForegroundColor DarkGray
}

$programCmds += "program {$bin} $AppBase $verify"

Write-Host "OpenOCD : $exe" -ForegroundColor DarkGray
Write-Host "配置    : $cfg（wlinke / SDI）" -ForegroundColor DarkGray
Write-Host "文件    : $bin（$((Get-Item $bin).Length) 字节）-> $AppBase" -ForegroundColor DarkGray
Write-Host ""

$openocdArgs = @('-f', $cfg)
foreach ($c in $programCmds) {
    $openocdArgs += @('-c', $c)
}
$openocdArgs += @('-c', 'reset run', '-c', 'resume', '-c', 'shutdown')

& $exe @openocdArgs
if ($LASTEXITCODE -ne 0) {
    throw "烧录失败（openocd 退出码 $LASTEXITCODE）"
}

Write-Host ""
Write-Host '烧录完成，并已复位运行。' -ForegroundColor Green
Write-Host '  - WCH-Link 走板子独立的 SDI 调试口，与 PA5/PA6 上的 SWD 目标互不影响。' -ForegroundColor DarkGray
if (-not $SkipBootloader) {
    Write-Host '  - 之后升级应用不必再接线：上电按住 PB3 进 UF2，把 firmware.uf2 拖进 U 盘。' -ForegroundColor DarkGray
}
