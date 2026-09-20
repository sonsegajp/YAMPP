@echo off
cd /d "%~dp0"
python scripts\open_managed_test.py --hot-reload
if errorlevel 1 pause
