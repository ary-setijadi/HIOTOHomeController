# Enable Internet Connection Sharing: share "Wi-Fi" (public/internet) to "Ethernet" (private, connects to Pi).
# Must be run elevated.
$ErrorActionPreference = 'Continue'
$log = 'C:\Users\T495\Documents\MEGA\_OrangePiBasic\ics_log.txt'
"=== start $(Get-Date) ===" | Out-File $log -Encoding utf8

try {
    $m = New-Object -ComObject HNetCfg.HNetShare
} catch {
    "HNetCfg COM create failed: $_" | Out-File $log -Append
    exit 1
}

$pub = $null
$priv = $null
$names = @()
try {
    foreach($c in $m.EnumEveryConnection) {
        $props = $m.NetConnectionProps.Invoke($c)
        $names += $props.Name
        if($props.Name -eq 'Wi-Fi')    { $pub  = $c }
        if($props.Name -eq 'Ethernet') { $priv = $c }
    }
} catch {
    "enum failed: $_" | Out-File $log -Append
    exit 1
}

"adapters found: $($names -join ', ')" | Out-File $log -Append
"pub guid=$pub priv guid=$priv" | Out-File $log -Append

if(-not $pub -or -not $priv) {
    "ERROR: could not locate Wi-Fi and/or Ethernet adapter" | Out-File $log -Append
    exit 1
}

try {
    $privCfg = $m.INetSharingConfigurationForINetConnection.Invoke($priv)
    $privCfg.EnableSharing(1)   # 1 = private (home)
    "private (Ethernet) sharing set" | Out-File $log -Append
} catch {
    "private sharing failed: $_" | Out-File $log -Append
}

try {
    $pubCfg = $m.INetSharingConfigurationForINetConnection.Invoke($pub)
    $pubCfg.EnableSharing(0)    # 0 = public (internet)
    "public (Wi-Fi) sharing set" | Out-File $log -Append
} catch {
    "public sharing failed: $_" | Out-File $log -Append
}

Start-Sleep -Seconds 3
"=== Ethernet adapter config after ICS ===" | Out-File $log -Append
cmd /c "ipconfig" | Out-File $log -Append

"=== done $(Get-Date) ===" | Out-File $log -Append
