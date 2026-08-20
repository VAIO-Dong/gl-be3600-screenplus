[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
$architecture = 'aarch64_cortex-a53_neon-vfpv4'
$binaryPath = Join-Path $repositoryRoot 'build\prototype\screenplus'
$outputDirectory = Join-Path $repositoryRoot 'dist'
$outputPath = Join-Path $outputDirectory "screenplus_${version}-1_${architecture}.ipk"
$luciOutputPath = Join-Path $outputDirectory "luci-app-screenplus_${version}-1_all.ipk"

if (-not (Test-Path -LiteralPath $binaryPath)) {
    throw 'Prototype binary is missing. Run scripts/build-prototype.ps1 first.'
}
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

function Write-AsciiField {
    param(
        [byte[]]$Buffer,
        [int]$Offset,
        [int]$Length,
        [string]$Value
    )
    $bytes = [Text.Encoding]::ASCII.GetBytes($Value)
    if ($bytes.Length -gt $Length) {
        throw "Tar field is too long: $Value"
    }
    [Array]::Copy($bytes, 0, $Buffer, $Offset, $bytes.Length)
}

function Convert-ToOctalField {
    param([long]$Value, [int]$Length)
    $digits = [Convert]::ToString($Value, 8)
    return $digits.PadLeft($Length - 1, '0') + [char]0
}

function New-UstarGzip {
    param(
        [array]$Entries,
        [string]$Destination
    )
    $tarStream = [IO.MemoryStream]::new()
    try {
        foreach ($entry in $Entries) {
            $name = $entry.Name.Replace('\', '/')
            $isDirectory = $entry.Type -eq 'directory'
            if ($isDirectory -and -not $name.EndsWith('/')) { $name += '/' }
            $content = if ($isDirectory) { [byte[]]::new(0) } else { [byte[]]$entry.Content }
            $header = [byte[]]::new(512)
            Write-AsciiField $header 0 100 $name
            Write-AsciiField $header 100 8 (Convert-ToOctalField $entry.Mode 8)
            Write-AsciiField $header 108 8 (Convert-ToOctalField 0 8)
            Write-AsciiField $header 116 8 (Convert-ToOctalField 0 8)
            Write-AsciiField $header 124 12 (Convert-ToOctalField $content.Length 12)
            Write-AsciiField $header 136 12 (Convert-ToOctalField 0 12)
            for ($index = 148; $index -lt 156; $index++) { $header[$index] = 0x20 }
            $header[156] = if ($isDirectory) { [byte][char]'5' } else { [byte][char]'0' }
            Write-AsciiField $header 257 6 ("ustar" + [char]0)
            Write-AsciiField $header 263 2 '00'
            Write-AsciiField $header 265 32 'root'
            Write-AsciiField $header 297 32 'root'
            $checksum = 0
            foreach ($byte in $header) { $checksum += $byte }
            $checksumText = [Convert]::ToString($checksum, 8).PadLeft(6, '0') + [char]0 + ' '
            Write-AsciiField $header 148 8 $checksumText
            $tarStream.Write($header, 0, $header.Length)
            if ($content.Length -gt 0) {
                $tarStream.Write($content, 0, $content.Length)
                $padding = (512 - ($content.Length % 512)) % 512
                if ($padding -gt 0) {
                    $tarStream.Write([byte[]]::new($padding), 0, $padding)
                }
            }
        }
        $tarStream.Write([byte[]]::new(1024), 0, 1024)
        $tarStream.Position = 0
        $fileStream = [IO.File]::Open($Destination, [IO.FileMode]::Create,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $gzip = [IO.Compression.GZipStream]::new($fileStream,
                [IO.Compression.CompressionLevel]::Optimal, $true)
            try { $tarStream.CopyTo($gzip) } finally { $gzip.Dispose() }
        } finally { $fileStream.Dispose() }
    } finally { $tarStream.Dispose() }
}

function New-ArArchive {
    param(
        [array]$Members,
        [string]$Destination
    )
    $stream = [IO.File]::Open($Destination, [IO.FileMode]::Create,
        [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $magic = [Text.Encoding]::ASCII.GetBytes("!<arch>`n")
        $stream.Write($magic, 0, $magic.Length)
        foreach ($member in $Members) {
            $content = [byte[]]$member.Content
            $name = ($member.Name + '/').PadRight(16, ' ')
            $headerText = $name + '0'.PadRight(12, ' ') + '0'.PadRight(6, ' ') +
                '0'.PadRight(6, ' ') + '100644'.PadRight(8, ' ') +
                $content.Length.ToString().PadRight(10, ' ') + [char]0x60 + "`n"
            $header = [Text.Encoding]::ASCII.GetBytes($headerText)
            if ($header.Length -ne 60) { throw "Invalid ar header for $($member.Name)" }
            $stream.Write($header, 0, $header.Length)
            $stream.Write($content, 0, $content.Length)
            if (($content.Length % 2) -ne 0) { $stream.WriteByte(0x0a) }
        }
    } finally { $stream.Dispose() }
}

function Get-Bytes([string]$Path) {
    return [IO.File]::ReadAllBytes($Path)
}

function Get-TextBytes([string]$Text) {
    return [Text.UTF8Encoding]::new($false).GetBytes($Text)
}

$temporaryDirectory = Join-Path $repositoryRoot ("build\ipk\" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryDirectory -Force | Out-Null
$controlArchive = Join-Path $temporaryDirectory 'control.tar.gz'
$dataArchive = Join-Path $temporaryDirectory 'data.tar.gz'

$controlText = @"
Package: screenplus
Version: $version-1
Depends: libc
Source: screenplus
Section: utils
Architecture: $architecture
Maintainer: ScreenPlus Project
Installed-Size: $([Math]::Ceiling((Get-Item -LiteralPath $binaryPath).Length / 1024))
Description: Direct touchscreen dashboard for GL-BE3600
"@.Trim() + "`n"

$controlRoot = Join-Path $repositoryRoot 'package\screenplus\control'
$controlEntries = @(
    @{ Name='./'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./control'; Type='file'; Mode=420; Content=(Get-TextBytes $controlText) },
    @{ Name='./conffiles'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $controlRoot 'conffiles')) },
    @{ Name='./postinst'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $controlRoot 'postinst')) },
    @{ Name='./prerm'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $controlRoot 'prerm')) },
    @{ Name='./postrm'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $controlRoot 'postrm')) }
)
New-UstarGzip $controlEntries $controlArchive

$packageRoot = Join-Path $repositoryRoot 'package\screenplus\files'
$dataEntries = @(
    @{ Name='./'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./etc'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./etc/config'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./etc/config/screenplus'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $packageRoot 'screenplus.config')) },
	@{ Name='./etc/gl-switch.d'; Type='directory'; Mode=493; Content=$null },
	@{ Name='./etc/gl-switch.d/ScreenPlus.sh'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $packageRoot 'ScreenPlus.sh')) },
    @{ Name='./etc/init.d'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./etc/init.d/screenplus'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $packageRoot 'screenplus.init')) },
    @{ Name='./usr'; Type='directory'; Mode=493; Content=$null },
	@{ Name='./usr/libexec'; Type='directory'; Mode=493; Content=$null },
	@{ Name='./usr/libexec/screenplus-background'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $packageRoot 'screenplus-background')) },
	@{ Name='./usr/libexec/screenplus-diagnostics'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $packageRoot 'screenplus-diagnostics')) },
	@{ Name='./usr/libexec/screenplus-migrate'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $packageRoot 'screenplus-migrate')) },
	@{ Name='./usr/share'; Type='directory'; Mode=493; Content=$null },
	@{ Name='./usr/share/screenplus'; Type='directory'; Mode=493; Content=$null },
	@{ Name='./usr/share/screenplus/backgrounds'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/sbin'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/sbin/screenplus'; Type='file'; Mode=493; Content=(Get-Bytes $binaryPath) }
)
New-UstarGzip $dataEntries $dataArchive

$outerEntries = @(
    @{ Name='./debian-binary'; Type='file'; Mode=420; Content=(Get-TextBytes "2.0`n") },
    @{ Name='./data.tar.gz'; Type='file'; Mode=420; Content=(Get-Bytes $dataArchive) },
    @{ Name='./control.tar.gz'; Type='file'; Mode=420; Content=(Get-Bytes $controlArchive) }
)
New-UstarGzip $outerEntries $outputPath

$package = Get-Item -LiteralPath $outputPath
$hash = Get-FileHash -LiteralPath $outputPath -Algorithm SHA256
Write-Output ("output={0}" -f $package.FullName)
Write-Output ("bytes={0}" -f $package.Length)
Write-Output ("sha256={0}" -f $hash.Hash.ToLowerInvariant())

$luciControlArchive = Join-Path $temporaryDirectory 'luci-control.tar.gz'
$luciDataArchive = Join-Path $temporaryDirectory 'luci-data.tar.gz'
$luciRoot = Join-Path $repositoryRoot 'package\luci-app-screenplus'
$luciControlText = @"
Package: luci-app-screenplus
Version: $version-1
Depends: libc, luci-base, screenplus
Source: luci-app-screenplus
Section: luci
Architecture: all
Maintainer: ScreenPlus Project
Installed-Size: 20
Description: LuCI configuration for ScreenPlus
"@.Trim() + "`n"
$luciControlEntries = @(
    @{ Name='./'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./control'; Type='file'; Mode=420; Content=(Get-TextBytes $luciControlText) },
    @{ Name='./postinst'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $luciRoot 'control\postinst')) },
    @{ Name='./postrm'; Type='file'; Mode=493; Content=(Get-Bytes (Join-Path $luciRoot 'control\postrm')) }
)
New-UstarGzip $luciControlEntries $luciControlArchive

$luciDataEntries = @(
    @{ Name='./'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/share'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/share/luci'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/share/luci/menu.d'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/share/luci/menu.d/luci-app-screenplus.json'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $luciRoot 'root\usr\share\luci\menu.d\luci-app-screenplus.json')) },
    @{ Name='./usr/share/rpcd'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/share/rpcd/acl.d'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./usr/share/rpcd/acl.d/luci-app-screenplus.json'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $luciRoot 'root\usr\share\rpcd\acl.d\luci-app-screenplus.json')) },
    @{ Name='./www'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./www/luci-static'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./www/luci-static/resources'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./www/luci-static/resources/view'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./www/luci-static/resources/view/screenplus'; Type='directory'; Mode=493; Content=$null },
    @{ Name='./www/luci-static/resources/view/screenplus/general.js'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $luciRoot 'htdocs\luci-static\resources\view\screenplus\general.js')) },
    @{ Name='./www/luci-static/resources/view/screenplus/pages.js'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $luciRoot 'htdocs\luci-static\resources\view\screenplus\pages.js')) },
    @{ Name='./www/luci-static/resources/view/screenplus/appearance.js'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $luciRoot 'htdocs\luci-static\resources\view\screenplus\appearance.js')) },
    @{ Name='./www/luci-static/resources/view/screenplus/diagnostics.js'; Type='file'; Mode=420; Content=(Get-Bytes (Join-Path $luciRoot 'htdocs\luci-static\resources\view\screenplus\diagnostics.js')) }
)
New-UstarGzip $luciDataEntries $luciDataArchive
$luciOuterEntries = @(
    @{ Name='./debian-binary'; Type='file'; Mode=420; Content=(Get-TextBytes "2.0`n") },
    @{ Name='./data.tar.gz'; Type='file'; Mode=420; Content=(Get-Bytes $luciDataArchive) },
    @{ Name='./control.tar.gz'; Type='file'; Mode=420; Content=(Get-Bytes $luciControlArchive) }
)
New-UstarGzip $luciOuterEntries $luciOutputPath

$luciPackage = Get-Item -LiteralPath $luciOutputPath
$luciHash = Get-FileHash -LiteralPath $luciOutputPath -Algorithm SHA256
Write-Output ("output={0}" -f $luciPackage.FullName)
Write-Output ("bytes={0}" -f $luciPackage.Length)
Write-Output ("sha256={0}" -f $luciHash.Hash.ToLowerInvariant())
