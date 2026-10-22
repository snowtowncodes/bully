# Capture a screenshot of the OpenBully window (by title) to a BMP.
# Usage: powershell -File capture_openbully.ps1 [-Title OpenBully] [-Out openbully_geometry.bmp]
param(
    [string]$Title = "OpenBully",
    [string]$Out = "openbully_geometry.bmp"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Cap {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$hwnd = [Win32Cap]::FindWindow($null, $Title)
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Error "window not found: $Title"
    exit 1
}
[Win32Cap]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400

$r = New-Object Win32Cap+RECT
[Win32Cap]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) { Write-Error "bad rect"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out (${w}x${h})"
