Set-Location $PSScriptRoot\..
# Phase 8 Integration Run B: time-of-day + schedule focus (TIME_DEMO +1h every
# 3s => full day cycle in ~24s of script), NO npc demo (Jimmy idle). This
# maximizes schedule spawn/despawn churn to hunt the Track 27-reported crash.
Remove-Item Env:OPENBULLY_NPC_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MELEE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_DIALOGUE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_TIME_START = '06:00'
$env:OPENBULLY_TIME_DEMO  = '1'
$env:OPENBULLY_SHOT       = 'phase8_sched'
$env:OPENBULLY_SHOT_DELAY = '3'
$env:OPENBULLY_SHOT_EVERY = '20'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase8_run_b.log"
}
Start-Sleep -Seconds 150
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 15 | Out-Null
Remove-Job $job -Force
Write-Output "=== exit ==="
Write-Output "=== schedules ==="
Get-Content phase8_run_b.log | Select-String -Pattern "Schedules:|Schedule\[" | Select-Object -First 60
Write-Output "=== spawns/despawns count ==="
(Get-Content phase8_run_b.log | Select-String -Pattern "Schedule\[.*spawned").Count
(Get-Content phase8_run_b.log | Select-String -Pattern "Schedule\[.*despawned").Count
Write-Output "=== tail ==="
Get-Content phase8_run_b.log -Tail 6
