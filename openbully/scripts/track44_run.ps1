# Track 44: Minigame framework verification.
# Phase 1: OPENBULLY_MINIGAME_DEMO=1 run — scripted scramble WIN -> quiz FAIL
#          + R retry -> quiz WIN -> mash ESC-forfeit -> mash WIN. Screenshots
#          every 4s catch intro/playing/result overlays for all three games.
# Phase 2: 2-minute stability run.
Set-Location $PSScriptRoot\..
$exe = ".\build2\RelWithDebInfo\openbully.exe"

Write-Output "=== Phase 1: Minigame demo (40s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_MINIGAME_DEMO = '1'
    $env:OPENBULLY_SHOT          = 'track44_minigame'
    $env:OPENBULLY_SHOT_DELAY    = '2'
    $env:OPENBULLY_SHOT_EVERY    = '4'
    & ".\build2\RelWithDebInfo\openbully.exe" 2>&1 | Out-File -FilePath "track44_run.log" -Encoding utf8
}
Start-Sleep -Seconds 40
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "=== Minigame log: lifecycle + results ==="
Get-Content track44_run.log | Select-String -Pattern "Minigame:|MINIGAME|Class|progress" | Select-Object -First 40
Write-Output "=== Errors ==="
Get-Content track44_run.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
Write-Output "=== Screenshots ==="
Get-ChildItem -Path . -Filter "track44_minigame*.tga" | Select-Object Name, Length
Write-Output "=== Progress file ==="
if (Test-Path "build2\RelWithDebInfo\saves\minigames.json") {
    Get-Content "build2\RelWithDebInfo\saves\minigames.json"
} else { "minigames.json not found" }

# Phase 2: Stability test (120s)
Write-Output "`n=== Phase 2: Stability test (120s) ==="
$job2 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build2\RelWithDebInfo\openbully.exe" 2>&1 | Out-File -FilePath "track44_stab.log" -Encoding utf8
}
Start-Sleep -Seconds 120
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job2 -Timeout 10 | Out-Null
Remove-Job $job2 -Force

Write-Output "=== Stability: last lines ==="
Get-Content track44_stab.log -Tail 6
Write-Output "=== Stability: errors ==="
Get-Content track44_stab.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
