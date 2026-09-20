# Clean single-attempt serial login to Orange Pi (orangepi/orangepi).
$ErrorActionPreference = 'Continue'

$port = New-Object System.IO.Ports.SerialPort('COM4', 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$port.ReadTimeout  = 200
$port.WriteTimeout = 1000
$port.DtrEnable = $true
$port.RtsEnable = $true
try { $port.Open() } catch { Write-Output "ERROR_OPEN: $_"; exit 1 }
Start-Sleep -Milliseconds 600

$script:buf = New-Object System.Text.StringBuilder

function Drain([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while((Get-Date) -lt $end) {
        try { $d = $port.ReadExisting(); if($d) { [void]$script:buf.Append($d); Write-Host -NoNewline $d } } catch {}
        Start-Sleep -Milliseconds 50
    }
}
function WaitFor([string]$pat, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while((Get-Date) -lt $end) {
        try { $d = $port.ReadExisting(); if($d) { [void]$script:buf.Append($d); Write-Host -NoNewline $d } } catch {}
        if($script:buf.ToString() -match $pat) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return $false
}
function Send([string]$s) { $port.Write("$s`n") }

Write-Host "`n=== settling to login prompt ==="
$settled = $false
for($i=0; $i -lt 10; $i++) {
    Send ''
    if(WaitFor 'login:' 6000) { $settled = $true; break }
}
if(-not $settled) { Write-Host "`nNO LOGIN PROMPT FOUND"; $port.Close(); exit 2 }
Write-Host "`n=== login prompt found ==="

[void]$script:buf.Clear()
Write-Host "`n>>> sending username"
Send 'orangepi'
if(-not (WaitFor 'Password:' 10000)) { Write-Host "`nNO PASSWORD PROMPT"; $port.Close(); exit 3 }
Write-Host "`n>>> password prompt found, sending password"
[void]$script:buf.Clear()
Send 'orangepi'
Drain 4000

Write-Host "`n>>> verifying shell"
Send 'echo __SHELL_OK_$((1+1))__'
if(-not (WaitFor '__SHELL_OK_2__' 6000)) {
    Write-Host "`n=== LOGIN FAILED ==="
    $port.Close(); exit 4
}
Write-Host "`n=== LOGIN SUCCESS (orangepi) ==="

$cmds = @(
    'echo ====WHOAMI====; id',
    'echo ====HOST====; hostname',
    'echo ====OS====; cat /etc/os-release',
    'echo ====UNAME====; uname -a',
    'echo ====UPTIME====; uptime',
    'echo ====IP====; ip -4 addr show',
    'echo ====ROUTE====; ip route',
    'echo ====NETDEV====; ls /sys/class/net',
    'echo ====SSH====; systemctl is-active ssh sshd 2>/dev/null; ss -tlnp 2>/dev/null | grep -E "22|LISTEN" | head -n 20',
    'echo ====CPU====; grep -E "model name|Hardware|Revision" /proc/cpuinfo | head -n 8',
    'echo ====MEM====; free -h',
    'echo ====DISK====; df -h',
    'echo ====BLK====; lsblk',
    'echo ====DONE===='
)
foreach($cmd in $cmds) { Send $cmd; Drain 1000 }
Send 'exit'
Drain 1500
$port.Close()
Write-Host "`n=== session closed ==="
