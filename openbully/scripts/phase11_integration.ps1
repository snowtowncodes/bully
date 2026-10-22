# Phase 11 Integration smoke test.
# Covers: Track 38 (melee weapons), Track 39 (ranged), Track 40 (journal)
# plus cross-system interactions and a 2-minute stability run.
Set-Location $PSScriptRoot\..

function Scan-Log($log, $patterns, $label) {
    Write-Output "--- $label ---"
    $hits = Get-Content $log | Select-String -Pattern $patterns
    if ($hits) { $hits | Select-Object -First 25 | ForEach-Object { $_.Line } }
    else { Write-Output "(no matches)" }
    $errs = Get-Content $log | Select-String -Pattern "ERROR|assert failed|crash|Access violation"
    Write-Output "[$label errors: $(@($errs).Count)]"
    if ($errs) { $errs | Select-Object -First 5 | ForEach-Object { $_.Line } }
}

# ============ Test 1: Weapon demo (Track 38+39: pickup, equip, melee, slingshot fire, ammo) ============
Write-Output "=== Test 1: Weapon demo (30s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_WEAPON_DEMO = '1'
    $env:OPENBULLY_SHOT        = 'phase11_weapon'
    $env:OPENBULLY_SHOT_DELAY  = '3'
    $env:OPENBULLY_SHOT_EVERY  = '5'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase11_t1_weapon.log" -Encoding utf8
}
Start-Sleep -Seconds 30
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null; Remove-Job $job1 -Force
Scan-Log "phase11_t1_weapon.log" "Picked up|Equipped|Weapon demo|fired|Projectile|Ammo|ammo|Reload|HEADSHOT|bounce|blocked|reaction|AI state|Journal" "T1 weapon/ranged lines"

# ============ Test 2: Journal demo (Track 40 + interactions) ============
Write-Output "`n=== Test 2: Journal demo (30s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_JOURNAL_DEMO = '1'
    $env:OPENBULLY_WEAPON_DEMO  = '1'   # coexistence: combat runs while journal cycles
    $env:OPENBULLY_SHOT         = 'phase11_journal'
    $env:OPENBULLY_SHOT_DELAY   = '2'
    $env:OPENBULLY_SHOT_EVERY   = '4'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase11_t2_journal.log" -Encoding utf8
}
Start-Sleep -Seconds 30
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null; Remove-Job $job2 -Force
Scan-Log "phase11_t2_journal.log" "Journal|MISSION|objective|Objective|Picked up|Equipped|fired|Weapon demo|Projectile" "T2 journal+combat lines"

# ============ Test 3: 2-minute stability, all systems active ============
Write-Output "`n=== Test 3: Stability (120s) ==="
$job3 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_JOURNAL_DEMO = '1'
    $env:OPENBULLY_WEAPON_DEMO  = '1'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase11_t3_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 121
$proc = Get-Process -Name openbully -ErrorAction SilentlyContinue
if ($proc) { Write-Output "Process alive at 120s: YES"; Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue }
else { Write-Output "Process alive at 120s: NO (crashed?)" }
Wait-Job $job3 -Timeout 10 | Out-Null; Remove-Job $job3 -Force
$tail = Get-Content "phase11_t3_stab.log" -Tail 5
Write-Output "--- Stability tail ---"
$tail
$errs = Get-Content "phase11_t3_stab.log" | Select-String -Pattern "ERROR|assert failed|crash|Access violation"
Write-Output "[stability errors: $(@($errs).Count)]"
if ($errs) { $errs | Select-Object -First 8 | ForEach-Object { $_.Line } }
$frames = (Get-Content "phase11_t3_stab.log" | Select-String -Pattern "^frame|frame ").Count
Write-Output "[frames logged: $frames]"

Write-Output "`n=== Screenshots ==="
Get-ChildItem -Path . -Filter "phase11_*.tga" | Select-Object Name, Length
