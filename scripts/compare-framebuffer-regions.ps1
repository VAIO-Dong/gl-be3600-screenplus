[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BeforePath,
    [Parameter(Mandatory = $true)]
    [string]$AfterPath,
    [ValidateSet(90, 270)]
    [int]$Rotation = 90,
    [string[]]$Region = @('top=0:0:283:36', 'bottom=0:39:283:75')
)

$ErrorActionPreference = 'Stop'
$before = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $BeforePath))
$after = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $AfterPath))
if ($before.Length -ne 43168 -or $after.Length -ne 43168) {
    throw 'Both inputs must be 76 x 284 RGB565 framebuffers (43168 bytes).'
}

function Get-RegionHash([byte[]]$Buffer, [int]$StartX, [int]$StartY,
                        [int]$EndX, [int]$EndY) {
    $stream = [IO.MemoryStream]::new()
    try {
        for ($logicalY = $StartY; $logicalY -le $EndY; $logicalY++) {
            for ($logicalX = $StartX; $logicalX -le $EndX; $logicalX++) {
                if ($Rotation -eq 90) {
                    $nativeX = $logicalY
                    $nativeY = 283 - $logicalX
                } else {
                    $nativeX = 75 - $logicalY
                    $nativeY = $logicalX
                }
                $offset = (($nativeY * 76) + $nativeX) * 2
                $stream.WriteByte($Buffer[$offset])
                $stream.WriteByte($Buffer[$offset + 1])
            }
        }
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return [BitConverter]::ToString($sha.ComputeHash($stream.ToArray())).Replace('-', '').ToLowerInvariant()
        } finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}

$result = [ordered]@{}
foreach ($definition in $Region) {
    if ($definition -notmatch '^([^=]+)=(\d+):(\d+):(\d+):(\d+)$') {
        throw "Invalid region '$definition'; expected name=x0:y0:x1:y1."
    }
    $name = $Matches[1]
    $startX = [int]$Matches[2]
    $startY = [int]$Matches[3]
    $endX = [int]$Matches[4]
    $endY = [int]$Matches[5]
    if ($startX -lt 0 -or $endX -gt 283 -or $startX -gt $endX -or
        $startY -lt 0 -or $endY -gt 75 -or $startY -gt $endY) {
        throw "Invalid rectangle in '$definition'."
    }
    $beforeHash = Get-RegionHash $before $startX $startY $endX $endY
    $afterHash = Get-RegionHash $after $startX $startY $endX $endY
    $result[$name] = [ordered]@{
        changed = $beforeHash -ne $afterHash
        before_sha256 = $beforeHash
        after_sha256 = $afterHash
    }
}
$result | ConvertTo-Json -Depth 4
