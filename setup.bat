@echo off
rem Set up everything needed to build webc on Windows, build the release
rem preset, and run the crawler.
rem
rem   setup.bat               set up, build, run
rem   setup.bat --build-only  set up and build, don't run
rem
rem Needs Visual Studio (or its Build Tools) with the "Desktop development
rem with C++" workload: clang uses its headers and libraries.
setlocal

cd /d "%~dp0"

set RUN=1
if "%~1"=="--build-only" set RUN=0

rem 1. build tools, installed with winget when missing
call :require git Git.Git || exit /b 1
call :require cmake Kitware.CMake || exit /b 1
call :require ninja Ninja-build.Ninja || exit /b 1
call :require clang++ LLVM.LLVM || exit /b 1

rem 2. vcpkg (installs boost, sqlite3, openssl, lexbor from vcpkg.json)
if not defined VCPKG_ROOT (
  set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
  setx VCPKG_ROOT "%USERPROFILE%\vcpkg" >nul
  echo VCPKG_ROOT not set, using %USERPROFILE%\vcpkg ^(saved for new terminals^)
)
if not exist "%VCPKG_ROOT%\" (
  echo cloning vcpkg into %VCPKG_ROOT%...
  git clone https://github.com/microsoft/vcpkg "%VCPKG_ROOT%" || exit /b 1
)
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics || exit /b 1
)

rem 3. configure + build (the first configure builds the dependencies, which
rem    takes a few minutes)
cmake --preset release || exit /b 1
cmake --build --preset release || exit /b 1

rem 4. run
if "%RUN%"=="1" build\release\main.exe
exit /b %errorlevel%

rem :require <command> <winget id>
:require
where %1 >nul 2>&1 && exit /b 0
echo %1 not found, installing %2 with winget...
winget install --id %2 -e --accept-source-agreements --accept-package-agreements || (
  echo could not install %2. install it manually and re-run. 1>&2
  exit /b 1
)
echo installed %2. open a new terminal so PATH picks it up, then re-run setup.bat. 1>&2
exit /b 1
