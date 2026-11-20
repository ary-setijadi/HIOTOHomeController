# =============================================================================
#  provision_new_unit.ps1
#
#  One-pass bring-up for a NEW 2.8" CYD:
#    1. flash the genmon_35_st7796 firmware (manual download mode required)
#    2. capture the boot log (chip, panel, generated GUID)
#    3. provision over the serial console: wifi -> floor -> room -> save
#
#  The Wi-Fi password is taken as a parameter and is never written to disk;
#  the firmware only logs the SSID, so the captured log stays clean.
#
#  Usage:
#    powershell -File tools\provision_new_unit.ps1 -Pass "the-wifi-password"
#    powershell -File tools\provision_new_unit.ps1 -SkipWifi      # provision offline
# =============================================================================
param(
  [string]$Port = '',
  [string]$Ssid = 'routerkantor',
  [string]$Pass = '',
  [string]$Floor = 'Lantai 1',
  [string]$Room  = 'Kitchen',
  [switch]$SkipWifi,
  [switch]$SkipFlash,
  [int]$WifiWaitSec = 18,
  [string]$LogPath = 'provision_log.txt'
)

$ErrorActionPreference = 'Continue'
$proj = Split-Path -Parent $PSScriptRoot
Set-Location $proj
$baud = '115200'
$envName = 'genmon_35_st7796'
$log = New-Object System.Text.StringBuilder

function Say($t) { Write-Output $t; [void]$log.AppendLine($t) }

# ------------------------------------------------------- 0. pick the port ---
if (-not $Port) {
  $cands = @()
  foreach ($n in [System.IO.Ports.SerialPort]::GetPortNames()) {
    $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match [regex]::Escape($n) -and $_.Name -match 'CH34|CP21|FT23|USB-SERIAL' }
    if ($dev) { $cands += $n }
  }
  if ($cands.Count -ge 1) { $Port = $cands[0] } else { $Port = 'COM3' }
}
Say "port: $Port"

# ---------------------------------------------------------------- 1. flash ---
if (-not $SkipFlash) {
  $b = ".pio\build\$envName"
  $bootApp0 = Join-Path $env:USERPROFILE `
    '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
  foreach ($f in 'bootloader.bin', 'partitions.bin', 'firmware.bin') {
    if (-not (Test-Path (Join-Path $b $f))) { Say "MISSING $b\$f - build first"; exit 1 }
  }
  if (-not (Test-Path $bootApp0)) { Say "MISSING boot_app0.bin"; exit 1 }

  Say "== waiting for download mode (hold BOOT, reconnect USB, release after ~2 s) =="
  $connected = $false
  for ($i = 1; $i -le 60; $i++) {
    $out = (& esptool --port $Port --before no-reset --after no-reset flash-id 2>&1 | Out-String)
    if ($out -match 'Connected to ESP32') {
      $connected = $true
      Say "CONNECTED on poll $i"
      ($out -split "`r?`n" | Where-Object { $_ -match 'Chip type|MAC|Detected flash' }) |
        ForEach-Object { Say "  $_" }
      break
    }
    if ($i % 5 -eq 0) { Say "  ...still waiting (poll $i/60)" }
    Start-Sleep -Seconds 2
  }
  if (-not $connected) { Say "TIMEOUT - nothing flashed."; exit 1 }

  Start-Sleep -Seconds 2
  Say "== flashing $envName =="
  $fl = (& esptool --port $Port --baud $baud --before no-reset --after hard-reset `
          write-flash 0x1000 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" `
          0xe000 $bootApp0 0x10000 "$b\firmware.bin" 2>&1 | Out-String)
  ($fl -split "`r?`n" | Where-Object { $_ -match 'Wrote|Hash|error' }) | ForEach-Object { Say "  $_" }
  if ($fl -notmatch 'Hash of data verified') { Say "FLASH-UNCERTAIN" }
}

# ------------------------------------------------------------- 2. provision ---
Say "== opening $Port for provisioning =="
Start-Sleep -Milliseconds 800

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 300
$sp.DtrEnable = $false
$sp.RtsEnable = $false

function Pump([int]$ms) {
  $sb = New-Object System.Text.StringBuilder
  $until = (Get-Date).AddMilliseconds($ms)
  while ((Get-Date) -lt $until) {
    try { $c = $sp.ReadExisting(); if ($c) { [void]$sb.Append($c) } } catch {}
    Start-Sleep -Milliseconds 40
  }
  return $sb.ToString()
}

function Send($cmd, [int]$waitMs) {
  Say ">> $cmd"
  $sp.Write("$cmd`n")
  $out = Pump $waitMs
  foreach ($line in (($out -replace "`r", '') -split "`n")) {
    if ($line.Trim() -ne '') { Say "   $line" }
  }
}

try {
  $sp.Open()
} catch {
  Say ("cannot open ${Port}: " + $_.Exception.Message)
  exit 1
}

Say "-- boot log --"
foreach ($line in ((Pump 6000) -replace "`r", '' -split "`n")) {
  if ($line.Trim() -ne '') { Say "   $line" }
}

Send "id" 1200

if (-not $SkipWifi -and $Pass -ne '') {
  Send "wifi $Ssid $Pass" 1500
  Say "-- waiting ${WifiWaitSec}s for association + controller catalog --"
  foreach ($line in ((Pump ($WifiWaitSec * 1000)) -replace "`r", '' -split "`n")) {
    if ($line.Trim() -ne '') { Say "   $line" }
  }
} elseif (-not $SkipWifi) {
  Say "!! no -Pass supplied: skipping wifi (floor/room ids will stay 0)"
}

Send "floor $Floor" 1500
Send "room $Room" 1500
Send "status" 2000
Send "save" 6000

Say "-- after reboot --"
foreach ($line in ((Pump 8000) -replace "`r", '' -split "`n")) {
  if ($line.Trim() -ne '') { Say "   $line" }
}

if ($sp.IsOpen) { $sp.Close() }
$sp.Dispose()

$log.ToString() | Set-Content -Path (Join-Path $proj $LogPath) -Encoding UTF8
Say "ALL DONE (log: $LogPath)"


