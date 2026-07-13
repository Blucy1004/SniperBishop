$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $Root "build\Release\SniperBishop.exe"

if (-not (Test-Path $Exe)) {
    throw "Build the engine first: .\scripts\build_windows.ps1"
}

@"
uci
isready
position startpos
perft 4
eval
quit
"@ | & $Exe
