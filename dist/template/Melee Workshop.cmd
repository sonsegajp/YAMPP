@echo off
title Melee Workshop
cd /d "%~dp0"
if not exist "data\GALE01\sys\main.dol" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1"
  if not exist "data\GALE01\sys\main.dol" exit /b 1
)
if not exist "workshop\Melee Workshop.exe" (
  echo The Workshop is not included in this release.
  pause
  exit /b 1
)
start "" "%~dp0workshop\Melee Workshop.exe"
