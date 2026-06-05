@echo off

rem Pin the PHP SDK dependency series to 8.5 (vs17).
rem
rem We build php-src's dev tip (true-async == 8.6-dev) on the windows-2022
rem runner, i.e. the vs17 / VS 2022 toolchain. php.net's deps server only
rem publishes the bleeding edge (branches "8.6" and "master") for the vs18 /
rem VS 2026 toolchain -- there is no vs17 build of those. The newest series
rem that ships vs17 dependencies is 8.5, and those libs (openssl, libxml2,
rem ...) build the 8.6 tip fine: the series tracks the toolchain, not the PHP
rem minor. Asking for "8.6"/"master" under --crt vs17 fails with
rem "CRT 'vs17' doesn't match any available for branch ...".
rem
rem Bump this to 8.6 once php.net publishes packages-8.6-vs17-*, or switch the
rem runner+PHP_BUILD_CRT to vs18 to follow the dev-tip deps directly.
rem (Old logic derived "8.<minor>" and remapped a hardcoded 8.5 -> master;
rem it broke when php-src went 8.6 and the dev deps moved to vs18.)
set BRANCH=8.5
