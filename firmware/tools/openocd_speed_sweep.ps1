# SWD 频率扫描：在多个 SWCLK 频率上依次跑 openocd_speed_test.tcl，汇总成一张表。
#
# 依据 tools/openocd_speed_test.tcl（该 tcl 会先备份被测 RAM、测完恢复，不破坏目标程序）。
# 与 speed.ps1 的区别：speed.ps1 测单个频率，本脚本一次扫一串频率并汇总。
#
# 用法:
#   .\tools\openocd_speed_sweep.ps1                          # 默认频率表
#   .\tools\openocd_speed_sweep.ps1 -Khz 100,200,1000,12000
#   .\tools\openocd_speed_sweep.ps1 -Bytes 65536 -Rounds 5
#   .\tools\openocd_speed_sweep.ps1 -Repeats 3                # 每档连跑 3 次，可看波动
#   .\tools\openocd_speed_sweep.ps1 -Csv out.csv
#
# 注意:
#   - 参数用空格或冒号分隔（`-Khz 12000` / `-Khz:12000`），**不能写 `-Khz=12000`**。
#   - 频率很低时（100 kHz 量级）单轮耗时很长，按 -Bytes 控制总时长。

[CmdletBinding()]
param(
    # 要扫描的 SWCLK 频率（kHz）
    [double[]]$Khz = @(100, 200, 300, 400, 500, 600, 800, 1000, 1500, 2000, 3000, 4000, 6000, 8000, 10000, 11500, 12000),

    [int]$Bytes = 16384,

    [int]$Address = 0x20000000,

    [int]$Rounds = 3,

    [int]$Warmup = 1,

    # 每个频率重复几轮完整测量（用于观察波动；1 = 不重复）
    [int]$Repeats = 1,

    [string]$Target = 'stm32f4x',

    [string]$Interface = 'cmsis-dap',

    [string]$OpenOcdRoot = 'D:\program\wch\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD',

    # 固件切到「快路径」的 SWCLK 阈值（kHz），只用于打印标注；
    # 必须与 src/dap/DAP_config.h 的 DELAY_FAST_CYCLES 保持一致
    # （阈值 = (24 MHz) / (2 + DELAY_FAST_CYCLES)）
    [double]$FastMinKhz = 6000,

    # 结果另存为 CSV（可选）
    [string]$Csv,

    [switch]$Raw
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host "错误: $Message" -ForegroundColor Red
    exit 1
}

if ($Bytes -lt 4 -or ($Bytes % 4) -ne 0) { Fail "Bytes 必须是 4 的正倍数（当前 $Bytes）" }
if ($Rounds -lt 1) { Fail "Rounds 必须 >= 1" }
if ($Warmup -lt 0) { Fail "Warmup 不能为负" }
if ($Repeats -lt 1) { Fail "Repeats 必须 >= 1" }

$exe = Join-Path $OpenOcdRoot 'bin\openocd.exe'
$scripts = Join-Path $OpenOcdRoot 'scripts'
if (-not (Test-Path $exe)) { Fail "找不到 $exe（用 -OpenOcdRoot 指定安装根目录）" }
if (-not (Test-Path $scripts)) { Fail "找不到 $scripts" }

$tclPath = (Join-Path $PSScriptRoot 'openocd_speed_test.tcl') -replace '\\', '/'
if (-not (Test-Path $tclPath)) { Fail "找不到 $tclPath" }

$addr = '0x{0:X}' -f $Address

Write-Host ''
Write-Host "OpenOCD  : $exe" -ForegroundColor DarkGray
Write-Host "测试块   : $Bytes 字节 ($([math]::Round($Bytes / 1024, 1)) KiB) @ $addr" -ForegroundColor DarkGray
Write-Host "轮数     : $Rounds（预热 $Warmup 轮，不计入），每频率重复 $Repeats 次" -ForegroundColor DarkGray
Write-Host "频率     : $($Khz -join ', ') kHz" -ForegroundColor DarkGray
Write-Host ''

function Get-Median([double[]]$values) {
    $sorted = @($values | Sort-Object)
    $n = $sorted.Count
    if ($n -eq 0) { return $null }
    if ($n % 2 -eq 1) { return $sorted[[int](($n - 1) / 2)] }
    return ($sorted[$n / 2 - 1] + $sorted[$n / 2]) / 2
}

function Invoke-SpeedTest {
    param([double]$SpeedKhz)

    $tclVars = "set speed_khz $([math]::Round($SpeedKhz)); set test_addr $addr; " +
               "set test_bytes $Bytes; set rounds $Rounds; set warmup_rounds $Warmup"

    $ocdArgs = @(
        '-s', $scripts,
        '-f', "interface/$Interface.cfg",
        '-f', "target/$Target.cfg",
        '-c', $tclVars,
        '-c', "source {$tclPath}"
    )

    $output = & $exe @ocdArgs 2>&1 | ForEach-Object { [string]$_ }

    if ($Raw) { $output | ForEach-Object { Write-Host "    $_" } }

    $write = $null
    $read = $null
    $passed = $null
    foreach ($line in $output) {
        if ($line -match 'Write Avg Speed\s*:\s*([\d.]+)\s*KiB/s') { $write = [double]$Matches[1] }
        elseif ($line -match 'Read Avg Speed\s*:\s*([\d.]+)\s*KiB/s') { $read = [double]$Matches[1] }
        elseif ($line -match 'Passed Rounds\s*:\s*(\d+)\s*/\s*(\d+)') { $passed = "$($Matches[1])/$($Matches[2])" }
    }

    return [pscustomobject]@{
        Khz    = $SpeedKhz
        Write  = $write
        Read   = $read
        Passed = $passed
        Error  = ($null -eq $write -or $null -eq $read)
    }
}

$rows = New-Object System.Collections.Generic.List[object]

foreach ($f in $Khz) {
    $fast = if ($f -ge $FastMinKhz) { '快路径' } else { '慢路径' }
    Write-Host ("  {0,8} kHz [{1}]" -f [math]::Round($f), $fast) -ForegroundColor DarkGray

    $runResults = @()
    for ($r = 0; $r -lt $Repeats; $r++) {
        $res = Invoke-SpeedTest -SpeedKhz $f
        $runResults += $res
        $wText = if ($null -ne $res.Write) { "{0,7:N1}" -f $res.Write } else { '      —' }
        $rText = if ($null -ne $res.Read) { "{0,7:N1}" -f $res.Read } else { '      —' }
        $tag = if ($Repeats -gt 1) { "  #$($r + 1)" } else { '' }
        Write-Host ("      写 {0}  读 {1} KiB/s  通过 {2}{3}" -f $wText, $rText, $res.Passed, $tag)
    }

    $okRuns = $runResults | Where-Object { -not $_.Error }
    if ($okRuns.Count -eq 0) {
        $rows.Add([pscustomobject]@{
            Khz = $f; Write = $null; Read = $null; Passed = '—'; Runs = 0; Error = $true
        })
        continue
    }

    $writes = @($okRuns | ForEach-Object { $_.Write })
    $reads = @($okRuns | ForEach-Object { $_.Read })

    $rows.Add([pscustomobject]@{
        Khz    = $f
        Write  = Get-Median $writes
        Read   = Get-Median $reads
        WMin   = ($writes | Measure-Object -Minimum).Minimum
        WMax   = ($writes | Measure-Object -Maximum).Maximum
        RMin   = ($reads | Measure-Object -Minimum).Minimum
        RMax   = ($reads | Measure-Object -Maximum).Maximum
        Passed = ($okRuns | Select-Object -Last 1).Passed
        Runs   = $okRuns.Count
        Error  = $false
    })
}

Write-Host ''
Write-Host ('=' * 72)
if ($Repeats -gt 1) {
    Write-Host ("  {0,-10} {1,24} {2,24}" -f 'SWCLK', '写 KiB/s (中位/min/max)', '读 KiB/s (中位/min/max)')
    Write-Host ('-' * 72)
    foreach ($row in $rows) {
        if ($row.Error) {
            Write-Host ("  {0,-10} {1,24} {2,24}" -f "$([math]::Round($row.Khz))kHz", '失败', '失败')
        } else {
            $w = "{0:N1}/{1:N1}/{2:N1}" -f $row.Write, $row.WMin, $row.WMax
            $r = "{0:N1}/{1:N1}/{2:N1}" -f $row.Read, $row.RMin, $row.RMax
            Write-Host ("  {0,-10} {1,24} {2,24}" -f "$([math]::Round($row.Khz))kHz", $w, $r)
        }
    }
} else {
    Write-Host ("  {0,-10} {1,14} {2,14} {3,10}" -f 'SWCLK', '写 KiB/s', '读 KiB/s', '通过')
    Write-Host ('-' * 72)
    foreach ($row in $rows) {
        $w = if ($row.Error) { '—' } else { "{0:N1}" -f $row.Write }
        $r = if ($row.Error) { '—' } else { "{0:N1}" -f $row.Read }
        Write-Host ("  {0,-10} {1,14} {2,14} {3,10}" -f "$([math]::Round($row.Khz))kHz", $w, $r, $row.Passed)
    }
}
Write-Host ('=' * 72)
Write-Host ''

if ($Csv) {
    $rows | Export-Csv -Path $Csv -NoTypeInformation -Encoding UTF8
    Write-Host "已写入 $Csv" -ForegroundColor DarkGray
    Write-Host ''
}
