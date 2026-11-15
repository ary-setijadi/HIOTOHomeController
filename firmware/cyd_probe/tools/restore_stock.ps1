# =============================================================================
#  restore_stock.ps1 - write the saved 4 MB image back to the CYD.
#
#  Use this to return the board to exactly what it was before the probe run
#  (i.e. the HIOTO General Monitor hioto-genmon-28 build, or whatever the
#  factory image was).
#
#  The board cannot auto-reset. Start this script, then hold BOOT + tap RESET
#  (or hold BOOT while reconnecting USB).
#
#  Usage:  powershell -File tools\restore_stock.ps1
# =============================================================================
param(
  [string]$Backup
)

$ErrorActionPreference = 'Continue'
$proj = Split-Path -Parent $PSScriptRoot
if (-not $Backup) { $Backup = Join-Path $proj 'backups\cyd_stock_backup.bin' }
$baud = '115200'

if (-not (Test-Path $Backup)) {
  Write-Output "No backup at $Backup - nothing to restore."
  exit 1
}
$len = (Get-Item $Backup).Length
Write-Output "backup: $Backup  ($len bytes)"
if ($len -ne 4194304) {
  Write-Output "REFUSING: expected a full 4 MB image, got $len bytes."
  exit 1
}

Write-Output "== waiting for download mode (hold BOOT, tap RESET) =="
$connected = $false
for ($i = 1; $i -le 60; $i++) {
  $out = (& esptool --port COM3 --before no-reset --after no-reset flash-id 2>&1 | Out-String)
  if ($out -match 'Connected to ESP32') { $connected = $true; Write-Output "CONNECTED on poll $i"; break }
  if ($i % 5 -eq 0) { Write-Output "  ...still waiting (poll $i/60)" }
  Start-Sleep -Seconds 2
}
if (-not $connected) {
  Write-Output "TIMEOUT: chip never entered download mode. Nothing was changed."
  exit 1
}

Write-Output "== restoring full 4 MB image @ $baud (~6 min) =="
Start-Sleep -Seconds 2
$o = (& esptool --port COM3 --baud $baud --before no-reset --after hard-reset `
       write-flash 0x0 $Backup 2>&1 | Out-String)
Write-Output (($o -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -Last 12) -join "`n")

if ($o -match 'Hash of data verified') {
  Write-Output "RESTORE OK - board rebooting into the original firmware."
} else {
  Write-Output "RESTORE-UNCERTAIN: re-run and watch for 'Hash of data verified'."
}
