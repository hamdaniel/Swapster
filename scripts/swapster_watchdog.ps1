param(
  [int]$Port = 2003
)

$ErrorActionPreference = "SilentlyContinue"

$InstallDir = Join-Path $env:ProgramData "Swapster"
$ExePath = Join-Path $InstallDir "swapster.exe"
$StateFile = Join-Path $InstallDir "watchdog_state.txt"
$LogFile = Join-Path $InstallDir "swapster_log.txt"
$LockFile = Join-Path $InstallDir "watchdog.lock"

$Magic = "swapsterswapster"
$MagicReply = "SwapsterServerOK"
$UdpReq = "SWAPSTER_DISCOVER"
$UdpReply = "SWAPSTER_HERE"

$FailureThreshold = 3

function Write-Log([string]$msg) {
  $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
  Add-Content -Path $LogFile -Value "$ts [watchdog] $msg"
}

function Read-Failures {
  if (Test-Path $StateFile) {
    $v = (Get-Content $StateFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($v -match '^\d+$') { return [int]$v }
  }
  return 0
}

function Write-Failures([int]$n) {
  Set-Content -Path $StateFile -Value $n -Encoding Ascii
}

function Test-TcpMagic {
  $client = $null
  try {
    $client = New-Object System.Net.Sockets.TcpClient
    $iar = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
    if (-not $iar.AsyncWaitHandle.WaitOne(1200)) { return $false }
    $client.EndConnect($iar)

    $stream = $client.GetStream()
    $stream.ReadTimeout = 1200
    $stream.WriteTimeout = 1200

    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Magic)
    $stream.Write($bytes, 0, $bytes.Length)

    $buf = New-Object byte[] 64
    $n = $stream.Read($buf, 0, $buf.Length)
    if ($n -le 0) { return $false }

    $resp = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
    return ($resp -eq $MagicReply)
  } catch {
    return $false
  } finally {
    if ($client) { $client.Close() }
  }
}

function Test-UdpDiscovery {
  $udp = $null
  try {
    $udp = New-Object System.Net.Sockets.UdpClient
    $udp.Client.ReceiveTimeout = 1200

    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse("127.0.0.1"), $Port)
    $msg = [System.Text.Encoding]::ASCII.GetBytes($UdpReq)
    [void]$udp.Send($msg, $msg.Length, $ep)

    $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $respBytes = $udp.Receive([ref]$remote)
    $resp = [System.Text.Encoding]::ASCII.GetString($respBytes)
    return ($resp -eq $UdpReply)
  } catch {
    return $false
  } finally {
    if ($udp) { $udp.Close() }
  }
}

if (-not (Test-Path $InstallDir)) {
  exit 1
}

if (Test-Path $LockFile) {
  $lockAge = (Get-Date) - (Get-Item $LockFile).LastWriteTime
  if ($lockAge.TotalSeconds -lt 55) {
    exit 0
  }
}

Set-Content -Path $LockFile -Value (Get-Date).ToString("o") -Encoding Ascii

try {
  if (-not (Test-Path $ExePath)) {
    Write-Log "Executable not found: $ExePath"
    exit 1
  }

  $tcpOk = Test-TcpMagic
  $udpOk = Test-UdpDiscovery

  if ($tcpOk -or $udpOk) {
    if ((Read-Failures) -ne 0) {
      Write-Failures 0
      Write-Log "Health restored"
    }
    exit 0
  }

  $fails = (Read-Failures) + 1
  Write-Failures $fails
  Write-Log "Health check failed ($fails/$FailureThreshold)"

  if ($fails -lt $FailureThreshold) {
    exit 0
  }

  Write-Log "Restarting swapster.exe"
  Get-Process swapster -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 600
  Start-Process -FilePath $ExePath -ArgumentList "$Port" -WindowStyle Hidden
  Start-Sleep -Seconds 2

  if ((Test-TcpMagic) -or (Test-UdpDiscovery)) {
    Write-Failures 0
    Write-Log "Restart successful"
    exit 0
  }

  Write-Failures $FailureThreshold
  Write-Log "Restart attempted but server still unhealthy"
  exit 1
} finally {
  Remove-Item $LockFile -ErrorAction SilentlyContinue
}
