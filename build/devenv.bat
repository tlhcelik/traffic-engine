@echo off
rem ===========================================================================
rem  devenv.bat - Opens the Debug solution in Visual Studio.
rem  F5 in Visual Studio starts ..\bin\Debug\traffic_engine.exe.
rem ===========================================================================
setlocal
cd /d "%~dp0"
call "%~dp0set_env.bat" || goto :fail
if not exist "Debug\traffic_engine.sln" (
	"%TE_CMAKE%" -S . -B Debug -G "%TE_GENERATOR%" -A x64 || goto :fail
)
start "" "%TE_VS_DIR%\Common7\IDE\devenv.exe" "%~dp0Debug\traffic_engine.sln"
exit /b 0

:fail
echo [ERROR] Cannot open Visual Studio. Read the messages above.
pause
exit /b 1
