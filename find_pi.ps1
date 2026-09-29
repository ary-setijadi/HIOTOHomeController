# Discover the Orange Pi on the ICS subnet 192.168.137.0/24 (PS 5.1 compatible).
$ErrorActionPreference = 'Continue'
$subnet = '192.168.137'
$ips = 2..254 | ForEach-Object { "$subnet.$_" }
Write-Output ("=== sweeping {0}.2-254 ===" -f $subnet)
$job = Test-Connection -ComputerName $ips -Count 1 -AsJob -ErrorAction SilentlyContinue
$null = Wait-Job $job -Timeout 70
$res = Receive-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue
$alive = $res | Where-Object { $_.ResponseTime -ne $null }
Write-Output "=== responding hosts ==="
if($alive){ $alive | Select-Object Address, ResponseTime | Format-Table -AutoSize | Out-String } else { Write-Output "(none)" }
Write-Output "=== ARP (192.168.137.x interface) ==="
arp.exe -a | Select-String '192\.168\.137\.' | Out-String
Write-Output "=== done ==="
