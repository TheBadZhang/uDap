# 不用调试器，通过 WCH 芯片自带的 BOOT 区（系统 ISP，走 USB）烧录整片镜像
#
# 适用场景：板子上没有 SWD/SDI 可接，或者手头没有 WCH-Link。
# 烧写内容：build\<mode>\firmware_full.bin（bootloader@0x0 + 应用@0x4000），
#           起始地址 0x00000000。
#
# 用法:
#   .\flash_isp.ps1                        # 烧 build\release\firmware_full.bin
#   .\flash_isp.ps1 -File x.bin            # 烧指定文件
#   .\flash_isp.ps1 -Info                  # 只看芯片信息，不烧
#   .\flash_isp.ps1 -Tool C:\tools\wchisp.exe
#
# 依赖（任选其一）:
#   * wchisp（命令行，已支持 CH32X035）
#       - 预编译：https://github.com/ch32-rs/wchisp/releases/tag/nightly
#       - 或 cargo install wchisp
#       - Windows 若报找不到 CH375DLL64.dll：把该 dll 放到 wchisp.exe 旁边，
#         或用 Zadig 把设备的驱动换成 WinUSB
#   * WCHISPTool（WCH 官方 GUI）：芯片 CH32X035、接口 USB、文件选
#     firmware_full.bin、起始地址 0x00000000
#
# 为什么不用调试器也能烧：CH32X035 内置 3 KB 的系统 BOOT 区（厂家固化的
# bootloader）。用户区为空/无效、或应用主动把下次复位切到 BOOT 区时，芯片从那里
# 启动并枚举成 USB ISP 设备（VID:PID = 4348:55e0），wchisp / WCHISPTool 就是在跟
# 它说话。
#
# 如果电脑上看不到 ISP 设备：
#   * 换一根能传数据的 USB 线（很多线只有电源）；
#   * 板子上如果有 BOOT 按键/跳线，按住再上电（具体引脚看原理图）；
#   * 若片上还有能跑的固件：按住模式按键上电进 UF2，从 U 盘拖 firmware.uf2；
#   * 实在进不去，就借一块能用的 uDap（或任意 CMSIS-DAP）走 SWD 烧。

[CmdletBinding()]
param(
    [string]$File = 'build\release\firmware_full.bin',
    [switch]$Info,

    # wchisp 的可执行文件路径；留空则自动在 PATH 里找
    [string]$Tool = ''
)

$ErrorActionPreference = 'Stop'

function Find-Wchisp {
    if ($Tool) {
        if (-not (Test-Path $Tool)) { throw "找不到 wchisp: $Tool" }
        return $Tool
    }

    $cmd = Get-Command wchisp -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    return $null
}

function Show-ManualSteps {
    param([string]$Path)

    Write-Host ''
    Write-Host '没找到 wchisp。两种选择：' -ForegroundColor Yellow
    Write-Host ''
    Write-Host '  A) 装 wchisp（命令行，推荐，已支持 CH32X035）:' -ForegroundColor Cyan
    Write-Host '       下载预编译: https://github.com/ch32-rs/wchisp/releases/tag/nightly'
    Write-Host '       或:         cargo install wchisp'
    Write-Host '       Windows 报 CH375DLL64.dll 缺失时，把 dll 放到 exe 旁边即可。'
    Write-Host '       装好后重新运行: .\flash_isp.ps1'
    Write-Host ''
    Write-Host '  B) 用 WCH 官方 WCHISPTool（图形界面）:' -ForegroundColor Cyan
    Write-Host '       1. 芯片系列选 CH32X035，下载接口选 USB；'
    Write-Host "       2. 文件选 $Path；"
    Write-Host '       3. 起始地址填 0x00000000，点「下载」。'
    Write-Host ''
    Write-Host '  设备管理器里应能先看到 USB ISP 设备（4348:55E0）。看不到时：' -ForegroundColor DarkGray
    Write-Host '    - 换一根能传数据的 USB 线；' -ForegroundColor DarkGray
    Write-Host '    - 板上有 BOOT 按键/跳线的话，按住再上电；' -ForegroundColor DarkGray
    Write-Host '    - 片上还有固件的话，按住模式按键上电进 UF2，再从 U 盘升级。' -ForegroundColor DarkGray
}

if (-not (Test-Path $File)) {
    throw "找不到 $File —— 先运行 .\build.ps1（它会生成合并镜像 firmware_full.bin）"
}
$bin = (Resolve-Path $File).Path
$size = (Get-Item $bin).Length

Write-Host "文件    : $bin（$size 字节）" -ForegroundColor DarkGray
Write-Host "烧录地址: 0x00000000（bootloader 0x0 + 应用 0x4000 的合并镜像）" -ForegroundColor DarkGray

$exe = Find-Wchisp
if (-not $exe) {
    Show-ManualSteps -Path $bin
    exit 1
}

Write-Host "工具    : $exe" -ForegroundColor DarkGray

if ($Info) {
    & $exe info
    exit $LASTEXITCODE
}

Write-Host ''
Write-Host '==> 开始烧录（需要板子已被识别为 USB ISP 设备 4348:55E0）' -ForegroundColor Cyan
& $exe flash $bin
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Host "烧录失败（wchisp 退出码 $LASTEXITCODE）。" -ForegroundColor Red
    Write-Host '  常见原因：没进入 ISP 模式 / USB 线不通数据 / 芯片被读保护。' -ForegroundColor Yellow
    Show-ManualSteps -Path $bin
    exit $LASTEXITCODE
}

Write-Host ''
Write-Host '烧录完成。断电重新上电（或按复位）：' -ForegroundColor Green
Write-Host '  * 不按键 -> 直接进应用（CMSIS-DAP + 串口）；' -ForegroundColor DarkGray
Write-Host '  * 按住 PB3 再上电 -> 进 UF2，把 firmware.uf2 拖进 U 盘即可升级应用。' -ForegroundColor DarkGray
