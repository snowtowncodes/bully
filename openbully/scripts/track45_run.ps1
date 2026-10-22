# Track 45: Arcade games verification.
# Phase 1: OPENBULLY_ARCADE_DEMO=1 with a screenshot at t=10 (inside the
#          first Nut Shots round). NOTE: a pre-existing tree regression
#          kills the process right after ANY backbuffer capture (reproduces
#          with no arcade code involved - iso C control); the capture file
#          itself lands fine, so we still verify the arcade UI shot, then
#          expect the exit.
# Phase 2: 2-minute stability WITHOUT screenshots (the crashing subsystem)
#          + save/load demo: entry, F9 mid-round exit, re-entry, full round,
#          prize, exit, rotation to Monkey Fling.
Set-Location $PSScriptRoot\..

Write-Output "=== Phase 1: Arcade demo + in-view screenshot (25s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_ARCADE_DEMO = '1'
    $env:OPENBULLY_SHOT        = 'track45_arc'
    $env:OPENBULLY_SHOT_DELAY  = '10'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "track45_run.log" -Encoding utf8
}
Start-Sleep -Seconds 25
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "=== Arcade log: cabinets / entry / rounds / prizes ==="
Get-Content track45_run.log | Select-String -Pattern "Arcade:|Screenshot" | Select-Object -First 20
Write-Output "=== Screenshot file ==="
Get-ChildItem -Path . -Filter "track45_arc*.tga" | Select-Object Name, Length, LastWriteTime

# Phase 2: stability (no screenshots) 120s
Write-Output "`n=== Phase 2: Stability 120s + SAVE_DEMO round-trip ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_ARCADE_DEMO = '1'
    $env:OPENBULLY_SAVE_DEMO   = '1'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "track45_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 120
$wasAlive = Get-Process -Name openbully -ErrorAction SilentlyContinue
Write-Output ("process alive at 120s: " + ($null -ne $wasAlive))
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null
Remove-Job $job2 -Force

Write-Output "=== Stability: arcade + save lines ==="
Get-Content track45_stab.log | Select-String -Pattern "Arcade|Save demo" | Select-Object -First 30
Write-Output "=== Stability: errors ==="
Get-Content track45_stab.log | Select-String -Pattern "ERROR|assert|EXCEPTION" | Select-Object -First 10
Write-Output "=== Save file arcade section ==="
$save = ".\build\Release\saves\slot_0.json"
if (Test-Path $save) {
    Get-Content $save | Select-String -Pattern "high_scores|plays|wins|achievement" | Select-Object -First 6
} else { Write-Output "NO SAVE FILE at $save" }
Write-Output "=== Frame timing (last 3) ==="
Get-Content track45_stab.log | Select-String "Frame timing" | Select-Object -Last 3
