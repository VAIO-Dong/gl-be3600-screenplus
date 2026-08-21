[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$zigExecutable = Join-Path $repositoryRoot '.tools\zig-0.15.2\zig.exe'
$sourcePath = Join-Path $repositoryRoot 'tools\screenplus-touch.c'
$outputPath = Join-Path $repositoryRoot 'build\screenplus-touch'
$globalCache = Join-Path $repositoryRoot 'build\cache\zig-global'
$localCache = Join-Path $repositoryRoot 'build\cache\zig-touch-tool'

if (-not (Test-Path -LiteralPath $zigExecutable)) {
    throw 'Zig is missing. Run scripts/bootstrap-toolchain.ps1 first.'
}
New-Item -ItemType Directory -Path (Split-Path -Parent $outputPath) -Force | Out-Null
New-Item -ItemType Directory -Path $globalCache -Force | Out-Null
New-Item -ItemType Directory -Path $localCache -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = $globalCache
$env:ZIG_LOCAL_CACHE_DIR = $localCache

& $zigExecutable cc -target aarch64-linux-musl -static -O2 -std=c11 -o $outputPath $sourcePath
if ($LASTEXITCODE -ne 0) {
    throw "Touch tool build failed with exit code $LASTEXITCODE"
}
$file = Get-Item -LiteralPath $outputPath
$hash = Get-FileHash -LiteralPath $outputPath -Algorithm SHA256
Write-Output ("output={0}" -f $file.FullName)
Write-Output ("bytes={0}" -f $file.Length)
Write-Output ("sha256={0}" -f $hash.Hash.ToLowerInvariant())
