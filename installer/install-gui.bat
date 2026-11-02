@echo off
rem Double-click to open the reVC installer window. Needs Python 3 from https://www.python.org
rem Options are passed on, e.g. install-gui.bat --renderer gl3
where py >nul 2>nul
if %errorlevel%==0 (
	py -3 "%~dp0revc_installer.py" %*
) else (
	where python >nul 2>nul
	if errorlevel 1 (
		echo Python 3 is needed. Install it from https://www.python.org/downloads/
		echo and tick "Add python.exe to PATH" in the setup, then run this again.
		pause
		exit /b 1
	)
	python "%~dp0revc_installer.py" %*
)
if errorlevel 1 pause
