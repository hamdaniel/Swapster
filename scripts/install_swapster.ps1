param(
    [int]$Port = 2003
)

$ErrorActionPreference = 'Stop'
$logFile = Join-Path $env:TEMP 'swapster_install.log'
$PSNativeCommandUseErrorActionPreference = $false

function Write-InstallLog {
    param([string]$Message)

    $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Add-Content -LiteralPath $logFile -Value "[$timestamp] $Message"
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Relaunch as admin if needed.
# Important: do NOT use -Wait here, otherwise the original .bat/.cmd window stays open.
if (-not (Test-IsAdmin)) {
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-Port', $Port
    )

    try {
        Write-InstallLog "Requesting elevation..."
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList ($args -join ' ')
        exit 0
    }
    catch {
        Write-InstallLog "Elevation was cancelled or failed."
        Write-Error "Elevation was cancelled or failed."
        exit 1
    }
}

Write-InstallLog "Installer started elevated (Port=$Port)."

$taskName = 'Swapster_Server_OnStartup'
$installDir = Join-Path $env:ProgramData 'Swapster'
$scriptDir = Join-Path $installDir 'install'
$scriptDst = Join-Path $scriptDir 'install_swapster.ps1'
$exeSrc = Join-Path $PSScriptRoot 'swapster.exe'
$exeDst = Join-Path $installDir 'swapster.exe'
$interactiveUser = [Security.Principal.WindowsIdentity]::GetCurrent().Name

function Remove-SwapsterArtifacts {
    Write-InstallLog "Stopping existing Swapster processes if present..."
    Get-Process -Name 'swapster' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Write-InstallLog "Removing existing scheduled task if present..."
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue | Out-Null

    Write-InstallLog "Removing existing firewall rules if present..."
    cmd /c 'netsh advfirewall firewall delete rule name="Swapster Server" >nul 2>&1' | Out-Null
    cmd /c 'netsh advfirewall firewall delete rule name="Swapster Discovery" >nul 2>&1' | Out-Null

    Write-InstallLog "Removing existing install folder if present..."
    Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue
}

try {
    if (-not (Test-Path -LiteralPath $exeSrc)) {
        throw "swapster.exe was not found next to the installer script: $exeSrc"
    }

    Write-InstallLog "Found source exe: $exeSrc"

    Remove-SwapsterArtifacts

    Write-InstallLog "Creating install directories..."
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    New-Item -ItemType Directory -Path $scriptDir -Force | Out-Null

    Write-InstallLog "Copying installer script to: $scriptDst"
    Copy-Item -LiteralPath $PSCommandPath -Destination $scriptDst -Force

    Write-InstallLog "Copying exe to: $exeDst"
    Copy-Item -LiteralPath $exeSrc -Destination $exeDst -Force

    Write-InstallLog "Creating scheduled task for interactive user session..."

    $taskAction = New-ScheduledTaskAction -Execute $exeDst -Argument "$Port"
    $taskTrigger = New-ScheduledTaskTrigger -AtLogOn -User $interactiveUser
    $taskPrincipal = New-ScheduledTaskPrincipal `
        -UserId $interactiveUser `
        -LogonType Interactive `
        -RunLevel Highest

    $taskSettings = New-ScheduledTaskSettingsSet -StartWhenAvailable

    $task = New-ScheduledTask `
        -Action $taskAction `
        -Trigger $taskTrigger `
        -Principal $taskPrincipal `
        -Settings $taskSettings

    Register-ScheduledTask -TaskName $taskName -InputObject $task -Force | Out-Null

    Write-InstallLog "Scheduled task registered: $taskName (User=$interactiveUser, Trigger=AtLogOn)"

    Write-InstallLog "Adding firewall rules..."

    cmd /c 'netsh advfirewall firewall delete rule name="Swapster Server" >nul 2>&1' | Out-Null
    cmd /c 'netsh advfirewall firewall delete rule name="Swapster Discovery" >nul 2>&1' | Out-Null

    netsh advfirewall firewall add rule `
        name="Swapster Server" `
        dir=in `
        action=allow `
        protocol=TCP `
        localport=$Port `
        profile=any `
        program="$exeDst" | Out-Null

    netsh advfirewall firewall add rule `
        name="Swapster Discovery" `
        dir=in `
        action=allow `
        protocol=UDP `
        localport=$Port `
        profile=any `
        program="$exeDst" | Out-Null

    Write-InstallLog "Firewall rules added."

    Write-InstallLog "Starting Swapster immediately..."
    Start-Process -FilePath $exeDst -ArgumentList "$Port" -WindowStyle Hidden

    Write-InstallLog "Triggering scheduled task once..."
    Start-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue

    Write-InstallLog "Installer completed successfully."
    Write-Output "Swapster install complete. Startup task, firewall rules, and ProgramData copy are configured."
    exit 0
}
catch {
    $errMsg = $_.Exception.Message
    Write-InstallLog "ERROR: $errMsg"
    Write-Error "Swapster install failed: $errMsg"
    Write-Error "See log file: $logFile"
    exit 1
}