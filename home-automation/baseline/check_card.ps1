<#
.SYNOPSIS
  Addressability / counterfeit-capacity check for the microSD card.

.DESCRIPTION
  Detects cards that report more capacity than they physically have (or that
  wrap addresses), which is a documented hazard in this workspace:
  SETUP-OrangePi-PiHole-OTA.md lesson 2 records a "64 GB" card that was really
  16 GB.

  METHOD -- and why the obvious approach does NOT work:

    A naive "write block, read block back, compare" always PASSES on a fake
    card, because a wrapped write at 7 GiB and a wrapped read at 7 GiB both
    land on the same real address, so they agree with each other.

    Instead this does a two-phase test:
      phase 1  write 24 distinct, deterministic 1 MiB blocks at offsets spread
               across the FULL reported capacity, including the final MiB
      phase 2  read every offset back afterwards and compare
    If two offsets alias to one physical location, the later write destroys the
    earlier block, so the earlier offset reads back the wrong content and fails.

  A full-surface h2testw sweep would be more exhaustive; this is a sampled
  check across the whole address span, which is what catches fake capacity.
#>
[CmdletBinding()]
param(
    [int]$DiskNumber = 1,
    [int]$Samples = 24,
    [int]$BlockMiB = 1
)

$ErrorActionPreference = 'Stop'
function Log([string]$m) { Write-Output ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

function Get-BlockHash([long]$offset, [int]$size) {
    # Deterministic per-offset content, generated at .NET speed.
    $seed = [int]((($offset -shr 12) % 1000000000) + 1000003)
    $buf = New-Object byte[] $size
    (New-Object System.Random($seed)).NextBytes($buf)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash($buf) | ForEach-Object { $_.ToString('x2') }) -join '') }
    finally { $sha.Dispose() }
}

function Get-BlockBytes([long]$offset, [int]$size) {
    $seed = [int]((($offset -shr 12) % 1000000000) + 1000003)
    $buf = New-Object byte[] $size
    (New-Object System.Random($seed)).NextBytes($buf)
    return $buf
}

# ----------------------------------------------------------------- preflight
$d = Get-Disk -Number $DiskNumber -ErrorAction Stop
if ($d.IsSystem) { throw "REFUSING: disk $DiskNumber is the SYSTEM disk" }
if ($d.IsBoot) { throw "REFUSING: disk $DiskNumber is the BOOT disk" }
if ($d.FriendlyName -notmatch 'Reader|SD|SDHC|SDXC|Card|Flash|USB') {
    throw "REFUSING: disk $DiskNumber ('$($d.FriendlyName)') does not look like a card reader"
}
$diskSize = [long]$d.Size
Log ("card: disk {0} '{1}'  reported {2:N0} bytes ({3:N2} GiB)" -f $d.Number, $d.FriendlyName, $diskSize, ($diskSize / 1GB))

Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue |
    Where-Object { $_.DriveLetter } | ForEach-Object { & mountvol "$($_.DriveLetter):" /d 2>&1 | Out-Null }
Set-Disk -Number $DiskNumber -IsOffline $true
Start-Sleep -Seconds 2

$diskPath = "\\.\PhysicalDrive$DiskNumber"
$blockSize = [int]($BlockMiB * 1MB)
$lastStart = $diskSize - $blockSize
if ($lastStart -lt 0) { throw "card too small for a $BlockMiB MiB block" }

$offsets = New-Object System.Collections.Generic.List[long]
$offsets.Add([long]0)
for ($i = 1; $i -lt ($Samples - 1); $i++) {
    $offsets.Add([long]([math]::Floor($lastStart * $i / ($Samples - 1) / 4096) * 4096))
}
$offsets.Add([long]$lastStart)

Log ("testing {0} blocks of {1} MiB across 0 .. {2:N2} GiB" -f $offsets.Count, $BlockMiB, ($lastStart / 1GB))

# ------------------------------------------------------- phase 1: write all
Log 'phase 1: writing all sample blocks ...'
$fs = New-Object System.IO.FileStream -ArgumentList @(
    $diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
try {
    foreach ($off in $offsets) {
        $buf = Get-BlockBytes $off $blockSize
        $fs.Position = $off
        $fs.Write($buf, 0, $blockSize)
    }
    $fs.Flush($true)
}
finally { $fs.Close() }
Log 'phase 1 complete (handle closed)'

# -------------------------------------------------------- phase 2: read all
Log 'phase 2: reading every offset back and comparing ...'
$fail = 0
$fs = New-Object System.IO.FileStream -ArgumentList @(
    $diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    foreach ($off in $offsets) {
        $expected = Get-BlockHash $off $blockSize
        $rb = New-Object byte[] $blockSize
        $fs.Position = $off
        $read = 0
        while ($read -lt $blockSize) {
            $n = $fs.Read($rb, $read, $blockSize - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        $actual = if ($read -eq $blockSize) {
            (($sha.ComputeHash($rb) | ForEach-Object { $_.ToString('x2') }) -join '')
        } else { "<short read $read/$blockSize>" }

        if ($actual -eq $expected) {
            Log ("  OK    offset {0,14:N0}  ({1,7:N2} GiB)" -f $off, ($off / 1GB))
        }
        else {
            $fail++
            Log ("  FAIL  offset {0,14:N0}  ({1,7:N2} GiB)  expected {2} got {3}" -f $off, ($off / 1GB), $expected.Substring(0, 16), $actual.Substring(0, [Math]::Min(16, $actual.Length)))
        }
    }
}
finally {
    $fs.Close()
    $sha.Dispose()
    Set-Disk -Number $DiskNumber -IsOffline $false -ErrorAction SilentlyContinue
    try { Update-Disk -Number $DiskNumber -ErrorAction Stop } catch { }
}

$ok = $offsets.Count - $fail
Log ("result: {0} OK, {1} FAILED  (span 0 .. {2:N2} GiB of {3:N2} GiB reported)" -f $ok, $fail, ($lastStart / 1GB), ($diskSize / 1GB))
if ($fail -gt 0) {
    throw "CARD FAILED addressability check ($fail of $($offsets.Count) blocks) - do NOT use this card; it is counterfeit or failing"
}
Log 'CARD CAPACITY / ADDRESSING OK'
