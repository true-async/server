@echo off
rem Deps tree as build_task.bat prepares it, minus build_h3_deps.bat (HTTP/3 is
rem not built). Run inside phpsdk-vs18-x64.bat -t. Usage: deps.bat <deps dir> <vcpkg root>
set DEPS_DIR=%~1
set VCPKG=%~2\installed\x64-windows

cmd /c phpsdk_deps --update --no-backup --branch 8.6 --stability staging --deps %DEPS_DIR% --crt vs18
if %errorlevel% neq 0 exit /b 3

if not exist "%DEPS_DIR%\include\libuv"  mkdir "%DEPS_DIR%\include\libuv"
if not exist "%DEPS_DIR%\include\nghttp2" mkdir "%DEPS_DIR%\include\nghttp2"

copy /y "%VCPKG%\include\uv.h" "%DEPS_DIR%\include\libuv\uv.h"
xcopy /E /I /H /Y "%VCPKG%\include\uv" "%DEPS_DIR%\include\libuv\uv\"
copy /y "%VCPKG%\lib\uv.lib" "%DEPS_DIR%\lib\libuv.lib"
copy /y "%VCPKG%\bin\uv.dll" "%DEPS_DIR%\bin\uv.dll"

xcopy /E /I /H /Y "%VCPKG%\include\nghttp2" "%DEPS_DIR%\include\nghttp2\"
copy /y "%VCPKG%\lib\nghttp2.lib" "%DEPS_DIR%\lib\nghttp2.lib"
copy /y "%VCPKG%\bin\nghttp2.dll" "%DEPS_DIR%\bin\nghttp2.dll"
exit /b 0
