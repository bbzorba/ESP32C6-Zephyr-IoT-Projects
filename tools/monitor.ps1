param(
  [string]$ComPort,
  [int]$Baud = 115200,
  [int]$DurationSec = 0
)

$ErrorActionPreference = 'Stop'

function Extract-ComPort($friendlyName) {
  if ($friendlyName -match '\((COM\d+)\)') { return $Matches[1] }
  return $null
}

function Find-CdcComPort {
  # --- Source 1: Get-PnpDevice (catches USB-serial bridges that miss Win32_SerialPort) ---
  $pnp = Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue |
         Where-Object { $_.Status -eq 'OK' } |
         Sort-Object FriendlyName

  # Priority 1: Known USB-UART bridge VIDs and name patterns (CH340/CP210/FTDI/PL23xx/etc.)
  $bridgeVidPattern  = 'VID_1A86|VID_10C4|VID_0403|VID_067B|VID_04D8|VID_16C0'
  $bridgeNamePattern = 'CH340|CH341|CP210|FTDI|USB-?SERIAL|PL2303|MCP2200|Arduino|Prolific'
  foreach ($p in $pnp) {
    if ($p.InstanceId -match $bridgeVidPattern -or $p.FriendlyName -match $bridgeNamePattern) {
      $com = Extract-ComPort $p.FriendlyName
      if ($com) { return $com }
    }
  }

  # Priority 2: Generic CDC/ACM ports that are not ST-LINK (VID 0483)
  foreach ($p in $pnp) {
    if ($p.FriendlyName -match 'USB Serial Device|CDC|ACM' -and
        $p.FriendlyName -notmatch 'ST[- ]?LINK' -and
        $p.InstanceId   -notmatch 'VID_0483') {
      $com = Extract-ComPort $p.FriendlyName
      if ($com) { return $com }
    }
  }

  # --- Source 2: Win32_SerialPort fallback (same priority order) ---
  $wmi = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | Sort-Object DeviceID
  foreach ($p in $wmi) {
    if ($p.Name -match $bridgeNamePattern -or $p.PNPDeviceID -match $bridgeVidPattern) {
      return $p.DeviceID
    }
  }
  foreach ($p in $wmi) {
    if ($p.Name -match 'USB Serial Device|CDC|ACM' -and
        $p.Name         -notmatch 'ST[- ]?LINK' -and
        $p.PNPDeviceID  -notmatch 'VID_0483') {
      return $p.DeviceID
    }
  }

  return $null
}

function Find-AnyComPort {
  # Try PnP first, then WMI, ignoring status so we always get something
  $pnp = Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue |
         Where-Object { $_.Status -eq 'OK' } |
         Sort-Object FriendlyName
  foreach ($p in $pnp) {
    $com = Extract-ComPort $p.FriendlyName
    if ($com) { return $com }
  }
  $wmi = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | Sort-Object DeviceID
  if ($wmi) { return $wmi[0].DeviceID }
  return $null
}

if (-not $ComPort) { $ComPort = Find-CdcComPort }
if (-not $ComPort) {
  if ($DurationSec -gt 0) {
    Write-Error 'No USB-UART bridge COM port found (CH340/CP210/FTDI/CDC). Connect the bridge or pass -ComPort COMx.'
  }

  $fallbackPort = Find-AnyComPort
  if ($fallbackPort) {
    $ComPort = $fallbackPort
    Write-Warning "No USB-UART bridge detected. Falling back to $ComPort."
  } else {
    Write-Error 'No serial COM port found. Connect a device or pass -ComPort COMx.'
  }
}

if ($DurationSec -gt 0) {
  Write-Host "Opening $ComPort @ $Baud for $DurationSec second(s)..."
} else {
  Write-Host "Opening $ComPort @ $Baud... (Ctrl+C to exit)"
}
$port = New-Object System.IO.Ports.SerialPort $ComPort,$Baud,'None',8,'One'
$port.Handshake = 'None'
$port.DtrEnable = $true
$port.RtsEnable = $true
$port.NewLine = "`r`n"
$port.ReadTimeout = 50
$port.WriteTimeout = 500
$port.Open()

$receivedBytes = 0
$deadline = if ($DurationSec -gt 0) { (Get-Date).AddSeconds($DurationSec) } else { $null }

try {
  while ($true) {
    if ($deadline -and (Get-Date) -ge $deadline) { break }
    if ($port.BytesToRead -gt 0) {
      $data = $port.ReadExisting()
      if ($null -ne $data) {
        $receivedBytes += $data.Length
        Write-Host -NoNewline $data
      }
    }
    Start-Sleep -Milliseconds 10
  }
}
finally {
  if ($port.IsOpen) { $port.Close() }
  Write-Host "`nClosed $ComPort"
}

if ($DurationSec -gt 0) {
  if ($receivedBytes -gt 0) {
    Write-Host "Verification Passed: received $receivedBytes bytes from UART."
    exit 0
  }
  Write-Error "Verification Failed: no UART data received in $DurationSec second(s)."
}