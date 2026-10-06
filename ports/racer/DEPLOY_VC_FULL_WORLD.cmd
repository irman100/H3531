@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "PACK_DIR=%~1"
if "%PACK_DIR%"=="" set "PACK_DIR=%~dp0build\vc-full-pack"
set "USB_RACER=%~2"
if "%USB_RACER%"=="" set "USB_RACER=I:\H3531\APPS\racer"
set "VEHICLE_MODE=%~3"
if "%VEHICLE_MODE%"=="" set "VEHICLE_MODE=oceanic"
set "HANDLING_ID=%~4"
if "%HANDLING_ID%"=="" set "HANDLING_ID=OCEANIC"
set "GAME_ROOT=%VC_GAME_ROOT%"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "LOCAL_VCVEH=%~dp0build\vc-local\VCVEH.BIN"
set "LOCAL_VCHAND=%~dp0build\vc-handling\VCHAND.BIN"
set "LOCAL_VCSURF=%~dp0build\vc-handling\VCSURF.BIN"
set "VFW_STAGE=%USB_RACER%\__vfw_stage"
set "VFW_OLD=%USB_RACER%\pages.__old"

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

echo Validating indexed VFW pack completeness and physical readability...
py -3 "%~dp0vc_full_pack_local_audit.py" --pack-dir "%PACK_DIR%"
if errorlevel 1 (
  echo ERROR: VFW source pack failed physical readback.
  echo Nothing was copied to USB.
  echo If the error is 1392 / file or directory corrupted, repair the source drive first:
  echo   chkdsk E: /f
  pause
  exit /b 10
)

echo ===== STAGE PAGED VICE CITY ON H3531 USB =====
echo Source : %PACK_DIR%
echo Stage  : %VFW_STAGE%
echo Target : %USB_RACER%
echo.

if exist "%VFW_STAGE%" (
  rmdir /S /Q "%VFW_STAGE%"
  if exist "%VFW_STAGE%" (
    echo ERROR: could not remove old USB staging directory.
    echo The USB filesystem may be damaged. Repair it before deploy:
    echo   chkdsk I: /f
    pause
    exit /b 11
  )
)
mkdir "%VFW_STAGE%\pages"
if errorlevel 1 (
  echo ERROR: could not create USB staging directory.
  pause
  exit /b 11
)

copy /Y "%PACK_DIR%\VCWORLD.BIN" "%VFW_STAGE%\VCWORLD.BIN" >nul
if errorlevel 1 (
  echo ERROR: could not stage VCWORLD.BIN.
  pause
  exit /b 11
)

robocopy "%PACK_DIR%\pages" "%VFW_STAGE%\pages" /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP
set "RC=%ERRORLEVEL%"
if %RC% GEQ 8 (
  echo ERROR: staging page copy failed with code %RC%.
  echo Working pages were not touched.
  pause
  exit /b %RC%
)

echo Verifying staged USB pack by reading every binary back...
py -3 "%~dp0vc_full_pack_local_audit.py" --pack-dir "%VFW_STAGE%"
if errorlevel 1 (
  echo ERROR: staged USB pack failed readback.
  echo Working pages were not touched.
  echo Repair/check the USB filesystem:
  echo   chkdsk I: /f
  pause
  exit /b 12
)

echo.
echo ===== COMMIT VERIFIED VFW STAGE =====
if exist "%VFW_OLD%" (
  rmdir /S /Q "%VFW_OLD%"
  if exist "%VFW_OLD%" (
    echo ERROR: stale pages.__old cannot be removed.
    echo Repair the USB filesystem first:
    echo   chkdsk I: /f
    pause
    exit /b 13
  )
)

if exist "%USB_RACER%\pages" (
  move /Y "%USB_RACER%\pages" "%VFW_OLD%" >nul
  if errorlevel 1 (
    echo ERROR: current pages directory cannot be renamed.
    echo No staged page was activated. Repair the USB filesystem:
    echo   chkdsk I: /f
    pause
    exit /b 13
  )
)

move /Y "%VFW_STAGE%\pages" "%USB_RACER%\pages" >nul
if errorlevel 1 (
  echo ERROR: could not activate staged pages.
  if exist "%VFW_OLD%" move /Y "%VFW_OLD%" "%USB_RACER%\pages" >nul
  pause
  exit /b 14
)

copy /Y "%VFW_STAGE%\VCWORLD.BIN" "%USB_RACER%\VCWORLD.BIN" >nul
if errorlevel 1 (
  echo ERROR: could not activate staged VCWORLD.BIN.
  pause
  exit /b 14
)

if exist "%VFW_STAGE%" rmdir /S /Q "%VFW_STAGE%"

echo.
echo ===== DEPLOY VERIFIED RUNTIME / VEHICLE =====
echo Source : %PACK_DIR%
echo Target : %USB_RACER%
echo Vehicle : %VEHICLE_MODE%
echo Handling: %HANDLING_ID% from %GAME_ROOT%\data\handling.cfg
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
rem   oceanic (default) = original GTA Vice City Oceanic DFF/TXD/COL + OCEANIC handling.cfg.
rem                       This is the yellow/white car parked at Ocean View Hotel.
rem   sports             = optional development fallback using the downloaded built-in car.
rem   vcveh              = compatibility alias for the locally rebuilt GTA VCVEH.BIN.
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
  if not exist "%GAME_ROOT%\data\handling.cfg" if not exist "%GAME_ROOT%\DATA\HANDLING.CFG" (
    echo ERROR: handling.cfg was not found under "%GAME_ROOT%".
    pause
    exit /b 7
  )
  echo Building GTA handling profile %HANDLING_ID% for the optional downloaded sports car...
  py -3 "%~dp0vc_handling_import.py" ^
    --game-root "%GAME_ROOT%" ^
    --handling "%HANDLING_ID%" ^
    --output-bin "%~dp0build\vc-handling\VCHAND.BIN" ^
    --output-report "%~dp0build\vc-handling\vc_handling_report.json"
  if errorlevel 1 (
    echo ERROR: handling profile build failed.
    pause
    exit /b 8
  )
  copy /Y "%~dp0build\vc-handling\VCHAND.BIN" "%USB_RACER%\VCHAND.BIN" >nul
  if errorlevel 1 exit /b 1
) else if /I "%VEHICLE_MODE%"=="oceanic" (
  echo Rebuilding stock Oceanic directly from "%GAME_ROOT%\models\gta3.img"...
  echo Loose/extracted models are forbidden for this build.
  set "RACER_BATCH_NOPAUSE=1"
  call "%~dp0BUILD_VC_OCEANIC_ONLY.cmd" "%GAME_ROOT%"
  set "BUILD_RC=!ERRORLEVEL!"
  set "RACER_BATCH_NOPAUSE="
  if not "!BUILD_RC!"=="0" (
    echo ERROR: strict stock Oceanic build failed with code !BUILD_RC!.
    pause
    exit /b !BUILD_RC!
  )
  if not exist "!LOCAL_VCVEH!" (
    echo ERROR: stock Oceanic build returned success but VCVEH.BIN is missing:
    echo   !LOCAL_VCVEH!
    pause
    exit /b 5
  )
  echo Copying stock GTA Vice City Oceanic...
  copy /Y "!LOCAL_VCVEH!" "%USB_RACER%\VCVEH.BIN" >nul
  if errorlevel 1 exit /b 1

  if not exist "!LOCAL_VCHAND!" (
    echo ERROR: stock Oceanic build did not create VCHAND.BIN:
    echo   !LOCAL_VCHAND!
    pause
    exit /b 8
  )
  copy /Y "!LOCAL_VCHAND!" "%USB_RACER%\VCHAND.BIN" >nul
  if errorlevel 1 exit /b 1

  if not exist "!LOCAL_VCSURF!" (
    echo ERROR: stock Oceanic build did not create VCSURF.BIN:
    echo   !LOCAL_VCSURF!
    pause
    exit /b 9
  )
  copy /Y "!LOCAL_VCSURF!" "%USB_RACER%\VCSURF.BIN" >nul
  if errorlevel 1 exit /b 1
) else if /I "%VEHICLE_MODE%"=="vcveh" (
  if not exist "!LOCAL_VCVEH!" (
    echo ERROR: explicit vcveh mode requested but file is missing:
    echo   !LOCAL_VCVEH!
    echo Rebuild it first with BUILD_VC_OCEANIC_ONLY.cmd.
    pause
    exit /b 5
  )
  copy /Y "!LOCAL_VCVEH!" "%USB_RACER%\VCVEH.BIN" >nul
  if errorlevel 1 exit /b 1
  echo Building exact GTA handling profile %HANDLING_ID%...
  py -3 "%~dp0vc_handling_import.py" ^
    --game-root "%GAME_ROOT%" ^
    --handling "%HANDLING_ID%" ^
    --output-bin "%~dp0build\vc-handling\VCHAND.BIN" ^
    --output-report "%~dp0build\vc-handling\vc_handling_report.json"
  if errorlevel 1 exit /b 8
  copy /Y "%~dp0build\vc-handling\VCHAND.BIN" "%USB_RACER%\VCHAND.BIN" >nul
  if errorlevel 1 exit /b 1
) else (
  echo ERROR: unknown vehicle mode "%VEHICLE_MODE%".
  echo Use: oceanic ^(default original GTA Vice City Oceanic^)
  echo   or sports   ^(optional downloaded sports-car fallback^)
  echo   or vcveh    ^(compatibility alias for GTA VCVEH.BIN^)
  pause
  exit /b 6
)

echo.
if exist "%VFW_OLD%" (
  rmdir /S /Q "%VFW_OLD%"
  if exist "%VFW_OLD%" (
    echo WARNING: verified new pages are active, but pages.__old could not be removed.
    echo Run chkdsk I: /f before the next deploy.
  )
)
echo.
if /I "%VEHICLE_MODE%"=="oceanic" (
  echo VFW1 + Racer deployed with stock-archive Oceanic + OCEANIC handling + Vice City surface adhesion.
) else if /I "%VEHICLE_MODE%"=="sports" (
  echo VFW1 + Racer deployed with optional built-in sports car + GTA %HANDLING_ID% handling.
) else (
  echo VFW1 + Racer deployed with GTA VCVEH.BIN + %HANDLING_ID% handling.
)
echo GTA-derived page data remains on your local USB only.
echo Start RACER.APP and send /var/h3531-native-app.log after the drive test.
pause
