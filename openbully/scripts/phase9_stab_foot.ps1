# Phase 9 stability A: on-foot kill-plane stress. AUTO_WALK holds Forward so
# Jimmy walks off the rooftop edge again and again; each previously-fatal fall
# must now end in a kill-plane respawn (counted from the log).
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
Remove-Item Env:OPENBULLY_VEHICLE_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_AUTO_WALK = '1'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase9_stab_foot.log"
}
Start-Sleep -Seconds 150
$alive = Get-Process -Name openbully -ErrorAction SilentlyContinue
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "process alive at t=150s: $($null -ne $alive)"
$log = Get-Content phase9_stab_foot.log
Write-Output ("kill-plane respawns: " + ($log | Select-String -Pattern 'kill-plane' | Measure-Object).Count)
Write-Output ("last Jimmy line:     " + ($log | Select-String -Pattern 'Jimmy: \(' | Select-Object -Last 1).Line)
Write-Output ("last frame timing:   " + ($log | Select-String -Pattern 'Frame timing' | Select-Object -Last 1).Line)
Write-Output "--- errors ---"
$log | Select-String -Pattern "ERROR|assert" | Select-Object -First 5
