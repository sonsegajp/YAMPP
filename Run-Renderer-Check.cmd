@echo off
cd /d "%~dp0"
if not exist build\pc\bin\melee_renderer_check.exe (
    echo Build the renderer check first using scripts\build.ps1.
    pause
    exit /b 1
)
build\pc\bin\melee_renderer_check.exe --frames 600 --capture build\renderer-check.ppm
exit /b %errorlevel%
