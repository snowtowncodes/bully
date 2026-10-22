Set-Location $PSScriptRoot\..
# Phase 8 Integration Run D: Track 27 AI + Track 28 reactions focus.
# NPC demo drives Jimmy into NPCs -> ALERT/CHASE/ATTACK, KO drops to 20% ->
# FLEE, KO -> respawn; witnesses crowd + prefect respond; BUSTED possible if
# a prefect downs the player. No dialogue so combat flows freely.
Remove-Item Env:OPENBULLY_MELEE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_TIME_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_DIALOGUE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_NPC_DEMO   = '1'
$env:OPENBULLY_TIME_START = '08:00'
$env:OPENBULLY_SHOT       = 'phase8_combat'
$env:OPENBULLY_SHOT_DELAY = '5'
$env:OPENBULLY_SHOT_EVERY = '30'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase8_run_d.log"
}
Start-Sleep -Seconds 150
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 15 | Out-Null
Remove-Job $job -Force
Write-Output "=== transitions by type ==="
'CHASE','ATTACK','FLEE','RETURN','WATCH','KO' | ForEach-Object {
    $n = (Get-Content phase8_run_d.log | Select-String "-> $_`b").Count
    Write-Output "$_ : $n"
}
Write-Output "=== KOs / respawns ==="
(Get-Content phase8_run_d.log | Select-String "KO'd").Count
(Get-Content phase8_run_d.log | Select-String 'respawned at spawn point').Count
Write-Output "=== reactions ==="
Get-Content phase8_run_d.log | Select-String 'fights back|flees|answers help call|saw the fight|BUSTED' | Select-Object -First 15
Write-Output "=== player damage taken ==="
Get-Content phase8_run_d.log | Select-String 'Hit \(NPC punch\)' | Select-Object -First 8
Write-Output "=== tail ==="
Get-Content phase8_run_d.log -Tail 5
