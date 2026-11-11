@echo off
setlocal
rem Builds build\Sugrob.exe with MinGW-w64 g++ (no Visual Studio needed).
rem If g++ is not on PATH, set MINGW_BIN to the folder that contains g++.exe.
cd /d "%~dps0"
if defined MINGW_BIN set "PATH=%MINGW_BIN%;%PATH%"
where g++ >nul 2>nul || (echo g++ not found. Set MINGW_BIN to the MinGW-w64 bin folder. & exit /b 1)
if not exist build mkdir build

g++ -std=c++17 -O2 -municode -mwindows -static -s -Wall -Wextra ^
  src\main.cpp src\scene.cpp src\sim.cpp src\render.cpp src\panel.cpp ^
  -o build\Sugrob.exe ^
  -ld3d11 -ldxgi -ld2d1 -ldwrite -ldcomp -ldwmapi -lshell32 -luser32 -lgdi32 -lole32 -luuid
if errorlevel 1 exit /b 1
echo OK: build\Sugrob.exe
