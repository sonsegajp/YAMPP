@echo off
title Yet Another Melee PC Port (YAMPP)
cd /d "%~dp0"
if not exist "data\GALE01\sys\main.dol" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1"
  if not exist "data\GALE01\sys\main.dol" exit /b 1
)
:: Catch up to the release the master server is running, before starting.
:: A room only admits clients on a matching build, so an out-of-date copy
:: cannot play online at all. Silent when there is nothing to do; when
:: there is, it says so and exits 3, and the game is not started from files
:: that are about to be replaced underneath it. Set YAMPP_NO_UPDATE=1 to
:: skip the check entirely.
if exist "%~dp0scripts\update_yampp.py" (
  "%~dp0tools\python\python.exe" "%~dp0scripts\update_yampp.py"
  if errorlevel 3 exit /b 0
)
start "" "%~dp0tools\python\pythonw.exe" "%~dp0scripts\play_yampp.py"
