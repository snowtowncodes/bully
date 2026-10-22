# Phase 7: combined session — all combat systems at once, 2-minute stability run.
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_MELEE_DEMO   = '1'
$env:OPENBULLY_GRAPPLE_DEMO = '1'
$env:OPENBULLY_COMBAT_DEMO  = '1'
$env:OPENBULLY_WEAPON_DEMO  = '1'
$env:OPENBULLY_HUD_DEMO     = '1'
$env:OPENBULLY_SHOT         = 'phase7_combined'
$env:OPENBULLY_SHOT_DELAY   = '5'
$job = Start-Job -ScriptBlock {
    Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
    & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase7_combined.log"
}
Start-Sleep -Seconds 125
Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
Wait-Job $job -Timeout 10 | Out-Null
Remove-Job $job -Force
Write-Output "--- system events ---"
Get-Content phase7_combined.log | Select-String -Pattern "Melee:|Grapple|Block|Dodge|COUNTER|Weapon demo|Projectile|Error|ERROR|assert|crash" | Select-Object -First 50
Write-Output "--- fps / stability (first/last) ---"
Get-Content phase7_combined.log | Select-String -Pattern "Frame timing" | Select-Object -First 2
Get-Content phase7_combined.log | Select-String -Pattern "Frame timing" | Select-Object -Last 2
Write-Output "--- error scan ---"
(Get-Content phase7_combined.log | Select-String -Pattern "\[ERROR\]|FATAL|assert").Count
