@echo off
rem ===========================================================================
rem  TaskbarPlant build script
rem  - uses MinGW g++ if available on PATH
rem  - otherwise uses MSVC cl (run inside "x64 Native Tools Command Prompt for VS")
rem ===========================================================================
setlocal
cd /d "%~dp0"

rem compiler installed by me via MSYS2 (not on PATH)
if exist "D:\ziyi\msys64\ucrt64\bin\g++.exe" set "PATH=D:\ziyi\msys64\ucrt64\bin;%PATH%"

where g++ >nul 2>nul
if not errorlevel 1 goto build_mingw

where cl >nul 2>nul
if not errorlevel 1 goto build_msvc

echo [build] ERROR: no C++ compiler found on PATH.
echo [build] Option A: install Visual Studio with "Desktop development with C++",
echo [build]           then run this script inside "x64 Native Tools Command Prompt for VS".
echo [build] Option B: install MinGW-w64 and add its bin folder to PATH.
goto end

:build_mingw
echo [build] using MinGW g++ ...
g++ -std=c++17 -O2 -municode -mwindows TaskbarPlant.cpp -o TaskbarPlant.exe -lgdiplus -lgdi32 -luser32 -ldwmapi -static -s
if errorlevel 1 goto failed
goto success

:build_msvc
echo [build] using MSVC cl ...
cl /nologo /W3 /O2 /EHsc /MT /utf-8 /DUNICODE /D_UNICODE TaskbarPlant.cpp /link gdiplus.lib user32.lib gdi32.lib dwmapi.lib /SUBSYSTEM:WINDOWS /OUT:TaskbarPlant.exe
if errorlevel 1 goto failed
goto success

:success
echo [build] OK -^> TaskbarPlant.exe
goto end

:failed
echo [build] FAILED

:end
pause
