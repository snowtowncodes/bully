# Phase 9 integration: wheelie + eject verification (DEMO=3, bike).
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_VEHICLE_DEMO = '3'
$env:OPENBULLY_SHOT = 'phase9_wheelie'
$env:OPENBULLY_SHOT_EVERY = '3'
$env:OPENBULLY_SHOT_DELAY = '4'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase9_wheelie.log"
}
Start-Sleep -Seconds 50
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "--- wheelie/crash/eject ---"
Get-Content phase9_wheelie.log | Select-String -Pattern "wheelie|STUNT|CRASH|eject|mounted|engine start|void guard" | Select-Object -First 30
Write-Output "--- errors ---"
Get-Content phase9_wheelie.log | Select-String -Pattern "ERROR|assert" | Select-Object -First 10
Write-Output "--- shots ---"
Get-ChildItem phase9_wheelie*.tga | Select-Object Name, Length
