# Track 33: wheelie + crash verification run.
# DEMO=3 drives the bike: builds speed, holds W+Shift (wheelie) from t=5 s;
# the progressive instability over-rotates past 65 deg -> rider ejected ->
# "CRASH" log. OPENBULLY_SHOT catches frames from t=4 s every 3 s, so one of
# them lands mid-wheelie and a later one right after the eject.
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_VEHICLE_DEMO = '3'
$env:OPENBULLY_SHOT = 'track33_wheelie'
$env:OPENBULLY_SHOT_EVERY = '3'
$env:OPENBULLY_SHOT_DELAY = '4'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track33_run.log"
}
Start-Sleep -Seconds 50
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "--- vehicle/wheelie/crash log lines ---"
Get-Content "C:\Users\Snowtown\Projects\bully\openbully\track33_run.log" -ErrorAction SilentlyContinue |
    Select-String -Pattern "wheelie|WHEELIE|STUNT|CRASH|crash|eject|mounted|Vehicle" | Select-Object -First 50
Write-Output "--- errors ---"
Get-Content "C:\Users\Snowtown\Projects\bully\openbully\track33_run.log" -ErrorAction SilentlyContinue |
    Select-String -Pattern "ERROR|assert" | Select-Object -First 10
Write-Output "--- shots ---"
Get-ChildItem "C:\Users\Snowtown\Projects\bully\openbully\track33_wheelie*.tga" -ErrorAction SilentlyContinue |
    Select-Object Name, Length
