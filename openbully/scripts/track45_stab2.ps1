# Track 45: 2-minute stability (arcade demo cycling, no screenshots).
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_ARCADE_DEMO = '1'
Remove-Item Env:OPENBULLY_SHOT -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_SHOT_DELAY -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_SHOT_EVERY -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_SAVE_DEMO -ErrorAction SilentlyContinue
$p = Start-Process -FilePath '.\build\Release\openbully.exe' `
    -RedirectStandardOutput 'track45_stab2.log' -PassThru
Start-Sleep -Seconds 122
$alive = -not $p.HasExited
Write-Output ("alive after 122s: " + $alive)
if ($alive) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
Write-Output "--- arcade cycle summary ---"
Get-Content track45_stab2.log | Select-String "Arcade" | Select-Object -First 40
Write-Output "--- errors ---"
Get-Content track45_stab2.log | Select-String "ERROR|EXCEPTION|assert" | Select-Object -First 5
Write-Output "--- frame timing last ---"
Get-Content track45_stab2.log | Select-String "Frame timing" | Select-Object -Last 2
Write-Output "--- total frame-timing logs (1/s => ~120 expected) ---"
(Get-Content track45_stab2.log | Select-String "Frame timing").Count
