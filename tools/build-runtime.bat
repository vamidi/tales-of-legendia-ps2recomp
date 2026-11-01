@echo off
setlocal EnableExtensions

rem ===========================================================================
rem  build-runtime.bat -- incremental Debug build of ps2xRuntime / ps2EntryRunner
rem
rem  Runs the equivalent of:
rem
rem      cmake --build out/build --config Debug
rem
rem  but from the correct directory and with the MSVC x64 environment already
rem  set up, so it works no matter where you launch it from.
rem
rem  USAGE
rem      scripts\build-runtime.bat                         build ps2EntryRunner.exe (Debug)
rem      scripts\build-runtime.bat --target ps2_runtime    just the runtime lib
rem      scripts\build-runtime.bat --target all            everything
rem      scripts\build-runtime.bat --check                 verify the toolchain only
rem
rem  A BARE RUN BUILDS ONLY ps2EntryRunner, not the whole tree, so the machine is
rem  not spent on ps2xTest / ps2xStudio / analyzer / recomp targets. The exe it
rem  produces is printed both in the header and on the last line:
rem
rem      PS2Recomp\out\build\ps2xRuntime\ps2EntryRunner.exe
rem
rem  Everything except a leading --check is forwarded verbatim to `cmake --build`.
rem
rem  WHY THIS EXISTS - the usual bare-cmd.exe failures this avoids:
rem    * cl.exe / ninja / INCLUDE / LIB are not set up. cmd.exe on its own knows
rem      nothing about MSVC; VsDevCmd.bat has to be called first. That is the
rem      most common cause of "No CMAKE_CXX_COMPILER could be found" and of
rem      lib/obj link errors when building from a plain shell.
rem    * `out/build` is a RELATIVE path. It only resolves if the current
rem      directory is PS2Recomp, so running the same command from the repo root
rem      fails with "does not appear to contain CMakeCache.txt".
rem    * quoting paths with spaces, e.g. under "C:\Program Files (x86)", needs
rem      doubled quotes inside cmd /c "..." chains. This script is called
rem      directly, so none of that is needed.
rem
rem  NOTES
rem    * --config Debug is accepted but is a no-op for this tree: out/build is a
rem      single-configuration Ninja build (CMakeCache.txt: CMAKE_BUILD_TYPE).
rem    * NEVER add --clean-first and never delete out/build  (see the build rules).
rem    * The runner is killed first: the linker cannot replace a running image.
rem    * Set NO_PAUSE=1 in the environment to suppress the double-click pause.
rem ===========================================================================

set "CHECK_ONLY="
set "CMAKE_ARGS=%*"
if /i "%~1"=="--check" (
    set "CHECK_ONLY=1"
    set "CMAKE_ARGS=%CMAKE_ARGS:--check=%"
)

rem ---- default target: the runner exe. A bare run must not build the world. ----
rem Do NOT detect this with the "%VAR:--target=%" substring trick: for an
rem undefined/empty VAR that syntax does NOT expand, which silently skips the
rem default. Scanning the arguments is reliable.
set "TARGET_ARG=--target ps2EntryRunner"
call :scan_args %*
goto :after_scan

:scan_args
if "%~1"=="" goto :eof
if /i "%~1"=="--target" set "TARGET_ARG="
shift
goto :scan_args

:after_scan

rem ---- locate the repo root from this script's own location ------------------
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
set "BUILD_ROOT=%REPO_ROOT%\PS2Recomp"
set "OUT_DIR=%BUILD_ROOT%\out\build"
set "RUNNER=%OUT_DIR%\ps2xRuntime\ps2EntryRunner.exe"

if not exist "%OUT_DIR%\CMakeCache.txt" (
    echo [build] ERROR: "%OUT_DIR%\CMakeCache.txt" not found.
    echo [build]        Configure the tree once first:
    echo [build]          cd /d "%BUILD_ROOT%"
    echo [build]          cmake -S . -B out/build
    exit /b 10
)

rem ---- MSVC x64 environment --------------------------------------------------
if not defined VSDEVCMD (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSINSTALL=%%I"
    if defined VSINSTALL if exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
)
if not defined VSDEVCMD (
    for %%P in (
        "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat"
    ) do if not defined VSDEVCMD if exist %%P set "VSDEVCMD=%%~P"
)
if not defined VSDEVCMD (
    echo [build] ERROR: VsDevCmd.bat not found.
    echo [build]        Install the "Desktop development with C++" workload, or set
    echo [build]        VSDEVCMD to its full path before running this script.
    exit /b 11
)

echo [build] VsDevCmd : "%VSDEVCMD%"
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 -no_logo >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: VsDevCmd.bat failed. Verbose output follows.
    call "%VSDEVCMD%" -arch=x64 -host_arch=x64 -no_logo
    exit /b 12
)

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: cl.exe is not on PATH after VsDevCmd - the MSVC C++ tools are missing.
    exit /b 13
)

if defined CHECK_ONLY (
    echo [build] repo     : "%REPO_ROOT%"
    echo [build] build dir: "%OUT_DIR%"
    echo [build] cl       : & where cl.exe
    echo [build] ninja    : & where ninja.exe
    echo [build] cmake    : & where cmake.exe
    cmake --version
    echo [build] --check OK
    exit /b 0
)

pushd "%BUILD_ROOT%"
if errorlevel 1 (
    echo [build] ERROR: cannot enter "%BUILD_ROOT%"
    exit /b 14
)

rem the linker cannot replace a running image
taskkill /F /IM ps2EntryRunner.exe >nul 2>&1

echo [build] target   : %TARGET_ARG% %CMAKE_ARGS%
echo [build] output   : "%RUNNER%"
echo [build] started  : %TIME%
echo [build] command  : cmake --build out/build --config Debug %TARGET_ARG% %CMAKE_ARGS%
echo.
cmake --build out/build --config Debug %TARGET_ARG% %CMAKE_ARGS%
set "RC=%ERRORLEVEL%"

echo.
echo [build] finished : %TIME%
if "%RC%"=="0" (
    if exist "%RUNNER%" for %%F in ("%RUNNER%") do echo [build] OK - OUTPUT "%RUNNER%"  %%~tF  %%~zF bytes
) else (
    echo [build] FAILED - cmake exit code %RC%
)
echo [build] expected output: "%RUNNER%"

popd

rem keep the window open on double-click, but never block a scripted caller
if not defined NO_PAUSE echo %cmdcmdline% | find /i "%~nx0" >nul && pause

endlocal & exit /b %RC%
