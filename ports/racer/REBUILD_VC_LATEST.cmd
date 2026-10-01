@echo off
setlocal

set "GAME_ROOT=E:\Gta Vice City 2010"
set "USB_RACER=I:\H3531\APPS\racer"

echo ===== Stayplaytion Racer - rebuild Starfish R700 city pack =====
echo Game: %GAME_ROOT%
echo USB : %USB_RACER%
echo.

if not exist "%GAME_ROOT%\gta-vc.exe" if not exist "%GAME_ROOT%\gta_vc.exe" (
  echo ERROR: Vice City was not found at "%GAME_ROOT%".
  echo Edit GAME_ROOT at the top of this file if your install moved.
  pause
  exit /b 2
)

if not exist "%USB_RACER%" (
  echo ERROR: Racer USB folder was not found at "%USB_RACER%".
  echo Insert/mount the USB drive or edit USB_RACER at the top of this file.
  pause
  exit /b 3
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0VC_LOCAL_IMPORT.ps1" ^
  -GameRoot "%GAME_ROOT%" ^
  -Region "starfish_large" ^
  -MaxInstances 0 ^
  -VehicleModel "auto" ^
  -VehicleDetailBudget 4500 ^
  -UsbRacerDir "%USB_RACER%"

if errorlevel 1 (
  echo.
  echo ERROR: Vice City rebuild failed.
  pause
  exit /b 1
)

echo.
echo SUCCESS: VCMAP.BIN, VCCOL.BIN and VCVEH.BIN were rebuilt and copied.
echo Start RACER.APP on the H3531 and send the new /var/h3531-native-app.log.
pause
