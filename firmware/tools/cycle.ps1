# 一条命令跑完「编译 -> 烧录 -> 测速」，省掉手工等待。
#
# 链路：RVLink(WCH-LinkE) --SDI--> uDap2(CH32X035) --SWD--> 目标(默认 STM32F411)
#   烧录：WCH 版 OpenOCD + wlinke 适配器，用 RVLink 给 uDap 刷固件
#   测速：tools\speed.ps1 用 uDap 的 CMSIS-DAP 给目标做 SWD 内存读写
#
# 用法:
#   .\tools\cycle.ps1                          # release + ESIG 序列号，6MHz/64KiB/5轮
#   .\tools\cycle.ps1 -Rounds 1 -Bytes 16384   # 快速一轮
#   .\tools\cycle.ps1 -SkipBuild               # 跳过编译，只烧录 + 测速
#   .\tools\cycle.ps1 -FlashOnly               # 编到烧录为止，不测速
#   .\tools\cycle.ps1 -EsigSn n                # 换构建选项
#
# 参数用空格或冒号分隔（-Khz 6000 / -Khz:6000），不能写 -Khz=6000。
#
# 注意：uDap 就是被烧录的目标 —— 烧录时它会被 halt、USB 随之消失，
# reset+resume 之后才重新枚举，所以脚本会自动等它回来。
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')] [string]$Mode = 'release',
    [ValidateSet('y', 'n')][string]$EsigSn = 'y',

    # 带 wlinke 适配器的 WCH 版 OpenOCD（MounRiver 自带）
    [string]$OpenOcdRoot = 'C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD',

    # 透传给 tools\speed.ps1
    [double]$Khz = 6000,
    [int]$Bytes = 65536,
    [int]$Rounds = 5,
    [int]$Warmup = 1,
    [string]$Target = 'stm32f4x',
    [switch]$RawSpeed,

    [switch]$SkipBuild,
    [switch]$SkipFlash,
    [switch]$FlashOnly,

    # 烧录后等待 uDap 重新枚举的最长秒数
    [int]$EnumTimeoutSec = 30
)

$ErrorActionPreference = 'Stop'

$firmwareRoot = Split-Path $PSScriptRoot -Parent
$buildScript = Join-Path $firmwareRoot 'build.ps1'
$speedScript = Join-Path $firmwareRoot 'tools\speed.ps1'
$binPath = Join-Path $firmwareRoot "build\$Mode\firmware.bin"

function Fail([string]$Message) {
    Write-Host ''
    Write-Host "错误: $Message" -ForegroundColor Red
    exit 1
}
function Step([string]$Message) {
    Write-Host ''
    Write-Host "==== $Message ====" -ForegroundColor Cyan
}

$watch = [System.Diagnostics.Stopwatch]::StartNew()

# ---------- 1. 编译 ----------
if (-not $SkipBuild) {
    Step "编译（$Mode, -EsigSn $EsigSn）"
    try {
        & $buildScript -Mode $Mode -EsigSn $EsigSn
    } catch {
        Fail "编译失败: $($_.Exception.Message)"
    }
    if ($LASTEXITCODE -ne 0) { Fail '编译失败' }
}

if (-not (Test-Path $binPath)) {
    Fail "找不到 $binPath（去掉 -SkipBuild 重新编译）"
}
$bin = Get-Item $binPath
Write-Host ("固件: {0}  {1} B  {2}  sha256 {3}" -f `
        $bin.Name, $bin.Length, $bin.LastWriteTime, `
        (Get-FileHash $binPath -Algorithm SHA256).Hash.Substring(0, 16)) -ForegroundColor DarkGray

# ---------- 2. 烧录 ----------
if (-not $SkipFlash) {
    $exe = Join-Path $OpenOcdRoot 'bin\openocd.exe'
    $cfg = Join-Path $OpenOcdRoot 'bin\wch-riscv.cfg'
    if (-not (Test-Path $exe)) { Fail "找不到 $exe（用 -OpenOcdRoot 指定 WCH 版 OpenOCD）" }
    if (-not (Test-Path $cfg)) { Fail "找不到 $cfg" }

    Step '烧录（RVLink / SDI）'

    # 工程路径含中文时 OpenOCD 的 TCL 解析不稳，先拷到 ASCII 临时目录
    $img = Join-Path $env:TEMP 'udap_cycle_fw.bin'
    Copy-Item $binPath $img -Force

    $ocdArgs = @('-f', $cfg)
    foreach ($cmd in 'init', 'halt', "program {$img} 0 verify", 'reset', 'resume', 'exit') {
        $ocdArgs += @('-c', $cmd)
    }
    $log = & $exe @ocdArgs 2>&1 | ForEach-Object { [string]$_ }
    Remove-Item $img -Force -ErrorAction SilentlyContinue

    $log | Where-Object { $_ -match 'Programming|Verified|Error|error' } |
        ForEach-Object { Write-Host "  $_" }
    if (-not ($log -match 'Verified OK')) { Fail '烧录未通过校验（看上面的 OpenOCD 输出）' }

    Step '等待 uDap 重新枚举'
    $deadline = (Get-Date).AddSeconds($EnumTimeoutSec)
    $dev = $null
    do {
        Start-Sleep -Milliseconds 500
        $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
               Where-Object InstanceId -like '*VID_0D28&PID_0204*' |
               Select-Object -First 1
    } while (-not $dev -and (Get-Date) -lt $deadline)

    if (-not $dev) {
        Fail "等了 $EnumTimeoutSec s 仍未见 uDAP CMSIS-DAP —— 板子是否被切到 WCH-Link 模式了？"
    }
    Start-Sleep -Seconds 1        # 让驱动/COM 口稳定下来
    Write-Host "  $($dev.InstanceId)" -ForegroundColor DarkGray
}

if ($FlashOnly) {
    Write-Host ''
    Write-Host ("完成（仅编译 + 烧录）  用时 {0:N1} s" -f $watch.Elapsed.TotalSeconds) -ForegroundColor Green
    exit 0
}

# ---------- 3. 测速 ----------
Step '测速（tools\speed.ps1）'

$spdParams = @{
    OpenOcdRoot = $OpenOcdRoot
    Khz         = $Khz
    Bytes       = $Bytes
    Rounds      = $Rounds
    Warmup      = $Warmup
    Target      = $Target
}
if ($RawSpeed) { $spdParams['Raw'] = $true }

# Write-Host 走的是信息流，(2>&1) 抓不到；用 (*>&1) 才能连它一起收下来做汇总，
# 同时逐行回放，保证和单独跑 speed.ps1 看到的一样。
$lines = [System.Collections.Generic.List[string]]::new()
& $speedScript @spdParams *>&1 | ForEach-Object {
    $text = [string]$_
    $lines.Add($text)
    Write-Host $text
}
$speedExit = $LASTEXITCODE

Write-Host ''
if ($speedExit -ne 0) {
    Write-Host ("失败：测速未通过  用时 {0:N1} s" -f $watch.Elapsed.TotalSeconds) -ForegroundColor Red
    exit 1
}

$row = $lines | Select-String -Pattern '^\s+(\d+)kHz\s+([\d.]+)\s+([\d.]+)' | Select-Object -First 1
$result = if ($row) {
    "写 $($row.Matches[0].Groups[2].Value) / 读 $($row.Matches[0].Groups[3].Value) KiB/s @ $($row.Matches[0].Groups[1].Value) kHz"
} else {
    '（未解析到速度行，见上方输出）'
}
Write-Host ("完成  {0}  用时 {1:N1} s" -f $result, $watch.Elapsed.TotalSeconds) -ForegroundColor Green
