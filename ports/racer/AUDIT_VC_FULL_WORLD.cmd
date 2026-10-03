@echo off
setlocal EnableExtensions
set "GAME_ROOT=%~1"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "EXTRACTED_ROOT=%~2"
if "%EXTRACTED_ROOT%"=="" set "EXTRACTED_ROOT=%GAME_ROOT%\models\gta3"
set "OUT_DIR=%~dp0build\vc-full-audit"

if not exist "%GAME_ROOT%" (
  echo ERROR: GTA Vice City root not found: %GAME_ROOT%
  pause
  exit /b 2
)

echo ===== Stayplaytion Racer - Vice City FULL WORLD AUDIT =====
echo Game root      : %GAME_ROOT%
echo Extracted gta3 : %EXTRACTED_ROOT%
echo Output         : %OUT_DIR%
echo.
echo No GTA DFF/TXD/COL/IMG bytes are copied into GitHub or reports.
echo.

if not exist "%EXTRACTED_ROOT%" (
  echo WARNING: extracted gta3 folder not found. gta3.img fallback will be used.
)

py -3 --version
if errorlevel 1 (
  echo ERROR: Python 3 launcher "py" not found.
  pause
  exit /b 2
)

py -3 -c "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('rwfury') else 1)"
if errorlevel 1 (
  echo Installing rwfury...
  py -3 -m pip install rwfury
  if errorlevel 1 (
    echo ERROR: rwfury installation failed.
    pause
    exit /b 1
  )
)

py -3 "%~dp0vc_full_world_audit.py" ^
  --game-root "%GAME_ROOT%" ^
  --extracted-root "%EXTRACTED_ROOT%" ^
  --page-m 192 ^
  --interior 0 ^
  --output-dir "%OUT_DIR%"

if errorlevel 1 (
  echo.
  echo ERROR: full-world audit failed.
  pause
  exit /b 1
)

echo.
echo ===== AUDIT COMPLETE =====
echo Send back these metadata files:
echo   %OUT_DIR%\vc_full_world_summary.txt
echo   %OUT_DIR%\vc_full_world_audit.json
echo   %OUT_DIR%\vc_full_models.csv
echo   %OUT_DIR%\vc_full_txds.csv
echo   %OUT_DIR%\vc_full_pages.csv
echo.
echo Do NOT upload the extracted GTA assets.
pause
