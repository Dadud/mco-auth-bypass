@echo off
REM Build script for authlogin.dll
REM Requires MinGW (gcc) to be installed

echo Building authlogin.dll...

REM Try gcc first
where gcc >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo Found GCC, compiling...
    gcc -shared -o authlogin.dll authlogin.c -static -Wl,--subsystem,windows
    if %ERRORLEVEL% EQU 0 (
        echo Build successful!
        dir authlogin.dll
    ) else (
        echo GCC compile failed!
        exit /b 1
    )
) else (
    echo GCC not found in PATH.
    echo.
    echo Please install MinGW to compile this DLL.
    echo Download from: https://sourceforge.net/projects/mingw/
    echo.
    echo Alternatively, you can use MSVC:
    echo   cl /LD authlogin.c /Fe:authlogin.dll
    echo.
    echo Or use an online compiler like https://www.tutorialrepublic.com/
    exit /b 1
)
