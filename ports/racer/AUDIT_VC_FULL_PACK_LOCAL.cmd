@echo off
setlocal EnableExtensions
set "PACK_DIR=%~1"
if "%PACK_DIR%"=="" set "PACK_DIR=%~dp0build\vc-full-pack"
if not exist "%PACK_DIR%\pages" (
  echo ERROR: VFW pages not found: %PACK_DIR%\pages
  pause
  exit /b 2
)
py -3 "%~dp0vc_full_pack_local_audit.py" --pack-dir "%PACK_DIR%"
if errorlevel 1 (
  echo ERROR: local VFW audit failed.
  pause
  exit /b 1
)
echo.
echo Send back:
echo   %PACK_DIR%\vc_full_pack_local_audit.txt
echo   %PACK_DIR%\vc_full_pack_local_audit.json
pause
