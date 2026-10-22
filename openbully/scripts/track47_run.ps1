# Track 47: Photo Challenges verification.
# Phase 1: OPENBULLY_PHOTO_DEMO=1 run — camera equip -> viewfinder -> 3 shots
#          -> mission complete; screenshots catch the viewfinder UI.
# Phase 2: album persistence + log analysis.
# Phase 3: 2-minute stability run (photo demo stays on the whole time).
Set-Location $PSScriptRoot\..

# Start from a clean album so counts are deterministic.
Remove-Item -Path "saves\photo_album.json" -Force -ErrorAction SilentlyContinue

Write-Output "=== Phase 1: Photo demo (25s) ==="
$job1 = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_PHOTO_DEMO = '1'
    $env:OPENBULLY_SHOT       = 'track47_photo'
    $env:OPENBULLY_SHOT_DELAY = '5'
    $env:OPENBULLY_SHOT_EVERY = '1'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track47_run.log" -Encoding utf8
}
Start-Sleep -Seconds 25
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job1 -Timeout 10 | Out-Null
Remove-Job $job1 -Force

Write-Output "=== Photo log: equip/activate/viewfinder/subjects/progress/album ==="
Get-Content track47_run.log | Select-String -Pattern "Photo demo|Photo system|camera equipped|Photo mode|PHOTO PROGRESS|PHOTO TAKEN|Photo #|MISSION (ACTIVATED|ADVANCED|COMPLETE)|OBJECTIVE COMPLETE|Photo album" | Select-Object -First 40
Write-Output "=== Errors ==="
Get-Content track47_run.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 10
Write-Output "=== Screenshots ==="
Get-ChildItem -Path . -Filter "track47_photo*.tga" | Select-Object Name, Length

Write-Output "=== Album file ==="
if (Test-Path "saves\photo_album.json") {
    Get-Content "saves\photo_album.json"
} else {
    Write-Output "MISSING: saves/photo_album.json"
}
