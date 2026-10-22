# Phase 9 integration: vehicle integration run (DEMO=2 orbit drive).
# Mounts nearest vehicle, closed-loop orbit exercises throttle/brake/steer,
# 1 Hz Vehicle log is the physics evidence. Screenshots via OPENBULLY_SHOT.
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_VEHICLE_DEMO = '2'
$env:OPENBULLY_SHOT = 'phase9_drive'
$env:OPENBULLY_SHOT_EVERY = '6'
$env:OPENBULLY_SHOT_DELAY = '4'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase9_run.log"
}
Start-Sleep -Seconds 45
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "--- vehicle log lines ---"
Get-Content phase9_run.log | Select-String -Pattern "Vehicle|kill-plane|respawn" | Select-Object -Last 60
Write-Output "--- errors ---"
Get-Content phase9_run.log | Select-String -Pattern "ERROR|assert" | Select-Object -First 10
Write-Output "--- shots ---"
Get-ChildItem phase9_drive*.tga | Select-Object Name, Length
