# Phases 12-15 final integration: combined smoke test.
# Run A (coexistence, ~75s): CLASS_DEMO walks Jimmy to class (Track 41),
#   MINIGAME_DEMO auto-plays the activity (Track 44), ARCADE_DEMO drives the
#   cabinet loop when the class window is free (Track 45), CUTSCENE_DEMO
#   re-plays the scripted cutscene between systems (Track 48), WANTED_DEMO
#   runs its assault->bust->escape cycle (Track 43). Class minigame claims
#   input first; arcade yields while class/minigame open (same driver order
#   as the per-track verifications).
# Run B (stability, 130s): plain open-world run — classes + schedules live,
#   cutscenes armed, photo album system resident, trick scoring armed on
#   mount, bosses + wanted on their idle triggers. No position drivers.
# Screenshots: 3 in run A (class minigame, arcade UI, wanted HUD window).
Set-Location $PSScriptRoot\..

Write-Output "=== Run A: coexistence (80s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_CLASS_DEMO    = '1'
    $env:OPENBULLY_MINIGAME_DEMO = '1'
    $env:OPENBULLY_ARCADE_DEMO   = '1'
    $env:OPENBULLY_CUTSCENE_DEMO = '1'
    $env:OPENBULLY_WANTED_DEMO   = '1'
    $env:OPENBULLY_TIME_START    = '08:57'
    $env:OPENBULLY_NO_CUTSCENE   = '1'
    $env:OPENBULLY_SHOT          = 'p1215_combo'
    $env:OPENBULLY_SHOT_DELAY    = '8'
    $env:OPENBULLY_SHOT_EVERY    = '15'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "p1215_combo.log" -Encoding utf8
}
Start-Sleep -Seconds 80
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "--- per-system evidence ---"
Get-Content p1215_combo.log | Select-String -Pattern "Class session|Class demo|CLASSES|MINIGAME|Word Scramble" | Select-Object -First 12
Get-Content p1215_combo.log | Select-String -Pattern "Arcade" | Select-Object -First 12
Get-Content p1215_combo.log | Select-String -Pattern "WANTED|BUSTED|escape" | Select-Object -First 12
Get-Content p1215_combo.log | Select-String -Pattern "Cutscene" | Select-Object -First 8
Write-Output "--- errors ---"
Get-Content p1215_combo.log | Select-String -Pattern "ERROR|assert|crash|EXCEPTION" | Select-Object -First 10

Write-Output "`n=== Run B: stability (135s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_TIME_START = '08:55'
    Remove-Item Env:OPENBULLY_CLASS_DEMO    -ErrorAction SilentlyContinue
    Remove-Item Env:OPENBULLY_MINIGAME_DEMO -ErrorAction SilentlyContinue
    Remove-Item Env:OPENBULLY_ARCADE_DEMO   -ErrorAction SilentlyContinue
    Remove-Item Env:OPENBULLY_CUTSCENE_DEMO -ErrorAction SilentlyContinue
    Remove-Item Env:OPENBULLY_WANTED_DEMO   -ErrorAction SilentlyContinue
    Remove-Item Env:OPENBULLY_SHOT          -ErrorAction SilentlyContinue
    Remove-Item Env:OPENBULLY_SHOT_DELAY    -ErrorAction SilentlyContinue
    Remove-Item Env:OPENBULLY_SHOT_EVERY    -ErrorAction SilentlyContinue
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "p1215_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 135
$alive = -not ($job2.State -eq 'Failed')
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null
Remove-Job $job2 -Force

Write-Output "--- stability: frame timing count (1/s => ~130 expected) ---"
(Get-Content p1215_stab.log | Select-String "Frame timing").Count
Write-Output "--- stability: last lines ---"
Get-Content p1215_stab.log -Tail 6
Write-Output "--- stability: errors ---"
Get-Content p1215_stab.log | Select-String -Pattern "ERROR|assert|crash|EXCEPTION" | Select-Object -First 10
Write-Output "--- screenshots ---"
Get-ChildItem -Path . -Filter "p1215_combo*.tga" | Select-Object Name, Length
