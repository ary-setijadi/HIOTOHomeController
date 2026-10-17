<#
.SYNOPSIS
  Raw-write an Armbian image to a microSD card and FULLY verify it by read-back.

.DESCRIPTION
  Hardened replacement for flash_image.ps1. Every safeguard here exists because
  the naive version of this script produced a silently-corrupt card.

  WHY THE ORIGINAL APPROACH FAILED (verified on this machine, 2026-10-13):

    The card still carried a live, whole-disk FAT32 volume left behind by a
    Windows format. On removable media Windows refuses BOTH
      Set-Disk -IsOffline      -> "Not Supported / Removable media cannot be set to offline"
      Remove-Partition         -> "Not Supported"
    so the volume stayed mounted. The raw image write APPEARED to succeed, but
    Windows later flushed its dirty FAT32 metadata back over the start of the
    card -- boot sector at 0 MiB plus FAT tables at 2/3/4/5 MiB. Read-back then
    showed exactly those 5 chunks mismatching, and further raw writes were denied
    outright ("Access to the path is denied").

    Fix: Clear-Disk + `diskpart clean` + rescan, assert no volume remains, and
    prove write access with a probe before writing. Windows then leaves the
    Linux-partitioned image alone because it cannot mount ext4.

  ALSO:
   * verification hashes the ENTIRE written region read back off the card and
     compares it to the source image SHA-256 (the old script compared only the
     first and last 1 MiB, which is how a flaky reader silently corrupted a flash
     -- SETUP-OrangePi-PiHole-OTA.md lesson 1);
   * verification runs TWICE, the second time after a rescan and delay, so a
     late write-back by Windows cannot masquerade as success;
   * refuses the system/boot disk and any target that is not a card reader.

.PARAMETER ImagePath
  Path to the raw .img file (already decompressed).

.PARAMETER DiskNumber
  Physical disk number of the card reader (default 1).

.PARAMETER PreflightOnly
  Run every safety check and stop without writing anything.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ImagePath,
    [int]$DiskNumber = 1,
    [switch]$PreflightOnly
)

$ErrorActionPreference = 'Stop'
$bufSize = 8MB

# NOTE: must NOT use Write-Output here. PowerShell functions return everything
# written to the pipeline, so Write-Output inside a function pollutes its return
# value -- that bug made a passing hash comparison look like a failure.
function Log([string]$m) { [Console]::WriteLine(("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)) }

function Open-DiskStream {
    param([string]$Path, [System.IO.FileAccess]$Access, [switch]$WriteThrough)
    if ($WriteThrough) {
        return New-Object System.IO.FileStream -ArgumentList @(
            $Path, [System.IO.FileMode]::Open, $Access,
            [System.IO.FileShare]::ReadWrite, 65536, [System.IO.FileOptions]::WriteThrough
        )
    }
    return New-Object System.IO.FileStream -ArgumentList @(
        $Path, [System.IO.FileMode]::Open, $Access, [System.IO.FileShare]::ReadWrite
    )
}

function Get-VolumesOnDisk {
    param([int]$Number)
    $parts = Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue
    if (-not $parts) { return @() }
    return @($parts | Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_ -and $_.Size -gt 0 })
}

function Test-WriteAccess {
    param([string]$Path, [int]$Number)
    $probe = New-Object System.IO.FileStream -ArgumentList @(
        $Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::ReadWrite, 65536, [System.IO.FileOptions]::WriteThrough)
    try {
        $buf = New-Object byte[] 4096
        foreach ($off in @(0, 4MB, 100MB)) {
            if ($off -ge (Get-Disk -Number $Number).Size) { continue }
            $probe.Position = $off
            $null = $probe.Read($buf, 0, 4096)
            $probe.Position = $off
            $probe.Write($buf, 0, 4096)     # write back identical bytes: harmless
            $probe.Flush($true)
        }
        return $true
    }
    finally { $probe.Close() }
}

# ---------------------------------------------------------------- 0. preflight
if (-not (Test-Path -LiteralPath $ImagePath)) { throw "image not found: $ImagePath" }
$img    = Get-Item -LiteralPath $ImagePath
$imgLen = [long]$img.Length
if ($imgLen -lt 100MB) { throw "image is only $([math]::Round($imgLen/1MB,1)) MB - looks truncated, refusing" }

Log ("image  : {0}" -f $img.FullName)
Log ("size   : {0:N0} bytes ({1:N2} GiB)" -f $imgLen, ($imgLen / 1GB))

$d = Get-Disk -Number $DiskNumber -ErrorAction Stop
if ($d.IsSystem) { throw "REFUSING: disk $DiskNumber is the SYSTEM disk" }
if ($d.IsBoot)   { throw "REFUSING: disk $DiskNumber is the BOOT disk" }
if ($d.Size -gt 128GB) { throw "REFUSING: disk $DiskNumber is $([math]::Round($d.Size/1GB,1)) GiB - too large for microSD" }
if ($d.FriendlyName -notmatch 'Reader|SD|SDHC|SDXC|Card|Flash|USB') {
    throw "REFUSING: disk $DiskNumber ('$($d.FriendlyName)') does not look like a card reader"
}
if ($imgLen -ge $d.Size) { throw "REFUSING: image ($imgLen bytes) does not fit target ($($d.Size) bytes)" }

Log ("target : disk {0} '{1}' {2:N2} GiB [{3}]" -f $d.Number, $d.FriendlyName, ($d.Size / 1GB), $d.PartitionStyle)
$diskPath = "\\.\PhysicalDrive$DiskNumber"

if ($PreflightOnly) { Log 'preflight OK (PreflightOnly set - nothing written)'; return }

$srcHash = (Get-FileHash -LiteralPath $ImagePath -Algorithm SHA256).Hash.ToLower()
Log ("source sha256 : $srcHash")

# ------------------------------- 1. forcibly release the disk from Windows
Log 'releasing the card from Windows (clear + clean + rescan)'
if ($d.IsReadOnly) { Log '  clearing read-only attribute'; Set-Disk -Number $DiskNumber -IsReadOnly $false }

Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.DriveLetter) { & mountvol "$($_.DriveLetter):" /d 2>&1 | Out-Null }
    if ($_.AccessPaths) {
        foreach ($ap in $_.AccessPaths) {
            try { Remove-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $_.PartitionNumber -AccessPath $ap -ErrorAction Stop } catch { }
        }
    }
}

try { Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop; Log '  Clear-Disk OK' }
catch { Log "  Clear-Disk: $($_.Exception.Message.Split("`n")[0].Trim())" }

if ((Get-VolumesOnDisk -Number $DiskNumber).Count -gt 0) {
    Log '  volume survived Clear-Disk -> running diskpart clean'
    $script = "select disk $DiskNumber`r`nclean`r`nrescan`r`nexit`r`n"
    $tmp = Join-Path $env:TEMP ("dpclean_$DiskNumber.txt")
    Set-Content -Path $tmp -Value $script -Encoding ASCII
    & diskpart /s $tmp 2>&1 | ForEach-Object { Log "    $_" }
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}

try { Update-Disk -Number $DiskNumber -ErrorAction Stop } catch { }
try { Update-HostStorageCache } catch { }
Start-Sleep -Seconds 2

$vols = Get-VolumesOnDisk -Number $DiskNumber
if ($vols.Count -gt 0) {
    throw ("could not release the card from Windows: {0} volume(s) still mounted ({1}). " -f `
            $vols.Count, (($vols | ForEach-Object { "$($_.FileSystem) $($_.Size)" }) -join ', ')) +
          "Raw writes would be silently clobbered afterwards."
}
Log '  no volume on the card - Windows will not touch it'

if (-not (Test-WriteAccess -Path $diskPath -Number $DiskNumber)) { throw 'raw write probe denied' }
Log '  raw write probe: ALLOWED at offsets 0 / 4 MiB / 100 MiB'

# ------------------------------------------------------------- 2. raw write
Log "writing $imgLen bytes -> $diskPath (write-through) ..."
$buf      = New-Object byte[] $bufSize
$in       = [System.IO.File]::Open($ImagePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
$out      = Open-DiskStream -Path $diskPath -Access ([System.IO.FileAccess]::Write) -WriteThrough
$sw       = [System.Diagnostics.Stopwatch]::StartNew()
$total    = [long]0
$lastMark = -10
try {
    while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
        $out.Write($buf, 0, $n)
        $total += $n
        $pct = [int](100 * $total / $imgLen)
        if ($pct -ge ($lastMark + 10)) { $lastMark = $pct; Log ("  write {0,3}%" -f $pct) }
    }
    $out.Flush($true)
    $sw.Stop()
    Log ("wrote {0:N0} bytes in {1:N1}s ({2:N1} MB/s)" -f $total, $sw.Elapsed.TotalSeconds, ($total / 1MB / $sw.Elapsed.TotalSeconds))
}
finally {
    $in.Close()
    $out.Close()
}
if ($total -ne $imgLen) { throw "SHORT WRITE: wrote $total of $imgLen bytes" }

# ------------------------------------------------- 3. verify (function, reusable)
function Invoke-Verify {
    param([string]$Label)
    Log "verifying ($Label): reading the whole written region back ..."
    $sha    = [System.Security.Cryptography.SHA256]::Create()
    $handle = Open-DiskStream -Path $diskPath -Access ([System.IO.FileAccess]::Read)
    $rbuf   = New-Object byte[] $bufSize
    $remain = [long]$imgLen
    $mark   = -10
    try {
        while ($remain -gt 0) {
            $want = [int][Math]::Min([long]$rbuf.Length, $remain)
            $got  = $handle.Read($rbuf, 0, $want)
            if ($got -le 0) { throw ("unexpected EOF after {0:N0} of {1:N0} bytes" -f ($imgLen - $remain), $imgLen) }
            [void]$sha.TransformBlock($rbuf, 0, $got, $null, 0)
            $remain -= $got
            $pct = [int](100 * ($imgLen - $remain) / $imgLen)
            if ($pct -ge ($mark + 20)) { $mark = $pct; Log ("  verify {0,3}%" -f $pct) }
        }
        [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        return (($sha.Hash | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    finally { $handle.Close(); $sha.Dispose() }
}

$h1 = Invoke-Verify -Label 'pass 1, immediately after write'
Log ("pass 1 sha256 : $h1")
if ($h1 -ne $srcHash) { throw "VERIFY FAILED (pass 1):`n  expected $srcHash`n  actual   $h1" }
Log '  PASS 1 OK'

# Second pass AFTER a rescan + delay: this is what catches Windows flushing a
# stale volume over the image once the write is done.
Log 'rescans + settling, then re-verifying to catch any late write-back'
try { Update-Disk -Number $DiskNumber -ErrorAction Stop } catch { }
try { Update-HostStorageCache } catch { }
Start-Sleep -Seconds 8

$h2 = Invoke-Verify -Label 'pass 2, after rescan + 8s'
Log ("pass 2 sha256 : $h2")
if ($h2 -ne $srcHash) { throw "VERIFY FAILED (pass 2 - content changed after the write):`n  expected $srcHash`n  actual   $h2" }
Log '  PASS 2 OK'

Log 'final state:'
Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue |
    Select-Object PartitionNumber, DriveLetter, Offset, Size, Type | Format-Table -AutoSize | Out-String | Write-Output

Log 'FLASH COMPLETE OK - both read-back passes match the source image'
