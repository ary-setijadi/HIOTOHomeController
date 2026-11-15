# =============================================================================
#  flash_probe.ps1 - back up the stock CYD firmware, then flash the probe.
#
#  The CYD cannot auto-reset, so this script does NOT reset the chip. Start it
#  first, then hold BOOT + tap RESET. The script polls for the bootloader,
#  takes a verified 4 MB backup, flashes, and dumps the probe's serial report.
#
#  Usage:  pwsh -File tools\flash_probe.ps1 [-Env st7789|ili9341]
# =============================================================================
param(
  [ValidateSet('st7789', 'ili9341')]
  [string]$Env = 'st7789'
)

$ErrorActionPreference = 'Continue'
$proj = Split-Path -Parent $PSScriptRoot
Set-Location $proj

$backup = Join-Path $proj 'backups\cyd_stock_backup.bin'
New-Item -ItemType Directory -Force -Path (Split-Path $backup) | Out-Null
$baud = '115200'          # 460800 drops packets on this CH340 link

function Get-Tail($text, $n) {
  ($text -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -Last $n) -join "`n"
}

# ---------------------------------------------------------------- 1. wait for bootloader
Write-Output "== waiting for download mode (hold BOOT, tap RESET) =="
$connected = $false
for ($i = 1; $i -le 60; $i++) {
  $out = (& esptool --port COM3 --before no-reset --after no-reset flash-id 2>&1 | Out-String)
  if ($out -match 'Connected to ESP32') {
    $connected = $true
    Write-Output "CONNECTED on poll $i"
    Write-Output (Get-Tail $out 8)
    break
  }
  if ($i % 5 -eq 0) { Write-Output "  ...still waiting (poll $i/60)" }
  Start-Sleep -Seconds 2
}
if (-not $connected) {
  Write-Output "TIMEOUT: chip never entered download mode. Nothing was changed."
  exit 1
}

# ---------------------------------------------------------------- 2. backup
if ((Test-Path $backup) -and ((Get-Item $backup).Length -eq 4194304)) {
  Write-Output "== verified backup already present - skipping dump =="
  Write-Output "   $backup"
} else {
  Write-Output "== backing up stock flash (4 MB @ $baud, ~6 min) =="
  $bk = (& esptool --port COM3 --baud $baud --before no-reset --after no-reset `
          read-flash 0 0x400000 $backup 2>&1 | Out-String)
  Write-Output (Get-Tail $bk 4)

  $len = if (Test-Path $backup) { (Get-Item $backup).Length } else { 0 }
  Write-Output "backup bytes: $len / 4194304"
  if ($len -ne 4194304) {
    Write-Output "BACKUP-INCOMPLETE: refusing to flash without a restorable image."
    exit 1
  }
  Write-Output "BACKUP OK -> $backup"
}

# ---------------------------------------------------------------- 3. flash
$b = ".pio\build\$Env"
# boot_app0.bin ships with the Arduino framework, not in the build directory.
$bootApp0 = Join-Path $env:USERPROFILE `
  '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
foreach ($f in 'bootloader.bin', 'partitions.bin', 'firmware.bin') {
  if (-not (Test-Path (Join-Path $b $f))) {
    Write-Output "MISSING $b\$f - run: pio run -e $Env"
    exit 1
  }
}
if (-not (Test-Path $bootApp0)) {
  Write-Output "MISSING boot_app0.bin at $bootApp0"
  exit 1
}

Start-Sleep -Seconds 2      # let the polling esptool release the COM handle

Write-Output "== flashing env '$Env' @ $baud =="
$fl = (& esptool --port COM3 --baud $baud --before no-reset --after hard-reset `
        write-flash 0x1000 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" `
        0xe000 $bootApp0 0x10000 "$b\firmware.bin" 2>&1 | Out-String)
Write-Output (Get-Tail $fl 10)
if ($fl -notmatch 'Hash of data verified') {
  Write-Output "FLASH-MAYBE-FAILED: verify manually with the monitor."
} else {
  Write-Output "FLASH OK"
}

# ---------------------------------------------------------------- 4. capture the report
Write-Output "== reading probe output =="
Start-Sleep -Milliseconds 600
try {
  $sp = New-Object System.IO.Ports.SerialPort COM3, 115200, 'None', 8, 'One'
  $sp.ReadTimeout = 500
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.Open()
  Start-Sleep -Milliseconds 200

  # The one-time report prints during setup(). RTS is wired to EN, so pulse a
  # reset to re-run setup() and capture the report from the first line.
  $sp.RtsEnable = $true
  Start-Sleep -Milliseconds 150
  $sp.RtsEnable = $false

  $sb = New-Object System.Text.StringBuilder
  $until = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $until) {
    try { $c = $sp.ReadExisting(); if ($c) { [void]$sb.Append($c) } } catch {}
    Start-Sleep -Milliseconds 80
  }
  $sp.Close(); $sp.Dispose()
  Write-Output '----- PROBE_SERIAL_BEGIN -----'
  Write-Output ($sb.ToString() -replace "`r", '')
  Write-Output '----- PROBE_SERIAL_END -----'
} catch {
  Write-Output ("serial capture failed: " + $_.Exception.Message)
}

Write-Output "ALL DONE"
