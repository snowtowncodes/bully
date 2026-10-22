Set-Location $PSScriptRoot\..
# Phase 8 Integration Run F: dialogue end-to-end in a calm scenario.
# OPENBULLY_NPC_COUNT=0 removes the legacy crowd; only scheduled NPCs spawn
# (07:00+ students/prefect/teacher wander near Jimmy). DIALOGUE_DEMO retries
# every 2s, auto-advances lines, picks choices, ends -> mission hook.
Remove-Item Env:OPENBULLY_NPC_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MELEE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_TIME_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_NPC_COUNT     = '4'
# (Jimmy is position-driven by the dialogue demo itself; JIMMY_DEMO removed -
#  it walks the character off the rooftop: no plane guard.)
$env:OPENBULLY_TIME_START    = '07:00'
$env:OPENBULLY_DIALOGUE_DEMO = '1'
$env:OPENBULLY_SHOT          = 'phase8_talk'
$env:OPENBULLY_SHOT_DELAY    = '3'
$env:OPENBULLY_SHOT_EVERY    = '10'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase8_run_f.log"
}
Start-Sleep -Seconds 110
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 15 | Out-Null
Remove-Job $job -Force
Write-Output "=== dialogue lifecycle ==="
Get-Content phase8_run_f.log | Select-String 'talk started|picking choice|conversation ended|completing TALK|started tree' | Select-Object -First 25
Write-Output "=== counts ==="
(Get-Content phase8_run_f.log | Select-String 'talk started').Count
(Get-Content phase8_run_f.log | Select-String 'conversation ended').Count
(Get-Content phase8_run_f.log | Select-String 'completing TALK').Count
Write-Output "=== schedules (spawn + interplay) ==="
Get-Content phase8_run_f.log | Select-String 'Schedule\[' | Select-Object -First 10
Write-Output "=== tail ==="
Get-Content phase8_run_f.log -Tail 4
