# Builds Tiny Browser with the installed Visual Studio 2022 toolchain.
# Usage: .\build.ps1            (Release)
#        .\build.ps1 -Config Debug
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
if (-not (Test-Path build\CMakeCache.txt)) {
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 | Out-Host
}
cmake --build build --config $Config -- /nologo /verbosity:minimal | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Write-Host "Built: $PSScriptRoot\build\$Config\tinybrowser.exe"
