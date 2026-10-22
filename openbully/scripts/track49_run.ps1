# Track 49: Boss Fights verification.
# Phase 1: OPENBULLY_BOSS_DEMO=1 run - scripted engagement, phase transitions,
#          specials, QTE success/fail, retry, victory cutscene + mission.
#          Screenshots catch the boss health bar + QTE prompt + banner.
# Phase 2: 2-minute stability run (no demo; boss idles until approached).
Set-Location $PSScriptRoot\..

Write-Output "=== Phase 1: Boss demo (40s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_BOSS_DEMO = '1'
    $env:OPENBULLY_SHOT       = 'track49_boss'
    $env:OPENBULLY_SHOT_DELAY = '5'
    $env:OPENBULLY_SHOT_EVERY = '2'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track49_run.log" -Encoding utf8
}
Start-Sleep -Seconds 40
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "=== Boss log: spawn/engage/phase/special/QTE/victory lines ==="
Get-Content track49_run.log | Select-String -Pattern "BOSS|QTE|boss_duel|Cutscenes" | Select-Object -First 60
Write-Output "=== Errors ==="
Get-Content track49_run.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
Write-Output "=== Screenshots ==="
Get-ChildItem -Path . -Filter "track49_boss*.tga" | Select-Object Name, Length

# Phase 2: Stability test (120s)
Write-Output "`n=== Phase 2: Stability test (120s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track49_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 120
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null
Remove-Job $job2 -Force

Write-Output "=== Stability: last lines ==="
Get-Content track49_stab.log -Tail 6
Write-Output "=== Stability: errors ==="
Get-Content track49_stab.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
