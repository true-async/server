@echo off

if /i "%GITHUB_ACTIONS%" neq "True" (
    echo for CI only
    exit /b 3
)

call %~dp0find-target-branch.bat
set STABILITY=staging
set DEPS_DIR=%PHP_BUILD_CACHE_BASE_DIR%\deps-%BRANCH%-%PHP_SDK_VS%-%PHP_SDK_ARCH%
echo Updating dependencies in %DEPS_DIR%
cmd /c phpsdk_deps --update --no-backup --branch %BRANCH% --stability %STABILITY% --deps %DEPS_DIR% --crt %PHP_BUILD_CRT%
if %errorlevel% neq 0 exit /b 3

if not exist "%DEPS_DIR%" (
    cmd /c phpsdk_deps --update --force --no-backup --branch %BRANCH% --stability %STABILITY% --deps %DEPS_DIR%
)
if %errorlevel% neq 0 exit /b 3

rem ----------------------------------------------------------------
rem Copy libuv from vcpkg into the SDK deps tree (same pattern
rem ext/async uses). nghttp2 too — http_server needs it for H2.
rem ----------------------------------------------------------------
if not exist "%DEPS_DIR%\include\libuv"  mkdir "%DEPS_DIR%\include\libuv"
if not exist "%DEPS_DIR%\include\nghttp2" mkdir "%DEPS_DIR%\include\nghttp2"
if not exist "%DEPS_DIR%\lib" mkdir "%DEPS_DIR%\lib"
if not exist "%DEPS_DIR%\bin" mkdir "%DEPS_DIR%\bin"

copy "C:\vcpkg\installed\x64-windows\include\uv.h" "%DEPS_DIR%\include\libuv\uv.h"
xcopy /E /I /H /Y "C:\vcpkg\installed\x64-windows\include\uv" "%DEPS_DIR%\include\libuv\uv\"
copy "C:\vcpkg\installed\x64-windows\lib\uv.lib" "%DEPS_DIR%\lib\libuv.lib"
copy "C:\vcpkg\installed\x64-windows\bin\uv.dll" "%DEPS_DIR%\bin\uv.dll"

xcopy /E /I /H /Y "C:\vcpkg\installed\x64-windows\include\nghttp2" "%DEPS_DIR%\include\nghttp2\"
copy "C:\vcpkg\installed\x64-windows\lib\nghttp2.lib" "%DEPS_DIR%\lib\nghttp2.lib"
copy "C:\vcpkg\installed\x64-windows\bin\nghttp2.dll" "%DEPS_DIR%\bin\nghttp2.dll"

cmd /c buildconf.bat --force
if %errorlevel% neq 0 exit /b 3

if "%THREAD_SAFE%" equ "0" set ADD_CONF=%ADD_CONF% --disable-zts
if "%INTRINSICS%" neq "" set ADD_CONF=%ADD_CONF% --enable-native-intrinsics=%INTRINSICS%
if "%ASAN%" equ "1" set ADD_CONF=%ADD_CONF% --enable-sanitizer --enable-debug-pack

rem Same /WX waiver set as ext/async (PHP-headers warnings).
set CFLAGS=/W3 /WX /wd4018 /wd4146 /wd4244 /wd4267

cmd /c configure.bat ^
    --enable-snapshot-build ^
    --disable-debug-pack ^
    --without-analyzer ^
    --enable-object-out-dir=%PHP_BUILD_OBJ_DIR% ^
    --with-php-build=%DEPS_DIR% ^
    --enable-async ^
    --enable-true-async-server ^
    --enable-http2 ^
    --disable-http3 ^
    %ADD_CONF% ^
    --disable-test-ini
if %errorlevel% neq 0 exit /b 3

nmake /NOLOGO
if %errorlevel% neq 0 exit /b 3

exit /b 0
