@echo off
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=E:\Games\GTA Vice City"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=E:\H3531\APPS\racer\audio"
set "STATION=%~3"
if "%STATION%"=="" set "STATION=WAVE"

echo [HuDu/H3531] Importing Vice City audio from:
echo   GAME=%GAME%
echo   OUT=%OUT%
echo   STATION=%STATION%
echo.

py -3 "%~dp0vc_audio_import.py" "%GAME%" --out "%OUT%" --station "%STATION%"
if errorlevel 1 exit /b %errorlevel%

if exist "I:\H3531\APPS\racer" (
  if not exist "I:\H3531\APPS\racer\audio" mkdir "I:\H3531\APPS\racer\audio"
  robocopy "%OUT%" "I:\H3531\APPS\racer\audio" /E
  if errorlevel 8 exit /b %errorlevel%
)

echo.
echo Audio import complete.
echo Engine/impact/wind are built into RACER.BIN.
echo RADIO0.PCM enables the single R3 radio channel.
exit /b 0
