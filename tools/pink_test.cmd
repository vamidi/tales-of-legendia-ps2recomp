@echo off
rem ===========================================================================
rem  pink_test.cmd -- run scripts\pink_test.ps1 with the execution policy lifted.
rem
rem  Why a wrapper: a .ps1 cannot bypass its OWN execution policy. The block
rem  happens before the file is ever read, so this fails instantly:
rem
rem      .\scripts\pink_test.ps1 -Seconds 45
rem      -> "cannot be loaded because running scripts is disabled on this system"
rem
rem  ...and because the script never runs, the PREVIOUS logs stay on disk and look
rem  like fresh results. Always go through this wrapper:
rem
rem      scripts\pink_test.cmd                  (defaults to -Seconds 55)
rem      scripts\pink_test.cmd -Seconds 45
rem
rem  From cmd.exe you can also just type:  pink_test -Seconds 45
rem  (cmd looks up .CMD via PATHEXT; it never looks for .ps1)
rem
rem  All arguments are forwarded verbatim.
rem ===========================================================================
setlocal

set "PS1=%~dp0pink_test.ps1"
if not exist "%PS1%" (
    echo [pink_test] ERROR: "%PS1%" not found.
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
set "RC=%ERRORLEVEL%"

echo.
echo [pink_test] exit code %RC%
if "%RC%"=="0" goto :ok
echo [pink_test] NON-ZERO exit - the runner or capture failed; check logs\pink_test.err.log

:ok
rem keep the console open when launched by double-click
if not defined NO_PAUSE echo %cmdcmdline% | find /i "%~nx0" >nul && pause
endlocal & exit /b %RC%
