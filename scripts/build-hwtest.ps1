[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$zigExecutable = Join-Path $repositoryRoot '.tools\zig-0.15.2\zig.exe'
$sourcePath = Join-Path $repositoryRoot 'tools\hwtest\screenplus_hwtest.c'
$buildDirectory = Join-Path $repositoryRoot 'build\hwtest'
$outputPath = Join-Path $buildDirectory 'screenplus-hwtest'
$globalCache = Join-Path $repositoryRoot 'build\cache\zig-global'
$localCache = Join-Path $repositoryRoot 'build\cache\zig-local'

if (-not (Test-Path -LiteralPath $zigExecutable)) {
    throw 'Zig is not installed. Run scripts/bootstrap-toolchain.ps1 first.'
}

New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $globalCache -Force | Out-Null
New-Item -ItemType Directory -Path $localCache -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = $globalCache
$env:ZIG_LOCAL_CACHE_DIR = $localCache
& $zigExecutable cc `
    -target aarch64-linux-musl `
    -static `
    -O2 `
    -std=c11 `
    -Wall `
    -Wextra `
    -Werror `
    -o $outputPath `
    $sourcePath
if ($LASTEXITCODE -ne 0) {
    throw "Hardware probe build failed with exit code $LASTEXITCODE"
}

$output = Get-Item -LiteralPath $outputPath
$hash = Get-FileHash -LiteralPath $outputPath -Algorithm SHA256
Write-Output ("output={0}" -f $output.FullName)
Write-Output ("bytes={0}" -f $output.Length)
Write-Output ("sha256={0}" -f $hash.Hash.ToLowerInvariant())
