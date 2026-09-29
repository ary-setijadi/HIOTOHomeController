# Re-flash the Armbian image and verify the ENTIRE image byte-for-byte.
$ErrorActionPreference = 'Stop'
$img       = 'C:\Users\T495\Documents\MEGA\_OrangePiBasic\Armbian_orangePiPC_trixie.img'
$diskPath  = '\\.\PhysicalDrive1'
$diskNumber = 1

function Log([string]$m){ Write-Output ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

if(-not (Test-Path $img)){ throw "img not found: $img" }
$imgLen = (Get-Item $img).Length
Log ("img = {0:N2} GiB" -f ($imgLen/1GB))

# 1. Clean target disk to release any volume locks (removes old partition table).
Log "diskpart clean ..."
@"
select disk $diskNumber
clean
"@ | diskpart | Out-Null
Start-Sleep -Seconds 1

# 2. Raw write.
Log "writing ..."
$buf = New-Object byte[] (8MB)
$in  = [System.IO.File]::Open($img, 'Open', 'Read', 'Read')
$out = [System.IO.File]::Open($diskPath, 'Open', 'Write', 'ReadWrite')
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$total = [long]0
try {
    while(($n = $in.Read($buf, 0, $buf.Length)) -gt 0){ $out.Write($buf, 0, $n); $total += $n }
    $out.Flush($true)
} finally { $in.Close(); $out.Close() }
$sw.Stop()
Log ("wrote {0:N2} GiB in {1:N1}s ({2:N1} MB/s)" -f ($total/1GB), $sw.Elapsed.TotalSeconds, ($total/1MB/$sw.Elapsed.TotalSeconds))

# 3. Full byte-for-byte verification (read the whole image back from the card and hash).
Log "full read-back verification ..."
$imgHash = (Get-FileHash $img -Algorithm SHA256).Hash
$sha = [System.Security.Cryptography.SHA256]::Create()
$rbuf = New-Object byte[] (4MB)
$fs = [System.IO.File]::Open($diskPath, 'Open', 'Read', 'ReadWrite')
try {
    $remaining = [long]$imgLen
    while($remaining -gt 0){
        $n = $fs.Read($rbuf, 0, [int][Math]::Min($rbuf.Length, $remaining))
        if($n -le 0){ Log ("WARN: read returned $n at remaining=$remaining"); break }
        [void]$sha.TransformBlock($rbuf, 0, $n, $null, 0)
        $remaining -= $n
    }
} finally { $fs.Close() }
$sha.TransformFinalBlock([byte[]]@(), 0, 0)
$diskHash = [BitConverter]::ToString($sha.Hash).Replace('-','').ToLower()
Log ("img  SHA256: $imgHash")
Log ("disk SHA256: $diskHash")
Log ("FULL MATCH: " + ($imgHash -eq $diskHash))
if($imgHash -ne $diskHash){ throw "VERIFY FAILED - card content does not match image (likely bad/fake SD card)" }
Log "REFLASH OK"
