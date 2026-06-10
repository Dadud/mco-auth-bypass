@echo off
REM Build authlogin.dll on Windows.
REM Requires MinGW-w64 (gcc) in PATH. Get it from one of:
REM   - WinLibs (https://winlibs.com/)  - easiest, just extract a ZIP
REM   - MSYS2   (https://www.msys2.org/) then: pacman -S mingw-w64-i686-gcc
REM   - choco install mingw

setlocal

echo Building authlogin.dll...

where gcc >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo GCC not found in PATH. Install MinGW-w64 first.
    echo Easiest: download WinLibs from https://winlibs.com/ and put its bin\ on PATH.
    echo.
    exit /b 1
)

gcc -shared -o authlogin.dll authlogin.c -static -Wl,--subsystem,windows
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED. See errors above.
    exit /b 1
)

if not exist authlogin.dll (
    echo Build reported success but authlogin.dll was not created.
    exit /b 1
)

echo.
echo Build successful!
dir authlogin.dll

endlocal
