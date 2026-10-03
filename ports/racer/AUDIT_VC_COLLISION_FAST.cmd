@echo off
setlocal EnableExtensions
set "GAME_ROOT=%~1"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "EXTRACTED_ROOT=%~2"
if "%EXTRACTED_ROOT%"=="" set "EXTRACTED_ROOT=%GAME_ROOT%\models\gta3"
set "OUT_DIR=%~dp0build\vc-collision-audit"

echo ===== Stayplaytion Racer - Vice City COLLISION FAST AUDIT =====
echo Game root      : %GAME_ROOT%
echo Extracted gta3 : %EXTRACTED_ROOT%
echo Output         : %OUT_DIR%
echo.

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

py -3 -c "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('rwfury') else 1)"
if errorlevel 1 (
  py -3 -m pip install rwfury
  if errorlevel 1 exit /b 1
)

py -3 "%~dp0vc_full_world_audit.py" ^
  --game-root "%GAME_ROOT%" ^
  --extracted-root "%EXTRACTED_ROOT%" ^
  --page-m 192 ^
  --interior 0 ^
  --fast ^
  --output-dir "%OUT_DIR%"

if errorlevel 1 (
  echo ERROR: collision audit failed.
  pause
  exit /b 1
)

echo.
echo ===== COLLISION AUDIT COMPLETE =====
echo Send back:
echo   %OUT_DIR%\vc_full_world_summary.txt
echo   %OUT_DIR%\vc_full_world_audit.json
pause
