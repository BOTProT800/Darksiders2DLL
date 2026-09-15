[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedCurrentProxySha256,

    [Parameter()]
    [string] $GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition',

    [Parameter()]
    [string] $ProxySource = (Join-Path $PSScriptRoot '..\x64\Release\dinput8.dll'),

    [Parameter()]
    [string] $ProxySmokeSource = (Join-Path $PSScriptRoot '..\x64\Release\proxy_smoke.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedGameHash = '5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB'
$expectedAssetHash = '2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719'
$relativeAsset = 'mods\first_test_mod\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds'

if (Get-Process -Name 'Darksiders2' -ErrorAction SilentlyContinue) {
    throw 'Darksiders2.exe is running. Close the game before updating the proxy.'
}

$gameRoot = (Resolve-Path -LiteralPath $GameDirectory -ErrorAction Stop).Path
$gameInfo = Get-Item -LiteralPath $gameRoot -Force
if (($gameInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'The game directory is a reparse point; refusing an elevated update.'
}

$gameExecutable = Join-Path $gameRoot 'Darksiders2.exe'
$proxyTarget = Join-Path $gameRoot 'dinput8.dll'
$assetTarget = Join-Path $gameRoot $relativeAsset
if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf) -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $gameExecutable).Hash -ne $expectedGameHash) {
    throw 'The installed executable is not the fail-closed supported build.'
}
if (-not (Test-Path -LiteralPath $proxyTarget -PathType Leaf)) {
    throw 'The deployed proxy is missing.'
}
if (-not (Test-Path -LiteralPath $assetTarget -PathType Leaf) -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -ne $expectedAssetHash) {
    throw 'The controlled first_test_mod asset is missing or has changed.'
}

$proxyInfo = Get-Item -LiteralPath $proxyTarget -Force
$assetInfo = Get-Item -LiteralPath $assetTarget -Force
if (($proxyInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    ($assetInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'A deployed file is a reparse point; refusing an elevated update.'
}

$expectedCurrentHash = $ExpectedCurrentProxySha256.ToUpperInvariant()
$currentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $proxyTarget).Hash
if ($currentHash -ne $expectedCurrentHash) {
    throw "The deployed proxy hash is not the explicitly expected version: $currentHash"
}

$resolvedProxySource = (Resolve-Path -LiteralPath $ProxySource -ErrorAction Stop).Path
$resolvedProxySmoke = (Resolve-Path -LiteralPath $ProxySmokeSource -ErrorAction Stop).Path
& $resolvedProxySmoke $resolvedProxySource
if ($LASTEXITCODE -ne 0) {
    throw "The replacement proxy failed its smoke test with exit code $LASTEXITCODE."
}
$sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedProxySource).Hash
if ($sourceHash -eq $currentHash) {
    [pscustomobject]@{
        ProxyPath = $proxyTarget
        PreviousSha256 = $currentHash
        ProxySha256 = $sourceHash
        AlreadyCurrent = $true
        UpakMetadataUnchanged = $true
    }
    exit 0
}

$upakBefore = @(
    Get-ChildItem -LiteralPath (Join-Path $gameRoot 'media') -Filter '*.upak' -File |
        Sort-Object FullName |
        ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
)

$operationId = [Guid]::NewGuid().ToString('N')
$proxyStage = Join-Path $gameRoot ".$operationId.proxy.tmp"
$proxyBackup = Join-Path $gameRoot ".$operationId.proxy.bak"
$gameLock = $null
$committed = $false
$succeeded = $false

try {
    Copy-Item -LiteralPath $resolvedProxySource -Destination $proxyStage
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $proxyStage).Hash -ne $sourceHash) {
        throw 'Staged proxy verification failed.'
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
        throw 'Darksiders2.exe started during the proxy update.'
    }
    $gameHasher = [Security.Cryptography.SHA256]::Create()
    try {
        $gameLock.Position = 0
        $lockedGameHash = [BitConverter]::ToString(
            $gameHasher.ComputeHash($gameLock)).Replace('-', '')
    } finally {
        $gameHasher.Dispose()
    }
    if ($lockedGameHash -ne $expectedGameHash) {
        throw 'Darksiders2.exe changed after validation; refusing to replace the proxy.'
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $assetTarget).Hash -ne
            $expectedAssetHash) {
        throw 'The controlled first_test_mod asset changed after validation.'
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $proxyTarget).Hash -ne $expectedCurrentHash) {
        throw 'The deployed proxy changed concurrently; refusing to replace it.'
    }
    if (((Get-Item -LiteralPath $proxyTarget -Force).Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The deployed proxy became a reparse point.'
    }

    [IO.File]::Replace($proxyStage, $proxyTarget, $proxyBackup, $true)
    $committed = $true
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $proxyTarget).Hash -ne $sourceHash) {
        throw 'Post-update proxy verification failed.'
    }

    $upakAfter = @(
        Get-ChildItem -LiteralPath (Join-Path $gameRoot 'media') -Filter '*.upak' -File |
            Sort-Object FullName |
            ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
    )
    if (Compare-Object -ReferenceObject $upakBefore -DifferenceObject $upakAfter) {
        throw 'Unexpected .upak metadata change detected during the proxy update.'
    }
    $succeeded = $true
} catch {
    $failure = $_
    if ($committed -and (Test-Path -LiteralPath $proxyBackup -PathType Leaf)) {
        try {
            [IO.File]::Replace($proxyBackup, $proxyTarget, $null, $true)
            $committed = $false
        } catch {
            throw "Proxy update failed and automatic rollback failed. Backup retained at: $proxyBackup"
        }
    }
    throw $failure
} finally {
    if ($null -ne $gameLock) {
        $gameLock.Dispose()
    }
    if (Test-Path -LiteralPath $proxyStage -PathType Leaf) {
        [IO.File]::Delete($proxyStage)
    }
    if ($succeeded -and (Test-Path -LiteralPath $proxyBackup -PathType Leaf)) {
        [IO.File]::Delete($proxyBackup)
    }
}

[pscustomobject]@{
    ProxyPath = $proxyTarget
    PreviousSha256 = $currentHash
    ProxySha256 = $sourceHash
    AlreadyCurrent = $false
    UpakMetadataUnchanged = $true
}
