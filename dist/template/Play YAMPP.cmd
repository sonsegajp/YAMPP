@echo off
title Yet Another Melee PC Port (YAMPP)
cd /d "%~dp0"
if not exist "data\GALE01\sys\main.dol" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1"
  if not exist "data\GALE01\sys\main.dol" exit /b 1
)
start "" "%~dp0tools\python\pythonw.exe" "%~dp0scripts\play_yampp.py"
