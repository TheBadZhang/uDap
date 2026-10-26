# SWD 链路健康度检查（比测速更能反映物理链路质量）
#
# 背景：这台板子的 PA5/PA6 → 目标 SWD 是跳线连接，实测在 6 MHz 下时好时坏：
# 好的时候 DPIDR 10/10 正确、64 KiB 读 0.13~0.16 s（≈400 KiB/s）；差的时候连
# examine 都过不去（`wrong parity` / `Failed to read memory`）。同一条线、同一份
# 固件、同一个 OpenOCD，只随时间变化 —— 所以测速结果必须先确认链路健康。
#
# 判据（本脚本每次尝试都做一次完整 examine）：
#   全通过率   >= 90%  → 链路良好，可以相信测速数字
#   50% ~ 90%         → 边缘，测速会因重试而偏低
#   < 50%             → 链路不可用，先查接线（短线 + 共地 + 目标供电）
#
# 用法:
#   .\swd_link_check.ps1                          # 6 MHz，20 次
#   .\swd_link_check.ps1 -Khz 1000 -Attempts 10   # 换频率
#   .\swd_link_check.ps1 -OpenOcdRoot 'C:\Users\lab\scoop\apps\openocd\current'

[CmdletBinding()]
param(
    [double]$Khz = 6000,

    [int]$Attempts = 20,

    [string]$Target = 'stm32f4x',

    # 默认用定制构建（含异步 libusb 补丁）；可指向其他 OpenOCD 安装根目录
    [string]$OpenOcdRoot = 'D:\repo\closeocd\dist'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host "错误: $Message" -ForegroundColor Red
    exit 1
}

# ---------- 定位 OpenOCD ----------

function Find-OpenOcdScripts {
    param([string]$Root)
    foreach ($rel in 'scripts', 'share\openocd\scripts', 'share\scripts') {
        $cand = Join-Path $Root $rel
        if ((Test-Path (Join-Path $cand 'interface')) -and (Test-Path (Join-Path $cand 'target'))) {
            return $cand
        }
    }
    return $null
}

$exe = Join-Path $OpenOcdRoot 'bin\openocd.exe'
if (-not (Test-Path $exe)) { Fail "找不到 $exe（用 -OpenOcdRoot 指定安装根目录）" }
$scripts = Find-OpenOcdScripts $OpenOcdRoot
if (-not $scripts) { Fail "在 $OpenOcdRoot 下找不到 scripts 目录" }

Write-Host ''
Write-Host "OpenOCD : $exe" -ForegroundColor DarkGray
Write-Host "SWCLK   : $([math]::Round($Khz)) kHz" -ForegroundColor DarkGray
Write-Host "尝试次数: $Attempts" -ForegroundColor DarkGray
Write-Host ''

# ---------- 逐次尝试 ----------

$ok = 0
$parity = 0
$junk = 0
$fault = 0
$apfail = 0
$dpidr_bad = 0

for ($i = 1; $i -le $Attempts; $i++) {
    $out = & $exe -s $scripts -f interface/cmsis-dap.cfg -f "target/$Target.cfg" `
        -c 'transport select swd' -c "adapter speed $([math]::Round($Khz))" `
        -c 'init' -c 'shutdown' -d3 2>&1 | Out-String

    $parity += ([regex]::Matches($out, 'wrong parity')).Count
    $junk += ([regex]::Matches($out, 'JUNK')).Count
    $fault += ([regex]::Matches($out, 'FAULT')).Count
    $apfail += ([regex]::Matches($out, 'Failed to read memory')).Count

    $dpidr = if ($out -match 'DPIDR (0x[0-9a-f]+)') { $matches[1] } else { $null }
    if ($dpidr -ne '0x2ba01477') { $dpidr_bad++ }

    if ($out -match 'Examination succeed') { $ok++ }
}

# ---------- 汇总 ----------

$rate = 100.0 * $ok / $Attempts
Write-Host ('=' * 62)
Write-Host ("  examine 全通过   : {0}/{1}  ({2:N0}%)" -f $ok, $Attempts, $rate)
Write-Host ("  DPIDR 非 0x2ba01477: {0}" -f $dpidr_bad)
Write-Host ("  wrong parity      : {0}" -f $parity)
Write-Host ("  ACK JUNK          : {0}" -f $junk)
Write-Host ("  ACK FAULT         : {0}" -f $fault)
Write-Host ("  Failed to read    : {0}" -f $apfail)
Write-Host ('-' * 62)

if ($rate -ge 90) {
    Write-Host '  结论：链路良好，测速数字可信' -ForegroundColor Green
} elseif ($rate -ge 50) {
    Write-Host '  结论：链路边缘，测速会因重试偏低（建议先收紧接线）' -ForegroundColor Yellow
} else {
    Write-Host '  结论：链路不可用 —— 查接线（短线、共地、目标供电、SWDIO 上拉）' -ForegroundColor Red
}
Write-Host ('=' * 62)
Write-Host ''

if ($rate -lt 50) { exit 1 }
