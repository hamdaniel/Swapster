@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%install_swapster.ps1"
set "LOG_FILE=%TEMP%\swapster_install.log"

if not exist "%PS_SCRIPT%" (
    echo ERROR: Missing installer script: "%PS_SCRIPT%"
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*

set "EXITCODE=%ERRORLEVEL%"

if not "%EXITCODE%"=="0" (
    echo Installer failed with exit code %EXITCODE%.
    echo Check log: "%LOG_FILE%"
    echo.
    pause
)

exit /b %EXITCODE%