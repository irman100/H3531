@echo off
setlocal

if "%~1"=="" (
  set "GAME_ROOT=E:\Games\GTA Vice City"
) else (
  set "GAME_ROOT=%~1"
)

echo Source must be a clean, legally obtained Vice City installation.
set "USB_RACER=I:\H3531\APPS\racer"

if not exist "%GAME_ROOT%" (
  echo ERROR: game root not found: "%GAME_ROOT%"
  pause
  exit /b 2
)

if not exist "%USB_RACER%" (
  echo ERROR: USB racer directory not found: "%USB_RACER%"
  pause
  exit /b 2
)

echo ===== Stayplaytion Racer - clean Vice City Oceanic rebuild =====
echo Game: %GAME_ROOT%
echo USB : %USB_RACER%
echo Vehicle: oceanic
echo Region : starfish_large
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0VC_LOCAL_IMPORT.ps1" ^
  -GameRoot "%GAME_ROOT%" ^
  -Region "starfish_large" ^
  -MaxInstances 0 ^
  -VehicleModel "oceanic" ^
  -VehicleDetailBudget 14000 ^
  -UsbRacerDir "%USB_RACER%"

if errorlevel 1 (
  echo.
  echo ERROR: clean Oceanic rebuild failed.
  pause
  exit /b 1
)

echo.
echo SUCCESS: clean Vice City map/collision + Oceanic rebuilt and copied.
pause
