Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_VEHICLE_DEMO = '2'
$env:OPENBULLY_SHOT = 'track32_drive'
$env:OPENBULLY_SHOT_EVERY = '6'
$env:OPENBULLY_SHOT_DELAY = '4'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "track32_run.log"
}
Start-Sleep -Seconds 45
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "--- vehicle log lines ---"
Get-Content track32_run.log | Select-String -Pattern "Vehicle" | Select-Object -First 50
Write-Output "--- errors ---"
Get-Content track32_run.log | Select-String -Pattern "ERROR|crash|assert" | Select-Object -First 10
Write-Output "--- shots ---"
Get-ChildItem track32_drive*.tga | Select-Object Name, Length
