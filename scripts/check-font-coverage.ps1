[CmdletBinding()]
param(
    [string]$SourcePath = 'src\main.c',
    [string]$FontPath = '.tools\lvgl-9.5.0\src\font\lv_font_source_han_sans_sc_14_cjk.c',
    [string]$SupplementalFontPath = 'src\screenplus_ui_14.c'
)

$ErrorActionPreference = 'Stop'
$fontSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $FontPath
$supplementalFontSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $SupplementalFontPath
$applicationSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $SourcePath
$missing = [regex]::Matches($applicationSource, '[^\u0000-\u007f]') |
    ForEach-Object Value |
    Sort-Object -Unique |
    Where-Object {
        -not $fontSource.Contains($_) -and
        -not $supplementalFontSource.Contains($_)
    }

if ($missing) {
    Write-Output ('missing=' + ($missing -join ''))
    exit 1
}
Write-Output 'missing='
