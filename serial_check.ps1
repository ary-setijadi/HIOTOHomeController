# Serial: check system state, dump apt sources, disable sshd UseDNS.
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
    'echo ===LOAD===; uptime; free -m | head -3',
    'echo ===PROCS===; ps aux | grep -E "apt|dpkg" | grep -v grep || echo NO_APT_RUNNING',
    'echo ===LOG===; tail -15 /tmp/upgrade.log 2>/dev/null || echo NO_LOG',
    'echo ===SRC_LIST===; cat /etc/apt/sources.list 2>/dev/null',
    'echo ===SRC_D===; ls -la /etc/apt/sources.list.d/ 2>/dev/null',
    'echo ===SRC_D_FILES===; cat /etc/apt/sources.list.d/* 2>/dev/null',
    'echo ===USEDNS_FIX===; grep -q "^UseDNS" /etc/ssh/sshd_config && sed -i "s/^UseDNS.*/UseDNS no/" /etc/ssh/sshd_config || echo "UseDNS no" >> /etc/ssh/sshd_config; systemctl restart ssh; echo SSHD_RESTARTED',
    'echo ===DONE==='
)
foreach($cmd in $cmds){ Send $cmd; Drain 1500 }
Send 'exit'
Drain 1500
$port.Close()
Write-Host "`n=== session closed ==="
