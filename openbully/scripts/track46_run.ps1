# Track 46: bike tricks — headless verification run.
# DEMO=4 scripts: mount -> wheelie -> wheelie -> stoppie -> bunny hop chain,
# then a held crash. Screenshots capture the trick popup + combo meter; the
# log carries the TRICK:/ACHIEVEMENT: evidence lines. At t=20 s the journal
# opens (OPENBULLY_JOURNAL_AT) so late shots show the STYLE stats panel.
Set-Location $PSScriptRoot\..
$env:OPENBULLY_VEHICLE_DEMO = '4'
$env:OPENBULLY_NO_CUTSCENE = '1'
$env:OPENBULLY_JOURNAL_AT  = '20'
$env:OPENBULLY_SHOT = 'track46_trick'
$env:OPENBULLY_SHOT_DELAY = '10'
$env:OPENBULLY_SHOT_EVERY = '4'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "track46_run.log"
}
# Trick chain runs ~20 s, then OPENBULLY_JOURNAL_AT opens the journal whose
# STYLE panel (score / per-type rows / achievement checkboxes) draws while
# open — the late screenshots deterministically show the stats screen.
Start-Sleep -Seconds 38
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "--- trick log lines ---"
Get-Content track46_run.log | Select-String -Pattern "TRICK|ACHIEVEMENT|STUNT|CRASH" | Select-Object -First 40
Write-Output "--- errors ---"
Get-Content track46_run.log | Select-String -Pattern "ERROR|assert" | Select-Object -First 10
Write-Output "--- tail ---"
Get-Content track46_run.log -Tail 8
Write-Output "--- shots ---"
Get-ChildItem track46_trick*.tga | Select-Object Name, Length
