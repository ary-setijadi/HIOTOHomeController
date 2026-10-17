<#
.SYNOPSIS
  Chunk-level repair + verification of a raw flash.

.DESCRIPTION
  Compares the card against the image 1 MiB at a time and rewrites only the
  chunks that differ, up to N rounds. This separates two cases:

    * a chunk that matches after one rewrite  -> the media is fine; the original
      write hit a transient fault (the documented flaky reader).
    * a chunk that still differs after several rewrites -> those blocks cannot
      hold data, so the card must be replaced.

  Uses .NET FileStream, not Python's open(), because Python cannot open a raw
  Windows device for writing (OSError 9, bad file descriptor) even though it can
  read one.

.PARAMETER ImagePath
  Source .img file.

.PARAMETER DiskNumber
  Physical disk number (default 1).

.PARAMETER Rounds
  Maximum verify/repair rounds (default 4).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ImagePath,
    [int]$DiskNumber = 1,
    [int]$Rounds = 4,
    [int]$ChunkMiB = 1
)

$ErrorActionPreference = 'Stop'
function Log([string]$m) { Write-Output ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

function Read-Exact {
    param([System.IO.Stream]$Stream, [byte[]]$Buffer, [long]$Offset, [int]$Count)
    $Stream.Position = $Offset
    $read = 0
    while ($read -lt $Count) {
        $n = $Stream.Read($Buffer, $read, $Count - $read)
        if ($n -le 0) { throw "short read at offset $Offset (got $read of $Count)" }
        $read += $n
    }
}

function Test-DiskIdentity {
    param([int]$Number)
    $d = Get-Disk -Number $Number -ErrorAction Stop
    if ($d.IsSystem) { throw "REFUSING: disk $Number is the SYSTEM disk" }
    if ($d.IsBoot) { throw "REFUSING: disk $Number is the BOOT disk" }
    if ($d.FriendlyName -notmatch 'Reader|SD|SDHC|SDXC|Card|Flash|USB') {
        throw "REFUSING: disk $Number ('$($d.FriendlyName)') does not look like a card reader"
    }
    return $d
}

$d = Test-DiskIdentity -Number $DiskNumber
$imgLen = (Get-Item -LiteralPath $ImagePath).Length
$chunkSize = [int]($ChunkMiB * 1MB)
$diskPath = "\\.\PhysicalDrive$DiskNumber"

Log ("card : disk {0} '{1}' {2:N2} GiB" -f $d.Number, $d.FriendlyName, ($d.Size / 1GB))
Log ("image: {0:N0} bytes ({1} chunks of {2} MiB)" -f $imgLen, [math]::Ceiling($imgLen / $chunkSize), $ChunkMiB)

$sha = [System.Security.Cryptography.SHA256]::Create()
$imgFs = [System.IO.File]::Open($ImagePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
$diskFs = New-Object System.IO.FileStream -ArgumentList @(
    $diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::ReadWrite, 65536, [System.IO.FileOptions]::WriteThrough)

$imgBuf = New-Object byte[] $chunkSize
$diskBuf = New-Object byte[] $chunkSize
$history = @()

try {
    for ($round = 1; $round -le $Rounds; $round++) {
        Log "===== round $round : verifying"
        $mismatches = New-Object System.Collections.Generic.List[long]
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $off = [long]0
        while ($off -lt $imgLen) {
            $want = [int][Math]::Min([long]$chunkSize, $imgLen - $off)
            Read-Exact -Stream $diskFs -Buffer $diskBuf -Offset $off -Count $want
            Read-Exact -Stream $imgFs  -Buffer $imgBuf  -Offset $off -Count $want
            $hCard = [System.BitConverter]::ToString($sha.ComputeHash($diskBuf, 0, $want))
            $hImg  = [System.BitConverter]::ToString($sha.ComputeHash($imgBuf, 0, $want))
            if ($hCard -ne $hImg) { $mismatches.Add($off) }
            $off += $want
        }
        $sw.Stop()
        Log ("  {0} chunk(s) mismatching of {1}  ({2:N1}s)" -f `
                $mismatches.Count, [math]::Ceiling($imgLen / $chunkSize), $sw.Elapsed.TotalSeconds)
        foreach ($m in $mismatches) { Log ("    @ {0,14:N0} ({1,6:N0} MiB)" -f $m, ($m / 1MB)) }

        $history += [pscustomobject]@{ Round = $round; Mismatches = $mismatches.Count }

        if ($mismatches.Count -eq 0) {
            Log '*** CARD MATCHES THE IMAGE EXACTLY ***'
            break
        }

        Log ("  rewriting {0} chunk(s) ..." -f $mismatches.Count)
        foreach ($m in $mismatches) {
            $want = [int][Math]::Min([long]$chunkSize, $imgLen - $m)
            Read-Exact -Stream $imgFs -Buffer $imgBuf -Offset $m -Count $want
            $diskFs.Position = $m
            $diskFs.Write($imgBuf, 0, $want)
        }
        $diskFs.Flush($true)
        Start-Sleep -Seconds 1
    }
}
finally {
    $diskFs.Close()
    $imgFs.Close()
    $sha.Dispose()
}

Log '===== history'
foreach ($h in $history) { Log ("  round {0}: {1} mismatching chunk(s)" -f $h.Round, $h.Mismatches) }

$final = $history[-1].Mismatches
if ($final -eq 0) {
    Log 'RESULT: card verified good'
    exit 0
}
Log ("RESULT: card STILL has {0} bad chunk(s) after {1} round(s) - suspect failing media" -f $final, $history.Count)
exit 1
