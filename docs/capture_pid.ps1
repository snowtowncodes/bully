param([int]$ProcId, [string]$Out = "shot.bmp")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Cap2 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$found = [IntPtr]::Zero
$cb = {
  param($h, $l)
  $pid2 = 0
  [Cap2]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
  if ($pid2 -eq $ProcId -and [Cap2]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 256
    [Cap2]::GetWindowText($h, $sb, 256) | Out-Null
    if ($sb.ToString() -ne '') { $script:found = $h; return $false }
  }
  return $true
}
[Cap2]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($found -eq [IntPtr]::Zero) { Write-Error "window for pid $ProcId not found"; exit 1 }
[Cap2]::SetForegroundWindow($found) | Out-Null
Start-Sleep -Milliseconds 400
$r = New-Object Cap2+RECT
[Cap2]::GetWindowRect($found, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h2 = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $h2)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h2)))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out (${w}x${h2})"
