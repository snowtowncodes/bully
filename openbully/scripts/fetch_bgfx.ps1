# Fetches the pinned bgfx.cmake release and vendors it as external/bgfx.
# bgfx version: v1.129.8958-499 (bgfx.cmake-archived release, 2025-10-30)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot | Split-Path
$version = "v1.129.8958-499"
$zip = Join-Path $root "bgfx_release.zip"
$dest = Join-Path $root "external"
$url = "https://github.com/bkaradzic/bgfx.cmake-archived/releases/download/$version/bgfx.cmake.$version.zip"

if (Test-Path (Join-Path $dest "bgfx\CMakeLists.txt")) {
    Write-Host "external/bgfx already present; nothing to do."
    exit 0
}

Write-Host "Downloading $url ..."
Invoke-WebRequest -Uri $url -OutFile $zip

$tmp = Join-Path $root "external\_bgfx_extract"
Expand-Archive -Path $zip -DestinationPath $tmp -Force

$inner = Join-Path $tmp "bgfx.cmake"
Remove-Item (Join-Path $inner ".git") -Recurse -Force -ErrorAction SilentlyContinue
Move-Item $inner (Join-Path $dest "bgfx")
Remove-Item $tmp -Recurse -Force
Remove-Item $zip

Write-Host "Vendored bgfx.cmake $version -> external/bgfx"
