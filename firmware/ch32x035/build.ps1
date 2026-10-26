# 构建 ch32x035f8u-dap (uDap) 固件
#
# 调试器模式：
#   CMSIS-DAP            固定编入
#   WCH-Link（RVSWD）    默认**不编入**（省 Flash 与约 6 KiB RAM）；
#                        传 -Wchlink y 编成双模式固件，上电时按住 PB3 切换。
#
# 用法:
#   .\build.ps1               # release 构建（默认用芯片 ESIG UID 作序列号）
#   .\build.ps1 -Wchlink y    # 把 WCH-Link（RVSWD）也编进去（双模式）
#   .\build.ps1 -EsigSn n     # 退回固定串 DEADBEEF（所有板子相同）
#   .\build.ps1 -SwdSpi y     # 写数据相位改用硬件 SPI1 24 MHz（需 pa5_pa7 方案）
#   .\build.ps1 -SwdPins pa5_pa6   # 换 SWD 引脚方案（A/B 对照用）
#   .\build.ps1 -Clean        # 先清理再构建
#   .\build.ps1 -Mode debug   # debug 构建
#   .\build.ps1 -Reconfigure  # 强制重新生成配置（工具链路径变化后用）
#
# 参数用空格或冒号分隔（-Mode debug 或 -Mode:debug），**不能写 -Mode=debug**。
#
# 产物: build\<mode>\firmware.elf / .bin / .map

# [CmdletBinding()] 是必需的：没有它时，无法识别的参数（例如误写成
# `-Mode=debug`）会被静默收进 $args 并被忽略，脚本照常按默认值构建且不报错
# —— 那是很危险的失败模式（以为编了 debug，其实没有）。
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Mode = 'release',

    # 用芯片出厂 UID（ESIG 区，96 位）作 CMSIS-DAP 的 USB 序列号，对应
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
    # pa5_pa7 = 现板（默认，SWDIO 在 PA7）；pa5_pa6 / pa2_pa3 用于 A/B 对照。
    [ValidateSet('pa5_pa7', 'pa5_pa6', 'pa2_pa3')]
    [string]$SwdPins = 'pa5_pa7',

    # 32 位数据相位改用硬件 SPI1（单线半双工），对应 xmake.lua 的 option("swd_spi")。
    # 只有 pa5_pa7 方案能用（PA5 = SPI1_SCK、PA7 = SPI1_MOSI）。默认 n。
    [string]$SwdSpi = 'n',

    # SPI 位相位的时钟分频（CTLR1 的 BR[2:0]）：0 = /2 24 MHz、1 = /4 12 MHz、
    # 2 = /8 6 MHz……7 = /256。对应 xmake.lua 的 option("swd_spi_br")。
    [ValidateRange(0, 7)]
    [int]$SwdSpiBr = 0,

    # 读数据相位的 CPHA：固定用 SPI 模式 3（空闲高、上升沿采样），见
    # src/dap/swd_spi_x035.c 的文件头，不需要配置。

    [switch]$Clean,
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'

# WCH RISC-V 工具链（MounRiver Studio 2 自带）
# toolchains/wch-riscv/xmake.lua 只会自动探测 C:/MounRiver/MounRiver_Studio2/...，
# 这里再显式指定常见安装位置，找不到时交给 xmake 默认探测。
foreach ($candidate in @(
        'D:\program\wch\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12',
        'C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12')) {
    if (Test-Path $candidate) {
        $env:WCH_TOOLCHAIN_ROOT = $candidate
        break
    }
}

Push-Location $PSScriptRoot
try {
    $snLabel = if ($EsigSn -eq 'y') { '芯片 ESIG UID（每颗唯一）' } else { '固定 DEADBEEF' }
    $dbgLabel = if ($Wchlink -eq 'y') { '双模式（CMSIS-DAP + WCH-Link，上电按住 PB3 切换）' } else { 'CMSIS-DAP only（-Wchlink y 可加编 WCH-Link）' }
    Write-Host "工具链   : $env:WCH_TOOLCHAIN_ROOT" -ForegroundColor DarkGray
    Write-Host "调试器   : $dbgLabel" -ForegroundColor Yellow
    Write-Host "SWD 引脚 : $SwdPins" -ForegroundColor DarkGray
    $spiLabel = if ($SwdSpi -eq 'y') {
        "硬件 SPI1 PCLK2/{0}（写数据相位；请求头/读相位仍 GPIO）" -f (1 -shl ($SwdSpiBr + 1))
    } else { 'GPIO 位时序（全部相位）' }
    Write-Host "数据相位 : $spiLabel (-SwdSpi $SwdSpi)" -ForegroundColor DarkGray
    Write-Host "序列号   : $snLabel (-EsigSn $EsigSn)" -ForegroundColor DarkGray

    if ($Clean) {
        Write-Host '==> xmake clean --all' -ForegroundColor Cyan
        xmake clean --all -P $PSScriptRoot
        if ($LASTEXITCODE -ne 0) { throw 'xmake clean 失败' }
    }

    # -P 指定本目录为工程根：仓库根目录也有一个只含 task 的 xmake.lua，
    # 不指定时 xmake 会向上找到它，导致找不到 --esig_sn 等本工程选项。
    $configArgs = @('f', '-P', $PSScriptRoot, '-m', $Mode, "--esig_sn=$EsigSn",
                    "--wchlink=$Wchlink", "--swd_pins=$SwdPins", "--swd_spi=$SwdSpi",
                    "--swd_spi_br=$SwdSpiBr", '-y')
    if ($Reconfigure) { $configArgs += '-c' }
    Write-Host "==> xmake $($configArgs -join ' ')" -ForegroundColor Cyan
    & xmake @configArgs
    if ($LASTEXITCODE -ne 0) { throw 'xmake 配置失败' }

    Write-Host '==> xmake -r' -ForegroundColor Cyan
    xmake -r -P $PSScriptRoot
    if ($LASTEXITCODE -ne 0) { throw 'xmake 构建失败' }

    Write-Host ''
    Get-ChildItem "build\$Mode\firmware.*" | Select-Object Name, Length, LastWriteTime
}
finally {
    Pop-Location
}
