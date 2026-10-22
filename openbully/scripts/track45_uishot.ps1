# Track 45: arcade-view screenshot - capture at t=20 (inside Nut Shots round).
Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
$env:OPENBULLY_ARCADE_DEMO = '1'
$env:OPENBULLY_SHOT        = 'track45_ui'
$env:OPENBULLY_SHOT_DELAY  = '20'
Remove-Item Env:OPENBULLY_SHOT_EVERY -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_SAVE_DEMO -ErrorAction SilentlyContinue
$p = Start-Process -FilePath '.\build\Release\openbully.exe' `
    -RedirectStandardOutput 'track45_uishot.log' -PassThru
Start-Sleep -Seconds 35
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
Write-Output "--- log: arcade + screenshot ---"
Get-Content track45_uishot.log | Select-String "Arcade|Screenshot" | Select-Object -First 12
Write-Output "--- convert to png ---"
powershell -ExecutionPolicy Bypass -File scripts\tga_to_png.ps1 `
    -InPath 'track45_ui.tga' -OutPath 'track45_ui.png'
