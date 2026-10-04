@echo off
setlocal EnableExtensions
set "GAME_ROOT=%~1"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "EXTRACTED_ROOT=%~2"
if "%EXTRACTED_ROOT%"=="" set "EXTRACTED_ROOT=%GAME_ROOT%\models\gta3"
set "OUT_DIR=%~dp0build\vc-full-pack"

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

echo ===== Stayplaytion Racer - LOCAL STREAMING VICE CITY BUILD =====
echo Game root      : %GAME_ROOT%
echo Extracted gta3 : %EXTRACTED_ROOT%
echo Output         : %OUT_DIR%
echo.
echo This build stays local. Do not upload generated GTA-derived packs to GitHub.
echo Streaming grid : 96m pages, 5x5 bounded runtime window.
echo Old 192m page directories not present in the new grid will be pruned.
echo.

py -3 -c "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('rwfury') else 1)"
if errorlevel 1 (
  py -3 -m pip install rwfury
  if errorlevel 1 exit /b 1
)

py -3 "%~dp0vc_full_world_pack.py" ^
  --game-root "%GAME_ROOT%" ^
  --extracted-root "%EXTRACTED_ROOT%" ^
  --output-dir "%OUT_DIR%" ^
  --page-m 96 ^
  --sector-m 24 ^
  --atlas-size 1024 ^
  --texture-max 40 ^
  --resume ^
  --prune-stale-pages

if errorlevel 1 (
  echo.
  echo ERROR: full-world pack failed or atlas capacity was insufficient.
  echo Check %OUT_DIR%\vc_full_pack_report.json
  pause
  exit /b 1
)

echo.
echo ===== FULL WORLD PACK COMPLETE =====
echo Index   : %OUT_DIR%\VCWORLD.BIN
echo Summary : %OUT_DIR%\vc_full_pack_summary.txt
echo Report  : %OUT_DIR%\vc_full_pack_report.json
echo Pages   : %OUT_DIR%\pages
echo.
echo Send back only the summary/report, not the generated page assets.
pause
