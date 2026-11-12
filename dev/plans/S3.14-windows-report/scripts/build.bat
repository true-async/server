@echo off
rem One Release ZTS build with the configure line of build_task.bat, without
rem --enable-http3. Run inside phpsdk-vs18-x64.bat -t.
rem Usage: build.bat <php-src tree> <obj dir> <deps dir>
cd /d %~1
set DEPS_DIR=%~3
if not exist "%~2" mkdir "%~2"

cmd /c buildconf.bat --force
if %errorlevel% neq 0 exit /b 3

set CFLAGS=/W3 /WX /wd4018 /wd4146 /wd4244 /wd4267

@echo on
cmd /c configure.bat ^
    --disable-debug-pack ^
    --without-analyzer ^
    --enable-object-out-dir=%~2 ^
    --with-php-build=%DEPS_DIR% ^
    --enable-async ^
    --enable-true-async-server ^
    --enable-http2 ^
    --enable-http-server-test-hooks ^
    --enable-tas-test-hooks ^
    --enable-sockets ^
    --with-openssl=yes ^
    --enable-native-intrinsics=AVX2 ^
    --disable-test-ini
if %errorlevel% neq 0 exit /b 3
@echo off

rem configure.js writes the localized OS caption into config.w32.h in the ANSI
rem code page; cl reads it as UTF-8 and C4828 under /WX stops the build.
pwsh -NoProfile -File %~dp0ascii-build-system.ps1 %~1\main\config.w32.h
if %errorlevel% neq 0 exit /b 6

nmake /NOLOGO
if %errorlevel% neq 0 exit /b 3

copy /y "%DEPS_DIR%\bin\*.dll" "%~2\Release_TS\" >nul
exit /b 0
