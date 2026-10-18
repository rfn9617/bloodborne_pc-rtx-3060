@echo off
setlocal
cd /d "%~dp0"
set "APPDATA=%CD%\launcher-settings"
"%CD%\Bloodborne.exe" --script "%CD%\scripts\rtx3060_play.py" --fg-preview %*
if errorlevel 1 pause
