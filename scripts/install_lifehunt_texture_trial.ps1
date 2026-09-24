#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Check','Install','Restore')][string]$Action = 'Check',
    [Parameter(Mandatory)][string]$GameDirectory,
    [Parameter(Mandatory)][string]$BackupDirectory,
    [string]$ExpectedCurrentProxySha256 = '',
    [string]$TrialDirectory = (Join-Path $PSScriptRoot '..\build\texture-hd-trial'),
    [string]$ValidationDirectory = (Join-Path $PSScriptRoot '..\build\validation-texture-trial')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Process Darksiders2 -ErrorAction SilentlyContinue) { throw 'Cierra Darksiders2 antes de instalar/restaurar.' }
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).Path.TrimEnd('\')
$backupRoot = [IO.Path]::GetFullPath($BackupDirectory)
if ($backupRoot.StartsWith($gameRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or $backupRoot -eq $gameRoot) {
    throw 'El respaldo debe estar fuera de la carpeta del juego y de mods.'
}
function Assert-NoLinks([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "No se permiten enlaces en la ruta: $cursor"
            }
        }
        $cursor = Split-Path -Parent $cursor
    }
}
function Target([string]$Relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $gameRoot $Relative))
    if (!$path.StartsWith($gameRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Destino fuera del juego.' }
    Assert-NoLinks $path
    return $path
}
function Hash-OrAbsent([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return 'ABSENT' }
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "No es un archivo: $Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}
Assert-NoLinks $backupRoot
if ((Hash-OrAbsent (Target 'Darksiders2.exe')) -ne '5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB') {
    throw 'Ejecutable incompatible.'
}
$relativeMod = 'mods\first_test_mod\media\characters\death'
$paths = @('dinput8.dll')
foreach ($map in @('diff','norm','spec')) {
    $paths += "$relativeMod\reaper_scythe_$map.dds"
    $paths += "$relativeMod\reaper_scythe_$map.dds.png"
}
$manifestPath = Join-Path $backupRoot 'installation.json'
if ($Action -eq 'Restore') {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.gameRoot -ne $gameRoot -or $manifest.files.Count -ne $paths.Count) { throw 'Respaldo incompatible.' }
    for ($i=0; $i -lt $paths.Count; ++$i) {
        $item = $manifest.files[$i]
        if ($item.relative -ne $paths[$i] -or (Hash-OrAbsent (Target $paths[$i])) -ne $item.installed) {
            throw "El destino cambio despues de la instalacion: $($paths[$i])"
        }
        if ($item.before -ne 'ABSENT' -and (Hash-OrAbsent (Join-Path $backupRoot "$i.original")) -ne $item.before) {
            throw 'Respaldo original modificado.'
        }
    }
    for ($i=0; $i -lt $paths.Count; ++$i) {
        $destination = Target $paths[$i]
        if ($manifest.files[$i].before -eq 'ABSENT') {
            if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination }
        } else { Copy-Item -LiteralPath (Join-Path $backupRoot "$i.original") -Destination $destination -Force }
        if ((Hash-OrAbsent $destination) -ne $manifest.files[$i].before) { throw 'Fallo la comprobacion de restauracion.' }
    }
    Write-Output 'RESTORED: DLL y mapas previos; los respaldos se conservan.'
    return
}
if ($ExpectedCurrentProxySha256 -notmatch '^[a-fA-F0-9]{64}$' -or
    (Hash-OrAbsent (Target 'dinput8.dll')) -ne $ExpectedCurrentProxySha256) { throw 'La DLL instalada no es la version esperada.' }
$validation = Get-Content -LiteralPath (Join-Path $ValidationDirectory 'VALIDACION.json') -Raw | ConvertFrom-Json
$dll = (Resolve-Path -LiteralPath (Join-Path $ValidationDirectory 'release-a\out\dinput8.dll')).Path
if ($validation.status -ne 'PASS' -or $validation.version -ne '0.6.0-texture-trial' -or
    (Hash-OrAbsent $dll) -ne $validation.dllSha256) { throw 'La DLL no coincide con la compilacion validada.' }
$ini = Get-Content -LiteralPath (Target 'Darksiders2DLL.ini') -Raw
if ($ini -notmatch '(?m)^\s*enabled\s*=\s*true\s*$' -or $ini -notmatch '(?m)^\s*mode\s*=\s*override\s*$' -or
    $ini -match '(?m)^\s*textures\s*=\s*exact\s*$') { throw 'La configuracion debe permitir sustituciones y texturas nativas.' }
$items = @()
for ($i=0; $i -lt $paths.Count; ++$i) {
    $source = ''
    if ($i -eq 0) { $source = $dll }
    elseif ($paths[$i].EndsWith('.png')) { $source = (Resolve-Path -LiteralPath (Join-Path $TrialDirectory $paths[$i])).Path }
    $installed = 'ABSENT'
    if ($source) { $installed = Hash-OrAbsent $source }
    $items += [pscustomobject]@{relative=$paths[$i]; source=$source; before=(Hash-OrAbsent (Target $paths[$i])); installed=$installed}
}
foreach ($map in @('diff','norm','spec')) {
    if ((Hash-OrAbsent (Target "$relativeMod\reaper_scythe_$map.png")) -ne 'ABSENT') { throw 'Hay otro alias PNG para el mismo mapa.' }
}
if ($Action -eq 'Check') { $items | Select-Object relative,before,installed | ConvertTo-Json; return }
if (Test-Path -LiteralPath $backupRoot) { throw 'Usa una carpeta de respaldo nueva.' }
New-Item -ItemType Directory -Path $backupRoot | Out-Null
for ($i=0; $i -lt $items.Count; ++$i) {
    if ($items[$i].before -ne 'ABSENT') {
        Copy-Item -LiteralPath (Target $paths[$i]) -Destination (Join-Path $backupRoot "$i.original")
        if ((Hash-OrAbsent (Join-Path $backupRoot "$i.original")) -ne $items[$i].before) { throw 'El archivo cambio durante el respaldo.' }
    }
}
@{gameRoot=$gameRoot; utc=[DateTime]::UtcNow.ToString('o'); files=$items; inGameValidated=$false} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
try {
    for ($i=0; $i -lt $items.Count; ++$i) {
        $item = $items[$i]; $destination = Target $item.relative
        if ((Hash-OrAbsent $destination) -ne $item.before) { throw 'Un destino cambio antes de instalar.' }
        if ($item.source) { Copy-Item -LiteralPath $item.source -Destination $destination -Force }
        elseif (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination }
        if ((Hash-OrAbsent $destination) -ne $item.installed) { throw 'Fallo la verificacion del archivo instalado.' }
    }
} catch {
    $installationError = $_
    for ($i=0; $i -lt $items.Count; ++$i) {
        $destination = Target $paths[$i]
        if ($items[$i].before -eq 'ABSENT') {
            if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination }
        } else { Copy-Item -LiteralPath (Join-Path $backupRoot "$i.original") -Destination $destination -Force }
    }
    throw $installationError
}
Write-Output "INSTALLED: 0.6.0-texture-trial + Lifehunt diff/norm/spec. Respaldo: $manifestPath"
