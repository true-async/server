@echo off
setlocal EnableDelayedExpansion

if /i "%GITHUB_ACTIONS%" neq "True" (
    echo for CI only
    exit /b 3
)

echo === http_server phpt suite ===

set PHP_BUILD_DIR=C:\obj\Release_TS

if not exist "%PHP_BUILD_DIR%\php.exe" (
    echo ERROR: %PHP_BUILD_DIR%\php.exe not found
    exit /b 1
)

rem Drop the dep DLLs (libuv, nghttp2, openssl, libxml, ...) next to
rem php.exe so tests find them. Same trick ext/async uses.
call %~dp0find-target-branch.bat
set DEPS_DIR=%PHP_BUILD_CACHE_BASE_DIR%\deps-%BRANCH%-%PHP_SDK_VS%-%PHP_SDK_ARCH%

if exist "%DEPS_DIR%\bin" (
    copy /y "%DEPS_DIR%\bin\*.dll" "%PHP_BUILD_DIR%\" >nul
) else (
    echo WARNING: %DEPS_DIR%\bin missing — DLLs may be unresolved
)

REM protect_memory is process-global page protection toggled outside the compile
REM lock; the threaded worker pool shares one address space and races on it. Off.
%PHP_BUILD_DIR%\php.exe run-tests.php ^
    -d opcache.protect_memory=0 ^
    -P -q -j2 ^
    -g FAIL,BORK,LEAK,XLEAK ^
    --no-progress ^
    --offline ^
    --show-diff ^
    --set-timeout 120 ^
    ext\http_server\tests\phpt

exit /b %errorlevel%
