[CmdletBinding()]
param(
    [string]$SourcePath = 'src\main.c',
    [string]$FontPath = '.tools\lvgl-9.5.0\src\font\lv_font_source_han_sans_sc_14_cjk.c'
)

$ErrorActionPreference = 'Stop'
$fontSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $FontPath
$applicationSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $SourcePath
$missing = [regex]::Matches($applicationSource, '[\u3400-\u9fff]') |
    ForEach-Object Value |
    Sort-Object -Unique |
    Where-Object { -not $fontSource.Contains($_) }

if ($missing) {
    Write-Output ('missing=' + ($missing -join ''))
    exit 1
}
Write-Output 'missing='
