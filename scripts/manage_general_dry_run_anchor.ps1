[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Install', 'Remove')]
    [string] $Mode,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedCurrentProxySha256,

    [Parameter()]
    [string] $GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition',

    [Parameter()]
    [string] $AssetSource = 'C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_core\ui_icon_highlight.dds'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedGameHash = '5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB'
$expectedAssetHash = '7D7CA1D2B1A411DA28BA4071BB6D0A9AC54FFB5F2C0E608B28333A2D34EA3254'
$expectedAssetLength = 4224
$relativeAsset = 'mods\zz_general_dry_run_probe\media\ui\ui_core\ui_icon_highlight.dds'

function Assert-NoReparsePoint {
    param(
        [Parameter(Mandatory)] [string] $Root,
        [Parameter(Mandatory)] [string] $Candidate
    )

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $candidateFull = [IO.Path]::GetFullPath($Candidate)
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $candidateFull.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Diagnostic anchor escaped the game directory: $candidateFull"
    }

    $current = $rootFull
    foreach ($component in $candidateFull.Substring($prefix.Length).Split(
            [IO.Path]::DirectorySeparatorChar,
            [StringSplitOptions]::RemoveEmptyEntries)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "A diagnostic path component is a reparse point: $current"
            }
        }
        $current = Join-Path $current $component
    }
    if (Test-Path -LiteralPath $current) {
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "A diagnostic path component is a reparse point: $current"
        }
    }
}

function Remove-EmptyProbeTree {
    param(
        [Parameter(Mandatory)] [string] $ProbeRoot,
        [Parameter(Mandatory)] [string] $StartDirectory
    )

    $probeFull = [IO.Path]::GetFullPath($ProbeRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $current = [IO.Path]::GetFullPath($StartDirectory).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    while ($current.StartsWith(
            $probeFull, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $current -PathType Container)) {
        if (Get-ChildItem -LiteralPath $current -Force | Select-Object -First 1) {
            break
        }
        [IO.Directory]::Delete($current, $false)
        if ($current.Equals($probeFull, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = Split-Path -Parent $current
    }
}

if (Get-Process -Name 'Darksiders2' -ErrorAction SilentlyContinue) {
    throw 'Darksiders2.exe is running. Close the game before managing the diagnostic anchor.'
}

$gameRoot = (Resolve-Path -LiteralPath $GameDirectory -ErrorAction Stop).Path
$gameExecutable = Join-Path $gameRoot 'Darksiders2.exe'
$proxyTarget = Join-Path $gameRoot 'dinput8.dll'
$probeRoot = Join-Path $gameRoot 'mods\zz_general_dry_run_probe'
$assetTarget = Join-Path $gameRoot $relativeAsset
$assetParent = Split-Path -Parent $assetTarget
Assert-NoReparsePoint -Root $gameRoot -Candidate $assetTarget

if ((Get-FileHash -Algorithm SHA256 -LiteralPath $gameExecutable).Hash -ne
        $expectedGameHash) {
    throw 'The installed executable is not the fail-closed supported build.'
}
$expectedProxyHash = $ExpectedCurrentProxySha256.ToUpperInvariant()
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $proxyTarget).Hash -ne
        $expectedProxyHash) {
    throw 'The deployed proxy is not the explicitly expected diagnostic build.'
}

$upakBefore = @(
    Get-ChildItem -LiteralPath (Join-Path $gameRoot 'media') -Filter '*.upak' -File |
        Sort-Object FullName |
        ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
)

$changed = $false
if ($Mode -eq 'Install') {
    $source = (Resolve-Path -LiteralPath $AssetSource -ErrorAction Stop).Path
    if ((Get-Item -LiteralPath $source).Length -ne $expectedAssetLength -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne
            $expectedAssetHash) {
        throw 'The passive anchor source does not match the verified extracted DDS.'
    }
    if (Test-Path -LiteralPath $assetTarget) {
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -ne
                $expectedAssetHash) {
            throw 'A different file already occupies the diagnostic anchor target.'
        }
    } else {
        if (Test-Path -LiteralPath $probeRoot) {
            throw 'The diagnostic mod root already exists; refusing to merge into it.'
        }
        New-Item -ItemType Directory -Path $assetParent -Force | Out-Null
        Assert-NoReparsePoint -Root $gameRoot -Candidate $assetTarget
        $stage = Join-Path $assetParent ('.' + [Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            Copy-Item -LiteralPath $source -Destination $stage
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $stage).Hash -ne
                    $expectedAssetHash) {
                throw 'The staged diagnostic anchor failed hash verification.'
            }
            [IO.File]::Move($stage, $assetTarget)
            $changed = $true
            if ((Get-Item -LiteralPath $assetTarget).Length -ne
                    $expectedAssetLength -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -ne
                    $expectedAssetHash) {
                throw 'The installed diagnostic anchor failed verification.'
            }
        } catch {
            if (Test-Path -LiteralPath $stage -PathType Leaf) {
                [IO.File]::Delete($stage)
            }
            if ($changed -and (Test-Path -LiteralPath $assetTarget -PathType Leaf) -and
                (Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -eq
                    $expectedAssetHash) {
                [IO.File]::Delete($assetTarget)
            }
            Remove-EmptyProbeTree -ProbeRoot $probeRoot -StartDirectory $assetParent
            throw
        }
    }
} else {
    if (-not (Test-Path -LiteralPath $assetTarget -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -ne
            $expectedAssetHash) {
        throw 'The exact passive diagnostic anchor is not installed.'
    }
    $files = @(Get-ChildItem -LiteralPath $probeRoot -File -Recurse -Force)
    if ($files.Count -ne 1 -or
        -not $files[0].FullName.Equals(
            $assetTarget, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The diagnostic mod contains unexpected files; refusing removal.'
    }
    Get-ChildItem -LiteralPath $probeRoot -Directory -Recurse -Force |
        ForEach-Object {
            if (($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "The diagnostic mod contains a reparse point: $($_.FullName)"
            }
        }
    [IO.File]::Delete($assetTarget)
    $changed = $true
    Remove-EmptyProbeTree -ProbeRoot $probeRoot -StartDirectory $assetParent
    if (Test-Path -LiteralPath $probeRoot) {
        throw 'The diagnostic mod root was not empty after removing its only file.'
    }
}

$upakAfter = @(
    Get-ChildItem -LiteralPath (Join-Path $gameRoot 'media') -Filter '*.upak' -File |
        Sort-Object FullName |
        ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
)
if (Compare-Object -ReferenceObject $upakBefore -DifferenceObject $upakAfter) {
    throw 'Unexpected .upak metadata change detected while managing the diagnostic anchor.'
}

[pscustomobject]@{
    Mode = $Mode
    Changed = $changed
    AssetPath = $assetTarget
    AssetSha256 = $expectedAssetHash
    UpakMetadataUnchanged = $true
}
