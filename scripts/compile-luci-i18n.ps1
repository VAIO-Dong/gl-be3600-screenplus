param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function ConvertFrom-PoQuotedString {
    param([string]$Value)

    return ConvertFrom-Json -InputObject ('"' + $Value + '"')
}

function Get-PoMessages {
    param([string]$Path)

    $messages = [ordered]@{}
    $messageId = $null
    $messageValue = $null
    $activeField = $null

    function Add-CurrentMessage {
        if ($null -ne $messageId -and $messageId.Length -gt 0) {
            if ($null -eq $messageValue -or $messageValue.Length -eq 0) {
                throw "Missing Simplified Chinese translation for '$messageId'."
            }
            if ($messages.Contains($messageId)) {
                throw "Duplicate translation key '$messageId'."
            }
            $messages.Add($messageId, $messageValue)
        }
    }

    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        if ($line -match '^msgid "(.*)"$') {
            Add-CurrentMessage
            $messageId = ConvertFrom-PoQuotedString -Value $Matches[1]
            $messageValue = $null
            $activeField = 'id'
        }
        elseif ($line -match '^msgstr "(.*)"$') {
            $messageValue = ConvertFrom-PoQuotedString -Value $Matches[1]
            $activeField = 'value'
        }
        elseif ($line -match '^"(.*)"$') {
            $fragment = ConvertFrom-PoQuotedString -Value $Matches[1]
            if ($activeField -eq 'id') {
                $messageId += $fragment
            }
            elseif ($activeField -eq 'value') {
                $messageValue += $fragment
            }
        }
    }

    Add-CurrentMessage
    return $messages
}

function Limit-UInt32 {
    param([Int64]$Value)

    return [BitConverter]::ToUInt32([BitConverter]::GetBytes($Value), 0)
}

function Get-LittleEndianUInt16 {
    param(
        [byte[]]$Bytes,
        [int]$Offset
    )

    return [UInt32]$Bytes[$Offset] + ([UInt32]$Bytes[$Offset + 1] -shl 8)
}

function Get-SuperFastHash {
    param([byte[]]$Bytes)

    $length = $Bytes.Length
    if ($length -eq 0) {
        return [UInt32]0
    }

    [UInt32]$hash = $length
    $remainder = $length -band 3
    $blocks = $length -shr 2
    $offset = 0

    for ($index = 0; $index -lt $blocks; $index++) {
        $hash = Limit-UInt32 ([Int64]$hash + (Get-LittleEndianUInt16 -Bytes $Bytes -Offset $offset))
        $part = [UInt64](Get-LittleEndianUInt16 -Bytes $Bytes -Offset ($offset + 2)) -shl 11
        [UInt32]$temporary = Limit-UInt32 ([Int64]((Limit-UInt32 ([Int64]$part)) -bxor $hash))
        $shiftedHash = [UInt64]$hash -shl 16
        $hash = Limit-UInt32 ([Int64]((Limit-UInt32 ([Int64]$shiftedHash)) -bxor $temporary))
        $offset += 4
        $hash = Limit-UInt32 ([Int64]$hash + ([UInt64]$hash -shr 11))
    }

    switch ($remainder) {
        3 {
            $hash = Limit-UInt32 ([Int64]$hash + (Get-LittleEndianUInt16 -Bytes $Bytes -Offset $offset))
            $hash = Limit-UInt32 ([Int64]($hash -bxor (Limit-UInt32 ([Int64]([UInt64]$hash -shl 16)))))
            $signedByte = if ($Bytes[$offset + 2] -ge 128) {
                [Int64]$Bytes[$offset + 2] - 256
            }
            else {
                [Int64]$Bytes[$offset + 2]
            }
            $hash = Limit-UInt32 ([Int64]($hash -bxor (Limit-UInt32 ($signedByte -shl 18))))
            $hash = Limit-UInt32 ([Int64]$hash + ([UInt64]$hash -shr 11))
        }
        2 {
            $hash = Limit-UInt32 ([Int64]$hash + (Get-LittleEndianUInt16 -Bytes $Bytes -Offset $offset))
            $hash = Limit-UInt32 ([Int64]($hash -bxor (Limit-UInt32 ([Int64]([UInt64]$hash -shl 11)))))
            $hash = Limit-UInt32 ([Int64]$hash + ([UInt64]$hash -shr 17))
        }
        1 {
            $signedByte = if ($Bytes[$offset] -ge 128) {
                [Int64]$Bytes[$offset] - 256
            }
            else {
                [Int64]$Bytes[$offset]
            }
            $hash = Limit-UInt32 ([Int64]$hash + $signedByte)
            $hash = Limit-UInt32 ([Int64]($hash -bxor (Limit-UInt32 ([Int64]([UInt64]$hash -shl 10)))))
            $hash = Limit-UInt32 ([Int64]$hash + ([UInt64]$hash -shr 1))
        }
    }

    $hash = Limit-UInt32 ([Int64]($hash -bxor (Limit-UInt32 ([Int64]([UInt64]$hash -shl 3)))))
    $hash = Limit-UInt32 ([Int64]$hash + ([UInt64]$hash -shr 5))
    $hash = Limit-UInt32 ([Int64]($hash -bxor (Limit-UInt32 ([Int64]([UInt64]$hash -shl 4)))))
    $hash = Limit-UInt32 ([Int64]$hash + ([UInt64]$hash -shr 17))
    $hash = Limit-UInt32 ([Int64]($hash -bxor (Limit-UInt32 ([Int64]([UInt64]$hash -shl 25)))))
    $hash = Limit-UInt32 ([Int64]$hash + ([UInt64]$hash -shr 6))
    return $hash
}

function Add-BigEndianUInt32 {
    param(
        [System.Collections.Generic.List[byte]]$Buffer,
        [UInt32]$Value
    )

    $Buffer.Add([byte]($Value -shr 24))
    $Buffer.Add([byte](($Value -shr 16) -band 0xff))
    $Buffer.Add([byte](($Value -shr 8) -band 0xff))
    $Buffer.Add([byte]($Value -band 0xff))
}

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$messages = Get-PoMessages -Path $resolvedInput
$utf8 = New-Object System.Text.UTF8Encoding($false)
$output = New-Object 'System.Collections.Generic.List[byte]'
$entries = New-Object System.Collections.Generic.List[object]

foreach ($entry in $messages.GetEnumerator()) {
    $keyBytes = $utf8.GetBytes([string]$entry.Key)
    $valueBytes = $utf8.GetBytes([string]$entry.Value)
    $keyHash = Get-SuperFastHash -Bytes $keyBytes
    $valueHash = Get-SuperFastHash -Bytes $valueBytes
    if ($keyHash -eq $valueHash) {
        continue
    }

    $entries.Add([pscustomobject]@{
        KeyId = $keyHash
        ValueId = [UInt32]1
        Offset = [UInt32]$output.Count
        Length = [UInt32]$valueBytes.Length
    })
    $output.AddRange($valueBytes)
    while (($output.Count -band 3) -ne 0) {
        $output.Add(0)
    }
}

$indexOffset = [UInt32]$output.Count
foreach ($entry in $entries | Sort-Object KeyId) {
    Add-BigEndianUInt32 -Buffer $output -Value $entry.KeyId
    Add-BigEndianUInt32 -Buffer $output -Value $entry.ValueId
    Add-BigEndianUInt32 -Buffer $output -Value $entry.Offset
    Add-BigEndianUInt32 -Buffer $output -Value $entry.Length
}
Add-BigEndianUInt32 -Buffer $output -Value $indexOffset

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
[System.IO.File]::WriteAllBytes($OutputPath, $output.ToArray())
Write-Output "Compiled $($entries.Count) Simplified Chinese LuCI messages to $OutputPath"
