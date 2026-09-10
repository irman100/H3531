@echo off
setlocal
title H3531 LIVE UART AUTO-BOOT TERMINAL 0.6
set "SCRIPT=%~dp0H3531-UART-BOOT.ps1"

if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Port "%~1"
)

exit /b %ERRORLEVEL%
