[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Device,
    [string]$IdentityFile,
    [string]$KnownHostsFile,
    [string]$OutputPath = 'build\device-screen.png',
    [ValidateSet(90, 270)]
    [int]$Rotation = 90
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$destination = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputPath))
$rawPath = [IO.Path]::ChangeExtension($destination, '.fb')
$remotePath = '/tmp/screenplus-capture.fb'
$common = @('-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')
$temporaryKnownHostsPath = $null

function Resolve-RepositoryPath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
}

$resolvedIdentity = Resolve-RepositoryPath $IdentityFile
if ($resolvedIdentity) {
    if (-not (Test-Path -LiteralPath $resolvedIdentity -PathType Leaf)) {
        throw "SSH identity file does not exist: $IdentityFile"
    }
    $common += @('-i', $resolvedIdentity)
}
$resolvedKnownHosts = Resolve-RepositoryPath $KnownHostsFile
if ($resolvedKnownHosts) {
    if (-not (Test-Path -LiteralPath $resolvedKnownHosts -PathType Leaf)) {
        throw "SSH known-hosts file does not exist: $KnownHostsFile"
    }
    $temporaryKnownHostsPath = Join-Path $repositoryRoot "build\.screenplus-known-hosts-$PID"
    New-Item -ItemType Directory -Path (Split-Path -Parent $temporaryKnownHostsPath) -Force | Out-Null
    Copy-Item -LiteralPath $resolvedKnownHosts -Destination $temporaryKnownHostsPath -Force
    $common += @('-o', "UserKnownHostsFile=build/.screenplus-known-hosts-$PID")
}

Push-Location $repositoryRoot
try {
    & ssh @common $Device "dd if=/dev/fb0 of=$remotePath bs=43168 count=1"
    if ($LASTEXITCODE -ne 0) { throw 'Framebuffer capture failed.' }
    $remoteSource = $Device + ':' + $remotePath
    & scp -O @common $remoteSource $rawPath
    if ($LASTEXITCODE -ne 0) { throw 'Framebuffer download failed.' }
    & (Join-Path $PSScriptRoot 'convert-framebuffer.ps1') -InputPath $rawPath -OutputPath $destination -Rotation $Rotation
} finally {
    Pop-Location
    if ($temporaryKnownHostsPath -and (Test-Path -LiteralPath $temporaryKnownHostsPath)) {
        Remove-Item -LiteralPath $temporaryKnownHostsPath -Force
    }
}
