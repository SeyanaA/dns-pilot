@echo off
setlocal
cd /d "%~dp0"

echo [1/2] Building dnspilot.exe...
call build.cmd
if errorlevel 1 exit /b 1

echo [2/2] Building installer with NSIS...
set "NSIS_EXE=%LOCALAPPDATA%\Programs\nsis-3.10\nsis-3.10\makensis.exe"
if exist "%NSIS_EXE%" goto have_nsis
where makensis.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo NSIS not found.
    echo   - Install NSIS 3.x and make sure makensis.exe is on PATH, or
    echo   - set NSIS_EXE to the full path of makensis.exe
    exit /b 1
)
set "NSIS_EXE=makensis.exe"
:have_nsis

if not exist dist mkdir dist
"%NSIS_EXE%" /V2 installer\dnspilot.nsi
if errorlevel 1 exit /b 1

echo.
echo Installer: %CD%\dist\DNS-Pilot-Setup-1.1.3.exe
