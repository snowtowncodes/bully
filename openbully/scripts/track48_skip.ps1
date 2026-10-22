# Track 48: skip-path verification — SPACE press while a cutscene plays.
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_CUTSCENE_DEMO = '1'
Remove-Item Env:OPENBULLY_SHOT -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_SHOT_DELAY -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_SHOT_EVERY -ErrorAction SilentlyContinue
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    $env:OPENBULLY_CUTSCENE_DEMO      = '1'
    $env:OPENBULLY_CUTSCENE_SKIP_DEMO = '1'
    & ".\build2\Release\openbully.exe" 2>&1 | Out-File -FilePath "track48_skip.log" -Encoding utf8
}
Start-Sleep -Seconds 12  # cutscene plays from ~t=0; skip demo fires at t=5

# Bring the window to the foreground and send SPACE.
$sh = New-Object -ComObject WScript.Shell
$null = $sh.AppActivate('OpenBully')
Start-Sleep -Milliseconds 400
$sh.SendKeys(' ')
Start-Sleep -Seconds 2
$sh.SendKeys(' ')        # second cycle plays cs2 (9s); skip that too if live
Start-Sleep -Seconds 2

Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force

Write-Output "=== Skip evidence ==="
Get-Content track48_skip.log | Select-String -Pattern "skip|playing|ended" | Select-Object -First 12
Write-Output "=== Errors ==="
Get-Content track48_skip.log | Select-String -Pattern "ERROR|assert|crash" | Select-Object -First 5
