# Phase 9 stability B: vehicle DEMO=2 orbit drive, 150 s soak.
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
Remove-Item Env:OPENBULLY_AUTO_WALK -ErrorAction SilentlyContinue
$env:OPENBULLY_VEHICLE_DEMO = '2'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase9_stab_veh.log"
}
Start-Sleep -Seconds 150
$alive = Get-Process -Name openbully -ErrorAction SilentlyContinue
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "process alive at t=150s: $($null -ne $alive)"
$log = Get-Content phase9_stab_veh.log
Write-Output ("vehicle t= samples:  " + ($log | Select-String -Pattern 'Vehicle: t=' | Measure-Object).Count)
Write-Output ("void-guard resets:   " + ($log | Select-String -Pattern 'void guard' | Measure-Object).Count)
Write-Output ("last frame timing:   " + ($log | Select-String -Pattern 'Frame timing' | Select-Object -Last 1).Line)
Write-Output ("last Vehicle line:   " + ($log | Select-String -Pattern 'Vehicle: t=' | Select-Object -Last 1).Line)
Write-Output "--- errors ---"
$log | Select-String -Pattern "ERROR|assert" | Select-Object -First 5
