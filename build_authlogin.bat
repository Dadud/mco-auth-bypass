@echo off
REM Build script for authlogin.dll (v2.1.0+)
REM Requires MinGW-w64 (gcc) to be installed and in PATH.
REM
REM IMPORTANT: You need MinGW-W64, not the old MinGW from SourceForge.
REM The old MinGW lacks <winhttp.h> and <winsock2.h>. MinGW-w64 has them.
REM Easiest install: MSYS2 (https://www.msys2.org/) then:
REM   pacman -S mingw-w64-i686-gcc
REM and add C:\msys64\mingw32\bin to your PATH.
REM
REM Optional flags you can set on the command line or in a build wrapper:
REM   ENABLE_INPROC_FALLBACK=1   Build with the in-proc NPS responder
REM   LOGIN_ALLOW_INSECURE=1     Use http:// for the login endpoint (dev only)
REM   LOGIN_FALLBACK_OFFLINE=1   Fall back to MCO1 ticket on login network error
REM
REM Examples:
REM   build_authlogin.bat
REM   set LOGIN_ALLOW_INSECURE=1 && set LOGIN_FALLBACK_OFFLINE=1 && build_authlogin.bat
REM   set ENABLE_INPROC_FALLBACK=1 && build_authlogin.bat

setlocal

echo Building authlogin.dll...

where gcc >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo GCC not found in PATH.
    echo Install MinGW-w64 (easiest: MSYS2 + pacman -S mingw-w64-i686-gcc)
    echo MSVC also works:
    echo   cl /LD authlogin.c /Fe:authlogin.dll ws2_32.lib winhttp.lib
    echo.
    exit /b 1
)

echo Found GCC, compiling...

REM Build the optional flag list.
set "CFLAGS="
if defined ENABLE_INPROC_FALLBACK set "CFLAGS=%CFLAGS% -DENABLE_INPROC_FALLBACK"
if defined LOGIN_ALLOW_INSECURE   set "CFLAGS=%CFLAGS% -DLOGIN_ALLOW_INSECURE"
if defined LOGIN_FALLBACK_OFFLINE set "CFLAGS=%CFLAGS% -DLOGIN_FALLBACK_OFFLINE"

if defined CFLAGS (
    echo Optional flags: %CFLAGS%
)

REM -lws2_32   for inet_addr / WSAConnect (NPS redirect)
REM -lwinhttp  for WinHTTP (modernized login, v2.1.0+)
gcc -shared -o authlogin.dll authlogin.c -static -Wl,--subsystem,windows %CFLAGS% -lws2_32 -lwinhttp >build.log 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED. See build.log for the full output.
    echo.
    REM Detect the most common cause and print a targeted hint.
    findstr /C:"winhttp.h" /C:"winsock2.h" /C:"No such file" build.log >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        echo Looks like you're missing Windows headers.
        echo That's the OLD MinGW from SourceForge, not MinGW-W64.
        echo MinGW-w64 ships winsock2.h + winhttp.h; old MinGW does not.
        echo.
        echo Fix: install MSYS2 (https://www.msys2.org/), then in its terminal:
        echo   pacman -S mingw-w64-i686-gcc
        echo and put C:\msys64\mingw32\bin on your PATH.
    )
    findstr /C:"undefined reference" build.log >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        echo.
        echo Linker error. If it mentions inet_addr, WSAConnect, WinHttpSendRequest,
        echo or similar Win32 API functions, check that the gcc command line
        echo includes -lws2_32 and -lwinhttp.
    )
    type build.log
    del build.log
    exit /b 1
)
del build.log

if not exist authlogin.dll (
    echo.
    echo Build reported success but authlogin.dll was not created.
    echo Check the warnings/errors above.
    exit /b 1
)

echo.
echo Build successful!
dir authlogin.dll

endlocal
