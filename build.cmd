@echo off
setlocal
cd /d "%~dp0"
set SRC=src\model.cpp src\dns.cpp src\proc.cpp src\stats.cpp src\engine.cpp src\proxy.cpp src\systemdns.cpp src\httpd.cpp src\ui_native.cpp src\process_picker.cpp src\ui_chart.cpp src\browsers.cpp src\main.cpp
echo [1/3] Embedding web assets...
python tools\embed.py
if errorlevel 1 exit /b 1
if not exist build mkdir build
echo [2/3] Compiling resources...
python -m ziglang rc src\app.rc build\app_res.o
if errorlevel 1 exit /b 1
echo [3/3] Compiling dnspilot.exe...
python -m ziglang c++ -std=c++17 -O2 -Wall -Wextra %SRC% build\app_res.o -o dnspilot.exe -lws2_32 -liphlpapi -ladvapi32 -lshell32 -lole32 -loleaut32 -luser32 -lgdi32 -lshlwapi -lversion -ldwmapi -lgdiplus -lcomctl32 -lcomdlg32 -static -Wl,--subsystem,windows -Wl,--entry,mainCRTStartup
if errorlevel 1 exit /b 1
echo.
echo Build OK: %CD%\dnspilot.exe
