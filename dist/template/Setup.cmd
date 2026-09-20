@echo off
title YAMPP - Setup
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %1
