[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [ValidateSet(90, 270)]
    [int]$Rotation = 90
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $InputPath))
if ($bytes.Length -ne 43168) {
    throw "Expected a 76 x 284 RGB565 framebuffer (43168 bytes), got $($bytes.Length)."
}

$bitmap = [Drawing.Bitmap]::new(284, 76, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
try {
    for ($y = 0; $y -lt 76; $y++) {
        for ($x = 0; $x -lt 284; $x++) {
            if ($Rotation -eq 90) {
                $nativeX = $y
                $nativeY = 283 - $x
            } else {
                $nativeX = 75 - $y
                $nativeY = $x
            }
            $offset = (($nativeY * 76) + $nativeX) * 2
            $rgb565 = [int]$bytes[$offset] -bor ([int]$bytes[$offset + 1] -shl 8)
            $red = (($rgb565 -shr 11) -band 0x1f) * 255 / 31
            $green = (($rgb565 -shr 5) -band 0x3f) * 255 / 63
            $blue = ($rgb565 -band 0x1f) * 255 / 31
            $bitmap.SetPixel($x, $y, [Drawing.Color]::FromArgb(
                [int]$red, [int]$green, [int]$blue))
        }
    }
    $destination = [IO.Path]::GetFullPath($OutputPath)
    $directory = [IO.Path]::GetDirectoryName($destination)
    if ($directory) {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    $bitmap.Save($destination, [Drawing.Imaging.ImageFormat]::Png)
    Write-Output $destination
} finally {
    $bitmap.Dispose()
}
