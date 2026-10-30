# 构建 ch32x035f8u-dap (uDap) 固件
#
# 调试器模式：
#   CMSIS-DAP            固定编入
#   WCH-Link（RVSWD）    默认**不编入**（省 Flash 与约 6 KiB RAM）；
#                        传 -Wchlink y 编成双模式固件，运行中双击 PB3 切换。
#
# UF2 bootloader：
#   默认一起构建（同一份 xmake.lua 里的 bootloader target），应用链接到 0x4000，
#   产物 firmware.uf2 可以直接拖进 bootloader 的 U 盘里升级；
#   firmware_full.bin 是 bootloader + 应用的合并镜像（整片烧录用）。
#   双模式固件约 57 KiB，62 KiB Flash 里放不下 16 KiB 的 bootloader，
#   所以 -Wchlink y 时自动退回「应用从 0x0 起、只走 SWD 烧录」的老布局。
#
# 用法:
#   .\build.ps1               # release 构建（默认用芯片 ESIG UID 作序列号）
#   .\build.ps1 -Wchlink y    # 把 WCH-Link（RVSWD）也编进去（双模式，无 UF2）
#   .\build.ps1 -EsigSn n     # 退回固定串 DEADBEEF（所有板子相同）
#   .\build.ps1 -SwdPins pa5_pa6   # 换 SWD 引脚方案（A/B 对照用）
#   .\build.ps1 -Bootloader n # 不要 bootloader（应用从 0x0 起，只走 SWD）
#   .\build.ps1 -Clean        # 先清理再构建
#   .\build.ps1 -Mode debug   # debug 构建
#   .\build.ps1 -Reconfigure  # 强制重新生成配置（工具链路径变化后用）
#
# 参数用空格或冒号分隔（-Mode debug 或 -Mode:debug），**不能写 -Mode=debug**。
# 也可以直接双击/命令行跑 build.bat（参数原样转给本脚本）。
#
# 产物（都在 build\<mode>\ 下）:
#   firmware.elf / .bin / .map
#   firmware.uf2          （带 bootloader 的布局）
#   firmware_full.bin     （bootloader + 应用，整片烧录用）
#   bootloader.elf / .bin / .map   （带 bootloader 的布局）

# [CmdletBinding()] 是必需的：没有它时，无法识别的参数（例如误写成
# `-Mode=debug`）会被静默收进 $args 并被忽略，脚本照常按默认值构建且不报错
# —— 那是很危险的失败模式（以为编了 debug，其实没有）。
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Mode = 'release',

    # 用芯片出厂 UID（ESIG 区，64 位）作 CMSIS-DAP 的 USB 序列号，对应
    # xmake.lua 的 option("esig_sn")。**默认 y** —— 每颗芯片唯一，多块板子
    # 同时插在一台机器上才能被主机区分；传 n 退回固定的 "DEADBEEF"。
    # 注意：xmake 会把选项值持久化到本地配置。所以这里每次都显式传
    # --esig_sn=...，保证构建结果只由参数决定。
    [ValidateSet('y', 'n')]
    [string]$EsigSn = 'y',

    # 是否把 WCH-Link（RVSWD）调试器也编进固件，对应 xmake.lua 的
    # option("wchlink")。**默认 n**：它的代码/数据占数 KiB Flash 与约 6 KiB RAM，
    # 平时用不到；双串口等功能要在 20 KiB SRAM 里放得下，离不开这份空间。
    # 同样每次都显式传，避免 xmake 记住上一次的选项。
    [ValidateSet('y', 'n')]
    [string]$Wchlink = 'n',

    # SWD 引脚方案，对应 xmake.lua 的 option("swd_pins")。
    # pa5_pa6 = 默认（SWDIO 在 PA6，PA7 留给 nRESET）；pa5_pa7 / pa2_pa3 用于 A/B 对照。
    [ValidateSet('pa5_pa7', 'pa5_pa6', 'pa2_pa3')]
    [string]$SwdPins = 'pa5_pa6',

    # 是否按「UF2 bootloader 布局」构建应用（应用从 0x4000 起），对应
    # xmake.lua 的 option("bootloader")。它同时决定是否构建 bootloader target。
    # 双模式固件会自动忽略它。
    [ValidateSet('y', 'n')]
    [string]$Bootloader = 'y',

    [switch]$Clean,
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'

# WCH RISC-V 工具链（MounRiver Studio 2 自带）
# toolchains/wch-riscv/xmake.lua 只会自动探测 C:/MounRiver/MounRiver_Studio2/...，
# 这里再显式指定常见安装位置，找不到时交给 xmake 默认探测。
# 版本优先 GCC15（-Os 下 bootloader/应用都更小），其次 GCC12；
# 已经设了 WCH_TOOLCHAIN_ROOT 环境变量时以环境变量为准（想固定用某个版本就设它）。
$toolchainRoot = $env:WCH_TOOLCHAIN_ROOT
if (-not $toolchainRoot) {
    $toolchainBases = @(
        'D:\program\wch\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain',
        'C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain')

    foreach ($version in @('RISC-V Embedded GCC15', 'RISC-V Embedded GCC12')) {
        foreach ($base in $toolchainBases) {
            $candidate = Join-Path $base $version
            if (Test-Path $candidate) {
                $toolchainRoot = $candidate
                break
            }
        }
        if ($toolchainRoot) { break }
    }
    if ($toolchainRoot) { $env:WCH_TOOLCHAIN_ROOT = $toolchainRoot }
}

Push-Location $PSScriptRoot
try {
    # 双模式固件（约 57 KiB）放不下 16 KiB 的 bootloader，xmake 里也会自动关掉；
    # 这里先算出来，好让打印的布局与实际一致。
    $useBootloader = ($Bootloader -eq 'y') -and ($Wchlink -ne 'y')

    $snLabel = if ($EsigSn -eq 'y') { '芯片 ESIG UID（每颗唯一）' } else { '固定 DEADBEEF' }
    $dbgLabel = if ($Wchlink -eq 'y') { '双模式（CMSIS-DAP + WCH-Link，运行中双击 PB3 切换）' } else { 'CMSIS-DAP only（-Wchlink y 可加编 WCH-Link）' }
    $layoutLabel = if ($useBootloader) {
        'UF2 bootloader @0x0 + 应用 @0x4000（可拖 firmware.uf2 升级）'
    } elseif ($Wchlink -eq 'y') {
        '应用 @0x0（双模式固件放不下 bootloader，只走 SWD 烧录）'
    } else {
        '应用 @0x0（不带 bootloader，只走 SWD 烧录）'
    }

    Write-Host "工具链   : $(if ($toolchainRoot) { $toolchainRoot } else { '交给 xmake 自动探测' })" -ForegroundColor DarkGray
    Write-Host "调试器   : $dbgLabel" -ForegroundColor Yellow
    Write-Host "布局     : $layoutLabel" -ForegroundColor Yellow
    Write-Host "SWD 引脚 : $SwdPins" -ForegroundColor DarkGray
    Write-Host "序列号   : $snLabel (-EsigSn $EsigSn)" -ForegroundColor DarkGray

    # 换编译器必须重编：xmake 的对象依赖里没有编译器版本，切了工具链却不清缓存，
    # 会继续拿旧编译器编出来的 .o 去链接（现象：打印的路径变了，尺寸却一点没变）。
    $stampFile = Join-Path $PSScriptRoot 'build\.toolchain-root'
    if ($toolchainRoot) {
        $previousRoot = if (Test-Path $stampFile) { (Get-Content -Path $stampFile -Raw).Trim() } else { '' }
        if ($previousRoot -and ($previousRoot -ne $toolchainRoot)) {
            Write-Host "==> 工具链已从 $previousRoot 换成 $toolchainRoot，先清缓存" -ForegroundColor Yellow
            $Clean = $true
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $stampFile -Parent) | Out-Null
        Set-Content -Path $stampFile -Value $toolchainRoot -NoNewline
    }

    if ($Clean) {
        Write-Host '==> xmake clean --all' -ForegroundColor Cyan
        xmake clean --all -P $PSScriptRoot
        if ($LASTEXITCODE -ne 0) { throw 'xmake clean 失败' }
    }

    # -P 指定本目录为工程根：仓库根目录也有一个只含 task 的 xmake.lua，
    # 不指定时 xmake 会向上找到它，导致找不到 --esig_sn 等本工程选项。
    #
    # 应用与 bootloader 现在**同一个工程**（本目录的 xmake.lua 里两个 target），
    # 所以这里只配置/构建一次，xmake 会把 bootloader 排在前、firmware 排在后。
    $configArgs = @('f', '-P', $PSScriptRoot, '-m', $Mode, "--esig_sn=$EsigSn",
                    "--wchlink=$Wchlink", "--swd_pins=$SwdPins",
                    "--bootloader=$Bootloader", '-y')
    if ($Reconfigure) { $configArgs += '-c' }
    Write-Host "==> xmake $($configArgs -join ' ')" -ForegroundColor Cyan
    & xmake @configArgs
    if ($LASTEXITCODE -ne 0) { throw 'xmake 配置失败' }

    Write-Host '==> xmake -r' -ForegroundColor Cyan
    xmake -r -P $PSScriptRoot
    if ($LASTEXITCODE -ne 0) { throw 'xmake 构建失败' }

    # ===== 合并镜像：bootloader(0x0) + 应用(0x4000) =====
    #
    # 用途：没有调试器（RVSWD/WCH-Link 烧不了）时，用「整片烧录」的方式一次写进去。
    # 常见工具都接受「一个 bin + 起始地址 0x0」：
    #   * WCH 芯片自带的 BOOT 区（USB ISP）：WCHISPTool / wchisp，见 .\flash_isp.ps1
    #   * 任何 SWD/SDI 调试器（WCH-Link、J-Link、DAPLink…）
    if ($useBootloader) {
        $bootBin = Join-Path $PSScriptRoot "build\$Mode\bootloader.bin"
        $appBin = Join-Path $PSScriptRoot "build\$Mode\firmware.bin"

        if ((Test-Path $bootBin) -and (Test-Path $appBin)) {
            $boot = [System.IO.File]::ReadAllBytes($bootBin)
            $app = [System.IO.File]::ReadAllBytes($appBin)
            if ($boot.Length -gt 0x4000) {
                throw "bootloader.bin 有 $($boot.Length) 字节，超过 16 KiB 的 bootloader 区"
            }

            $full = New-Object byte[] (0x4000 + $app.Length)
            [Array]::Fill($full, [byte]0xFF)
            [Array]::Copy($boot, 0, $full, 0, $boot.Length)
            [Array]::Copy($app, 0, $full, 0x4000, $app.Length)

            $fullPath = Join-Path $PSScriptRoot "build\$Mode\firmware_full.bin"
            [System.IO.File]::WriteAllBytes($fullPath, $full)
            Write-Host ''
            Write-Host ("合并镜像: {0}（{1} 字节 = bootloader {2} + 应用 {3}）" -f `
                    $fullPath, $full.Length, $boot.Length, $app.Length) -ForegroundColor Green
            Write-Host '  手动整片烧录（不用调试器）: .\flash_isp.ps1' -ForegroundColor Green
        } else {
            Write-Host '跳过合并镜像：bootloader.bin 或 firmware.bin 不存在' -ForegroundColor Yellow
        }
    }

    Write-Host ''
    Write-Host "产物（build\$Mode\）:" -ForegroundColor Cyan
    $artifacts = @('bootloader.elf', 'bootloader.bin', 'bootloader.map',
                   'firmware.elf', 'firmware.bin', 'firmware.map', 'firmware.uf2',
                   'firmware_full.bin')
    foreach ($name in $artifacts) {
        $path = Join-Path $PSScriptRoot "build\$Mode\$name"
        if (Test-Path $path) {
            Write-Host ("  {0,-18} {1,9} B" -f $name, (Get-Item $path).Length)
        }
    }

    if ($useBootloader) {
        Write-Host ''
        Write-Host '升级：上电时按住 PB3 约 1.2 s 进入 UF2，把 firmware.uf2 拖进 U 盘即可。' -ForegroundColor Green
        Write-Host '首次烧录（有 WCH-Link / SDI）: .\flash.ps1' -ForegroundColor Green
        Write-Host '首次烧录（没有调试器，走芯片自带 USB ISP）: .\flash_isp.ps1' -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
