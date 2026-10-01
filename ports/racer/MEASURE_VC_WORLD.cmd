@echo off
setlocal

set "GAME_ROOT=E:\Gta Vice City 2010"
set "REPORT=%~dp0build\vc-world-size.json"

if not exist "%GAME_ROOT%" (
  echo ERROR: Vice City was not found at "%GAME_ROOT%".
  pause
  exit /b 2
)

if not exist "%~dp0build" mkdir "%~dp0build"

py -3 "%~dp0vc_local_import.py" ^
  --game-root "%GAME_ROOT%" ^
  --inventory-only ^
  --output-report "%REPORT%"

if errorlevel 1 (
  echo.
  echo ERROR: inventory failed.
  pause
  exit /b 1
)

powershell -NoProfile -Command ^
  "$j=Get-Content -Raw '%REPORT%' | ConvertFrom-Json; " ^
  "$mn=$j.bounds.min; $mx=$j.bounds.max; " ^
  "Write-Host ''; Write-Host '===== VICE CITY WORLD BOUNDS ====='; " ^
  "Write-Host ('min X/Y/Z : {0:N2} / {1:N2} / {2:N2}' -f $mn[0],$mn[1],$mn[2]); " ^
  "Write-Host ('max X/Y/Z : {0:N2} / {1:N2} / {2:N2}' -f $mx[0],$mx[1],$mx[2]); " ^
  "Write-Host ('size X/Y/Z: {0:N2} / {1:N2} / {2:N2}' -f ($mx[0]-$mn[0]),($mx[1]-$mn[1]),($mx[2]-$mn[2])); " ^
  "Write-Host ('instances : {0}' -f $j.instances); " ^
  "Write-Host ('report    : %REPORT%')"

echo.
pause
