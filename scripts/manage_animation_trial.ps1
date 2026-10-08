# Local, reversible observation session for the prepared D_Idle.anm probe.
# Plan and Report never modify the installation. Observe installs the verified
# 0.8.0 DLL, adds animations=observe to the INI and copies the probe; Restore
# puts back exactly what Observe replaced. Run only with the game closed.
#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Plan','Observe','Report','Restore')][string]$Action = 'Plan',
    [string]$GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$trial = Join-Path $root 'build\animation-trial'
$package = Join-Path $trial 'package'
$backup = Join-Path $trial 'installation-backup'
$statePath = Join-Path $backup 'state.json'
$game = [IO.Path]::GetFullPath($GameDirectory).TrimEnd('\')
$version = '0.8.0-animation-trial'
$expectedGame = '5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB'
$expectedProbe = '18669CCE074A036E4105EB66670E13C3CB12CD7AF42A2E367BB9C862353DDCE6'
$probeRelative = 'mods\anim_probe\media\characters\death\D_Idle.anm'
$probeSource = Join-Path $trial $probeRelative
$allowed = @('dinput8.dll', 'Darksiders2DLL.ini', $probeRelative)
$probeDirectories = @('mods\anim_probe', 'mods\anim_probe\media', 'mods\anim_probe\media\characters',
    'mods\anim_probe\media\characters\death')

function HashBytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-','') }
    finally { $sha.Dispose() }
}
function Hash([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return '' }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe file: $Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}
function Assert-PlainPath([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Reparse point: $cursor"
        }
        $cursor = Split-Path -Parent $cursor
    }
}
function Target([string]$Relative) {
    if ($allowed -notcontains $Relative -and $probeDirectories -notcontains $Relative) { throw 'Unexpected deployment path.' }
    $path = [IO.Path]::GetFullPath((Join-Path $game $Relative))
    if (!$path.StartsWith($game + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside game.' }
    Assert-PlainPath $path
    return $path
}
function Publish([string]$Path, [byte[]]$Bytes) {
    $temp = $Path + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllBytes($temp, $Bytes)
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temp, $Path, [NullString]::Value) }
        else { [IO.File]::Move($temp, $Path) }
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
    }
}
function Assert-Package {
    $proof = Get-Content -LiteralPath (Join-Path $package 'VALIDACION.json') -Raw | ConvertFrom-Json
    if ($proof.status -ne 'PASS' -or $proof.version -ne $version -or !$proof.reproducible) {
        throw 'Missing animation-trial release evidence; run scripts/prepare_animation_trial.ps1.'
    }
    foreach ($line in Get-Content -LiteralPath (Join-Path $package 'CHECKSUMS.sha256')) {
        $hash, $name = $line -split '  ', 2
        if ((Hash (Join-Path $package $name)) -ne $hash) { throw "Package checksum mismatch: $name" }
    }
    if ((Hash (Join-Path $package 'dinput8.dll')) -ne $proof.dllSha256) { throw 'Package DLL differs from evidence.' }
    if ((Hash $probeSource) -ne $expectedProbe) { throw 'Prepared probe checksum mismatch.' }
}
function New-Config {
    # The current settings are kept; only the animation observation key is added.
    $path = Target 'Darksiders2DLL.ini'
    $text = "[loader]`r`nenabled=true`r`nmode=override`r`n"
    if (Test-Path -LiteralPath $path) {
        $bytes = [IO.File]::ReadAllBytes($path)
        if ($bytes | Where-Object { ($_ -lt 32 -and $_ -notin 9,10,13) -or $_ -gt 126 }) { throw 'Existing INI is not plain ASCII.' }
        $text = [Text.Encoding]::ASCII.GetString($bytes)
    }
    if ($text -match '(?im)^\s*animations\s*=') { throw 'The INI already configures animations; preserving it.' }
    if ($text -notmatch '(?im)^\s*\[loader\]\s*$') { throw 'The INI has no [loader] section; preserving it.' }
    if (!$text.EndsWith("`n")) { $text += "`r`n" }
    return [Text.Encoding]::ASCII.GetBytes($text + "animations=observe`r`n")
}

Assert-PlainPath $game
Assert-PlainPath $trial
if (Get-Process -Name Darksiders2 -ErrorAction SilentlyContinue) { throw 'Close the game normally first.' }

if ($Action -eq 'Report') {
    # Read-only: copy the newest session of this build and summarize animation events.
    $logs = Join-Path $env:LOCALAPPDATA 'Darksiders2DLL\logs'
    $session = Get-ChildItem -LiteralPath $logs -Filter '*.log' -File | Sort-Object LastWriteTimeUtc -Descending |
        Where-Object { (Get-Content -LiteralPath $_.FullName -TotalCount 1) -match "SESSION_START version=$([regex]::Escape($version)) " } |
        Select-Object -First 1
    if (!$session) { throw "No $version session log found." }
    Copy-Item -LiteralPath $session.FullName -Destination (Join-Path $trial 'observation.log') -Force
    $lines = Get-Content -LiteralPath $session.FullName
    Write-Output "Session: $($session.Name)"
    $lines | Where-Object { $_ -match ' ANIMATION_| MODEL_OVERRIDE_FAILED| CONFIG_REJECTED| LOADER_DISABLED' } |
        ForEach-Object { if ($_.Length -gt 400) { $_.Substring(0, 400) + ' ...' } else { $_ } }
    $stats = $lines | Where-Object { $_ -match ' RESOLVER_PROBE_STATS ' } | Select-Object -Last 1
    if ($stats) { Write-Output "Last stats: $stats" }
    $hit = $lines | Where-Object { $_ -match 'ANIMATION_VERIFIED_WOULD_OVERRIDE .*member_ordinal=610 .*changed=true .*path=media/characters/death/d_idle\.anm' }
    $failures = $lines | Where-Object { $_ -match 'MODEL_OVERRIDE_FAILED|model_failures=[1-9]|general_write_failures=[1-9]|general_write_verify_failures=[1-9]|dropped_events=[1-9]' }
    if ($hit -and !$failures) { Write-Output 'OBSERVATION PASS: the edited D_Idle.anm range was read, identified and left untouched.' }
    elseif ($failures) { Write-Output 'OBSERVATION REVIEW: failures or dropped events present.' }
    else { Write-Output 'OBSERVATION INCOMPLETE: no verified read of the edited range; see the lines above.' }
    return
}

$lock = [IO.File]::Open((Join-Path $game 'Darksiders2.exe'), 'Open', 'Read', 'None')
try {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $gameHash = [BitConverter]::ToString($sha.ComputeHash($lock)).Replace('-','') }
    finally { $sha.Dispose() }
    if ($gameHash -ne $expectedGame) { throw 'Unsupported executable.' }

    if ($Action -eq 'Restore') {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        if ($state.game -ne $game -or @($state.files).Count -ne $allowed.Count) { throw 'Unexpected backup state.' }
        foreach ($entry in $state.files) {
            if ($entry.backupName -notmatch '^[0-2]\.bin$') { throw 'Unexpected backup name.' }
            $current = Hash (Target $entry.relative)
            if ($current -ne $entry.afterHash -and $current -ne $entry.beforeHash) {
                throw "File changed since deployment; preserving it: $($entry.relative)"
            }
            if ($entry.beforeHash -and (Hash (Join-Path $backup $entry.backupName)) -ne $entry.beforeHash) {
                throw 'Backup checksum mismatch.'
            }
        }
        foreach ($entry in $state.files) {
            $path = Target $entry.relative
            if ($entry.beforeHash) { Publish $path ([IO.File]::ReadAllBytes((Join-Path $backup $entry.backupName))) }
            elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
            if ((Hash $path) -ne $entry.beforeHash) { throw "Restore checksum mismatch: $($entry.relative)" }
        }
        # Only directories this script created, deepest first, and only when empty.
        foreach ($relative in @($state.createdDirectories | Sort-Object Length -Descending)) {
            $directory = Target $relative
            if ((Test-Path -LiteralPath $directory) -and !(Get-ChildItem -LiteralPath $directory -Force)) {
                Remove-Item -LiteralPath $directory
            }
        }
        $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
        Rename-Item -LiteralPath $backup -NewName "installation-backup-restored-$stamp"
        Write-Output 'RESTORED: previous DLL, INI presence/content and mods folder.'
        return
    }

    Assert-Package
    $validator = Join-Path $package 'offline_tests.exe'
    & $validator --anim-catalog $game (Join-Path $trial 'mods')
    if ($LASTEXITCODE -ne 0) { throw 'Animation catalog preflight failed.' }
    if (Test-Path -LiteralPath (Target $probeRelative)) { throw 'The probe already exists in the game; preserving it.' }
    $sources = @([IO.File]::ReadAllBytes((Join-Path $package 'dinput8.dll')), (New-Config),
                 [IO.File]::ReadAllBytes($probeSource))
    $entries = @()
    for ($i = 0; $i -lt $allowed.Count; $i++) {
        $entries += [pscustomobject]@{relative=$allowed[$i]; backupName="$i.bin";
            beforeHash=(Hash (Target $allowed[$i])); afterHash=(HashBytes $sources[$i])}
    }
    $entries | Format-List
    Write-Output ('New INI:' + "`r`n" + [Text.Encoding]::ASCII.GetString($sources[1]))
    if ($Action -eq 'Plan') { Write-Output 'PLAN ONLY: no installation files modified.'; return }

    if (Test-Path -LiteralPath $backup) { throw 'Existing backup is never overwritten; restore first.' }
    New-Item -ItemType Directory -Path $backup | Out-Null
    $created = @($probeDirectories | Where-Object { !(Test-Path -LiteralPath (Target $_)) })
    for ($i = 0; $i -lt $allowed.Count; $i++) {
        $path = Target $allowed[$i]
        if ($entries[$i].beforeHash) {
            Copy-Item -LiteralPath $path -Destination (Join-Path $backup $entries[$i].backupName)
            if ((Hash (Join-Path $backup $entries[$i].backupName)) -ne $entries[$i].beforeHash) { throw 'Backup copy failed.' }
        }
    }
    $state = [pscustomobject]@{game=$game; version=$version; utc=[DateTime]::UtcNow.ToString('o');
        files=$entries; createdDirectories=$created}
    [IO.File]::WriteAllText($statePath, ($state | ConvertTo-Json -Depth 5))
    try {
        foreach ($relative in $created) { New-Item -ItemType Directory -Path (Target $relative) | Out-Null }
        for ($i = 0; $i -lt $allowed.Count; $i++) {
            $path = Target $allowed[$i]
            Publish $path $sources[$i]
            if ((Hash $path) -ne $entries[$i].afterHash) { throw "Install verification failed: $($allowed[$i])" }
        }
    } catch {
        Write-Warning "Installation failed; restoring: $_"
        foreach ($entry in $entries) {
            $path = Target $entry.relative
            if ($entry.beforeHash) { Publish $path ([IO.File]::ReadAllBytes((Join-Path $backup $entry.backupName))) }
            elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
            if ((Hash $path) -ne $entry.beforeHash) { throw "Rollback incomplete; backup kept in $backup" }
        }
        foreach ($relative in @($created | Sort-Object Length -Descending)) {
            $directory = Target $relative
            if ((Test-Path -LiteralPath $directory) -and !(Get-ChildItem -LiteralPath $directory -Force)) {
                Remove-Item -LiteralPath $directory
            }
        }
        Rename-Item -LiteralPath $backup -NewName ('installation-backup-failed-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
        throw
    }
    Write-Output "OBSERVATION INSTALLED ($version, animations=observe). Launch the game, load a save, stay idle as Death, then close normally and run -Action Report."
} finally {
    $lock.Dispose()
}
