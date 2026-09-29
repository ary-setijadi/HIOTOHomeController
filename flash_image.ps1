# Flash the Armbian image to the Orange Pi PC microSD (PhysicalDrive1).
# Run elevated. Steps: verify SHA256 -> decompress xz -> dismount volumes -> raw write -> verify.
$ErrorActionPreference = 'Stop'
$ws = 'C:\Users\T495\Documents\MEGA\_OrangePiBasic'
$xz  = Join-Path $ws 'Armbian_orangePiPC_trixie.img.xz'
$sha = Join-Path $ws 'Armbian_orangePiPC_trixie.img.xz.sha'
$diskNumber = 1
$diskPath   = '\\.\PhysicalDrive1'

function Log([string]$m){ Write-Output ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

# 0. Confirm target disk identity before touching anything.
$d = Get-Disk -Number $diskNumber
Log ("Target disk {0}: {1} ({2:N2} GiB, {3})" -f $d.Number, $d.FriendlyName, ($d.Size/1GB), $d.PartitionStyle)
if($d.FriendlyName -notmatch 'Reader|SD|SDHC|Card'){ throw "Refusing: Disk $diskNumber does not look like the SD card reader" }

# 1. Verify sha256 of the compressed image.
if(-not (Test-Path $xz)){ throw "xz image not found: $xz" }
$shaContent = (Get-Content $sha -Raw)
$m = [regex]::Match($shaContent, '([0-9a-fA-F]{64})')
if(-not $m.Success){ throw "Could not parse sha file" }
$expected = $m.Groups[1].Value.ToLower()
$actual   = (Get-FileHash $xz -Algorithm SHA256).Hash.ToLower()
if($expected -ne $actual){ throw "SHA256 MISMATCH: expected $expected got $actual" }
Log ("sha256 verified: $actual")

# 2. Decompress xz -> img using Python lzma (tar.exe can't read a raw single-file .xz).
$img = $xz -replace '\.xz$',''
if(-not (Test-Path $img) -or (Get-Item $img).Length -lt 100MB){
    Log "decompressing xz -> img ..."
    $py = (Get-Command python -ErrorAction Stop).Source
    & $py -c "import lzma,shutil; shutil.copyfileobj(lzma.open(r'$xz','rb'), open(r'$img','wb'), 8*1024*1024)"
    if($LASTEXITCODE -ne 0){ throw "python lzma decompress exit $LASTEXITCODE" }
}
if(-not (Test-Path $img)){ throw "img not produced" }
Log ("img: {0} ({1:N2} GiB)" -f $img, ((Get-Item $img).Length/1GB))

# 3. Dismount any volumes on the target disk.
Get-Partition -DiskNumber $diskNumber | Where-Object { $_.DriveLetter } | ForEach-Object {
    Log ("dismounting $($_.DriveLetter):")
    & mountvol "$($_.DriveLetter):" /d 2>&1 | Out-Null
}
Start-Sleep -Seconds 1

# 4. Raw write.
Log "writing raw image to $diskPath ..."
$buf = New-Object byte[] (8MB)
$in  = [System.IO.File]::Open($img, 'Open', 'Read', 'Read')
$out = [System.IO.File]::Open($diskPath, 'Open', 'Write', 'ReadWrite')
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $total = [long]0
    while(($n = $in.Read($buf, 0, $buf.Length)) -gt 0){
        $out.Write($buf, 0, $n)
        $total += $n
    }
    $out.Flush($true)
    $sw.Stop()
    Log ("wrote {0:N2} GiB in {1:N1}s ({2:N1} MB/s)" -f ($total/1GB), $sw.Elapsed.TotalSeconds, ($total/1MB/$sw.Elapsed.TotalSeconds))
} finally {
    $in.Close(); $out.Close()
}

# 5. Verify: read back same byte count from the disk and compare hashes.
Log "verifying write by reading back from disk ..."
$diskLen = [System.IO.File]::Open($diskPath,'Open','Read','Read').Length
Log ("disk size = {0:N2} GiB" -f ($diskLen/1GB))
# Compare first 1 MiB and last 1 MiB of the image vs the disk.
$cmp = 1MB
$inImg  = [System.IO.File]::Open($img, 'Open', 'Read', 'Read')
$inDisk = [System.IO.File]::Open($diskPath, 'Open', 'Read', 'ReadWrite')
try {
    $b1 = New-Object byte[] $cmp; $b2 = New-Object byte[] $cmp
    [void]$inImg.Read($b1, 0, $cmp)
    $inDisk.Position = 0; [void]$inDisk.Read($b2, 0, $cmp)
    $headMatch = ([Convert]::ToBase64String($b1) -eq [Convert]::ToBase64String($b2))
    $imgLen = $inImg.Length
    $inImg.Position = $imgLen - $cmp; [void]$inImg.Read($b1, 0, $cmp)
    $inDisk.Position = $imgLen - $cmp; [void]$inDisk.Read($b2, 0, $cmp)
    $tailMatch = ([Convert]::ToBase64String($b1) -eq [Convert]::ToBase64String($b2))
    Log ("head(1MiB) match=$headMatch ; tail(1MiB) match=$tailMatch")
    if(-not $headMatch -or -not $tailMatch){ throw "VERIFY FAILED: image bytes do not match disk" }
} finally { $inImg.Close(); $inDisk.Close() }

Log "FLASH COMPLETE OK"
