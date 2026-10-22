# Track 42: Respect & Reputation verification.
# Phase 1: OPENBULLY_FACTION_DEMO run - beat-down (respect loss + witnesses),
#          gift (+respect), stats overlay open at t=10; screenshots catch it.
# Phase 2: save/load demo - faction scores round-trip through slot_0.json.
# Phase 3: 2-minute stability run.
Set-Location $PSScriptRoot\..
$exe = ".\build\Release\openbully.exe"

# Phase 1: faction demo (30s), shots every 2s from t=12.
Write-Output "=== Phase 1: faction demo (30s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_FACTION_DEMO = '1'
    $env:OPENBULLY_SHOT         = 'track42_stats'
    $env:OPENBULLY_SHOT_DELAY   = '12'
    $env:OPENBULLY_SHOT_EVERY   = '2'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "track42_run.log" -Encoding utf8
}
Start-Sleep -Seconds 30
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "=== Respect lines ==="
Get-Content track42_run.log | Select-String -Pattern "RESPECT|GIFT|MISSION COMPLETE|Faction demo|Faction stats|answers help call" | Select-Object -First 50
Write-Output "=== Errors ==="
Get-Content track42_run.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
Write-Output "=== Screenshots ==="
Get-ChildItem -Path . -Filter "track42_stats*.tga" | Select-Object Name, Length

# Phase 2: save/load (faction + save demo; save t=5, load t=15, run 20s).
Write-Output "`n=== Phase 2: save/load (20s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_FACTION_DEMO = '1'
    $env:OPENBULLY_SAVE_DEMO    = '1'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "track42_save.log" -Encoding utf8
}
Start-Sleep -Seconds 20
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null
Remove-Job $job2 -Force

Write-Output "=== Save/load faction lines ==="
Get-Content track42_save.log | Select-String -Pattern "SaveSystem|Reputation|RESPECT-CENSUS: OVERALL|load demo|Save demo" | Select-Object -First 25
$saveJson = ".\build\Release\saves\slot_0.json"
if (Test-Path $saveJson) {
    Write-Output "=== slot_0.json reputation block ==="
    Get-Content $saveJson | Select-String -Pattern "reputation|respect|completed_missions" -Context 0,2 | Select-Object -First 6
} else {
    Write-Output "slot_0.json NOT FOUND"
}

# Phase 3: 2-minute stability (plain run).
Write-Output "`n=== Phase 3: stability (120s) ==="
$job3 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "track42_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 125
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job3 -Timeout 10 | Out-Null
Remove-Job $job3 -Force

Write-Output "=== Stability: last lines ==="
Get-Content track42_stab.log -Tail 6
Write-Output "=== Stability: errors ==="
Get-Content track42_stab.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
