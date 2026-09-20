@echo off
cd /d "%~dp0"
python "%~dp0scripts\project_config.py" --launch game --runtime-profile config/netplay-preview.xml
if errorlevel 1 pause
