[CmdletBinding()]
param(
    [string]$SourcePath = 'src\main.c',
    [string]$FontPath = '.tools\lvgl-9.5.0\src\font\lv_font_source_han_sans_sc_14_cjk.c',
    [string]$SupplementalFontPath = 'src\screenplus_ui_14.c',
    [string]$ResetFontPath = 'src\screenplus_reset_16.c'
)

$ErrorActionPreference = 'Stop'
$fontSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $FontPath
$supplementalFontSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $SupplementalFontPath
$resetFontSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $ResetFontPath
$applicationSource = Get-Content -Raw -Encoding UTF8 -LiteralPath $SourcePath
$missing = [regex]::Matches($applicationSource, '[^\u0000-\u007f]') |
    ForEach-Object Value |
    Sort-Object -Unique |
    Where-Object {
        -not $fontSource.Contains($_) -and
        -not $supplementalFontSource.Contains($_) -and
        -not $resetFontSource.Contains($_)
    }

if ($missing) {
    Write-Output ('missing=' + ($missing -join ''))
    exit 1
}

$resetBuildSection = [regex]::Match($applicationSource,
    '(?s)static lv_obj_t \*create_reset_stage_page.*?static lv_obj_t \*build_reset_screen.*?\r?\n\}')
$resetStatusSection = [regex]::Match($applicationSource,
    '(?s)static void show_reset_stage.*?static void open_reset_overlay')
if (-not $resetBuildSection.Success -or -not $resetStatusSection.Success) {
    throw 'Reset interface source section was not found.'
}
$resetSource = $resetBuildSection.Value + $resetStatusSection.Value
$resetMissing = [regex]::Matches($resetSource, '[^\u0000-\u007f]') |
    ForEach-Object Value |
    Sort-Object -Unique |
    Where-Object {
        $glyphMarker = [string][char]34 + $_ + [char]34
        -not $resetFontSource.Contains($glyphMarker)
    }
if ($resetMissing) {
    Write-Output ('reset_missing=' + ($resetMissing -join ''))
    exit 1
}
Write-Output 'missing='
Write-Output 'reset_missing='
