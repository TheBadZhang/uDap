# 测 STM32F4 的 flash 烧录速度（erase / program / verify 分别计时）
#
# 用随机生成（或指定）的 bin 文件，通过 openocd 直写目标 flash，统计三段耗时。
#
# 用法:
#   .\flash_speed.ps1                                   # 128 KiB 随机数据，fork OpenOCD
#   .\flash_speed.ps1 -Bytes 262144                      # 256 KiB（自动按扇区对齐）
#   .\flash_speed.ps1 -File D:\some\image.bin            # 用现成文件
#   .\flash_speed.ps1 -OpenOcdRoot C:\Users\lab\scoop\apps\openocd\current   # 换 openocd
#   .\flash_speed.ps1 -Rounds 2                          # 跑多轮（每轮都会重新擦写）
#
# 默认目标：stm32f411（扇区 0..3 = 16 KiB，扇区 4 = 64 KiB，扇区 5.. = 128 KiB）

[CmdletBinding()]
param(
    [int]$Bytes = 131072,
    [string]$Address = '0x08000000',
    [string]$File = '',
    [string]$Target = 'stm32f4x',
    [string]$Interface = 'cmsis-dap',
    [int]$Khz = 6000,
    [int]$Rounds = 1,
    [string]$OpenOcdRoot = 'D:\repo\closeocd\dist'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host $Message -ForegroundColor Red
    exit 1
}

# ---- 找 openocd ----
function Resolve-OpenOcd([string]$Root) {
    foreach ($candidate in @(
            (Join-Path $Root 'bin\openocd.exe'),
            (Join-Path $Root 'openocd.exe'))) {
        if (Test-Path $candidate) { return $candidate }
    }
    $cmd = Get-Command openocd -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$exe = Resolve-OpenOcd $OpenOcdRoot
if (-not $exe) { Fail "找不到 openocd（-OpenOcdRoot $OpenOcdRoot）" }
$scripts = Join-Path (Split-Path (Split-Path $exe)) 'share\openocd\scripts'
if (-not (Test-Path $scripts)) { $scripts = Join-Path (Split-Path $exe) '..\share\openocd\scripts' }

# ---- 生成 / 校验 bin ----
if (-not $File) {
    $File = Join-Path $env:TEMP 'flash_speed_random.bin'
    $buf = [byte[]]::new($Bytes)
    (New-Object System.Random 20261026).NextBytes($buf)
    [System.IO.File]::WriteAllBytes($File, $buf)
    $generated = $true
} else {
    $generated = $false
}
$size = (Get-Item $File).Length

# ---- 扇区数：128 KiB 以内用 0..4；更大就按 128 KiB 扇区算 ----
# STM32F4（F411/F401）：0..3 各 16 KiB，4 是 64 KiB，5 以上各 128 KiB。
$lastSec = 0
$covered = 0
$sizes = @(16KB, 16KB, 16KB, 16KB, 64KB)
while ($covered -lt $size) {
    if ($lastSec -lt $sizes.Count) { $covered += $sizes[$lastSec] }
    else { $covered += 128KB }
    $lastSec++
}
$lastSec = [Math]::Max(0, $lastSec - 1)

$tcl = Join-Path $PSScriptRoot 'flash_speed_test.tcl'
if (-not (Test-Path $tcl)) { Fail "找不到 $tcl" }

Write-Host "OpenOCD  : $exe" -ForegroundColor DarkGray
Write-Host "文件     : $File（$([math]::Round($size/1KB,1)) KiB，$(if ($generated) { '随机生成' } else { '指定' })）" -ForegroundColor DarkGray
Write-Host "目标     : $Target @ $Address，擦除扇区 0..$lastSec，SWCLK $Khz kHz" -ForegroundColor DarkGray
Write-Host "轮数     : $Rounds`n"

$rows = @()
for ($r = 1; $r -le $Rounds; $r++) {
    $quoted = $File -replace '\\', '/'
    $args = @(
        '-s', $scripts,
        '-f', "interface/$Interface.cfg",
        '-f', "target/$Target.cfg",
        '-c', "adapter speed $Khz",
        '-c', "set BINFILE {$quoted}",
        '-c', "set BINADDR $Address",
        '-c', "set LASTSEC $lastSec",
        '-f', $tcl
    )
    $out = & $exe @args 2>&1 | ForEach-Object { [string]$_ }
    $line = $out | Select-String -Pattern 'RESULT erase_ms=(\d+) program_ms=(\d+) verify_ms=(\d+) total_ms=(\d+)'
    if (-not $line) {
        Write-Host ($out -join "`n") -ForegroundColor DarkYellow
        Fail "第 $r 轮没有拿到结果（openocd 失败？）"
    }
    $m = [regex]::Match($line.Line, 'erase_ms=(\d+) program_ms=(\d+) verify_ms=(\d+) total_ms=(\d+)')
    $rows += [pscustomobject]@{
        Round   = $r
        Erase   = [int]$m.Groups[1].Value
        Program = [int]$m.Groups[2].Value
        Verify  = [int]$m.Groups[3].Value
        Total   = [int]$m.Groups[4].Value
    }
}

$kib = $size / 1KB
Write-Host ('=' * 72)
Write-Host ("  {0,-6} {1,-14} {2,-16} {3,-16} {4,-14}" -f '轮', 'erase', 'program', 'verify', '合计')
Write-Host ('-' * 72)
foreach ($row in $rows) {
    Write-Host ("  {0,-6} {1,-4} ms {2,7:N1} K/s  {3,-4} ms {4,7:N1} K/s  {5,-4} ms {6,7:N1} K/s  {7} ms" -f `
            $row.Round,
        $row.Erase, $(if ($row.Erase -gt 0) { $kib / ($row.Erase / 1000.0) } else { 0 }),
        $row.Program, $(if ($row.Program -gt 0) { $kib / ($row.Program / 1000.0) } else { 0 }),
        $row.Verify, $(if ($row.Verify -gt 0) { $kib / ($row.Verify / 1000.0) } else { 0 }),
        $row.Total)
}
Write-Host ('=' * 72)

if ($generated) { Remove-Item $File -ErrorAction SilentlyContinue }
