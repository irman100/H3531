@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "GAME_ROOT=%~1"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "OUT_DIR=%~dp0build\vc-local"
set "GTA3_IMG=%GAME_ROOT%\models\gta3.img"

if not exist "%GAME_ROOT%" (
  echo ERROR: GTA Vice City root not found: %GAME_ROOT%
  pause
  exit /b 2
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

echo ===== BUILD ORIGINAL GTA VICE CITY OCEANIC ONLY =====
if not exist "%GTA3_IMG%" (
  echo ERROR: original archive not found:
  echo   %GTA3_IMG%
  echo Oceanic stock build will not use models\gta3 or loose replacement files.
  pause
  exit /b 3
)

echo Game      : %GAME_ROOT%
echo Archive   : %GTA3_IMG%
echo Policy    : STOCK ARCHIVE ONLY
echo Model     : oceanic
echo Output    : %OUT_DIR%\VCVEH.BIN
echo.
echo Extracted models\gta3 is intentionally ignored for this vehicle.
echo This does NOT rebuild VCMAP/VCCOL or the full-city pages.
echo.

py -3 -c "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('rwfury') else 1)"
if errorlevel 1 (
  py -3 -m pip install rwfury
  if errorlevel 1 exit /b 1
)

del /F /Q "%OUT_DIR%\VCVEH.BIN" 2>nul
del /F /Q "%OUT_DIR%\vc_vehicle_report.json" 2>nul

py -3 "%~dp0vc_vehicle_import.py" ^
  --game-root "%GAME_ROOT%" ^
  --stock-archive-only ^
  --model oceanic ^
  --detail-budget 14000 ^
  --output-bin "%OUT_DIR%\VCVEH.BIN" ^
  --output-report "%OUT_DIR%\vc_vehicle_report.json"
if errorlevel 1 (
  echo ERROR: Oceanic vehicle build failed.
  pause
  exit /b 1
)

if not exist "%OUT_DIR%\VCVEH.BIN" (
  echo ERROR: strict Oceanic build produced no VCVEH.BIN.
  pause
  exit /b 4
)
if not exist "%OUT_DIR%\vc_vehicle_report.json" (
  echo ERROR: strict Oceanic build produced no provenance report.
  pause
  exit /b 4
)

findstr /C:"\"source_policy\": \"stock-archive-only\"" "%OUT_DIR%\vc_vehicle_report.json" >nul
if errorlevel 1 (
  echo ERROR: provenance report is not stock-archive-only.
  pause
  exit /b 5
)
findstr /I /C:"gta3.img" "%OUT_DIR%\vc_vehicle_report.json" >nul
if errorlevel 1 (
  echo ERROR: Oceanic report does not identify gta3.img as the DFF source.
  pause
  exit /b 5
)

py -3 "%~dp0vc_handling_import.py" ^
  --game-root "%GAME_ROOT%" ^
  --handling OCEANIC ^
  --output-bin "%~dp0build\vc-handling\VCHAND.BIN" ^
  --output-report "%~dp0build\vc-handling\vc_handling_report.json"
if errorlevel 1 (
  echo ERROR: Oceanic handling build failed.
  pause
  exit /b 1
)

py -3 "%~dp0vc_surface_import.py" ^
  --game-root "%GAME_ROOT%" ^
  --output-bin "%~dp0build\vc-handling\VCSURF.BIN" ^
  --output-report "%~dp0build\vc-handling\vc_surface_report.json"
if errorlevel 1 (
  echo ERROR: Vice City surface adhesion build failed.
  pause
  exit /b 1
)

echo.
echo ===== STOCK OCEANIC READY =====
echo Vehicle : %OUT_DIR%\VCVEH.BIN
echo Report  : %OUT_DIR%\vc_vehicle_report.json
echo Handling: %~dp0build\vc-handling\VCHAND.BIN
echo Surface : %~dp0build\vc-handling\VCSURF.BIN
echo Verify in report:
echo   source_policy = stock-archive-only
echo   dff_archive   = ...\models\gta3.img
echo Now run DEPLOY_VC_FULL_WORLD.cmd
if /I not "%RACER_BATCH_NOPAUSE%"=="1" pause
