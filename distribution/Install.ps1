#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [ValidateSet('Install','Uninstall','Verify')][string]$Action = 'Install',
    # Required to replace an existing proxy. Obtain from Get-FileHash yourself.
    [ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ReplaceSha256
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$expectedGame = '5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB'
$held = [System.Collections.Generic.List[System.IDisposable]]::new()

# Pin existing directory components against rename/deletion for the operation.
# Check attributes on handles opened without following reparse points.
if (-not ('Ds2InstallPaths' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Ds2InstallPaths {
  [StructLayout(LayoutKind.Sequential)] struct Tag { public uint attributes, tag; }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
    IntPtr security, uint disposition, uint flags, IntPtr template);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool GetFileInformationByHandleEx(SafeFileHandle h, int kind, out Tag tag, uint size);
  public static SafeFileHandle Pin(string path) {
    var h = CreateFileW(path, 0x80, 3, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
    if(h.IsInvalid) { h.Dispose(); throw new Win32Exception(); }
    Tag tag;
    if(!GetFileInformationByHandleEx(h, 9, out tag, 8) || (tag.attributes & 0x410) != 0x10) {
      h.Dispose(); throw new InvalidOperationException("Directorio inseguro/reparse: " + path);
    }
    return h;
  }
}
'@
}
function Pin-Directory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^[A-Za-z]:\\' -or $full.Substring(2).Contains(':')) {
        throw 'Solo se admiten rutas locales absolutas sin ADS.'
    }
    $parts = [System.Collections.Generic.List[string]]::new()
    $cursor = [IO.DirectoryInfo]::new($full)
    while ($null -ne $cursor) { $parts.Add($cursor.FullName); $cursor = $cursor.Parent }
    for ($i = $parts.Count - 1; $i -ge 0; $i--) { $held.Add([Ds2InstallPaths]::Pin($parts[$i])) }
    return $full
}
function Assert-PlainFile([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Archivo inseguro/reparse: $Path"
    }
}
function Read-LockedBytes([string]$Path, [int]$Limit, [IO.FileShare]$Share = 'Read') {
    Assert-PlainFile $Path
    $stream = [IO.File]::Open($Path, 'Open', 'Read', $Share)
    $held.Add($stream)
    if ($stream.Length -le 0 -or $stream.Length -gt $Limit) { throw "Tamano invalido: $Path" }
    $bytes = [byte[]]::new([int]$stream.Length)
    $offset = 0
    while ($offset -lt $bytes.Length) {
        $count = $stream.Read($bytes, $offset, $bytes.Length - $offset)
        if ($count -le 0) { throw "Lectura incompleta: $Path" }
        $offset += $count
    }
    return ,$bytes
}
function Hash-Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','') }
    finally { $sha.Dispose() }
}
function Hash-File([string]$Path) {
    Assert-PlainFile $Path
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}
function Write-New([string]$Path, [byte[]]$Bytes) {
    $stream = [IO.File]::Open($Path, 'CreateNew', 'Write', 'None')
    try { $stream.Write($Bytes, 0, $Bytes.Length); $stream.Flush($true) }
    catch {
        $stream.Dispose()
        Remove-Item -LiteralPath $Path
        throw
    }
    finally { $stream.Dispose() }
}
try {
    $game = Pin-Directory $GameDirectory
    $package = Pin-Directory $PSScriptRoot
    if ($game.TrimEnd('\') -eq $package.TrimEnd('\')) { throw 'Extrae el paquete fuera de la carpeta del juego.' }
    $exe = Join-Path $game 'Darksiders2.exe'
    Assert-PlainFile $exe
    # Keep the executable exclusively open: no launch/replacement during writes.
    $exeBytes = Read-LockedBytes $exe (128 * 1024 * 1024) 'None'
    if ((Hash-Bytes $exeBytes) -ne $expectedGame) { throw 'Ejecutable no compatible; no se ha modificado la instalacion.' }
    if (@(Get-Process -Name 'Darksiders2' -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'Cierra Darksiders II normalmente antes de continuar.'
    }
    $dll = Join-Path $game 'dinput8.dll'
    $backup = Join-Path $game 'dinput8.dll.ds2-backup'
    $statePath = Join-Path $game 'Darksiders2DLL.install.json'
    $config = Join-Path $game 'Darksiders2DLL.ini'
    $manifestBytes = Read-LockedBytes (Join-Path $package 'manifest.json') 16384
    $manifest = [Text.Encoding]::UTF8.GetString($manifestBytes).TrimStart([char]0xFEFF) | ConvertFrom-Json
    if ($manifest.version -ne '0.4.0' -or $manifest.dllSha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
        $manifest.configSha256 -notmatch '^[A-Fa-f0-9]{64}$') { throw 'Manifiesto invalido.' }
    $newDll = Read-LockedBytes (Join-Path $package 'dinput8.dll') (16 * 1024 * 1024)
    $newConfig = Read-LockedBytes (Join-Path $package 'Darksiders2DLL.ini') 16384
    if ((Hash-Bytes $newDll) -ne $manifest.dllSha256 -or (Hash-Bytes $newConfig) -ne $manifest.configSha256) {
        throw 'El paquete no coincide con sus hashes; no se instalara.'
    }
    if ($Action -eq 'Verify') {
        if ((Hash-File $dll) -ne $manifest.dllSha256) { throw 'La DLL instalada no coincide con este paquete.' }
        Write-Output 'Verificacion correcta: ejecutable y DLL coinciden. El INI y los mods pueden estar personalizados.'
        return
    }
    if ($Action -eq 'Uninstall') {
        $stateBytes = Read-LockedBytes $statePath 16384
        $stateHandle = $held[$held.Count - 1]
        $state = [Text.Encoding]::UTF8.GetString($stateBytes).TrimStart([char]0xFEFF) | ConvertFrom-Json
        if ($state.version -ne '0.4.0' -or $state.dllSha256 -ne $manifest.dllSha256 -or
            (Hash-File $dll) -ne $manifest.dllSha256) { throw 'Estado o DLL diferente: se conserva todo para revision manual.' }
        if ($state.backupSha256) {
            if ($state.backupSha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
                (Hash-File $backup) -ne $state.backupSha256) { throw 'Backup modificado; se conserva todo.' }
            [IO.File]::Replace($backup, $dll, [NullString]::Value, $true)
        } else {
            if (Test-Path -LiteralPath $backup) { throw 'Backup inesperado; se conserva todo.' }
            Remove-Item -LiteralPath $dll
        }
        # Release the state handle before deleting only this known file.
        $stateHandle.Dispose()
        Remove-Item -LiteralPath $statePath
        Write-Output 'Desinstalado. Se conservaron el INI, los mods y los registros; se restauro el proxy previo si existia.'
        return
    }
    if (Test-Path -LiteralPath $statePath) {
        throw 'Ya existe una instalacion gestionada. Verifica o desinstala con su paquete antes de instalar otra version.'
    }
    if (Test-Path -LiteralPath $backup) { throw 'Ya existe un backup. No se sobrescribira.' }
    $oldHash = ''
    if (Test-Path -LiteralPath $dll) {
        $oldHash = Hash-File $dll
        if (!$ReplaceSha256 -or $oldHash -ne $ReplaceSha256) {
            throw "Ya existe dinput8.dll ($oldHash). Para reemplazar ESTA copia, usa -ReplaceSha256 con ese hash. Se guardara un backup."
        }
    }
    if (Test-Path -LiteralPath $config) { Assert-PlainFile $config }
    $stage = Join-Path $game ('Darksiders2DLL-' + [guid]::NewGuid().ToString('N') + '.tmp')
    $createdConfig = $false
    $installed = $false
    try {
        Write-New $stage $newDll
        if (!(Test-Path -LiteralPath $config)) { Write-New $config $newConfig; $createdConfig = $true }
        if ($oldHash) {
            if ((Hash-File $dll) -ne $oldHash) { throw 'El proxy cambio durante la instalacion.' }
            [IO.File]::Replace($stage, $dll, $backup, $true)
        } else { [IO.File]::Move($stage, $dll) }
        $installed = $true
        if ((Hash-File $dll) -ne $manifest.dllSha256) { throw 'Fallo la verificacion posterior.' }
        $state = @{ version='0.4.0'; dllSha256=$manifest.dllSha256; backupSha256=$oldHash }
        Write-New $statePath ([Text.Encoding]::UTF8.GetBytes(($state | ConvertTo-Json)))
    } catch {
        if ($installed) {
            if ($oldHash -and (Test-Path -LiteralPath $backup)) { [IO.File]::Replace($backup, $dll, [NullString]::Value, $true) }
            elseif (!$oldHash -and (Test-Path -LiteralPath $dll)) { Remove-Item -LiteralPath $dll }
        }
        if ($createdConfig) { Remove-Item -LiteralPath $config }
        throw
    } finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage }
    }
    Write-Output 'Instalado y verificado. Configura mods y abre el juego normalmente. Consulta INSTALACION.md.'
} finally {
    foreach ($handle in $held) { $handle.Dispose() }
}
