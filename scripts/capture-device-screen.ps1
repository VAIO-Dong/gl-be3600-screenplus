[CmdletBinding()]
param(
    [string]$Device = 'root@192.168.8.1',
    [string]$OutputPath = 'build\device-screen.png',
    [ValidateSet(90, 270)]
    [int]$Rotation = 90
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$destination = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputPath))
$rawPath = [IO.Path]::ChangeExtension($destination, '.fb')
$remotePath = '/tmp/screenplus-capture.fb'
$common = @('-i', '.dev/screenplus_dev_key', '-o', 'UserKnownHostsFile=.device_known_hosts', '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')

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
}
