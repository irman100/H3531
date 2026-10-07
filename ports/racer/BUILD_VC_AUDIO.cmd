@echo off
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=E:\Games\GTA Vice City"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=E:\H3531\APPS\racer\audio"
set "STATIONS=%~3"
if "%STATIONS%"=="" set "STATIONS=WAVE VROCK FEVER EMOTION"

echo [HuDu/H3531] Importing Vice City audio from:
echo   GAME=%GAME%
echo   OUT=%OUT%
echo   STATIONS=%STATIONS%
echo.

py -3 "%~dp0vc_audio_import.py" "%GAME%" --out "%OUT%" --stations %STATIONS%
if errorlevel 1 exit /b %errorlevel%

if exist "I:\H3531\APPS\racer" (
  if not exist "I:\H3531\APPS\racer\audio" mkdir "I:\H3531\APPS\racer\audio"
  robocopy "%OUT%" "I:\H3531\APPS\racer\audio" /E
  if errorlevel 8 exit /b %errorlevel%
)

echo.
echo Audio import complete.
echo Engine/impact/wind are built into RACER.BIN.
echo R3 cycles imported stations and then switches radio off.
echo Default stations: WAVE VROCK FEVER EMOTION
echo To import all stations, pass ALL as the third argument.
exit /b 0
