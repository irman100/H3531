@echo off
setlocal
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0H3531-UART-BOOT.ps1"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0H3531-UART-BOOT.ps1" -Port "%~1"
)
endlocal
