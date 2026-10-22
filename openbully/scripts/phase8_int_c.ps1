Set-Location $PSScriptRoot\..
# Phase 8 Integration Run C: dialogue + schedule interplay (despawn guard),
# plus mid-fight BUSTED path. TIME_DEMO churns schedules while DIALOGUE_DEMO
# keeps starting conversations; NPC demo off so conversations start freely.
Remove-Item Env:OPENBULLY_NPC_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MELEE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_TIME_START    = '07:00'
$env:OPENBULLY_TIME_DEMO     = '1'
$env:OPENBULLY_DIALOGUE_DEMO = '1'
$env:OPENBULLY_SHOT          = 'phase8_dlg'
$env:OPENBULLY_SHOT_DELAY    = '3'
$env:OPENBULLY_SHOT_EVERY    = '20'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase8_run_c.log"
}
Start-Sleep -Seconds 150
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 15 | Out-Null
Remove-Job $job -Force
Write-Output "=== exit ==="
Write-Output "=== dialogue events ==="
Get-Content phase8_run_c.log | Select-String -Pattern "Dialogue" | Select-Object -First 40
Write-Output "=== schedule events ==="
Get-Content phase8_run_c.log | Select-String -Pattern "Schedule\[" | Select-Object -First 30
Write-Output "=== tail ==="
Get-Content phase8_run_c.log -Tail 6
