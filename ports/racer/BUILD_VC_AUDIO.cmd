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
echo Engine, skid, landing and impact SFX were imported from your local Vice City.
echo Radio is stored as compact 24 kHz G.711 mu-law and decoded to 48 kHz on H3531.
echo Legacy RADIO_*.PCM files are removed station-by-station after compact conversion succeeds.
echo Wind has a quiet built-in fallback.
echo R3 cycles imported stations and then switches radio off.
echo Default stations: WAVE VROCK FEVER EMOTION
echo ALL imports all nine Vice City stations, including Flash FM and Radio Espantoso.
exit /b 0
