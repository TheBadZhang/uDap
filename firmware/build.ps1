# 构建 ch32x035f8u-dap (uDap) 固件
#
# 用法:
#   .\build.ps1              # release 构建（CMSIS-DAP 启用）
#   .\build.ps1 -Dap n       # 关闭 CMSIS-DAP，只编 USB 骨架
#   .\build.ps1 -Clean       # 先清理再构建
#   .\build.ps1 -Mode debug  # debug 构建
#   .\build.ps1 -Reconfigure # 强制重新生成配置（工具链路径变化后用）
#
# 参数用空格或冒号分隔（-Mode debug 或 -Mode:debug），**不能写 -Mode=debug**。
#
# 产物: build\<mode>\firmware.elf / .bin / .map

# [CmdletBinding()] 是必需的：没有它时，无法识别的参数（例如误写成
# `-Mode=debug`、`-Dap=n`）会被静默收进 $args 并被忽略，脚本照常按默认值
# 构建且不报错 —— 那是很危险的失败模式（以为编了 debug / 关了 DAP，其实没有）。
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Mode = 'release',

    # CMSIS-DAP（CherryDAP + src/dap + src/drv）开关，对应 xmake.lua 的 option("dap")。
    # 注意：xmake 会把选项值持久化到本地配置，之后再执行不传 --dap 的 `xmake f`
    # 也会沿用旧值。所以这里每次都显式传 --dap=...，保证构建结果只由参数决定。
    [ValidateSet('y', 'n')]
    [string]$Dap = 'y',

    [switch]$Clean,
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'

# WCH RISC-V 工具链（MounRiver Studio 2 自带）
# toolchains/wch-riscv/xmake.lua 只会自动探测 C:/MounRiver/MounRiver_Studio2/...，
# 本机装在 D:\program\wch 下，所以必须显式指定，否则报
# "WCH toolchain not found; set WCH_TOOLCHAIN_ROOT ..."
$env:WCH_TOOLCHAIN_ROOT = 'D:\program\wch\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12'

Push-Location $PSScriptRoot
try {
    $dapLabel = if ($Dap -eq 'y') { '启用' } else { '关闭' }
    Write-Host "工具链: $env:WCH_TOOLCHAIN_ROOT" -ForegroundColor DarkGray
    Write-Host "CMSIS-DAP: $dapLabel (-Dap $Dap)" -ForegroundColor DarkGray

    if ($Clean) {
        Write-Host '==> xmake clean --all' -ForegroundColor Cyan
        xmake clean --all
        if ($LASTEXITCODE -ne 0) { throw 'xmake clean 失败' }
    }

    $configArgs = @('f', '-m', $Mode, "--dap=$Dap", '-y')
    if ($Reconfigure) { $configArgs += '-c' }
    Write-Host "==> xmake $($configArgs -join ' ')" -ForegroundColor Cyan
    & xmake @configArgs
    if ($LASTEXITCODE -ne 0) { throw 'xmake 配置失败' }

    Write-Host '==> xmake -r' -ForegroundColor Cyan
    xmake -r
    if ($LASTEXITCODE -ne 0) { throw 'xmake 构建失败' }

    Write-Host ''
    Get-ChildItem "build\$Mode\firmware.*" | Select-Object Name, Length, LastWriteTime
}
finally {
    Pop-Location
}
