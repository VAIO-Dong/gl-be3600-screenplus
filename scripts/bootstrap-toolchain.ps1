[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$version = '0.15.2'
$archiveName = "zig-x86_64-windows-$version.zip"
$downloadUrl = "https://ziglang.org/download/$version/$archiveName"
$expectedSha256 = '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$toolsDirectory = Join-Path $repositoryRoot '.tools'
$archivePath = Join-Path $toolsDirectory $archiveName
$installDirectory = Join-Path $toolsDirectory "zig-$version"
$zigExecutable = Join-Path $installDirectory 'zig.exe'

if (Test-Path -LiteralPath $zigExecutable) {
    & $zigExecutable version
    exit 0
}

New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null
Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath
$actualSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    throw "Zig archive checksum mismatch: expected $expectedSha256, got $actualSha256"
}

$expandedDirectory = Join-Path $toolsDirectory "zig-x86_64-windows-$version"
Expand-Archive -LiteralPath $archivePath -DestinationPath $toolsDirectory -Force
Move-Item -LiteralPath $expandedDirectory -Destination $installDirectory
Remove-Item -LiteralPath $archivePath
& $zigExecutable version
