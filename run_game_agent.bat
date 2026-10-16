@echo off
REM ============================================================
REM  run_game_agent.bat — Agent-friendly PS2Recomp test runner
REM  (Tales of Legendia USA / SLUS_212.01)
REM
REM  Usage:
REM   run_game_agent.bat           (15 sec timeout, default log)
REM   run_game_agent.bat 30        (30 sec timeout)
REM   run_game_agent.bat 10 mylog  (10 sec, custom log name)
REM ============================================================

set "RUNNER=D:\Personal\C++\tales-of-legendia-ps2recomp\PS2Recomp\out\build\ps2xRuntime\ps2EntryRunner.exe"
set "ELF=D:\Personal\C++\tales-of-legendia-ps2recomp\data\SLUS_212.01"
set "LOGDIR=D:\Personal\C++\tales-of-legendia-ps2recomp\logs"
set "TIMEOUT_SEC=%~1"
set "LOGNAME=%~2"

REM Default timeout 15 seconds
if "%TIMEOUT_SEC%"=="" set "TIMEOUT_SEC=15"

REM Default log name: agent_run_latest
if "%LOGNAME%"=="" set "LOGNAME=agent_run_latest"

set "LOGFILE=%LOGDIR%\%LOGNAME%.log"

REM Create logs dir if needed
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

REM Pre-flight checks
if not exist "%RUNNER%" (
    echo ERROR: Runner not found at %RUNNER%
    echo        Did you build? Run: cmake --build PS2Recomp\out\build
    exit /b 1
)
if not exist "%ELF%" (
    echo ERROR: ELF not found at %ELF%
    exit /b 1
)

REM Clear previous log
if exist "%LOGFILE%" del "%LOGFILE%"

echo [agent_runner] Launching with %TIMEOUT_SEC%s timeout...
echo [agent_runner] Log: %LOGFILE%
echo [agent_runner] Runner: %RUNNER%
echo.

REM Start the game in background, redirect stdout+stderr to log
start /B "" "%RUNNER%" "%ELF%" > "%LOGFILE%" 2>&1

REM Give it a moment to start
timeout /t 1 /nobreak > nul 2>&1

REM Wait for TIMEOUT_SEC seconds
echo [agent_runner] Waiting %TIMEOUT_SEC% seconds...
timeout /t %TIMEOUT_SEC% /nobreak > nul 2>&1

REM Kill the runner
echo [agent_runner] Timeout reached, killing runner...
taskkill /F /IM ps2EntryRunner.exe > nul 2>&1

REM Small delay for file flush
timeout /t 1 /nobreak > nul 2>&1

REM Report results
echo.
echo ============================================================
echo [agent_runner] DONE
echo [agent_runner] Log file: %LOGFILE%
echo [agent_runner] Log size:
for %%A in ("%LOGFILE%") do echo   %%~zA bytes
echo ============================================================

REM Count lines for quick summary
for /f %%C in ('find /c /v "" ^< "%LOGFILE%"') do echo [agent_runner] Log lines: %%C

exit /b 0
