# 用 OpenOCD 测 SWD 内存读写速度（单次或多次取平均）
#
# 基于 tools/openocd_speed_test.tcl。该 tcl 会先把被测 RAM 备份、测完恢复，
# 所以不会破坏目标正在运行的程序。
#
# 用法:
#   .\tools\speed.ps1                          # 默认 6000 kHz / 64 KiB / 单次
#   .\tools\speed.ps1 -Khz 12000               # 换频率（与 6000 同速，见下）
#   .\tools\speed.ps1 -Khz 8000 -Bytes 32768   # 换测试大小
#   .\tools\speed.ps1 -Khz 4000 -Rounds 10     # 跑 10 次取平均（更稳）
#   .\tools\speed.ps1 -Target stm32f4x -Khz 1000
#   .\tools\speed.ps1 -Raw                     # 打印 OpenOCD 全部输出
#
# 注意:
#   - 默认 6000 kHz 就是推荐档位：快路径的实际 SWCLK 由固定指令数决定（约 6 MHz），
#     填 6 MHz 和填 12 MHz 拿到同一个速度，阈值处时序余量还最厚。
#   - 参数必须用空格或冒号分隔：`-Khz 6000` 或 `-Khz:6000`。
#     **不能写 `-Khz=6000`** —— 等号不是 PowerShell 的参数语法，那种写法在
#     没有 [CmdletBinding()] 的脚本里会被静默收进 $args 并被忽略，
#     结果是照常按默认值运行、不报任何错。
#   - test_bytes 必须是 4 的倍数；被测地址必须在可写 RAM 内。
#   - SM32F411 的 SRAM 只有 128 KiB (0x20000000..0x20020000)，别超。
#   - Khz >= FastMinKhz（默认 6000）时固件会切到「快路径」，速度会有明显跳变。
#     FastMinKhz 要和固件 src/dap/DAP_config.h 的 DELAY_FAST_CYCLES 保持一致：
#     阈值 = (24 MHz) / (2 + DELAY_FAST_CYCLES)。

# [CmdletBinding()] 是必需的：没有它时，无法识别的参数（例如误写成
# `-Khz=6000`）会被静默收进 $args 并被忽略，脚本照常按默认值运行且不报错。
# 加上它之后，这类写法会直接报参数错误，避免「以为换了频率其实没换」。
[CmdletBinding()]
param(
    # 默认 6000 kHz：快路径拐点，也是推荐档位
    [double]$Khz = 6000,

    [int]$Bytes = 65536,

    [string]$Address = '0x20000000',

    [int]$Rounds = 1,

    [int]$Warmup = 2,

    [string]$Target = 'stm32f4x',

    [string]$Interface = 'cmsis-dap',

    # OpenOCD 安装根目录（bin/ 与 scripts/ 的父目录）
    [string]$OpenOcdRoot = 'D:\program\wch\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD',

    # 固件切到「快路径」的 SWCLK 阈值（kHz），只用于打印标注；
    # 必须与 src/dap/DAP_config.h 的 DELAY_FAST_CYCLES 保持一致
    [double]$FastMinKhz = 6000,

    [switch]$Raw
)

$ErrorActionPreference = 'Stop'

# 干净的失败退出：只打印一行错误，不输出 PowerShell 调用栈
function Fail([string]$Message) {
    Write-Host "错误: $Message" -ForegroundColor Red
    exit 1
}

# ---------- 定位 OpenOCD ----------

function Resolve-OpenOcd {
    param([string]$Root)

    if ($Root) {
        $exe = Join-Path $Root 'bin\openocd.exe'
        $scripts = Join-Path $Root 'scripts'
        if (Test-Path $exe) {
            return @{ Exe = $exe; Scripts = $scripts }
        }
    }

    # 退而求其次：从 PATH 里找，scripts 用同级的 scripts 目录
    $cmd = Get-Command openocd -ErrorAction SilentlyContinue
    if ($cmd) {
        $binDir = Split-Path $cmd.Source -Parent
        $scripts = Join-Path (Split-Path $binDir -Parent) 'scripts'
        return @{ Exe = $cmd.Source; Scripts = $scripts }
    }

    Fail "找不到 openocd.exe。请用 -OpenOcdRoot 指定安装根目录（含 bin\ 和 scripts\）。"
}

$ocd = Resolve-OpenOcd $OpenOcdRoot
if (-not (Test-Path $ocd.Scripts)) {
    Fail "找不到 OpenOCD scripts 目录: $($ocd.Scripts)"
}

# ---------- 校验参数 ----------

if ($Bytes -lt 4 -or ($Bytes % 4) -ne 0) {
    Fail "Bytes 必须是 4 的正倍数（当前 $Bytes）"
}
if ($Rounds -lt 1) {
    Fail "Rounds 必须 >= 1（当前 $Rounds）"
}
if ($Warmup -lt 0) {
    Fail "Warmup 不能为负（当前 $Warmup）"
}
if ($Khz -le 0) {
    Fail "Khz 必须 > 0（当前 $Khz）"
}

# tcl 里的 test_addr 用 TCL 数字解析，0x 前缀形式可以直接用
$addr = $Address
if ($addr -notmatch '^0[xX][0-9a-fA-F]+$') {
    Fail "Address 需为 0x 开头的十六进制（当前 $Address）"
}

# ---------- 组装命令 ----------

# tcl 路径用绝对路径 + 正斜杠：OpenOCD 的 TCL 对 Windows 反斜杠转义处理不统一，
# 用正斜杠最稳，也避免受调用者当前目录影响。
$tclPath = (Join-Path $PSScriptRoot 'openocd_speed_test.tcl') -replace '\\', '/'
if (-not (Test-Path $tclPath)) {
    Fail "找不到 $tclPath"
}

$tclVars = "set speed_khz $([math]::Round($Khz)); set test_addr $addr; " +
           "set test_bytes $Bytes; set rounds $Rounds; set warmup_rounds $Warmup"

$ocdArgs = @(
    '-s', $ocd.Scripts,
    '-f', "interface/$Interface.cfg",
    '-f', "target/$Target.cfg",
    '-c', $tclVars,
    '-c', "source {$tclPath}"
)

# 拼一份可读的命令行，便于复制重跑
function Quote-Arg([string]$text) {
    if ($text -match '[\s"]') { return '"' + ($text -replace '"', '\"') + '"' }
    return $text
}
$pretty = ($ocdArgs | ForEach-Object { Quote-Arg $_ }) -join ' '

Write-Host ''
Write-Host "OpenOCD  : $($ocd.Exe)" -ForegroundColor DarkGray
Write-Host "SWCLK    : $([math]::Round($Khz)) kHz$(if ($Khz -ge $FastMinKhz) { '  (快路径)' } else { '  (慢路径)' })" -ForegroundColor DarkGray
Write-Host "测试块   : $Bytes 字节 ($([math]::Round($Bytes / 1024, 1)) KiB) @ $addr" -ForegroundColor DarkGray
Write-Host "轮数     : $Rounds（预热 $Warmup 轮，不计入）" -ForegroundColor DarkGray
Write-Host ''

# ---------- 运行 ----------

$output = & $ocd.Exe @ocdArgs 2>&1 | ForEach-Object { [string]$_ }

if ($Raw) {
    $output | ForEach-Object { Write-Host $_ }
    Write-Host ''
    Write-Host '等价的 OpenOCD 命令行：' -ForegroundColor DarkGray
    Write-Host "  $pretty" -ForegroundColor DarkGray
    Write-Host ''
}

# ---------- 解析结果 ----------

function Get-Field {
    param([string[]]$Lines, [string]$Pattern)
    $match = $Lines | Select-String -Pattern $Pattern | Select-Object -First 1
    if ($match) { return $match.Matches[0].Groups[1].Value }
    return $null
}

$passed  = Get-Field $output 'Passed Rounds\s*:\s*(\d+\s*/\s*\d+)'
$write   = Get-Field $output 'Write Avg Speed\s*:\s*([\d.]+) KiB/s'
$read    = Get-Field $output 'Read Avg Speed\s*:\s*([\d.]+) KiB/s'
$errors  = $output | Select-String -Pattern 'Error|error:|No ACK|examination failed' |
           Where-Object { $_.Line -notmatch 'DEPRECATED' } |
           Select-Object -First 3

if (-not $passed) {
    Write-Host '测试未产出结果。OpenOCD 输出如下：' -ForegroundColor Red
    $output | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
    exit 1
}

Write-Host ('=' * 58)
Write-Host ("  {0,-10} {1,16} {2,16}" -f 'SWCLK', '写 KiB/s', '读 KiB/s')
Write-Host ('-' * 58)
Write-Host ("  {0,-10} {1,16} {2,16}" -f "$([math]::Round($Khz))kHz",
            $(if ($write) { $write } else { '—' }),
            $(if ($read)  { $read }  else { '—' }))
Write-Host ('-' * 58)
Write-Host "  通过轮数: $passed"

if ($errors) {
    Write-Host ''
    Write-Host 'OpenOCD 报告了错误:' -ForegroundColor Yellow
    $errors | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Yellow }
    exit 1
}

Write-Host ('=' * 58)
Write-Host ''

# 单次测量没做平均，提醒一下
if ($Rounds -eq 1) {
    Write-Host '提示：单次测量有约 ±5% 波动，需要更稳用 -Rounds 10' -ForegroundColor DarkGray
    Write-Host ''
}
