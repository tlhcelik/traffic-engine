@echo off
rem ===========================================================================
rem  run_Debug_x64.bat - Starts the Debug version in debug mode.
rem  Compile it first with build_Debug_x64.bat.
rem ===========================================================================
if not exist "%~dp0..\bin\Debug\traffic_engine.exe" (
	echo [ERROR] bin\Debug\traffic_engine.exe is not found. Run build_Debug_x64.bat first.
	pause
	exit /b 1
)
start "" "%~dp0..\bin\Debug\traffic_engine.exe" --debug %*
