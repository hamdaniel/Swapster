param(
    [int]$Port = 2003
)

$ErrorActionPreference = 'Stop'

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin)) {
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-Port', $Port
    )
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList ($args -join ' ')
    exit 0
}

$taskName = 'Swapster_Server_OnStartup'
$installDir = Join-Path $env:ProgramData 'Swapster'
$scriptDir = Join-Path $installDir 'install'
$scriptDst = Join-Path $scriptDir 'install_swapster.ps1'
$exeSrc = Join-Path $PSScriptRoot 'swapster.exe'
$exeDst = Join-Path $installDir 'swapster.exe'

if (-not (Test-Path -LiteralPath $exeSrc)) {
    throw "swapster.exe was not found next to the installer script: $exeSrc"
}

New-Item -ItemType Directory -Path $installDir -Force | Out-Null
New-Item -ItemType Directory -Path $scriptDir -Force | Out-Null

Copy-Item -LiteralPath $PSCommandPath -Destination $scriptDst -Force
Copy-Item -LiteralPath $exeSrc -Destination $exeDst -Force

# Replace task if it already exists.
$null = schtasks /Delete /TN $taskName /F 2>$null
$taskCommand = '"{0}" {1}' -f $exeDst, $Port
schtasks /Create /TN $taskName /TR $taskCommand /SC ONSTART /RU SYSTEM /RL HIGHEST /F | Out-Null

# Replace firewall rules so reruns remain idempotent.
$null = netsh advfirewall firewall delete rule name="Swapster Server" 2>$null
$null = netsh advfirewall firewall delete rule name="Swapster Discovery" 2>$null

netsh advfirewall firewall add rule name="Swapster Server" dir=in action=allow protocol=TCP localport=$Port profile=any program="$exeDst" | Out-Null
netsh advfirewall firewall add rule name="Swapster Discovery" dir=in action=allow protocol=UDP localport=$Port profile=any program="$exeDst" | Out-Null

Start-Process -FilePath $exeDst -ArgumentList $Port -WindowStyle Hidden
Write-Output "Swapster install complete. Startup task, firewall rules, and ProgramData copy are configured."
