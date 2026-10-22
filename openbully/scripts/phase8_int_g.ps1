Set-Location $PSScriptRoot\..
# Phase 8 Integration Run G: Track 28 reactions. MELEE_DEMO has Jimmy attack
# NPCs -> DamageNpc -> Hit events -> fight back / help calls / crowd / prefect
# response / KO -> respawn. TIME_DEMO churns schedules concurrently.
Remove-Item Env:OPENBULLY_NPC_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_DIALOGUE_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_MELEE_DEMO = '1'
$env:OPENBULLY_TIME_DEMO  = '1'
$env:OPENBULLY_TIME_START = '08:00'
$env:OPENBULLY_SHOT       = 'phase8_react'
$env:OPENBULLY_SHOT_DELAY = '5'
$env:OPENBULLY_SHOT_EVERY = '30'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase8_run_g.log"
}
Start-Sleep -Seconds 150
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 15 | Out-Null
Remove-Job $job -Force
Write-Output "=== reactions ==="
Get-Content phase8_run_g.log | Select-String 'fights back|flees \(|answers help call|saw the fight|BUSTED' | Select-Object -First 12
Write-Output "=== counts ==="
'LCHASE','LATTACK','LFLEE' | Out-Null
Write-Output ("fights-back: " + (Get-Content phase8_run_g.log | Select-String 'fights back').Count)
Write-Output ("help-call: " + (Get-Content phase8_run_g.log | Select-String 'answers help call').Count)
Write-Output ("prefect-fight: " + (Get-Content phase8_run_g.log | Select-String 'saw the fight').Count)
Write-Output ("flee: " + (Get-Content phase8_run_g.log | Select-String 'flees \(').Count)
Write-Output ("KO: " + (Get-Content phase8_run_g.log | Select-String "KO'd").Count)
Write-Output ("respawn: " + (Get-Content phase8_run_g.log | Select-String 'respawned at spawn point').Count)
Write-Output ("BUSTED: " + (Get-Content phase8_run_g.log | Select-String 'BUSTED').Count)
Write-Output ("melee-hits: " + (Get-Content phase8_run_g.log | Select-String 'Melee: .* hit NPC').Count)
Write-Output ("npc-punches-on-player: " + (Get-Content phase8_run_g.log | Select-String 'Hit \(NPC punch\)').Count)
Write-Output "=== tail ==="
Get-Content phase8_run_g.log -Tail 4
