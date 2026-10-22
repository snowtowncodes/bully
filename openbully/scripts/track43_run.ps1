# Track 43: Wanted System verification (Release build - the Debug bgfx
# readback asserts break on OPENBULLY_SHOT captures; see report).
# Phase 1: OPENBULLY_WANTED_DEMO=1 + screenshot - full end-to-end wanted flow.
# Phase 2: Debug build, demo WITHOUT screenshot - proves the demo itself is
#          crash-free on the assert-heavy Debug binary too.
Set-Location $PSScriptRoot\..

Write-Output "=== Phase 1: Wanted demo (Release, 60s, screenshot at t=6) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_WANTED_DEMO = '1'
    $env:OPENBULLY_SHOT        = 'track43_wanted'
    $env:OPENBULLY_SHOT_DELAY  = '6'
    $env:OPENBULLY_SHOT_EVERY  = '12'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track43_run.log" -Encoding utf8
    Write-Output "GAME EXIT CODE: $LASTEXITCODE"
}
Start-Sleep -Seconds 65
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 15 | Out-Null
Receive-Job $job1 -ErrorAction SilentlyContinue
Remove-Job $job1 -Force

Write-Output "=== Wanted log: crimes / chase / cops / decay / bust ==="
Get-Content track43_run.log | Select-String -Pattern "WANTED|BUSTED|cop|Cop" | Select-Object -First 45
Write-Output "=== Errors ==="
Get-Content track43_run.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
Write-Output "=== Screenshot ==="
Get-ChildItem -Path . -Filter "track43_wanted*" | Select-Object Name, Length

Write-Output "`n=== Phase 2: Debug demo, no screenshot (60s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_WANTED_DEMO = '1'
    $env:OPENBULLY_NO_CUTSCENE = '1'
    & ".\build2\Debug\openbully.exe" 2>&1 | Out-File -FilePath "track43_dbg.log" -Encoding utf8
}
Start-Sleep -Seconds 60
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 15 | Out-Null
Remove-Job $job2 -Force

Write-Output "=== Debug demo key lines ==="
Get-Content track43_dbg.log | Select-String -Pattern "WANTED|BUSTED" | Select-Object -First 30
Write-Output "=== Debug demo errors ==="
Get-Content track43_dbg.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
