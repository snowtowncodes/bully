# Track 48: Cutscene playback verification.
# Phase 1: OPENBULLY_CUTSCENE_DEMO=1 run — scripted cutscene cycles (camera
#          keyframes, actor moves, subtitles); screenshots catch the scripted
#          camera view, subtitle strip and the skip hint.
# Phase 2: 2-minute stability run (cutscene demo looping).
Set-Location $PSScriptRoot\..

# Phase 1: Cutscene demo (35s)
Write-Output "=== Phase 1: Cutscene demo (35s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_CUTSCENE_DEMO = '1'
    $env:OPENBULLY_SHOT          = 'track48_cut'
    $env:OPENBULLY_SHOT_DELAY    = '4'
    $env:OPENBULLY_SHOT_EVERY    = '3'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track48_run.log" -Encoding utf8
}
Start-Sleep -Seconds 35
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "=== Cutscene log: playback / actions / subtitles / mission hooks ==="
Get-Content track48_run.log | Select-String -Pattern "Cutscenes|CUTSCENE|subtitle|SKIP|skip" | Select-Object -First 50
Write-Output "=== Errors ==="
Get-Content track48_run.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
Write-Output "=== Screenshots ==="
Get-ChildItem -Path . -Filter "track48_cut*.tga" | Select-Object Name, Length

# Phase 2: Stability test (125s, cutscene demo loops)
Write-Output "`n=== Phase 2: Stability test (125s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_CUTSCENE_DEMO = '1'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track48_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 125
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null
Remove-Job $job2 -Force

Write-Output "=== Stability: cutscene cycle count ==="
(Get-Content track48_stab.log | Select-String -Pattern "playing").Count
Write-Output "=== Stability: last lines ==="
Get-Content track48_stab.log -Tail 6
Write-Output "=== Stability: errors ==="
Get-Content track48_stab.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
