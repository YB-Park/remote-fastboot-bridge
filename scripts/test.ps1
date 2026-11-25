[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$FastbootClient
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& "$PSScriptRoot/build.ps1" -Configuration $Configuration -FastbootClient $FastbootClient
Push-Location $root
try {
    & ctest --preset "win-x64-$($Configuration.ToLowerInvariant())"
    if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
} finally {
    Pop-Location
}
