@echo off
setlocal EnableExtensions
set "GAME_ROOT=%~1"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "EXTRACTED_ROOT=%~2"
if "%EXTRACTED_ROOT%"=="" set "EXTRACTED_ROOT=%GAME_ROOT%\models\gta3"
set "OUT_DIR=%~dp0build\vc-local"

if not exist "%GAME_ROOT%" (
  echo ERROR: GTA Vice City root not found: %GAME_ROOT%
  pause
  exit /b 2
)
if not exist "%EXTRACTED_ROOT%" (
  echo ERROR: extracted gta3 folder not found: %EXTRACTED_ROOT%
  pause
  exit /b 2
)

echo ===== BUILD ORIGINAL GTA VICE CITY OCEANIC ONLY =====
echo Game      : %GAME_ROOT%
echo Extracted : %EXTRACTED_ROOT%
echo Output    : %OUT_DIR%\VCVEH.BIN
echo.
echo This does NOT rebuild VCMAP/VCCOL or the full-city pages.
echo.

py -3 -c "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('rwfury') else 1)"
if errorlevel 1 (
  py -3 -m pip install rwfury
  if errorlevel 1 exit /b 1
)

py -3 "%~dp0vc_vehicle_import.py" ^
  --game-root "%GAME_ROOT%" ^
  --extracted-root "%EXTRACTED_ROOT%" ^
  --model oceanic ^
  --detail-budget 14000 ^
  --output-bin "%OUT_DIR%\VCVEH.BIN" ^
  --output-report "%OUT_DIR%\vc_vehicle_report.json"
if errorlevel 1 (
  echo ERROR: Oceanic vehicle build failed.
  pause
  exit /b 1
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

echo.
echo ===== OCEANIC READY =====
echo Vehicle : %OUT_DIR%\VCVEH.BIN
echo Handling: %~dp0build\vc-handling\VCHAND.BIN
echo Now run DEPLOY_VC_FULL_WORLD.cmd
pause
