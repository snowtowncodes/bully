# Convert a 24/32-bit uncompressed TGA to PNG (robust; single file).
param([string]$Path)
Add-Type -AssemblyName System.Drawing
$bytes = [IO.File]::ReadAllBytes($Path)
if ($bytes.Count -lt 18) { Write-Error "too small"; exit 1 }
$w = $bytes[12] -bor ($bytes[13] -shl 8)
$h = $bytes[14] -bor ($bytes[15] -shl 8)
$bpp = $bytes[16]
$idLen = $bytes[0]
if ($bytes[2] -ne 2 -or ($bpp -ne 24 -and $bpp -ne 32)) { Write-Error "unsupported type=$($bytes[2]) bpp=$bpp"; exit 1 }
$pixelBytes = [int]($bpp / 8)
$dataOff = 18 + $idLen
$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$rowBytes = $w * 4
$row = [byte[]]::new($rowBytes)
$topDown = (($bytes[17] -shr 5) -band 1) -eq 1
for ($y = 0; $y -lt $h; $y++) {
    $srcY = if ($topDown) { $y } else { $h - 1 - $y }
    $src = $dataOff + $srcY * $w * $pixelBytes
    for ($x = 0; $x -lt $w; $x++) {
        $s = $src + $x * $pixelBytes
        $row[$x*4+0] = $bytes[$s+2]  # B
        $row[$x*4+1] = $bytes[$s+1]  # G
        $row[$x*4+2] = $bytes[$s+0]  # R
        $row[$x*4+3] = if ($bpp -eq 32) { $bytes[$s+3] } else { 255 }
    }
    [Runtime.InteropServices.Marshal]::Copy($row, 0, [IntPtr]($bd.Scan0.ToInt64() + $y * $bd.Stride), $rowBytes)
}
$bmp.UnlockBits($bd)
$out = [IO.Path]::ChangeExtension($Path, ".png")
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "converted $Path -> $out ($w x $h, bpp $bpp)"
