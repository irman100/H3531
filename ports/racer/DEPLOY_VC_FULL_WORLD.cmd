@echo off
setlocal EnableExtensions
set "PACK_DIR=%~1"
if "%PACK_DIR%"=="" set "PACK_DIR=%~dp0build\vc-full-pack"
set "USB_RACER=%~2"
if "%USB_RACER%"=="" set "USB_RACER=I:\H3531\APPS\racer"

if not exist "%PACK_DIR%\VCWORLD.BIN" (
  echo ERROR: VFW index not found: %PACK_DIR%\VCWORLD.BIN
  pause
  exit /b 2
)
if not exist "%PACK_DIR%\pages" (
  echo ERROR: VFW pages not found: %PACK_DIR%\pages
  pause
  exit /b 2
)
if not exist "%USB_RACER%" (
  echo ERROR: USB Racer folder not found: %USB_RACER%
  pause
  exit /b 3
)

echo ===== DEPLOY PAGED VICE CITY TO H3531 USB =====
echo Source: %PACK_DIR%
echo Target: %USB_RACER%
echo.
copy /Y "%PACK_DIR%\VCWORLD.BIN" "%USB_RACER%\VCWORLD.BIN" >nul
if errorlevel 1 exit /b 1

robocopy "%PACK_DIR%\pages" "%USB_RACER%\pages" /MIR /R:1 /W:1 /NFL /NDL /NJH /NJS /NP
set "RC=%ERRORLEVEL%"
if %RC% GEQ 8 (
  echo ERROR: robocopy failed with code %RC%
  pause
  exit /b %RC%
)
echo.
echo VFW1 deployed. GTA-derived page data remains on your local USB only.
echo Start RACER.APP and send /var/h3531-native-app.log after the drive test.
pause
