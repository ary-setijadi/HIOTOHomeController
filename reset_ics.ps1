# Repair ICS: restart SharedAccess, then disable + re-enable sharing on Wi-Fi -> Ethernet.
# Must be run elevated.
$ErrorActionPreference = 'Continue'
$log = 'C:\Users\T495\Documents\MEGA\_OrangePiBasic\ics_reset_log.txt'
"=== start $(Get-Date) ===" | Out-File $log -Encoding ascii

# 1) Restart the ICS service to clear any stuck state.
try {
    Restart-Service SharedAccess -Force -ErrorAction Stop
    "SharedAccess restarted" | Out-File $log -Append
} catch {
    "SharedAccess restart failed: $_" | Out-File $log -Append
}
Start-Sleep -Seconds 3

# 2) Toggle sharing off/on via the HNetCfg COM API.
try {
    $m = New-Object -ComObject HNetCfg.HNetShare
    $pub = $null; $priv = $null
    $names = @()
    foreach ($c in $m.EnumEveryConnection) {
        $props = $m.NetConnectionProps.Invoke($c)
        $names += $props.Name
        if ($props.Name -eq 'Wi-Fi')    { $pub  = $c }
        if ($props.Name -eq 'Ethernet') { $priv = $c }
    }
    "adapters: $($names -join ', ')" | Out-File $log -Append

    if ($pub -and $priv) {
        $pubCfg  = $m.INetSharingConfigurationForINetConnection.Invoke($pub)
        $privCfg = $m.INetSharingConfigurationForINetConnection.Invoke($priv)

        try { $pubCfg.DisableSharing()  } catch { "disable pub: $_" | Out-File $log -Append }
        try { $privCfg.DisableSharing() } catch { "disable priv: $_" | Out-File $log -Append }
        "sharing disabled" | Out-File $log -Append
        Start-Sleep -Seconds 3

        $privCfg.EnableSharing(1)   # 1 = private (Ethernet)
        "private (Ethernet) sharing re-enabled" | Out-File $log -Append
        $pubCfg.EnableSharing(0)    # 0 = public (Wi-Fi)
        "public (Wi-Fi) sharing re-enabled" | Out-File $log -Append
    } else {
        "ERROR: could not locate Wi-Fi and/or Ethernet adapter" | Out-File $log -Append
    }
} catch {
    "COM sharing reset failed: $_" | Out-File $log -Append
}

Start-Sleep -Seconds 4
"=== SharedAccess status ===" | Out-File $log -Append
(Get-Service SharedAccess).Status | Out-File $log -Append
"=== Ethernet config ===" | Out-File $log -Append
cmd /c "ipconfig" | Out-File $log -Append
"=== done $(Get-Date) ===" | Out-File $log -Append
