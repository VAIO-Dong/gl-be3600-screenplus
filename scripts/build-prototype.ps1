[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$zigExecutable = Join-Path $repositoryRoot '.tools\zig-0.15.2\zig.exe'
$lvglDirectory = Join-Path $repositoryRoot '.tools\lvgl-9.5.0'
$sourceDirectory = Join-Path $repositoryRoot 'src'
$buildDirectory = Join-Path $repositoryRoot 'build\prototype'
$outputPath = Join-Path $buildDirectory 'screenplus'
$responsePath = Join-Path $buildDirectory 'sources.rsp'
$globalCache = Join-Path $repositoryRoot 'build\cache\zig-global'
$localCache = Join-Path $repositoryRoot 'build\cache\zig-prototype'

if (-not (Test-Path -LiteralPath $zigExecutable)) {
    throw 'Zig is not installed. Run scripts/bootstrap-toolchain.ps1 first.'
}
if (-not (Test-Path -LiteralPath (Join-Path $lvglDirectory 'lvgl.h'))) {
    throw 'LVGL v9.5.0 is not available under .tools/lvgl-9.5.0.'
}

New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $globalCache -Force | Out-Null
New-Item -ItemType Directory -Path $localCache -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = $globalCache
$env:ZIG_LOCAL_CACHE_DIR = $localCache

$arguments = @(
    '-target', 'aarch64-linux-musl',
    '-static', '-O2', '-std=c11', '-pthread',
    "-ffile-prefix-map=$repositoryRoot=.",
    "-fmacro-prefix-map=$repositoryRoot=.",
    '-ffunction-sections', '-fdata-sections',
    '-Wl,--gc-sections', '-Wl,--strip-all',
    '-DLV_CONF_INCLUDE_SIMPLE',
    '-D_GNU_SOURCE',
    "-I$sourceDirectory",
    "-I$lvglDirectory",
    '-Wno-unused-function',
    '-Wno-unused-variable',
    '-Wno-unused-parameter',
    '-o', $outputPath
)
$projectSources = Get-ChildItem -LiteralPath $sourceDirectory -Filter '*.c' -File |
    Sort-Object FullName |
    Select-Object -ExpandProperty FullName
$arguments += $projectSources
$lvglSources = Get-ChildItem -LiteralPath (Join-Path $lvglDirectory 'src') -Recurse -Filter '*.c' -File |
    Where-Object { $_.FullName -notmatch '[\\/]debugging[\\/]test[\\/]' } |
    Sort-Object FullName |
    Select-Object -ExpandProperty FullName
$arguments += $lvglSources
$arguments += '-lm'

$quotedArguments = $arguments | ForEach-Object {
    if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
}
[System.IO.File]::WriteAllLines($responsePath, $quotedArguments)

& $zigExecutable cc "@$responsePath"
if ($LASTEXITCODE -ne 0) {
    throw "ScreenPlus prototype build failed with exit code $LASTEXITCODE"
}

$output = Get-Item -LiteralPath $outputPath
$hash = Get-FileHash -LiteralPath $outputPath -Algorithm SHA256
Write-Output ("output={0}" -f $output.FullName)
Write-Output ("bytes={0}" -f $output.Length)
Write-Output ("sha256={0}" -f $hash.Hash.ToLowerInvariant())
