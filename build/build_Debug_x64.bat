@echo off
rem ===========================================================================
rem  build_Debug_x64.bat - Compiles the Debug version. Double-click to run.
rem  Output: ..\bin\Debug\traffic_engine.exe
rem   (symbolic links to the build output + Qt files from windeployqt)
rem  Argument "nopause": the script does not wait for a key at the end.
rem ===========================================================================
setlocal
cd /d "%~dp0"
call "%~dp0set_env.bat" || goto :fail

echo [*] Configure (CMake, %TE_GENERATOR%, x64)...
"%TE_CMAKE%" -S . -B Debug -G "%TE_GENERATOR%" -A x64 || goto :fail

echo [*] Compile (Debug)...
"%TE_CMAKE%" --build Debug --config Debug --parallel || goto :fail

echo.
echo [OK] The Debug build is complete: %~dp0..\bin\Debug\traffic_engine.exe
if /i not "%~1"=="nopause" pause
exit /b 0

:fail
echo.
echo [ERROR] The Debug build failed. Read the messages above.
if /i not "%~1"=="nopause" pause
exit /b 1
