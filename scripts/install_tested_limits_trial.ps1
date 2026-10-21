#requires -Version 5.1
# Prueba de 0.12.0-tested-limits-trial (RESTRICCIONES.md): DLL validada, INI sin textures= ni models=
# (comprueba los valores por defecto) y los mapas de la hombrera ds1a01_shoulder que haya en -TextureDirectory.
# Sin acentos: PowerShell 5.1 lee el script como ANSI.
# Check (predeterminado) solo muestra los cambios; Install respalda y escribe;
# Restore devuelve los archivos previos comprobando hashes.
[CmdletBinding()]
param(
    [ValidateSet('Check','Install','Restore')][string]$Action = 'Check',
    [Parameter(Mandatory)][string]$GameDirectory,
    [Parameter(Mandatory)][string]$BackupDirectory,
    # Carpeta con uno o varios de DS1A01_Shoulder_Diff.dds, _Norm.dds y _Spec.dds.
    [string]$TextureDirectory = '',
    [string]$ExpectedCurrentProxySha256 = '',
    [string]$ValidationDirectory = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$version = '0.12.0-tested-limits-trial'
if (!$ValidationDirectory) {
    $ValidationDirectory = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\build\validation-tested-limits-trial'
}
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
function Hash-Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('X2') }) -join '') } finally { $sha.Dispose() }
}
Assert-NoLinks $backupRoot
if ((Hash-OrAbsent (Target 'Darksiders2.exe')) -ne '5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB') {
    throw 'Ejecutable incompatible.'
}
$relativeMod = 'mods\first_test_mod\media\items\armor'
$allowed = @('dinput8.dll', 'Darksiders2DLL.ini')
foreach ($map in @('Diff','Norm','Spec')) { $allowed += "$relativeMod\DS1A01_Shoulder_$map.dds" }
$manifestPath = Join-Path $backupRoot 'installation.json'
if ($Action -eq 'Restore') {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $files = @($manifest.files)
    if ($manifest.gameRoot -ne $gameRoot -or $manifest.version -ne $version -or $files.Count -lt 3) { throw 'Respaldo incompatible.' }
    for ($i=0; $i -lt $files.Count; ++$i) {
        $item = $files[$i]
        if ($allowed -notcontains $item.relative -or (Hash-OrAbsent (Target $item.relative)) -ne $item.installed) {
            throw "El destino cambio despues de la instalacion: $($item.relative)"
        }
        if ($item.before -ne 'ABSENT' -and (Hash-OrAbsent (Join-Path $backupRoot "$i.original")) -ne $item.before) {
            throw 'Respaldo original modificado.'
        }
    }
    for ($i=0; $i -lt $files.Count; ++$i) {
        $destination = Target $files[$i].relative
        if ($files[$i].before -eq 'ABSENT') {
            if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination }
        } else { Copy-Item -LiteralPath (Join-Path $backupRoot "$i.original") -Destination $destination -Force }
        if ((Hash-OrAbsent $destination) -ne $files[$i].before) { throw 'Fallo la comprobacion de restauracion.' }
    }
    Write-Output 'RESTORED: DLL, INI y texturas previas; los respaldos se conservan.'
    return
}
if ($ExpectedCurrentProxySha256 -notmatch '^[a-fA-F0-9]{64}$' -or
    (Hash-OrAbsent (Target 'dinput8.dll')) -ne $ExpectedCurrentProxySha256) { throw 'La DLL instalada no es la version esperada.' }
$validation = Get-Content -LiteralPath (Join-Path $ValidationDirectory 'VALIDACION.json') -Raw | ConvertFrom-Json
$dll = (Resolve-Path -LiteralPath (Join-Path $ValidationDirectory 'release-a\out\dinput8.dll')).Path
if ($validation.status -ne 'PASS' -or $validation.version -ne $version -or
    (Hash-OrAbsent $dll) -ne $validation.dllSha256) { throw 'La DLL no coincide con la compilacion validada.' }

# INI: el actual sin textures= ni models=, para que se usen los valores por defecto (resize y override_unchecked).
$iniBytes = [IO.File]::ReadAllBytes((Target 'Darksiders2DLL.ini'))
$ini = [Text.Encoding]::ASCII.GetString($iniBytes)
if ($ini -notmatch '^\[loader\]\r?\n' -or @($ini -split '\r?\n' | Where-Object { $_ -match '^\s*\[' }).Count -ne 1 -or
    $ini -notmatch '(?m)^\s*enabled\s*=\s*true\s*$' -or $ini -notmatch '(?m)^\s*mode\s*=\s*override\s*$') {
    throw 'El INI debe tener solo [loader] con enabled=true y mode=override.'
}
$newIni = [regex]::Replace($ini, '(?m)^\s*(textures|models)\s*=.*(\r?\n|$)', '')
$newIniBytes = [Text.Encoding]::ASCII.GetBytes($newIni)

if (!$TextureDirectory) { throw 'Falta -TextureDirectory con los mapas de la hombrera.' }
$paths = @('dinput8.dll', 'Darksiders2DLL.ini')
$sources = @($dll, '')
foreach ($map in @('Diff','Norm','Spec')) {
    $candidate = Join-Path $TextureDirectory "DS1A01_Shoulder_$map.dds"
    if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
    $source = (Resolve-Path -LiteralPath $candidate).Path
    $head = [byte[]]::new(4)
    $stream = [IO.File]::OpenRead($source)
    try { if ($stream.Read($head, 0, 4) -ne 4) { throw "DDS truncado: $source" } } finally { $stream.Dispose() }
    if ([BitConverter]::ToUInt32($head, 0) -ne 0x20534444) { throw "No es un DDS: $source" }
    foreach ($alias in @("DS1A01_Shoulder_$map.png", "DS1A01_Shoulder_$map.dds.png")) {
        if ((Hash-OrAbsent (Target "$relativeMod\$alias")) -ne 'ABSENT') { throw "Hay otro alias para el mismo mapa: $alias" }
    }
    $paths += "$relativeMod\DS1A01_Shoulder_$map.dds"
    $sources += $source
}
if ($paths.Count -lt 3) { throw 'No hay ningun mapa DS1A01_Shoulder_*.dds en -TextureDirectory.' }
$items = @()
for ($i=0; $i -lt $paths.Count; ++$i) {
    $installed = if ($i -eq 1) { Hash-Bytes $newIniBytes } else { Hash-OrAbsent $sources[$i] }
    $items += [pscustomobject]@{relative=$paths[$i]; source=$sources[$i]; before=(Hash-OrAbsent (Target $paths[$i])); installed=$installed}
}
if ($Action -eq 'Check') {
    $items | Select-Object relative,before,installed,source | ConvertTo-Json
    Write-Output "INI nuevo:`n$newIni"
    return
}
if (Test-Path -LiteralPath $backupRoot) { throw 'Usa una carpeta de respaldo nueva.' }
New-Item -ItemType Directory -Path $backupRoot | Out-Null
for ($i=0; $i -lt $items.Count; ++$i) {
    if ($items[$i].before -ne 'ABSENT') {
        Copy-Item -LiteralPath (Target $paths[$i]) -Destination (Join-Path $backupRoot "$i.original")
        if ((Hash-OrAbsent (Join-Path $backupRoot "$i.original")) -ne $items[$i].before) { throw 'El archivo cambio durante el respaldo.' }
    }
}
$stagedIni = Join-Path $backupRoot 'Darksiders2DLL.ini.installed'
[IO.File]::WriteAllBytes($stagedIni, $newIniBytes)
$items[1].source = $stagedIni
@{gameRoot=$gameRoot; utc=[DateTime]::UtcNow.ToString('o'); version=$version; files=$items; inGameValidated=$false} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
try {
    New-Item -ItemType Directory -Path (Target $relativeMod) -Force | Out-Null
    for ($i=0; $i -lt $items.Count; ++$i) {
        $item = $items[$i]; $destination = Target $item.relative
        if ((Hash-OrAbsent $destination) -ne $item.before) { throw 'Un destino cambio antes de instalar.' }
        Copy-Item -LiteralPath $item.source -Destination $destination -Force
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
Write-Output "INSTALLED: $version, INI con valores por defecto, $($paths.Count - 2) mapas de la hombrera. Respaldo: $manifestPath"
