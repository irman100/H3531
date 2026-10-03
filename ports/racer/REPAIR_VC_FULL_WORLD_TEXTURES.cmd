@echo off
setlocal EnableExtensions
set "GAME_ROOT=%~1"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "EXTRACTED_ROOT=%~2"
if "%EXTRACTED_ROOT%"=="" set "EXTRACTED_ROOT=%GAME_ROOT%\models\gta3"
set "OUT_DIR=%~dp0build\vc-full-pack"

if not exist "%OUT_DIR%\vc_full_pack_report.json" (
  echo ERROR: existing full-world pack not found: %OUT_DIR%
  echo Run BUILD_VC_FULL_WORLD.cmd first.
  pause
  exit /b 2
)
if not exist "%GAME_ROOT%\models" (
  echo ERROR: Vice City models folder not found: %GAME_ROOT%\models
  pause
  exit /b 2
)
if not exist "%EXTRACTED_ROOT%" (
  echo ERROR: extracted gta3 folder not found: %EXTRACTED_ROOT%
  pause
  exit /b 2
)

echo ===== REPAIR VFW1 MISSING TEXTURES =====
echo Game root      : %GAME_ROOT%
echo Loose models   : %GAME_ROOT%\models
echo Extracted gta3 : %EXTRACTED_ROOT%
echo Pack           : %OUT_DIR%
echo.
echo Only pages whose page_report.json still contains texture_missing
echo or texture_atlas_full will be rebuilt. Clean pages are reused.
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
  --page-m 192 ^
  --sector-m 24 ^
  --atlas-size 1024 ^
  --texture-max 40 ^
  --repair-missing-textures

if errorlevel 1 (
  echo.
  echo ERROR: texture repair failed.
  pause
  exit /b 1
)

echo.
echo ===== TEXTURE REPAIR COMPLETE =====
echo Run AUDIT_VC_FULL_PACK_LOCAL.cmd now.
echo.
pause
