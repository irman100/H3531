@echo off
setlocal
set "SCRIPT=%~dp0H3531-UART-BOOT.ps1"

rem Start the UART terminal in its OWN PowerShell console, then end this batch
rem file immediately. This prevents CMD.EXE from ever owning Ctrl+C while the
rem serial terminal is active, so no "Terminate batch job [Y/N]?" prompt can
rem appear.
if "%~1"=="" (
  start "H3531 UART TERMINAL" powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
) else (
  start "H3531 UART TERMINAL" powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Port "%~1"
)
exit /b 0
