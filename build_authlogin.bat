@echo off
REM Build script for authlogin.dll (v2.1.0+)
REM Requires MinGW (gcc) to be installed and in PATH.
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
    echo Install MinGW: https://sourceforge.net/projects/mingw/
    echo Or use MSVC:
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
gcc -shared -o authlogin.dll authlogin.c -static -Wl,--subsystem,windows %CFLAGS% -lws2_32 -lwinhttp
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED. See errors above.
    exit /b 1
)

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
