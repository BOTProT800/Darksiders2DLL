#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition',
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(0,255)][int]$Slots = 0,
    [hashtable]$CategorySlots = @{}
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).Path.TrimEnd('\')
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
if ($outputRoot.Equals($gameRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $outputRoot.StartsWith($gameRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Genera el mod fuera de la instalacion; despues podras copiarlo a mods.'
}
$fields = @(
    @{name='PrimaryWeapon'; offset=0x80B242; original=21},
    @{name='SecondaryWeapon'; offset=0x80B2C2; original=21},
    @{name='Shoulder'; offset=0x80B33B; original=21},
    @{name='BodyArmor'; offset=0x80B3B5; original=22},
    @{name='Gauntlet'; offset=0x80B42E; original=22},
    @{name='Boot'; offset=0x80B4A3; original=22},
    @{name='Talisman'; offset=0x80B51C; original=21}
)
foreach ($key in $CategorySlots.Keys) {
    if ($key -cnotin $fields.name) { throw "Categoria desconocida: $key" }
}
$source = Join-Path $gameRoot 'media\scripts.obsp'
$bytes = [IO.File]::ReadAllBytes($source)
$sha = [Security.Cryptography.SHA256]::Create()
try { $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','') }
finally { $sha.Dispose() }
if ($bytes.Length -ne 18334463 -or $hash -ne 'B46DD3DA7F17A0ED016E30AF523BFBA86D358C9920DFFA866C69D4A0F4F67C7C') {
    throw 'scripts.obsp no coincide con la version original compatible.'
}
$changes = 0
foreach ($field in $fields) {
    $value = if ($CategorySlots.ContainsKey($field.name)) { $CategorySlots[$field.name] }
             elseif ($Slots -gt 0) { $Slots } else { $field.original }
    $parsed = 0
    if (![int]::TryParse([string]$value, [ref]$parsed) -or $parsed -lt $field.original -or $parsed -gt 255) {
        throw "Cantidad invalida para $($field.name): debe ser un entero entre $($field.original) y 255."
    }
    if ([BitConverter]::ToUInt32($bytes, $field.offset) -ne $field.original) {
        throw "La instruccion original no coincide: $($field.name)"
    }
    [BitConverter]::GetBytes($parsed).CopyTo($bytes, $field.offset)
    if ($parsed -ne $field.original) { $changes++ }
    Write-Output "$($field.name)=$parsed"
}
if ($changes -eq 0) { throw 'No hay cambios: indica -Slots o -CategorySlots.' }
$target = Join-Path $outputRoot 'media\scripts.obsp'
if (Test-Path -LiteralPath $target) { throw "El destino ya existe; usa otra carpeta: $target" }
New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
$file = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
Write-Output "Mod preparado: $target"
Write-Output 'No se instalo el mod. Requiere DLL 0.7.0 y scripts=inventory en [loader].'
