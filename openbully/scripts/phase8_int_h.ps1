Set-Location $PSScriptRoot\..
# Phase 8 Integration Run H: FINAL soak, 240s, all Phase 8 systems at once,
# new binary (disperse-on-sight + Alert-talk + dialogue auto-advance).
Remove-Item Env:OPENBULLY_MELEE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_NPC_DEMO      = '1'
$env:OPENBULLY_DIALOGUE_DEMO = '1'
$env:OPENBULLY_TIME_DEMO     = '1'
$env:OPENBULLY_TIME_START    = '07:00'
$env:OPENBULLY_SHOT          = 'phase8_final'
$env:OPENBULLY_SHOT_DELAY    = '3'
$env:OPENBULLY_SHOT_EVERY    = '40'
$proc = Start-Process -FilePath ".\build\Release\openbully.exe" `
    -WorkingDirectory (Get-Location) -RedirectStandardOutput "phase8_run_h.log" `
    -RedirectStandardError "phase8_run_h_err.log" -PassThru -NoNewWindow
$exited = $proc.WaitForExit(240000)
if (-not $exited) {
    Write-Output "STABILITY PASS: survived 240s (killed by harness)"
    $proc.Kill(); $proc.WaitForExit(10000) | Out-Null
} else {
    Write-Output ("PROCESS EXITED EARLY with code " + $proc.ExitCode)
}
Write-Output "=== coverage summary ==="
$log = Get-Content phase8_run_h.log
Write-Output ("talk-starts: " + ($log | Select-String 'talk started').Count)
Write-Output ("conv-ends: " + ($log | Select-String 'conversation ended').Count)
Write-Output ("chases: " + ($log | Select-String '-> CHASE').Count)
Write-Output ("attacks: " + ($log | Select-String '-> ATTACK').Count)
Write-Output ("fights-back: " + ($log | Select-String 'fights back').Count)
Write-Output ("KO: " + ($log | Select-String "KO'd").Count)
Write-Output ("schedule-spawns: " + ($log | Select-String 'Schedule\[.*spawned').Count)
Write-Output ("schedule-despawns: " + ($log | Select-String 'Schedule\[.*despawned').Count)
Write-Output ("BUSTED: " + ($log | Select-String 'BUSTED').Count)
Write-Output "=== tail ==="
Get-Content phase8_run_h.log -Tail 3
