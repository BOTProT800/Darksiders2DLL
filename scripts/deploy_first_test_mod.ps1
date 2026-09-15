[CmdletBinding()]
param(
    [Parameter()]
    [string] $GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition',

    [Parameter(Mandatory)]
    [string] $AssetSource,

    [Parameter()]
    [string] $ProxySource = (Join-Path $PSScriptRoot '..\x64\Release\dinput8.dll'),

    [Parameter()]
    [string] $ProxySmokeSource = (Join-Path $PSScriptRoot '..\x64\Release\proxy_smoke.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedGameHash = '5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB'
$expectedAssetHash = '2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719'
$expectedAssetLength = 4224
$relativeAsset = 'media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds'

function Assert-SafeDestinationPath {
    param(
        [Parameter(Mandatory)] [string] $BasePath,
        [Parameter(Mandatory)] [string] $CandidatePath
    )

    $baseFull = [IO.Path]::GetFullPath($BasePath).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $candidateFull = [IO.Path]::GetFullPath($CandidatePath)
    $prefix = $baseFull + [IO.Path]::DirectorySeparatorChar
    if (-not $candidateFull.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Deployment destination escaped the game directory: $candidateFull"
    }

    $baseItem = Get-Item -LiteralPath $baseFull -Force
    if (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "The game directory is a reparse point: $baseFull"
    }

    $relative = $candidateFull.Substring($prefix.Length)
    $current = $baseFull
    foreach ($component in $relative.Split(
            [IO.Path]::DirectorySeparatorChar,
            [StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $component
        if (-not (Test-Path -LiteralPath $current)) {
            break
        }
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "A deployment path component is a reparse point: $current"
        }
    }
}

if (Get-Process -Name 'Darksiders2' -ErrorAction SilentlyContinue) {
    throw 'Darksiders2.exe is running. Close the game before deployment.'
}

$gameRoot = (Resolve-Path -LiteralPath $GameDirectory -ErrorAction Stop).Path
$gameExecutable = Join-Path $gameRoot 'Darksiders2.exe'
if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    throw "Darksiders2.exe was not found under: $gameRoot"
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $gameExecutable).Hash -ne $expectedGameHash) {
    throw 'The installed executable is not the fail-closed supported build.'
}

$resolvedAssetSource = (Resolve-Path -LiteralPath $AssetSource -ErrorAction Stop).Path
$assetInfo = Get-Item -LiteralPath $resolvedAssetSource
if ($assetInfo.Length -ne $expectedAssetLength) {
    throw "The edited DDS has an unexpected length: $($assetInfo.Length)"
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedAssetSource).Hash -ne $expectedAssetHash) {
    throw 'The edited DDS SHA-256 does not match first_test_mod.'
}

$resolvedProxySource = (Resolve-Path -LiteralPath $ProxySource -ErrorAction Stop).Path
$resolvedProxySmoke = (Resolve-Path -LiteralPath $ProxySmokeSource -ErrorAction Stop).Path
if ((Get-Item -LiteralPath $resolvedProxySource).Length -le 0) {
    throw 'The Release proxy DLL is empty.'
}
& $resolvedProxySmoke $resolvedProxySource
if ($LASTEXITCODE -ne 0) {
    throw "The proxy smoke test failed with exit code $LASTEXITCODE."
}

$modsRoot = Join-Path $gameRoot 'mods'
$firstModRoot = Join-Path $modsRoot 'first_test_mod'
$proxyTarget = Join-Path $gameRoot 'dinput8.dll'
$assetTarget = Join-Path $firstModRoot $relativeAsset
$assetParent = Split-Path -Parent $assetTarget

if (Test-Path -LiteralPath $proxyTarget) {
    throw 'A local dinput8.dll already exists. This controlled first deployment will not overwrite it.'
}
if (Test-Path -LiteralPath $firstModRoot) {
    throw 'first_test_mod already exists. This controlled first deployment will not merge into it.'
}
if ((Test-Path -LiteralPath $modsRoot) -and
    (Get-ChildItem -LiteralPath $modsRoot -Force | Select-Object -First 1)) {
    throw 'The mods directory is not empty. The controlled first test requires an isolated mod set.'
}

Assert-SafeDestinationPath -BasePath $gameRoot -CandidatePath $proxyTarget
Assert-SafeDestinationPath -BasePath $gameRoot -CandidatePath $assetParent

$upakBefore = @(
    Get-ChildItem -LiteralPath (Join-Path $gameRoot 'media') -Filter '*.upak' -File |
        Sort-Object FullName |
        ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
)

New-Item -ItemType Directory -Path $assetParent -Force | Out-Null
Assert-SafeDestinationPath -BasePath $gameRoot -CandidatePath $assetParent

$stageId = [Guid]::NewGuid().ToString('N')
$assetStage = Join-Path $assetParent ".$stageId.asset.tmp"
$proxyStage = Join-Path $gameRoot ".$stageId.proxy.tmp"
$sourceProxyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedProxySource).Hash
$gameLock = $null
$assetCommitted = $false
$proxyCommitted = $false
$targetProxyHash = $null

try {
    Copy-Item -LiteralPath $resolvedAssetSource -Destination $assetStage
    Copy-Item -LiteralPath $resolvedProxySource -Destination $proxyStage
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $assetStage).Hash -ne $expectedAssetHash) {
        throw 'Staged DDS verification failed.'
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $proxyStage).Hash -ne $sourceProxyHash) {
        throw 'Staged proxy DLL verification failed.'
    }

    try {
        $gameLock = [IO.File]::Open(
            $gameExecutable,
            [IO.FileMode]::Open,
            [IO.FileAccess]::Read,
            [IO.FileShare]::None)
    } catch {
        throw 'Could not exclusively lock Darksiders2.exe; ensure the game is fully closed.'
    }
    if (Get-Process -Name 'Darksiders2' -ErrorAction SilentlyContinue) {
        throw 'Darksiders2.exe started during deployment.'
    }
    if ((Test-Path -LiteralPath $proxyTarget) -or (Test-Path -LiteralPath $assetTarget)) {
        throw 'A deployment target appeared concurrently; refusing to overwrite it.'
    }
    Assert-SafeDestinationPath -BasePath $gameRoot -CandidatePath $proxyTarget
    Assert-SafeDestinationPath -BasePath $gameRoot -CandidatePath $assetTarget

    [IO.File]::Move($assetStage, $assetTarget)
    $assetCommitted = $true
    [IO.File]::Move($proxyStage, $proxyTarget)
    $proxyCommitted = $true

    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -ne $expectedAssetHash) {
        throw 'Post-copy DDS verification failed.'
    }
    $targetProxyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $proxyTarget).Hash
    if ($targetProxyHash -ne $sourceProxyHash) {
        throw 'Post-copy proxy DLL verification failed.'
    }

    $upakAfter = @(
        Get-ChildItem -LiteralPath (Join-Path $gameRoot 'media') -Filter '*.upak' -File |
            Sort-Object FullName |
            ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
    )
    if (Compare-Object -ReferenceObject $upakBefore -DifferenceObject $upakAfter) {
        throw 'Unexpected .upak metadata change detected during deployment.'
    }
} catch {
    $failure = $_
    if ($proxyCommitted -and (Test-Path -LiteralPath $proxyTarget -PathType Leaf)) {
        try {
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $proxyTarget).Hash -eq $sourceProxyHash) {
                [IO.File]::Delete($proxyTarget)
            }
        } catch {}
    }
    if ($assetCommitted -and (Test-Path -LiteralPath $assetTarget -PathType Leaf)) {
        try {
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -eq $expectedAssetHash) {
                [IO.File]::Delete($assetTarget)
            }
        } catch {}
    }
    throw $failure
} finally {
    if ($null -ne $gameLock) {
        $gameLock.Dispose()
    }
    if (Test-Path -LiteralPath $assetStage -PathType Leaf) {
        [IO.File]::Delete($assetStage)
    }
    if (Test-Path -LiteralPath $proxyStage -PathType Leaf) {
        [IO.File]::Delete($proxyStage)
    }
}

[pscustomobject]@{
    GameExecutableSha256 = $expectedGameHash
    ProxyPath = $proxyTarget
    ProxySha256 = $targetProxyHash
    AssetPath = $assetTarget
    AssetSha256 = $expectedAssetHash
    UpakMetadataUnchanged = $true
}
