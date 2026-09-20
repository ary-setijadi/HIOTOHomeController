# Scripted serial login to Orange Pi over CDC-ACM gadget console (COM4).
$ErrorActionPreference = 'Continue'

$portName = 'COM4'
$baud     = 115200

$creds = @(
    @{ u='root';     p='1234' },
    @{ u='root';     p='orangepi' },
    @{ u='orangepi'; p='orangepi' },
    @{ u='root';     p='root' },
    @{ u='orangepi'; p='1234' },
    @{ u='root';     p='orangepi1234' }
)

$port = New-Object System.IO.Ports.SerialPort
$port.PortName   = $portName
$port.BaudRate   = $baud
$port.Parity     = [System.IO.Ports.Parity]::None
$port.DataBits   = 8
$port.StopBits   = [System.IO.Ports.StopBits]::One
$port.Handshake  = [System.IO.Ports.Handshake]::None
$port.ReadTimeout  = 200
$port.WriteTimeout = 1000
$port.DtrEnable = $true
$port.RtsEnable = $true

try {
    $port.Open()
} catch {
    Write-Output "ERROR_OPEN: $_"
    exit 1
}
Start-Sleep -Milliseconds 800

$script:buf = New-Object System.Text.StringBuilder

function Drain([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while((Get-Date) -lt $end) {
        try { $d = $port.ReadExisting(); if($d) { [void]$script:buf.Append($d); Write-Host -NoNewline $d } } catch {}
        Start-Sleep -Milliseconds 50
    }
}

function WaitFor([string]$pattern, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while((Get-Date) -lt $end) {
        try { $d = $port.ReadExisting(); if($d) { [void]$script:buf.Append($d); Write-Host -NoNewline $d } } catch {}
        if($script:buf.ToString() -match $pattern) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return $false
}

function Send([string]$s) {
    $port.Write("$s`n")
}

$loginOk = $false
$currentCred = $null

foreach($c in $creds) {
    Write-Host "`n=== trying $($c.u) / $($c.p) ==="
    Send ''
    Drain 1200
    [void]$script:buf.Clear()

    Send $c.u
    if(-not (WaitFor 'Password:' 6000)) {
        Write-Host "  (no password prompt)"
        continue
    }
    Send $c.p
    Drain 2500
    Send 'echo __SHELL_OK_$((1+1))__'
    if(WaitFor '__SHELL_OK_2__' 4500) {
        $loginOk = $true
        $currentCred = $c
        break
    }
    if($script:buf.ToString() -match 'incorrect') { Write-Host "  (login incorrect)" }
}

if($loginOk) {
    Write-Host "`n=== LOGIN SUCCESS with $($currentCred.u) ==="
    $cmds = @(
        'echo ====ID====; id',
        'echo ====HOST====; hostname',
        'echo ====OS====; cat /etc/os-release',
        'echo ====UNAME====; uname -a',
        'echo ====IP====; ip -4 addr show; echo ---; ip route',
        'echo ====NETDEV====; ls /sys/class/net',
        'echo ====SSHD====; systemctl is-active ssh sshd 2>/dev/null',
        'echo ====CPU====; grep -E "model name|Hardware|Processor|Revision" /proc/cpuinfo | head -n 8',
        'echo ====MEM====; free -h',
        'echo ====DISK====; df -h',
        'echo ====UPTIME====; uptime',
        'echo ====DONE===='
    )
    foreach($cmd in $cmds) {
        Send $cmd
        Drain 1200
    }
    Send 'exit'
    Drain 1500
} else {
    Write-Host "`n=== LOGIN FAILED for all default creds ==="
}

$port.Close()
Write-Host "`n=== session closed ==="
