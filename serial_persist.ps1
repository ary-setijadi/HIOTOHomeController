# Make eth0 IP persistent via NetworkManager (static 169.254.10.10/16 for direct link).
$ErrorActionPreference = 'Continue'
$port = New-Object System.IO.Ports.SerialPort('COM4', 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$port.ReadTimeout = 200; $port.WriteTimeout = 1000; $port.DtrEnable = $true; $port.RtsEnable = $true
try { $port.Open() } catch { Write-Output "ERROR_OPEN: $_"; exit 1 }
Start-Sleep -Milliseconds 800
$script:buf = New-Object System.Text.StringBuilder
function Drain([int]$ms){ $end=(Get-Date).AddMilliseconds($ms); while((Get-Date) -lt $end){ try{ $d=$port.ReadExisting(); if($d){[void]$script:buf.Append($d); Write-Host -NoNewline $d} }catch{}; Start-Sleep -Milliseconds 50 } }
function WaitFor([string]$pat,[int]$ms){ $end=(Get-Date).AddMilliseconds($ms); while((Get-Date) -lt $end){ try{ $d=$port.ReadExisting(); if($d){[void]$script:buf.Append($d); Write-Host -NoNewline $d} }catch{}; if($script:buf.ToString() -match $pat){return $true}; Start-Sleep -Milliseconds 50 }; return $false }
function Send([string]$s){ $port.Write("$s`n") }

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
    'echo ====CONNMOD====; nmcli con mod "Wired connection 1" ipv4.method manual ipv4.addresses 169.254.10.10/16 ipv4.gateway "" connection.autoconnect yes 2>&1; echo rc=$?',
    'echo ====CONNUP====; nmcli con up "Wired connection 1" 2>&1; echo rc=$?',
    'echo ====SHOW_ETH0====; ip -4 addr show eth0',
    'echo ====CONN_SHOW====; nmcli -t connection show "Wired connection 1" 2>&1 | grep -E "ipv4|connection.autoconnect|GENERAL.STATE"',
    'echo ====DONE===='
)
foreach($cmd in $cmds){ Send $cmd; Drain 1800 }
Send 'exit'
Drain 1500
$port.Close()
Write-Host "`n=== session closed ==="
