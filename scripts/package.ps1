[CmdletBinding()]
param([string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'dist' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

& "$PSScriptRoot/build.ps1" -Package
$name = 'fastboot-proxy-win-x64'
$staging = Join-Path ([IO.Path]::GetTempPath()) "fastboot-proxy-$([Guid]::NewGuid().ToString('N'))"
$package = Join-Path $staging $name
New-Item -ItemType Directory -Force -Path $package | Out-Null
try {
    & cmake --install "$root/out/win-x64-package" --config Release --prefix $package
    if ($LASTEXITCODE -ne 0) { throw 'CMake install failed.' }
    $exe = Join-Path $package 'fastboot-proxy.exe'
    & $exe --version
    if ($LASTEXITCODE -ne 0) { throw 'The packaged executable failed to start.' }
    $zip = Join-Path $OutputDirectory "$name.zip"
    Compress-Archive -Path $package -DestinationPath $zip -Force
    Get-FileHash -Algorithm SHA256 -LiteralPath $zip
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force
}
