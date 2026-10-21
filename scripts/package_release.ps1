#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$DllPath,
    [Parameter(Mandatory=$true)][string]$ValidationPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
$validation = Get-Content -LiteralPath $ValidationPath -Raw | ConvertFrom-Json
# Only plain Release versions: trial and diagnostic builds are never packaged.
$version = [string]$validation.version
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "La version $version no es de Release; no publiques una prueba o un diagnostico."
}
$hash = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash
if ($validation.status -ne 'PASS' -or $validation.dllSha256 -ne $hash -or !$validation.reproducible) {
    throw 'El binario no tiene validacion coincidente.'
}
foreach ($source in $validation.sources) {
    if ((Get-FileHash -LiteralPath (Join-Path $root $source.path) -Algorithm SHA256).Hash -ne $source.sha256) {
        throw "La fuente cambio desde la validacion: $($source.path)"
    }
}
# The ZIP mirrors the game folder: the player copies all of it next to
# Darksiders2.exe. Extras carry the Darksiders2DLL prefix so they are easy to
# tell apart from the game's own files.
$info = 'Darksiders2DLL-info'
$layout = [ordered]@{
    'dinput8.dll' = $DllPath
    'Darksiders2DLL.ini' = Join-Path $root 'distribution\Darksiders2DLL.ini'
    'Darksiders2DLL-LEEME.txt' = Join-Path $root 'distribution\Darksiders2DLL-LEEME.txt'
    'mods\LEEME.txt' = Join-Path $root 'distribution\mods\LEEME.txt'
    "$info\CREDITS.md" = Join-Path $root 'CREDITS.md'
    "$info\THIRD_PARTY_NOTICES.md" = Join-Path $root 'THIRD_PARTY_NOTICES.md'
    "$info\LICENSE" = Join-Path $root 'LICENSE'
}
foreach ($name in @($layout.Keys | Where-Object { $_ -ne 'dinput8.dll' })) {
    $path = $layout[$name].Substring($root.Length + 1).Replace('\','/')
    if (@($validation.sources | Where-Object path -eq $path).Count -ne 1) {
        throw "El archivo del paquete no forma parte de la validacion: $path"
    }
}
$output = Join-Path $root 'build\dist'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stage = Join-Path $output ('package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage,(Join-Path $stage 'mods'),(Join-Path $stage $info) | Out-Null
foreach ($name in $layout.Keys) {
    Copy-Item -LiteralPath $layout[$name] -Destination (Join-Path $stage $name)
}
$validation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage "$info\VALIDACION.json") -Encoding UTF8
# Install.ps1 (advanced, kept in the repository) reads this manifest.
$manifest = @{version=$version; dllSha256=$hash; configSha256=(Get-FileHash -LiteralPath (Join-Path $stage 'Darksiders2DLL.ini')).Hash}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage "$info\manifest.json") -Encoding UTF8
function Get-RelativeFiles {
    @(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
        [pscustomobject]@{Name=$_.FullName.Substring($stage.Length + 1); FullName=$_.FullName}
    } | Sort-Object Name)
}
$sums = Get-RelativeFiles | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name.Replace('\','/')
}
$sums | Set-Content -LiteralPath (Join-Path $stage "$info\SHA256SUMS.txt") -Encoding ASCII
# An explicit allowlist prevents copyrighted game assets, executables, PDBs or
# test fixtures from entering the distributable.
$allowed = @($layout.Keys) + @("$info\VALIDACION.json", "$info\manifest.json", "$info\SHA256SUMS.txt")
$files = @(Get-RelativeFiles)
if ($files.Count -ne $allowed.Count -or @($files | Where-Object Name -NotIn $allowed).Count) {
    throw 'Contenido inesperado en el paquete.'
}
$zipName = "Darksiders2DLL-$version-win64.zip"
$zip = Join-Path $output $zipName
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
# Entries use '/' as the ZIP format requires; Compress-Archive in Windows
# PowerShell 5.1 writes '\', which other extractors keep in the file name.
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in $files) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName,
            $file.Name.Replace('\','/'), [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try { $entries = @($archive.Entries | ForEach-Object FullName | Sort-Object) }
finally { $archive.Dispose() }
if (Compare-Object $entries @($files | ForEach-Object { $_.Name.Replace('\','/') })) {
    throw 'El ZIP no contiene exactamente los archivos del paquete.'
}
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$zipHash  $zipName" | Set-Content -LiteralPath "$zip.sha256" -Encoding ASCII
Write-Output "Paquete: $zip"
Write-Output "SHA256: $zipHash"
Write-Output "Carpeta: $stage"
