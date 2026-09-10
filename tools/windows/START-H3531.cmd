@echo off
setlocal
title H3531 ONE-CLICK UART RAM BOOT 0.4
set "SCRIPT=%~dp0H3531-UART-BOOT.ps1"

if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Port "%~1"
)

set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
  echo [H3531] Boot helper stopped with error code %RC%.
) else (
  echo [H3531] Boot helper ended normally.
)
echo.
echo This window will NOT close automatically.
echo Review H3531-UART-last.log if something failed.
pause
exit /b %RC%
