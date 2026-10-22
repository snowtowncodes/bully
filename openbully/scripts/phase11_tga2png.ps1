# Convert a TGA (24bpp, uncompressed, origin bottom-left) to PNG.
param([string]$Name = "phase11_journal_4")
Set-Location $PSScriptRoot\..
$src = "$Name.tga"
$dst = "docs\$Name.png"
$bytes = [IO.File]::ReadAllBytes((Join-Path (Get-Location) $src))
$w = [BitConverter]::ToUInt16($bytes, 12)
$h = [BitConverter]::ToUInt16($bytes, 14)
$bpp = $bytes[16]
Write-Host "$src : $w x $h bpp=$bpp"
if ($bpp -ne 32) { Write-Error "only 32bpp supported (got $bpp)"; exit 1 }
$off = 18
$pixels = New-Object byte[] ($w * $h * 4)
# TGA bottom-left origin: flip rows into a top-down PNG.
$rowBytes = $w * 4
for ($y = 0; $y -lt $h; $y++) {
    $srcY = $h - 1 - $y
    [Array]::Copy($bytes, $off + $srcY * $rowBytes, $pixels, $y * $rowBytes, $rowBytes)
}
Add-Type -AssemblyName PresentationCore
$fmt = [System.Windows.Media.PixelFormats]::Bgra32
$img = [System.Windows.Media.Imaging.WriteableBitmap]::new($w, $h, 96, 96, $fmt, $null)
$img.WritePixels((New-Object System.Windows.Int32Rect(0, 0, $w, $h)), $pixels, $rowBytes, 0)
$enc = [System.Windows.Media.Imaging.PngBitmapEncoder]::new()
$enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($img))
$fs = [IO.File]::Create((Join-Path (Get-Location) $dst))
$enc.Save($fs)
$fs.Close()
Write-Host "saved $dst"
