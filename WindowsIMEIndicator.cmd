@echo off
setlocal
title IME Steady - Windows input indicator
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\WindowsIMEIndicator.ps1" -Action Menu
set "helperExitCode=%ERRORLEVEL%"
if not "%helperExitCode%"=="0" (
    echo.
    echo The Windows input indicator helper could not start. See the error above.
    pause
)
exit /b %helperExitCode%
