param()

$ErrorActionPreference = 'Stop'

# PowerShell parameter names cannot contain hyphens, so the documented
# "--socks-proxy <host:port>" style is parsed manually from $args. This is
# also why the script must NOT use [CmdletBinding()], which would turn
# unrecognized named arguments into binding errors instead of $args entries.
$socksProxy = $null
for ($index = 0; $index -lt $args.Count; $index++) {
    $argument = [string]$args[$index]
    if ($argument -eq '--socks-proxy') {
        $index++
        if ($index -ge $args.Count) {
            throw '--socks-proxy requires a value: --socks-proxy <host:port>'
        }
        $socksProxy = [string]$args[$index]
    }
    elseif ($argument.StartsWith('--socks-proxy=', [System.StringComparison]::Ordinal)) {
        $socksProxy = $argument.Substring('--socks-proxy='.Length)
    }
    else {
        throw "Unknown argument '$argument'. Supported: --socks-proxy <host:port> (e.g. 127.0.0.1:1080)"
    }
}

if ($null -ne $socksProxy -and $socksProxy -notmatch '^[A-Za-z0-9.\[\]-]+:\d{1,5}$') {
    throw "Invalid SOCKS proxy address '$socksProxy'. Expected host:port, e.g. 127.0.0.1:1080."
}

function Invoke-FileDownload {
    param(
        [string]$Url,
        [string]$DestinationPath,
        [string]$SocksProxy
    )

    # Invoke-WebRequest has no SOCKS support, so proxied downloads must go
    # through curl.exe (ships with Windows 10 1803+). --socks5-hostname also
    # resolves DNS through the proxy, which is required when the target host
    # is not reachable directly.
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        $curlArguments = @(
            '--fail', '--show-error', '--location',
            '--retry', '3', '--retry-delay', '2', '--connect-timeout', '15',
            '--progress-bar',
            '--output', $DestinationPath
        )
        if ($SocksProxy) {
            $curlArguments += @('--socks5-hostname', $SocksProxy)
        }
        $curlArguments += $Url
        & curl.exe @curlArguments
        if ($LASTEXITCODE -ne 0) {
            throw "curl.exe exited with code $LASTEXITCODE while downloading $Url."
        }
        return
    }

    if ($SocksProxy) {
        throw 'A SOCKS proxy was requested but curl.exe is not available on this system.'
    }

    Invoke-WebRequest -Uri $Url -OutFile $DestinationPath
}

function Get-FileSha256 {
    param([string]$Path)

    $sha256Provider = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            return ([System.BitConverter]::ToString($sha256Provider.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha256Provider.Dispose()
    }
}

function Get-ContentTreeSha256 {
    param([string]$RootDirectory)

    # Builds a "<relative-path>\n<file-sha256>\n" manifest over every file under
    # $RootDirectory (paths separated with '/', sorted with the ordinal
    # comparer) and returns @(file-count, sha256-of-manifest). Two checkouts of
    # identical content always produce the same digest, unlike zip bytes.
    $fileEntries = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $RootDirectory -Recurse -File)) {
        $relativeName = $file.FullName.Substring($RootDirectory.Length + 1).Replace('\', '/')
        $fileEntries[$relativeName] = Get-FileSha256 -Path $file.FullName
    }

    $sortedNames = [string[]]$fileEntries.Keys
    [array]::Sort($sortedNames, [System.StringComparer]::Ordinal)

    $manifest = New-Object System.Text.StringBuilder
    foreach ($name in $sortedNames) {
        [void]$manifest.Append($name).Append([char]10).Append($fileEntries[$name]).Append([char]10)
    }

    $manifestSha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $manifestBytes = [System.Text.Encoding]::UTF8.GetBytes($manifest.ToString())
        $digest = ([System.BitConverter]::ToString($manifestSha256.ComputeHash($manifestBytes))).Replace('-', '').ToLowerInvariant()
        return @($sortedNames.Count, $digest)
    }
    finally {
        $manifestSha256.Dispose()
    }
}

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
}
else {
    New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null
    Invoke-FileDownload -Url $downloadUrl -DestinationPath $archivePath -SocksProxy $socksProxy
    $actualSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "Zig archive checksum mismatch: expected $expectedSha256, got $actualSha256"
    }

    $expandedDirectory = Join-Path $toolsDirectory "zig-x86_64-windows-$version"
    Expand-Archive -LiteralPath $archivePath -DestinationPath $toolsDirectory -Force
    Move-Item -LiteralPath $expandedDirectory -Destination $installDirectory
    Remove-Item -LiteralPath $archivePath
    & $zigExecutable version
}

# GitHub codeload generates tag archives on the fly and does NOT guarantee
# byte-stable zips over time, so the archive hash below is only a reference
# signal. The real integrity gate is content based: a full manifest digest
# when installing, and key-file hashes when skipping an existing install.
$lvglVersion = '9.5.0'
$lvglArchiveName = "lvgl-$lvglVersion.zip"
$lvglDownloadUrl = "https://github.com/lvgl/lvgl/archive/refs/tags/v$lvglVersion.zip"
$lvglReferenceArchiveSha256 = 'ed25a729864e6be6904cb2e5e0c8566366f4798b694c676b62e10f9b54865697'
$lvglExpectedTreeSha256 = 'cedc41dde8a63350905f06d93cfd789b0c2ad51ba47520083edb3efaf9f5ab58'
$lvglExpectedFileCount = 4847

# Lightweight integrity probe used when an installed copy is skipped: a few
# files that pin the version, licence and core implementation.
$lvglKeyFileHashes = @{
    'lvgl.h'            = 'bfd10da864777a7c44afd4c7de6ec4ededef6e7371a4108b64b80d5161a84400'
    'lv_version.h'      = 'fa0c49e41ae3125e82bedcc3e62e7202b5340f7dd4cd51fd212c414361b59879'
    'LICENCE.txt'       = '27a80bd36832ab42d35ad60c08b2b230a807a9bc0d58e94ec1531543dc49cbe8'
    'src/core/lv_obj.c' = '513314f0a766d916a2edf766b5e4865aa2001c14326489b5708139b5a3ea4cb0'
}

$lvglArchivePath = Join-Path $toolsDirectory $lvglArchiveName
$lvglInstallDirectory = Join-Path $toolsDirectory "lvgl-$lvglVersion"
$lvglHeaderMarker = Join-Path $lvglInstallDirectory 'lvgl.h'

if (Test-Path -LiteralPath $lvglHeaderMarker) {
    foreach ($entry in $lvglKeyFileHashes.GetEnumerator()) {
        $keyFilePath = Join-Path $lvglInstallDirectory ($entry.Key -replace '/', '\')
        if (-not (Test-Path -LiteralPath $keyFilePath) -or (Get-FileSha256 -Path $keyFilePath) -ne $entry.Value) {
            throw "LVGL installation under $lvglInstallDirectory failed the integrity check for '$($entry.Key)'. Remove that directory and re-run this script to reinstall it."
        }
    }
    Write-Output "LVGL $lvglVersion already available under $lvglInstallDirectory (key-file hashes verified)"
}
else {
    New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null
    Invoke-FileDownload -Url $lvglDownloadUrl -DestinationPath $lvglArchivePath -SocksProxy $socksProxy
    $lvglActualSha256 = Get-FileSha256 -Path $lvglArchivePath
    if ($lvglActualSha256 -ne $lvglReferenceArchiveSha256) {
        Write-Warning "LVGL archive SHA256 ($lvglActualSha256) differs from the recorded reference ($lvglReferenceArchiveSha256). GitHub regenerates tag archives, so this alone is not fatal; the extracted content will be verified instead."
    }

    # Extract into a staging directory first, verify the content, and only
    # then move it into place so a bad archive never replaces a good install.
    # The GitHub archive contains a single top-level "lvgl-<version>/" dir.
    $lvglStagingDirectory = "$lvglInstallDirectory.staging"
    if (Test-Path -LiteralPath $lvglStagingDirectory) {
        Remove-Item -LiteralPath $lvglStagingDirectory -Recurse -Force
    }
    Expand-Archive -LiteralPath $lvglArchivePath -DestinationPath $lvglStagingDirectory -Force
    $lvglStagedRoot = Join-Path $lvglStagingDirectory "lvgl-$lvglVersion"
    if (-not (Test-Path -LiteralPath (Join-Path $lvglStagedRoot 'lvgl.h'))) {
        throw "LVGL archive extracted without the expected $lvglStagedRoot layout."
    }

    $treeFileCount, $treeDigest = Get-ContentTreeSha256 -RootDirectory $lvglStagedRoot
    if ($treeFileCount -ne $lvglExpectedFileCount -or $treeDigest -ne $lvglExpectedTreeSha256) {
        throw "LVGL content verification failed: expected $lvglExpectedFileCount files with tree digest $lvglExpectedTreeSha256, got $treeFileCount files with digest $treeDigest. The downloaded archive does not match LVGL $lvglVersion; it is kept at $lvglArchivePath for inspection."
    }

    if (Test-Path -LiteralPath $lvglInstallDirectory) {
        Remove-Item -LiteralPath $lvglInstallDirectory -Recurse -Force
    }
    Move-Item -LiteralPath $lvglStagedRoot -Destination $lvglInstallDirectory
    Remove-Item -LiteralPath $lvglStagingDirectory
    Remove-Item -LiteralPath $lvglArchivePath
    Write-Output "LVGL $lvglVersion installed under $lvglInstallDirectory (content digest verified)"
}
