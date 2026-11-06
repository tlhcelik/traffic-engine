@echo off
rem ===========================================================================
rem  deploy_Release_x64.bat - Makes the release package. Double-click to run.
rem   1) Compiles the Release version (build_Release_x64.bat).
rem   2) Copies the real files (not the links) from bin\Release to
rem      ..\deploy\traffic_engine_<version>_x64.
rem   3) Runs windeployqt on the package folder.
rem   4) Copies the MSVC runtime DLLs.
rem   5) Puts the PDB file in a separate symbols folder (for crash dumps).
rem   6) Makes a 7z archive and, if Inno Setup is available, a setup file.
rem  Argument "nopause": the script does not wait for a key at the end.
rem ===========================================================================
setlocal
cd /d "%~dp0"

call "%~dp0build_Release_x64.bat" nopause || goto :fail
call "%~dp0set_env.bat" || goto :fail

set /p TE_VERSION=<"%~dp0Release\te_version.txt"
set "DEPLOY=%~dp0..\deploy"
set "PKG=%DEPLOY%\traffic_engine_%TE_VERSION%_x64"
set "SYM=%DEPLOY%\traffic_engine_%TE_VERSION%_x64_symbols"

echo [*] Package folder: %PKG%
if exist "%PKG%" rmdir /s /q "%PKG%"
if exist "%SYM%" rmdir /s /q "%SYM%"
mkdir "%PKG%" "%SYM%"

rem robocopy copies the targets of the links (real files). Exit codes below 8 are good.
robocopy "%~dp0..\bin\Release" "%PKG%" /E /XF *.pdb /NFL /NDL /NJH /NJS /NP
if errorlevel 8 goto :fail
copy /Y "%~dp0..\bin\Release\traffic_engine.pdb" "%SYM%\" >nul
rem Documents for the users (external API)
robocopy "%~dp0..\docs" "%PKG%\docs" /E /NFL /NDL /NJH /NJS /NP
if errorlevel 8 goto :fail

echo [*] windeployqt (package folder)...
"%TE_QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-angle --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime "%PKG%\traffic_engine.exe" || goto :fail

rem No parenthesis blocks below: a ")" in a path, for example "(x86)", stops a block.
echo [*] MSVC runtime DLLs...
if not defined TE_VC_CRT goto :no_crt
copy /Y "%TE_VC_CRT%\*.dll" "%PKG%\" >nul
echo     Source: "%TE_VC_CRT%"
goto :crt_done
:no_crt
echo [WARNING] The MSVC runtime folder is not known. Install vc_redist.x64.exe on the target computer.
:crt_done

echo [*] 7z archive...
if exist "%DEPLOY%\traffic_engine_%TE_VERSION%_x64.7z" del /q "%DEPLOY%\traffic_engine_%TE_VERSION%_x64.7z"
"%TE_7Z%" a -t7z -mx=9 "%DEPLOY%\traffic_engine_%TE_VERSION%_x64.7z" "%PKG%\*" >nul || goto :fail
echo     "%DEPLOY%\traffic_engine_%TE_VERSION%_x64.7z"

if not defined TE_ISCC goto :no_iscc
if not exist "%TE_ISCC%" goto :no_iscc
echo [*] Setup file, Inno Setup...
"%TE_ISCC%" /Q "/DAppVersion=%TE_VERSION%" "/DSourceDir=%PKG%" "%~dp0..\installer\traffic_engine.iss" || goto :fail
echo     "%~dp0..\installer\Output"
goto :iscc_done
:no_iscc
echo [WARNING] Inno Setup is not available. The script does not make a setup file.
:iscc_done

echo.
echo [OK] The release package is ready: %PKG%
start "" "%DEPLOY%"
if /i not "%~1"=="nopause" pause
exit /b 0

:fail
echo.
echo [ERROR] The deploy failed. Read the messages above.
if /i not "%~1"=="nopause" pause
exit /b 1
