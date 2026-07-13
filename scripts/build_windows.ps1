$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

cmake -S . -B build -A x64
cmake --build build --config Release

Write-Host ""
Write-Host "Built:"
Write-Host "  $Root\build\Release\SniperBishop.exe"
Write-Host "  $Root\build\Release\firstnet_v3.snnue"
