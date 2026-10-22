param([int]$ProcId)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class F2 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetFocus(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
"@
$found = [IntPtr]::Zero
$cb = { param($h,$l)
  $p=0; [F2]::GetWindowThreadProcessId($h,[ref]$p)|Out-Null
  if ($p -eq $ProcId -and [F2]::IsWindowVisible($h)) {
    $sb=New-Object System.Text.StringBuilder 256; [F2]::GetWindowText($h,$sb,256)|Out-Null
    if ($sb.ToString() -ne '') { $script:found=$h; return $false } }
  return $true }
[F2]::EnumWindows($cb,[IntPtr]::Zero)|Out-Null
if ($found -eq [IntPtr]::Zero) { exit 1 }
for ($i=0; $i -lt 5; $i++) { [F2]::SetForegroundWindow($found)|Out-Null; Start-Sleep -Milliseconds 200 }
$fg = [F2]::GetForegroundWindow()
Write-Host ("focused: " + ($fg -eq $found))
