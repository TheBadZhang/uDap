@echo off
rem Build uDap firmware (application + UF2 bootloader, one xmake.lua).
rem
rem All arguments are passed to build.ps1, e.g.:
rem   build.bat                 release build
rem   build.bat -Wchlink y      dual mode (CMSIS-DAP + WCH-Link)
rem   build.bat -Bootloader n   app only, at 0x0000
rem   build.bat -Clean          clean first
rem
rem Outputs in build\<mode>\: firmware.uf2, firmware_full.bin, bootloader.bin, ...
rem ASCII only on purpose: cmd.exe cannot be trusted with UTF-8 .bat files.
rem
rem Prefer pwsh (PowerShell 7); fall back to Windows PowerShell 5.1
rem (the .ps1 files carry a UTF-8 BOM so 5.1 parses them correctly).

setlocal
where pwsh >nul 2>nul
if %ERRORLEVEL%==0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
)
exit /b %ERRORLEVEL%