# Phase 7 integration: run all combat demos, collect logs + screenshots.
Set-Location $PSScriptRoot\..

function Run-Demo($name, $envvar, $extra) {
    Write-Output "=================== $name ==================="
    $log = "phase7_${name}.log"
    Remove-Item Env:OPENBULLY_COMBAT_DEMO, Env:OPENBULLY_MELEE_DEMO, Env:OPENBULLY_GRAPPLE_DEMO, Env:OPENBULLY_WEAPON_DEMO, Env:OPENBULLY_HUD_DEMO, Env:OPENBULLY_SHOT, Env:OPENBULLY_SHOT_DELAY -ErrorAction SilentlyContinue
    Set-Item -Path "Env:\$envvar" -Value '1'
    if ($extra) { Set-Item -Path "Env:\$extra" -Value '1' }
    $env:OPENBULLY_SHOT = "phase7_${name}"
    $env:OPENBULLY_SHOT_DELAY = '4'
    $job = Start-Job -ScriptBlock {
        Set-Location 'C:\Users\Snowtown\Projects\bully\openbully'
        & ".\build\Release\openbully.exe" 2>&1 | Out-File -FilePath "phase7_${using:name}.log"
    }
    Start-Sleep -Seconds 14
    Stop-Process -Name openbully -Force -ErrorAction SilentlyContinue
    Wait-Job $job -Timeout 10 | Out-Null
    Remove-Job $job -Force
    Write-Output "--- key lines ---"
    Get-Content $log | Select-String -Pattern "Melee|melee|Grapple|grapple|Dodge|Block|Counter|Weapon|weapon|Combo|hit|land" | Select-Object -First 45
    Write-Output "--- tail ---"
    Get-Content $log -Tail 8
}

Run-Demo 'melee'   'OPENBULLY_MELEE_DEMO'
Run-Demo 'grapple' 'OPENBULLY_GRAPPLE_DEMO'
Run-Demo 'combat'  'OPENBULLY_COMBAT_DEMO' 'OPENBULLY_HUD_DEMO'
Run-Demo 'weapon'  'OPENBULLY_WEAPON_DEMO'
Write-Output "--- shots ---"
Get-ChildItem phase7_*.tga | Select-Object Name, Length
