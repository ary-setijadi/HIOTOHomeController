# =============================================================================
#  flash_demo.ps1 - flash the CYD findings demo UI.
#
#  The board cannot auto-reset. Start this script, then hold BOOT + reconnect
#  USB (or tap RESET), releasing BOOT after ~2 s.
#
#  Usage:  powershell -File tools\flash_demo.ps1 [-Env st7789|ili9341_broken]
# =============================================================================
param(
  [ValidateSet('st7789', 'ili9341_broken')]
  [string]$Env = 'st7789'
)

$ErrorActionPreference = 'Continue'
$proj = Split-Path -Parent $PSScriptRoot
Set-Location $proj
$baud = '115200'

function Get-Tail($text, $n) {
  ($text -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -Last $n) -join "`n"
}

$b = ".pio\build\$Env"
$bootApp0 = Join-Path $env:USERPROFILE `
  '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'

foreach ($f in 'bootloader.bin', 'partitions.bin', 'firmware.bin') {
  if (-not (Test-Path (Join-Path $b $f))) {
    Write-Output "MISSING $b\$f - run: pio run -e $Env"
    exit 1
  }
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
if (-not $connected) { Write-Output "TIMEOUT: nothing was changed."; exit 1 }

Start-Sleep -Seconds 2
Write-Output "== flashing '$Env' @ $baud =="
$fl = (& esptool --port COM3 --baud $baud --before no-reset --after hard-reset `
        write-flash 0x1000 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" `
        0xe000 $bootApp0 0x10000 "$b\firmware.bin" 2>&1 | Out-String)
Write-Output (Get-Tail $fl 8)
if ($fl -match 'Hash of data verified') { Write-Output "FLASH OK" } else { Write-Output "FLASH-UNCERTAIN" }

Write-Output "== boot check =="
Start-Sleep -Milliseconds 600
try {
  $sp = New-Object System.IO.Ports.SerialPort COM3, 115200, 'None', 8, 'One'
  $sp.ReadTimeout = 400
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.Open()
  Start-Sleep -Milliseconds 200
  $sp.RtsEnable = $true; Start-Sleep -Milliseconds 150; $sp.RtsEnable = $false
  $sb = New-Object System.Text.StringBuilder
  $until = (Get-Date).AddSeconds(8)
  while ((Get-Date) -lt $until) {
    try { $c = $sp.ReadExisting(); if ($c) { [void]$sb.Append($c) } } catch {}
    Start-Sleep -Milliseconds 60
  }
  $sp.Close(); $sp.Dispose()
  Write-Output '----- SERIAL_BEGIN -----'
  Write-Output ($sb.ToString() -replace "`r", '')
  Write-Output '----- SERIAL_END -----'
} catch { Write-Output ("serial: " + $_.Exception.Message) }

Write-Output "ALL DONE"
