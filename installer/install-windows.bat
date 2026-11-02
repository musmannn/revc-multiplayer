@echo off
rem Double-click to build and install reVC multiplayer. Options are passed on,
rem e.g. install-windows.bat -Renderer gl3 -Source "D:\Games\GTA Vice City"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-windows.ps1" %*
echo.
pause
