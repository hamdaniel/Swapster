@echo off
setlocal EnableExtensions

REM ===== Self-elevate =====
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

REM ===== Enable unlock events (Event ID 4801) =====
auditpol /set /subcategory:"Other Logon/Logoff Events" /success:enable >nul

REM ===== CONFIG =====
set "APP_SRC=D:\SwapsterInstallation\swapster.exe"
set "INSTALL_DIR=%ProgramData%\Swapster"
set "APP_DST=%INSTALL_DIR%\swapster.exe"
set "TASK_LOGON=Swapster"
set "TASK_UNLOCK=Swapster_Unlock"
set "ARG1=2003"
REM ==================

REM Verify source
if not exist "%APP_SRC%" (
  echo ERROR: App not found: %APP_SRC%
  pause
  exit /b 1
)

REM Copy EXE
mkdir "%INSTALL_DIR%" 2>nul
copy /y "%APP_SRC%" "%APP_DST%" >nul || (echo Copy failed & pause & exit /b 1)

REM Remove old service (optional)
sc query swapster >nul 2>&1
if not errorlevel 1 (
  net stop swapster >nul 2>&1
  sc delete swapster >nul 2>&1
)

REM Remove existing tasks
schtasks /query /tn "%TASK_LOGON%" >nul 2>&1 && schtasks /delete /tn "%TASK_LOGON%" /f >nul
schtasks /query /tn "%TASK_UNLOCK%" >nul 2>&1 && schtasks /delete /tn "%TASK_UNLOCK%" /f >nul

REM ===== Task 1: Run at logon =====
schtasks /create ^
  /tn "%TASK_LOGON%" ^
  /tr "\"%APP_DST%\" %ARG1%" ^
  /sc ONLOGON ^
  /rl HIGHEST ^
  /f || (echo Failed to create logon task & pause & exit /b 1)

REM ===== Task 2: Run on unlock (Event ID 4801) =====
schtasks /create ^
  /tn "%TASK_UNLOCK%" ^
  /tr "\"%APP_DST%\" %ARG1%" ^
  /sc ONEVENT ^
  /ec Security ^
  /mo "*[System[(EventID=4801)]]" ^
  /rl HIGHEST ^
  /f || (echo Failed to create unlock task & pause & exit /b 1)

REM Start now
schtasks /run /tn "%TASK_LOGON%" >nul 2>&1

echo Installed. Starts at logon and on unlock.
pause
endlocal