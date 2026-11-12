@echo off
rem Build nghttp3 and ngtcp2 (with ngtcp2_crypto_ossl) as static Release /MD
rem libraries against the OpenSSL of the PHP SDK deps tree, and copy them into
rem that tree for config.w32. Run inside the SDK shell, so CMake uses the same
rem toolset as PHP. Usage: build_h3_deps.bat <deps dir>
rem
rem Versions follow build-linux.yml. Nothing is cached: the deps' OpenSSL
rem moves with --stability staging, and ngtcp2_crypto_ossl is built against it.

setlocal
set DEPS=%~1
if "%DEPS%" == "" (
    echo usage: build_h3_deps.bat ^<deps dir^>
    exit /b 3
)

set NGHTTP3_VERSION=1.15.0
set NGTCP2_VERSION=1.22.1
set WORK=C:\h3-build
set PREFIX=%WORK%\install

if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%"

set CMAKE_COMMON=-G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_INSTALL_PREFIX=%PREFIX% ^
    -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON -DENABLE_LIB_ONLY=ON ^
    -DBUILD_TESTING=OFF

git clone --depth=1 --branch=v%NGHTTP3_VERSION% --recurse-submodules ^
    https://github.com/ngtcp2/nghttp3.git "%WORK%\nghttp3"
if %errorlevel% neq 0 exit /b 3
cmake -S "%WORK%\nghttp3" -B "%WORK%\nghttp3\build" %CMAKE_COMMON%
if %errorlevel% neq 0 exit /b 3
cmake --build "%WORK%\nghttp3\build" --target install
if %errorlevel% neq 0 exit /b 3

git clone --depth=1 --branch=v%NGTCP2_VERSION% --recurse-submodules ^
    https://github.com/ngtcp2/ngtcp2.git "%WORK%\ngtcp2"
if %errorlevel% neq 0 exit /b 3
cmake -S "%WORK%\ngtcp2" -B "%WORK%\ngtcp2\build" %CMAKE_COMMON% ^
    -DENABLE_OPENSSL=ON -DOPENSSL_ROOT_DIR=%DEPS% -DOPENSSL_USE_STATIC_LIBS=OFF
if %errorlevel% neq 0 exit /b 3
cmake --build "%WORK%\ngtcp2\build" --target install
if %errorlevel% neq 0 exit /b 3

rem An import library names its DLL in its members; a static archive lists
rem only object files. config.w32 links these names with NGTCP2_STATICLIB and
rem NGHTTP3_STATICLIB, so an import library here would leave php.exe needing a
rem DLL nobody ships.
for %%L in (nghttp3 ngtcp2 ngtcp2_crypto_ossl) do (
    if not exist "%PREFIX%\lib\%%L.lib" (
        echo %%L.lib was not built
        exit /b 3
    )
    lib /nologo /list "%PREFIX%\lib\%%L.lib" | findstr /i /c:".dll" >nul
    if not errorlevel 1 (
        echo %%L.lib is an import library, not a static one
        exit /b 3
    )
    copy /y "%PREFIX%\lib\%%L.lib" "%DEPS%\lib\%%L.lib" >nul
)

xcopy /E /I /H /Y "%PREFIX%\include\nghttp3" "%DEPS%\include\nghttp3\" >nul
xcopy /E /I /H /Y "%PREFIX%\include\ngtcp2" "%DEPS%\include\ngtcp2\" >nul
if %errorlevel% neq 0 exit /b 3

echo nghttp3 %NGHTTP3_VERSION% and ngtcp2 %NGTCP2_VERSION% installed into %DEPS%
exit /b 0
