# 用 WCH-Link + WCH 官方 OpenOCD 烧录 CH32X035（本板 = DAP 自身）
#
# 走 SDI 传输（adapter driver wlinke），接在板子的 DIO/DCK 两脚上。
# 与 SWD 测速用的那个 CMSIS-DAP 探针**不能同时**接：同一对引脚。
#
# 用法:
#   .\flash.ps1                          # 烧 build\release\firmware.bin
#   .\flash.ps1 -File ..\..\x.bin        # 烧指定文件
#   .\flash.ps1 -NoVerify                # 跳过校验（更快，不建议）
#
# 产物与地址: firmware.bin 烧到 0x00000000（62 KiB Flash，末尾 256 B 留给选项字）。

[CmdletBinding()]
param(
    [string]$File = 'build\release\firmware.bin',

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

$verify = if ($NoVerify) { '' } else { 'verify ' }

# 注意：**不能**用 `program ... reset exit` —— WCH 的 SDI 复位会把核留在 HALT 状态，
# 结果 USB 不枚举（主机侧看不到设备），必须显式 `reset run` + `resume` 才真正跑起来。
# 所以这里分两条 -c：先烧写+校验，再复位并让核跑起来。
$cmd = "program {$bin} 0x00000000 $verify"

Write-Host "OpenOCD : $exe" -ForegroundColor DarkGray
Write-Host "配置    : $cfg（wlinke / SDI）" -ForegroundColor DarkGray
Write-Host "文件    : $bin（$((Get-Item $bin).Length) 字节）" -ForegroundColor DarkGray
Write-Host ""

& $exe -f $cfg -c $cmd -c "reset run" -c "resume" -c "shutdown"
if ($LASTEXITCODE -ne 0) {
    throw "烧录失败（openocd 退出码 $LASTEXITCODE）"
}

Write-Host ""
Write-Host '烧录完成，并已复位运行。' -ForegroundColor Green
Write-Host '  - WCH-Link 走板子独立的 SDI 调试口，与 PA5/PA6 上的 SWD 目标互不影响。' -ForegroundColor DarkGray
