Set-Location $PSScriptRoot\..
# Phase 8 Integration Run A: full-system soak across the crash window (~50-125s).
# All Phase 8 hooks active: NPC AI demo (Track 27), schedules (Track 29),
# dialogue demo (Track 30). Track 28 reactions trigger from the NPC demo's
# fights (help calls, crowds, prefect busts). Recurring shots for evidence.
Remove-Item Env:OPENBULLY_MELEE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_TIME_DEMO  -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO   -ErrorAction SilentlyContinue
$env:OPENBULLY_NPC_DEMO      = '1'
$env:OPENBULLY_DIALOGUE_DEMO = '1'
$env:OPENBULLY_TIME_START    = '08:00'
$env:OPENBULLY_SHOT          = 'phase8_int'
$env:OPENBULLY_SHOT_DELAY    = '3'
$env:OPENBULLY_SHOT_EVERY    = '30'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase8_run_a.log"
}
Start-Sleep -Seconds 180
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 15 | Out-Null
Remove-Job $job -Force
Write-Output "=== exit ==="
Write-Output "=== Phase 8: schedules ==="
Get-Content phase8_run_a.log | Select-String -Pattern "Schedules:|Schedule\[" | Select-Object -First 40
Write-Output "=== Phase 8: dialogue ==="
Get-Content phase8_run_a.log | Select-String -Pattern "Dialogue" | Select-Object -First 20
Write-Output "=== Phase 8: BUSTED / KO / prefect ==="
Get-Content phase8_run_a.log | Select-String -Pattern "BUSTED|KO'd|PREFECT|prefect" | Select-Object -First 20
Write-Output "=== Phase 8: AI state transitions ==="
Get-Content phase8_run_a.log | Select-String -Pattern "NPC AI: entity \d+ \w+ -> \w+" | Select-Object -First 40
Write-Output "=== tail ==="
Get-Content phase8_run_a.log -Tail 6
