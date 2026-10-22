# Track 41: Class System verification.
# Phase 1: OPENBULLY_CLASS_DEMO=1 + OPENBULLY_MINIGAME_DEMO=1 — the class demo
#          walks (teleports) Jimmy to the live-window class and attends; Track
#          44's minigame demo then auto-plays the class activity (English ->
#          Word Scramble) through pass -> rewards -> schedule overlay;
#          screenshots every second catch prompt, minigame, pass and schedule.
# Phase 2: class demo + save/load (F5/F9 scripted) to prove class progression
#          survives a save round trip.
# Phase 3: 2-minute stability run (open world, classes + schedules live).
Set-Location $PSScriptRoot\..

# Phase 1: class demo (75s), game clock starts at 08:57 so the 09:00 English
# session is already in session within ~3 seconds.
Write-Output "=== Phase 1: Class demo (75s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_CLASS_DEMO    = '1'
    $env:OPENBULLY_MINIGAME_DEMO = '1'
    $env:OPENBULLY_NO_CUTSCENE   = '1'
    $env:OPENBULLY_TIME_START    = '08:57'
    $env:OPENBULLY_SHOT          = 'track41_cls'
    $env:OPENBULLY_SHOT_DELAY    = '2'
    $env:OPENBULLY_SHOT_EVERY    = '1'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track41_run.log" -Encoding utf8
}
Start-Sleep -Seconds 75
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "=== Class system log ==="
Get-Content track41_run.log | Select-String -Pattern "Classes:|Class:|Class session|Class demo|Reward:|Schedule:" | Select-Object -First 45
Write-Output "=== Errors ==="
Get-Content track41_run.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
Write-Output "=== Screenshots ==="
Get-ChildItem -Path . -Filter "track41_cls*.tga" | Select-Object Name, Length

# Phase 2: class demo + scripted save/load (save at t=5s, load at t=15s)
Write-Output "`n=== Phase 2: Class demo + save/load (30s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_CLASS_DEMO    = '1'
    $env:OPENBULLY_MINIGAME_DEMO = '1'
    $env:OPENBULLY_NO_CUTSCENE   = '1'
    $env:OPENBULLY_TIME_START    = '08:57'
    $env:OPENBULLY_SAVE_DEMO     = '1'
    $env:OPENBULLY_SHOT          = 'track41_save'
    $env:OPENBULLY_SHOT_DELAY    = '12'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track41_saveload.log" -Encoding utf8
}
Start-Sleep -Seconds 30
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null
Remove-Job $job2 -Force

Write-Output "=== Save/load class log ==="
Get-Content track41_saveload.log | Select-String -Pattern "Class|Save|Load|snapshot|restore|session" | Select-Object -First 40
Write-Output "=== Save file ==="
Get-ChildItem -Recurse -Filter "slot_0.json" -ErrorAction SilentlyContinue | Select-Object FullName, Length

# Phase 3: stability (120s)
Write-Output "`n=== Phase 3: Stability test (120s) ==="
$job3 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_TIME_START = '08:55'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track41_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 120
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job3 -Timeout 10 | Out-Null
Remove-Job $job3 -Force

Write-Output "=== Stability: last lines ==="
Get-Content track41_stab.log -Tail 6
Write-Output "=== Stability: errors ==="
Get-Content track41_stab.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
