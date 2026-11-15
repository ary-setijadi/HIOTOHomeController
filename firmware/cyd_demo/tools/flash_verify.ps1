# =============================================================================
#  flash_verify.ps1 - flash the demo UI and capture touch telemetry.
#  Start it, hold BOOT + reconnect, then press/drag on the screen for ~20 s.
# =============================================================================
$ErrorActionPreference = 'Continue'
$proj = Split-Path -Parent $PSScriptRoot
$demo = Join-Path (Split-Path $proj) 'cyd_demo'
Set-Location $demo
$baud = '115200'
$envName = 'st7789'

$b = ".pio\build\$envName"
$bootApp0 = Join-Path $env:USERPROFILE `
  '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'

foreach ($f in 'bootloader.bin', 'partitions.bin', 'firmware.bin') {
  if (-not (Test-Path (Join-Path $b $f))) { Write-Output "MISSING $b\$f"; exit 1 }
}
if (-not (Test-Path $bootApp0)) { Write-Output "MISSING boot_app0.bin"; exit 1 }

Write-Output "== waiting for download mode =="
$connected = $false
for ($i = 1; $i -le 60; $i++) {
  $out = (& esptool --port COM3 --before no-reset --after no-reset flash-id 2>&1 | Out-String)
  if ($out -match 'Connected to ESP32') { $connected = $true; Write-Output "CONNECTED on poll $i"; break }
  if ($i % 5 -eq 0) { Write-Output "  ...waiting (poll $i/60)" }
  Start-Sleep -Seconds 2
}
if (-not $connected) { Write-Output "TIMEOUT"; exit 1 }

Start-Sleep -Seconds 2
Write-Output "== flashing demo UI =="
$fl = (& esptool --port COM3 --baud $baud --before no-reset --after hard-reset `
        write-flash 0x1000 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" `
        0xe000 $bootApp0 0x10000 "$b\firmware.bin" 2>&1 | Out-String)
($fl -split "`r?`n" | Where-Object { $_ -match 'Hash|Wrote' }) | Out-String | Write-Output

Write-Output "== capture: PRESS/DRAG the screen for ~20 s (45 s window) =="
Start-Sleep -Milliseconds 500
try {
  $sp = New-Object System.IO.Ports.SerialPort COM3, 115200, 'None', 8, 'One'
  $sp.ReadTimeout = 400
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.Open()
  Start-Sleep -Milliseconds 200
  $sp.RtsEnable = $true; Start-Sleep -Milliseconds 150; $sp.RtsEnable = $false

  $sb = New-Object System.Text.StringBuilder
  $until = (Get-Date).AddSeconds(45)
  while ((Get-Date) -lt $until) {
    try { $c = $sp.ReadExisting(); if ($c) { [void]$sb.Append($c) } } catch {}
    Start-Sleep -Milliseconds 50
  }
  $sp.Close(); $sp.Dispose()
  Write-Output '----- VERIFY_BEGIN -----'
  Write-Output ($sb.ToString() -replace "`r", '')
  Write-Output '----- VERIFY_END -----'
} catch { Write-Output ("serial: " + $_.Exception.Message) }

Write-Output "ALL DONE"
