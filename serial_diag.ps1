# Log in as root and gather network/board diagnostics.
$ErrorActionPreference = 'Continue'
$port = New-Object System.IO.Ports.SerialPort('COM4', 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$port.ReadTimeout = 200; $port.WriteTimeout = 1000; $port.DtrEnable = $true; $port.RtsEnable = $true
try { $port.Open() } catch { Write-Output "ERROR_OPEN: $_"; exit 1 }
Start-Sleep -Milliseconds 800
$script:buf = New-Object System.Text.StringBuilder
function Drain([int]$ms){ $end=(Get-Date).AddMilliseconds($ms); while((Get-Date) -lt $end){ try{ $d=$port.ReadExisting(); if($d){[void]$script:buf.Append($d); Write-Host -NoNewline $d} }catch{}; Start-Sleep -Milliseconds 50 } }
function WaitFor([string]$pat,[int]$ms){ $end=(Get-Date).AddMilliseconds($ms); while((Get-Date) -lt $end){ try{ $d=$port.ReadExisting(); if($d){[void]$script:buf.Append($d); Write-Host -NoNewline $d} }catch{}; if($script:buf.ToString() -match $pat){return $true}; Start-Sleep -Milliseconds 50 }; return $false }
function Send([string]$s){ $port.Write("$s`n") }

# settle
$settled=$false
for($i=0;$i -lt 8;$i++){ [void]$script:buf.Clear(); Send ''; if(WaitFor 'login:' 5000){$settled=$true;break} }
if(-not $settled){ Write-Host "NO LOGIN PROMPT"; $port.Close(); exit 2 }
[void]$script:buf.Clear(); Send 'root'
if(-not (WaitFor 'Password:' 8000)){ Write-Host "NO PASSWORD PROMPT"; $port.Close(); exit 3 }
[void]$script:buf.Clear(); Send '123456Aa!'
Drain 3500
Send 'echo __SHELL_OK_$((1+1))__'
if(-not (WaitFor '__SHELL_OK_2__' 6000)){ Write-Host "LOGIN FAILED"; $port.Close(); exit 4 }
Write-Host "`n=== LOGGED IN ==="

$cmds = @(
    'echo ====BOARD====; cat /proc/device-tree/model 2>/dev/null; echo; cat /etc/armbian-release 2>/dev/null',
    'echo ====LINKS====; ip link show',
    'echo ====ETH_STATS====; ip -s link show eth0',
    'echo ====NM====; nmcli -t device status 2>&1; echo --; nmcli -t connection show 2>&1',
    'echo ====INTERFACES====; cat /etc/network/interfaces 2>/dev/null; ls /etc/network/interfaces.d/ 2>/dev/null; cat /etc/network/interfaces.d/* 2>/dev/null',
    'echo ====NETPLAN====; ls -la /etc/netplan 2>/dev/null; cat /etc/netplan/*.yaml 2>/dev/null',
    'echo ====NETWORKD====; ls -la /etc/systemd/network/ 2>/dev/null; cat /etc/systemd/network/* 2>/dev/null',
    'echo ====SERVICES====; systemctl is-enabled NetworkManager networking systemd-networkd 2>/dev/null; systemctl is-active NetworkManager networking systemd-networkd 2>/dev/null',
    'echo ====DMESG_ETH====; dmesg 2>/dev/null | grep -iE "eth0|dwmac|emac|sun8i|stmmac|link is" | tail -40',
    'echo ====ETHTOOL====; ethtool eth0 2>&1 | head -20',
    'echo ====WIFI====; iw dev 2>/dev/null; echo --; nmcli -t device show wlan0 2>&1 | head -30',
    'echo ====SSHD_CONFIG====; grep -E "^(PermitRootLogin|PasswordAuthentication|Port|ListenAddress)" /etc/ssh/sshd_config 2>/dev/null',
    'echo ====DONE===='
)
foreach($cmd in $cmds){ Send $cmd; Drain 1200 }
Send 'exit'
Drain 1500
$port.Close()
Write-Host "`n=== session closed ==="
