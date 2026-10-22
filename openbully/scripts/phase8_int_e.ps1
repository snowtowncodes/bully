Set-Location $PSScriptRoot\..
# Phase 8 Integration Run E: FULL combo with exit-code capture + WER dumps.
# NPC demo (combat), dialogue demo (auto-advance + cycle), TIME_DEMO (schedule
# churn), schedules active. This run both verifies integration and hunts the
# reported intermittent crash (50-125s window, ~50% of runs).
# WER LocalDumps: full heap dump on any openbully.exe crash.
$dumps = Join-Path $PSScriptRoot '..\crash_dumps'
New-Item -ItemType Directory -Force -Path $dumps | Out-Null
$wer = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\openbully.exe'
try {
    New-Item -Path $wer -Force -ErrorAction Stop | Out-Null
    Set-ItemProperty -Path $wer -Name DumpFolder -Value $dumps -Type ExpandString
    Set-ItemProperty -Path $wer -Name DumpType -Value 2 -Type DWord
    Set-ItemProperty -Path $wer -Name DumpCount -Value 5 -Type DWord
    Write-Output "WER LocalDumps registered: $dumps"
} catch {
    Write-Output "WER registration failed (registry rights?): $($_.Exception.Message)"
}

Remove-Item Env:OPENBULLY_MELEE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_GRAPPLE_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_COMBAT_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_MISSION_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:OPENBULLY_HUD_DEMO -ErrorAction SilentlyContinue
$env:OPENBULLY_NPC_DEMO      = '1'
$env:OPENBULLY_DIALOGUE_DEMO = '1'
$env:OPENBULLY_TIME_DEMO     = '1'
$env:OPENBULLY_TIME_START    = '07:00'
$env:OPENBULLY_SHOT          = 'phase8_full'
$env:OPENBULLY_SHOT_DELAY    = '3'
$env:OPENBULLY_SHOT_EVERY    = '30'

$proc = Start-Process -FilePath ".\build\Release\openbully.exe" `
    -WorkingDirectory (Get-Location) -RedirectStandardOutput "phase8_run_e.log" `
    -RedirectStandardError "phase8_run_e_err.log" -PassThru -NoNewWindow
$exited = $proc.WaitForExit(160000)
if (-not $exited) {
    Write-Output "TIMEOUT: killing after 160s (stability window crossed)"
    $proc.Kill()
    $proc.WaitForExit(10000) | Out-Null
}
Write-Output ("EXIT CODE: " + $proc.ExitCode)
$code = $proc.ExitCode
if ($code -ne 0 -and $code -ne -1 -and $code -ne 1) {
    Write-Output "NON-ZERO EXIT: possible crash (0x{0:X})" -f $code
}
Write-Output "=== dumps present? ==="
Get-ChildItem $dumps -ErrorAction SilentlyContinue | Select-Object Name, Length
Write-Output "=== log tail ==="
Get-Content phase8_run_e.log -Tail 5
Write-Output "=== crash markers ==="
Get-Content phase8_run_e.log | Select-String 'ERROR|assert' | Select-Object -First 5
