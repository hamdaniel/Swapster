@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%install_swapster.ps1"
set "LOG_FILE=%TEMP%\swapster_install.log"

echo.
echo === Swapster installer BAT started ===
echo BAT dir: "%SCRIPT_DIR%"
echo PS script: "%PS_SCRIPT%"
echo Log file: "%LOG_FILE%"
echo.

if not exist "%PS_SCRIPT%" (
    echo ERROR: Missing installer script:
    echo "%PS_SCRIPT%"
    echo.
    pause
    exit /b 1
)

echo Starting PowerShell installer...
echo If this window stays here, PowerShell is still running or waiting.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*

set "EXITCODE=%ERRORLEVEL%"

echo.
echo PowerShell returned.
echo Exit code: %EXITCODE%
echo.

if exist "%LOG_FILE%" (
    echo Last log lines:
    echo ------------------------------------------------------------
    powershell.exe -NoProfile -Command "Get-Content -LiteralPath '%LOG_FILE%' -Tail 40"
    echo ------------------------------------------------------------
    echo.
) else (
    echo No log file found.
    echo.
)

if not "%EXITCODE%"=="0" (
    echo Installer failed with exit code %EXITCODE%.
    echo.
    pause
)

exit /b %EXITCODE%