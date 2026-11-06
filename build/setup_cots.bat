@echo off
rem ===========================================================================
rem  setup_cots.bat - Finds or gets the third-party components and collects
rem  them in ..\cots. Double-click to run. See setup_cots.ps1 for the details.
rem   setup_cots.bat          normal scan (uses the components in cots)
rem   setup_cots.bat -Force   scans the computer again and makes the links again
rem ===========================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_cots.ps1" %*
if errorlevel 1 (
	echo.
	echo [ERROR] The cots setup is not complete. Read the messages above.
	if /i not "%~1"=="nopause" pause
	exit /b 1
)
if /i not "%~1"=="nopause" pause
exit /b 0
