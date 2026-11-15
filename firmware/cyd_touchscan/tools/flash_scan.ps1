# =============================================================================
#  flash_scan.ps1 - flash the touch-bus scanner and capture its 30 s scan log.
#
#  Start this script, put the board in download mode, then KEEP TAPPING THE
#  SCREEN for the whole scan window (blue LED = scanning).
# =============================================================================
$ErrorActionPreference = 'Continue'
$proj = Split-Path -Parent $PSScriptRoot
Set-Location $proj
$baud = '115200'
$envName = 'st7789'

function Get-Tail($text, $n) {
  ($text -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -Last $n) -join "`n"
}

$b = ".pio\build\$envName"
$bootApp0 = Join-Path $env:USERPROFILE `
  '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'

foreach ($f in 'bootloader.bin', 'partitions.bin', 'firmware.bin') {
  if (-not (Test-Path (Join-Path $b $f))) { Write-Output "MISSING $b\$f"; exit 1 }
}
if (-not (Test-Path $bootApp0)) { Write-Output "MISSING boot_app0.bin"; exit 1 }

Write-Output "== waiting for download mode (hold BOOT, reconnect USB) =="
$connected = $false
for ($i = 1; $i -le 60; $i++) {
  $out = (& esptool --port COM3 --before no-reset --after no-reset flash-id 2>&1 | Out-String)
  if ($out -match 'Connected to ESP32') { $connected = $true; Write-Output "CONNECTED on poll $i"; break }
  if ($i % 5 -eq 0) { Write-Output "  ...still waiting (poll $i/60)" }
  Start-Sleep -Seconds 2
}
if (-not $connected) { Write-Output "TIMEOUT"; exit 1 }

Start-Sleep -Seconds 2
Write-Output "== flashing '$envName' =="
$fl = (& esptool --port COM3 --baud $baud --before no-reset --after hard-reset `
        write-flash 0x1000 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" `
        0xe000 $bootApp0 0x10000 "$b\firmware.bin" 2>&1 | Out-String)
Write-Output (Get-Tail $fl 6)

Write-Output "== SCAN WINDOW: TAP THE SCREEN NOW (60 s capture) =="
Start-Sleep -Milliseconds 500
try {
  $sp = New-Object System.IO.Ports.SerialPort COM3, 115200, 'None', 8, 'One'
  $sp.ReadTimeout = 400
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.Open()
  Start-Sleep -Milliseconds 200
  # restart cleanly so the 30 s window starts with the capture
  $sp.RtsEnable = $true; Start-Sleep -Milliseconds 150; $sp.RtsEnable = $false

  $sb = New-Object System.Text.StringBuilder
  $until = (Get-Date).AddSeconds(60)
  while ((Get-Date) -lt $until) {
    try { $c = $sp.ReadExisting(); if ($c) { [void]$sb.Append($c) } } catch {}
    Start-Sleep -Milliseconds 50
  }
  $sp.Close(); $sp.Dispose()
  Write-Output '----- SCANLOG_BEGIN -----'
  Write-Output ($sb.ToString() -replace "`r", '')
  Write-Output '----- SCANLOG_END -----'
} catch { Write-Output ("serial: " + $_.Exception.Message) }

Write-Output "ALL DONE"
