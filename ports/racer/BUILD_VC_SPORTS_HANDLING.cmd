@echo off
setlocal EnableExtensions
set "GAME_ROOT=%~1"
if "%GAME_ROOT%"=="" set "GAME_ROOT=E:\Games\GTA Vice City"
set "HANDLING=%~2"
if "%HANDLING%"=="" set "HANDLING=CHEETAH"
set "OUT_DIR=%~dp0build\vc-handling"

if not exist "%GAME_ROOT%" (
  echo ERROR: Vice City root not found: %GAME_ROOT%
  pause
  exit /b 2
)

py -3 "%~dp0vc_handling_import.py" ^
  --game-root "%GAME_ROOT%" ^
  --handling "%HANDLING%" ^
  --output-bin "%OUT_DIR%\VCHAND.BIN" ^
  --output-report "%OUT_DIR%\vc_handling_report.json"
if errorlevel 1 (
  echo ERROR: handling profile build failed.
  pause
  exit /b 1
)

echo.
echo Built %HANDLING% GTA handling profile:
echo   %OUT_DIR%\VCHAND.BIN
echo   %OUT_DIR%\vc_handling_report.json
pause
