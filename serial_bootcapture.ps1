# Capture the Orange Pi serial boot log (115200 8N1) for a fixed duration.
# Usage: .\serial_bootcapture.ps1 [-Seconds 120] [-PortName COM4]
param([int]$Seconds = 120, [string]$PortName = '')
$ErrorActionPreference = 'Continue'
$outFile = 'C:\Users\T495\Documents\MEGA\_OrangePiBasic\serial_boot.txt'

$all = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
Write-Output ("COM ports detected: {0}" -f ($all -join ', '))
if($PortName -eq ''){
    if($all.Count -eq 0){ Write-Output "NO COM PORT - is the USB-TTL adapter plugged in?"; exit 2 }
    if($all.Count -eq 1){ $PortName = $all[0] } else { $PortName = $all[0]; Write-Output "multiple ports; using first: $PortName" }
}

$port = New-Object System.IO.Ports.SerialPort($PortName, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$port.ReadTimeout = 200; $port.WriteTimeout = 1000; $port.DtrEnable = $true; $port.RtsEnable = $true
try { $port.Open() } catch { Write-Output "ERROR_OPEN: $_"; exit 1 }
Write-Output ("Opened {0} @ 115200. Capturing for {1} s ..." -f $PortName, $Seconds)

$sb = New-Object System.Text.StringBuilder
$end = (Get-Date).AddSeconds($Seconds)
while((Get-Date) -lt $end){
    try { $d = $port.ReadExisting(); if($d){ [void]$sb.Append($d); Write-Host -NoNewline $d } } catch {}
    Start-Sleep -Milliseconds 40
}
$port.Close()
$sb.ToString() | Out-File $outFile -Encoding utf8
Write-Output "`n=== captured ($($sb.Length) chars) to $outFile ==="
