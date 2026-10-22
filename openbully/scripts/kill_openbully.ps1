$procs = Get-Process -Name openbully -ErrorAction SilentlyContinue
if ($procs) {
    $procs | Stop-Process -Force
    Write-Host "killed $($procs.Count) instance(s)"
} else {
    Write-Host "no openbully running"
}
