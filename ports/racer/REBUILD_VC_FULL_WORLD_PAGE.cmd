@echo off
setlocal EnableExtensions
set "PAGE=%~1"
if "%PAGE%"=="" (
  echo Usage: REBUILD_VC_FULL_WORLD_PAGE.cmd page_x,page_y ["GTA root"] ["extracted gta3"]
  echo Example: REBUILD_VC_FULL_WORLD_PAGE.cmd -13,15
  exit /b 2
)
set "GAME_ROOT=%~2"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "EXTRACTED_ROOT=%~3"
if "%EXTRACTED_ROOT%"=="" set "EXTRACTED_ROOT=%GAME_ROOT%\models\gta3"
set "OUT_DIR=%~dp0build\vc-full-pack"

for /f "tokens=1,2 delims=," %%A in ("%PAGE%") do (
  set "PX=%%~A"
  set "PY=%%~B"
)
if "%PY%"=="" (
  echo ERROR: page must be x,y, for example -13,15
  exit /b 2
)

set "PAGE_DIR=%OUT_DIR%\pages\P_%PX%_%PY%"
echo ===== REBUILD ONE VFW OBJECT-STREAM PAGE =====
echo Page      : %PX%,%PY%
echo Page dir  : %PAGE_DIR%
echo Game root : %GAME_ROOT%
echo.
echo If Windows reports ERROR 1392 for this directory, repair the filesystem first:
echo   chkdsk E: /f
echo Then run this command again.
echo.

if exist "%PAGE_DIR%" (
  echo Removing the old page so no corrupt/resume file can be reused...
  rmdir /S /Q "%PAGE_DIR%"
  if exist "%PAGE_DIR%" (
    echo ERROR: could not remove %PAGE_DIR%
    echo The filesystem must be repaired before rebuilding this page.
    exit /b 1392
  )
)

py -3 "%~dp0vc_full_world_pack.py" ^
  --game-root "%GAME_ROOT%" ^
  --extracted-root "%EXTRACTED_ROOT%" ^
  --output-dir "%OUT_DIR%" ^
  --page-m 96 ^
  --sector-m 24 ^
  --atlas-size 1024 ^
  --texture-max 40 ^
  --only-page "%PX%,%PY%"

if errorlevel 1 (
  echo ERROR: page rebuild failed.
  exit /b 1
)

echo.
echo Physically verifying the full pack after page repair...
py -3 "%~dp0vc_full_pack_local_audit.py" --pack-dir "%OUT_DIR%"
if errorlevel 1 (
  echo ERROR: full-pack readback verification failed.
  exit /b 10
)

echo.
echo PAGE REPAIR COMPLETE: P_%PX%_%PY%
echo Global VCWORLD.BIN was preserved.
exit /b 0
