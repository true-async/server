@echo off
rem ============================================================
rem  run-tests-windows.bat -- phpt runner for the MSVC build.
rem
rem  Usage (paths relative to the project root):
rem    scripts\run-tests-windows.bat
rem    scripts\run-tests-windows.bat tests\phpt\server\tls
rem    scripts\run-tests-windows.bat tests\phpt\server\tls\063-tls-body.phpt
rem
rem  Override defaults via environment variables:
rem    PHP_SRC     PHP source tree (contains run-tests.php)
rem    PHP_BUILD   build dir with php.exe + extension DLLs
rem    DEPS_BIN    deps\bin with openssl.exe and curl.exe
rem    TEST_INI    ini file that loads the extension
rem ============================================================

setlocal enableextensions

rem ---- project root (parent of this scripts\ folder) ----
set "PROJECT_ROOT=%~dp0.."

rem ---- defaults ----
if "%PHP_SRC%"==""   set "PHP_SRC=E:\php\php-src"
if "%PHP_BUILD%"=="" set "PHP_BUILD=%PHP_SRC%\x64\Debug_TS"
if "%DEPS_BIN%"==""  set "DEPS_BIN=E:\php\deps\bin"
if "%TEST_INI%"==""  set "TEST_INI=%TEMP%\php-http-server-test.ini"

rem ---- generate ini if missing ----
if not exist "%TEST_INI%" (
    > "%TEST_INI%" echo extension_dir=%PHP_BUILD%
    >> "%TEST_INI%" echo extension=php_openssl.dll
    >> "%TEST_INI%" echo extension=php_http_server.dll
    echo Generated %TEST_INI%
)

rem ---- add deps\bin so openssl + curl are on PATH ----
set "PATH=%DEPS_BIN%;%PATH%"

rem ---- run tests ----
if "%~1"=="" (
    "%PHP_BUILD%\php.exe" -n -c "%TEST_INI%" "%PHP_SRC%\run-tests.php" -P -q --offline -c "%TEST_INI%" "%PROJECT_ROOT%\tests"
) else (
    "%PHP_BUILD%\php.exe" -n -c "%TEST_INI%" "%PHP_SRC%\run-tests.php" -P -q --offline -c "%TEST_INI%" %*
)

set RC=%ERRORLEVEL%
echo.
echo === run-tests-windows EXIT %RC% ===
endlocal & exit /b %RC%
