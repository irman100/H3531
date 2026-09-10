@echo off
setlocal
set "SCRIPT=%~dp0H3531-UART-BOOT.ps1"
if "%~1"=="" (
  start "H3531 UART TERMINAL" powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -NoAutoBoot
) else (
  start "H3531 UART TERMINAL" powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Port "%~1" -NoAutoBoot
)
exit /b 0
