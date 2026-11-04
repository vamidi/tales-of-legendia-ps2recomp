@echo off
REM ============================================================
REM  run_game_agent.bat — Agent-friendly runner for the ToL port
REM  (Tales of Legendia USA / SLUS_212.01, PS2Recomp port)
REM
REM  Usage:
REM   run_game_agent.bat                 (15 sec timeout, default log)
REM   run_game_agent.bat 30              (30 sec timeout)
REM   run_game_agent.bat 10 mylog        (10 sec, custom log name)
REM
REM  Extra arguments after the log name are forwarded to tol.exe,
REM  e.g. run_game_agent.bat 20 probe --iso="data\Tales of Legendia (USA).iso"
REM
REM  NOTE: the working directory must be the repo root, because the
REM  runtime derives cdRoot from the ELF's directory: data\CD.
REM    build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
REM ============================================================

set "ROOT=%~dp0.."
pushd "%ROOT%"

set "RUNNER=%ROOT%\build\tol2\src\runtime\tol.exe"
set "ELF=%ROOT%\data\CD\SLUS_212.01"
set "LOGDIR=%ROOT%\logs"
set "TIMEOUT_SEC=%~1"
set "LOGNAME=%~2"

if "%TIMEOUT_SEC%"=="" set "TIMEOUT_SEC=15"
if "%LOGNAME%"=="" set "LOGNAME=agent_run_latest"

set "LOGFILE=%LOGDIR%\%LOGNAME%.log"
set "ERRFILE=%LOGDIR%\%LOGNAME%.err.log"

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

if not exist "%RUNNER%" (
    echo ERROR: runner not found at %RUNNER%
    echo        build it with: cmd /c "call vcvars64.bat ^&^& ninja -C build/tol2 -j 12 tol"
    popd
    exit /b 1
)
if not exist "%ELF%" (
    echo ERROR: ELF not found at %ELF%
    popd
    exit /b 1
)

if exist "%LOGFILE%" del "%LOGFILE%"
if exist "%ERRFILE%" del "%ERRFILE%"

echo [agent_runner] runner: %RUNNER%
echo [agent_runner] elf   : %ELF%
echo [agent_runner] timeout: %TIMEOUT_SEC%s  log: %LOGFILE%

REM The game's own diagnostics (Deci2Call/kputs, [dvci], [fileio]) go to stderr.
start /B "" "%RUNNER%" "%ELF%" --gs=sdlgpu %3 %4 %5 %6 %7 %8 %9 > "%LOGFILE%" 2> "%ERRFILE%"

timeout /t %TIMEOUT_SEC% /nobreak > nul 2>&1
taskkill /F /IM tol.exe > nul 2>&1
timeout /t 1 /nobreak > nul 2>&1

echo.
echo ============================================================
echo [agent_runner] DONE
for %%A in ("%LOGFILE%") do echo   stdout: %%~zA bytes
for %%A in ("%ERRFILE%") do echo   stderr: %%~zA bytes
echo ============================================================

popd
exit /b 0
