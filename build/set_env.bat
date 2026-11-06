@echo off
rem ===========================================================================
rem  set_env.bat - Sets the build environment. The other scripts call it.
rem  The values come from ..\cots\cots_env.bat (made by setup_cots.ps1).
rem  If the cots folder is not ready, setup_cots.ps1 runs first.
rem ===========================================================================
if not exist "%~dp0..\cots\cots_env.bat" (
	echo [*] The cots folder is not ready. setup_cots.ps1 runs now.
	powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_cots.ps1"
	if errorlevel 1 exit /b 1
)
call "%~dp0..\cots\cots_env.bat"
if not exist "%TE_CMAKE%" (
	echo [ERROR] CMake is not found: %TE_CMAKE%. Run setup_cots.bat.
	exit /b 1
)
set "PATH=%TE_QT_DIR%\bin;%TE_COTS%\xerces-c\bin;%TE_COTS%\7zip;%PATH%"
exit /b 0
