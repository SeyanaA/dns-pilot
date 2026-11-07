@echo off
setlocal
cd /d "%~dp0"
if not exist dnspilot.exe (
  echo dnspilot.exe not found. Run build.cmd first.
  exit /b 1
)
dnspilot.exe %*
