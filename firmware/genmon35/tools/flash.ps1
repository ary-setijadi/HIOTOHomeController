# HIOTO General Monitor — flash helper (manual download mode)
# Usage: powershell -ExecutionPolicy Bypass -File tools\flash.ps1 [-Port COM3]
param([string]$Port = "COM3")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$et = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$fw = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32"
$build = Join-Path $root ".pio\build\hioto_genmon"

Write-Host "=== HIOTO General Monitor flash helper ==="
Write-Host "1) Hold BOOT, tap RESET (or unplug + hold BOOT + replug), keep ~1 s."
Write-Host "2) Waiting for the bootloader on $Port (Ctrl+C to abort)..."
Write-Host ""

$found = $false
for ($i = 0; $i -lt 120; $i++) {
  $out = & python $et --port $Port --connect-attempts 1 --before no_reset --after no_reset flash_id 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0 -and $out -match "Detected flash size") { $found = $true; break }
  Start-Sleep -Milliseconds 800
}
if (-not $found) {
  Write-Host "Bootloader not found. Redo the BOOT+RESET step and run again."
  exit 1
}
Write-Host "Bootloader found. Flashing..."

& python $et --chip esp32 --port $Port --baud 921600 --before no_reset --after hard_reset `
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB `
  "0x1000" "$build\bootloader.bin" `
  "0x8000" "$build\partitions.bin" `
  "0xe000" "$fw\tools\partitions\boot_app0.bin" `
  "0x10000" "$build\firmware.bin"
if ($LASTEXITCODE -ne 0) { Write-Host "Flash FAILED."; exit 1 }
Write-Host ""
Write-Host "Done. Serial console: python -m platformio device monitor -p $Port -b 115200"
