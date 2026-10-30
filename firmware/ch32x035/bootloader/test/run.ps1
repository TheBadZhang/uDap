# UF2 接收路径 + 虚拟 U 盘的主机端测试（不需要硬件）
#
# 编译固件里真正在用的那份逻辑：
#   src/boot_uf2.c   UF2 块校验 / 去重 / 落盘顺序 / 完成判定
#   src/boot_disk.c  虚拟 FAT16 的读路径（主机枚举、列目录、读 index.htm）
# 硬件相关的部分（Flash 擦写、SysTick）由 test/uf2_host_test.c 里的模拟实现替代。
#
# 用法（先跑一次 ..\build.ps1 生成 firmware.uf2）：
#   .\run.ps1
#   .\run.ps1 -Uf2 ..\build\release\firmware.uf2 -Bin ..\build\release\firmware.bin

[CmdletBinding()]
param(
    [string]$Uf2 = '',
    [string]$Bin = '',
    [string]$CC = 'gcc'
)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot                                  # ...\bootloader\test
$bootDir = Split-Path $here -Parent                    # ...\bootloader
$appDir = Split-Path $bootDir -Parent                  # ...\ch32x035
$repoDir = Split-Path (Split-Path $appDir -Parent) -Parent

if (-not $Uf2) { $Uf2 = Join-Path $appDir 'build\release\firmware.uf2' }
if (-not $Bin) { $Bin = Join-Path $appDir 'build\release\firmware.bin' }

$uf2Dir = Join-Path $repoDir 'firmware\third_party\uf2'
$outDir = Join-Path $here 'build'
$exe = Join-Path $outDir 'uf2_host_test.exe'

foreach ($f in @($Uf2, $Bin)) {
    if (-not (Test-Path $f)) {
        throw "找不到 $f —— 先运行 ..\build.ps1"
    }
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$stubDir = Join-Path $here 'stub'
$srcDir = Join-Path $bootDir 'src'
$commonInc = @('-I', $stubDir, '-I', $srcDir, '-I', $uf2Dir)

# 把 SysTick 计数换成主机变量（boot_time.h 里的 #ifndef 允许覆盖）
$tickOverride = '-DBOOT_SYSTICK_CNTL=(g_host_ticks)'
$preinclude = @('-include', (Join-Path $stubDir 'host_ticks.h'))
# 主机上没有中断：把 boot_uf2.c 里的临界区变成空操作
$hostTest = '-DBOOT_HOST_TEST'

Write-Host '==> 编译 boot_uf2.c + boot_disk.c + 测试' -ForegroundColor Cyan
& $CC -std=gnu11 -O1 -Wall -Wextra @commonInc @preinclude $tickOverride $hostTest `
    (Join-Path $here 'uf2_host_test.c') `
    (Join-Path $srcDir 'boot_uf2.c') `
    (Join-Path $srcDir 'boot_disk.c') `
    -o $exe
if ($LASTEXITCODE -ne 0) { throw '编译测试程序失败' }

Write-Host ''
& $exe $Uf2 $Bin
if ($LASTEXITCODE -ne 0) { throw 'UF2 主机测试失败' }
