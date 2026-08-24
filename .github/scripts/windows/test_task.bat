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

REM A build that produced no extension still runs the suite to the end: every
REM test skips on the missing module, run-tests.php exits 0, and the job passes
REM having compiled and checked nothing. Ask php.exe what it loaded before
REM trusting anything the run says (#271).
%PHP_BUILD_DIR%\php.exe -n -m | findstr /i /c:"true_async_server" >nul
if errorlevel 1 (
    echo ERROR: php.exe has no true_async_server module -- the build produced none
    echo Modules php.exe does have:
    %PHP_BUILD_DIR%\php.exe -n -m
    exit /b 1
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
    ext\http_server\tests\phpt > phpt-run.log 2>&1
set SUITE_RC=%errorlevel%

type phpt-run.log

REM A suite that skipped everything is not a pass, and run-tests.php reports one
REM the same way it reports a clean run. The second guard catches what the
REM module check cannot: a suite gated off for some other reason.
%PHP_BUILD_DIR%\php.exe -n %~dp0assert_executed.php phpt-run.log
if errorlevel 1 exit /b 1

exit /b %SUITE_RC%
