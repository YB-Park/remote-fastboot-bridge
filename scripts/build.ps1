[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$FastbootClient,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($FastbootClient) { $FastbootClient = (Resolve-Path -LiteralPath $FastbootClient).Path }
if ($Package -and ($Configuration -ne 'Release' -or $FastbootClient)) {
    throw 'The package configuration is Release and excludes tests.'
}
$configure = if ($Package) { 'win-x64-package' } else { 'win-x64' }
$build = if ($Package) { 'win-x64-package' } else { "win-x64-$($Configuration.ToLowerInvariant())" }

Push-Location $root
try {
    $arguments = @('--preset', $configure, "-DFASTBOOT_CLIENT:FILEPATH=$FastbootClient")
    & cmake @arguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & cmake --build --preset $build --parallel
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
} finally {
    Pop-Location
}
