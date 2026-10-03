@echo off
setlocal EnableExtensions
set "PACK_DIR=%~1"
if "%PACK_DIR%"=="" set "PACK_DIR=%~dp0build\vc-full-pack"
set "USB_RACER=%~2"
if "%USB_RACER%"=="" set "USB_RACER=I:\H3531\APPS\racer"
set "VEHICLE_MODE=%~3"
if "%VEHICLE_MODE%"=="" set "VEHICLE_MODE=sports"

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
echo Source : %PACK_DIR%
echo Target : %USB_RACER%
echo Vehicle: %VEHICLE_MODE%
echo.

rem The tool lives under H3531\SYSTEM\tools\vc-import. Resolve the matching
rem local overlay root and deploy the freshly built runtime together with VFW1.
for %%I in ("%~dp0..\..\..") do set "LOCAL_H3531=%%~fI"
if exist "%LOCAL_H3531%\APPS\racer\RACER.BIN" (
  copy /Y "%LOCAL_H3531%\APPS\racer\RACER.BIN" "%USB_RACER%\RACER.BIN" >nul
  if errorlevel 1 exit /b 1
)
if exist "%LOCAL_H3531%\APPS\racer\RACER.APP" (
  copy /Y "%LOCAL_H3531%\APPS\racer\RACER.APP" "%USB_RACER%\RACER.APP" >nul
  if errorlevel 1 exit /b 1
)

rem Player vehicle policy:
rem   sports (default) = use the downloaded/built-in higher-detail sports car.
rem                      Any stale VCVEH.BIN would override it, so remove it.
rem   vcveh            = explicitly deploy the locally rebuilt Vice City VCVEH.BIN.
if /I "%VEHICLE_MODE%"=="sports" (
  if exist "%USB_RACER%\VCVEH.BIN" (
    echo Removing stale VCVEH.BIN so the built-in sports car stays active...
    del /F /Q "%USB_RACER%\VCVEH.BIN"
    if exist "%USB_RACER%\VCVEH.BIN" (
      echo ERROR: could not remove stale VCVEH.BIN
      pause
      exit /b 4
    )
  )
) else if /I "%VEHICLE_MODE%"=="vcveh" (
  set "LOCAL_VCVEH=%~dp0build\vc-local\VCVEH.BIN"
  if not exist "%LOCAL_VCVEH%" (
    echo ERROR: explicit vcveh mode requested but file is missing:
    echo   %LOCAL_VCVEH%
    echo Rebuild it first with REBUILD_VC_ORIGINAL_OCEANIC.cmd.
    pause
    exit /b 5
  )
  copy /Y "%LOCAL_VCVEH%" "%USB_RACER%\VCVEH.BIN" >nul
  if errorlevel 1 exit /b 1
) else (
  echo ERROR: unknown vehicle mode "%VEHICLE_MODE%".
  echo Use: sports  ^(default downloaded sports car^)
  echo   or vcveh   ^(explicit local Vice City VCVEH.BIN^)
  pause
  exit /b 6
)

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
if /I "%VEHICLE_MODE%"=="sports" (
  echo VFW1 + Racer deployed with built-in downloaded sports car.
) else (
  echo VFW1 + Racer deployed with explicit VCVEH.BIN.
)
echo GTA-derived page data remains on your local USB only.
echo Start RACER.APP and send /var/h3531-native-app.log after the drive test.
pause
