#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$DllPath,
    [Parameter(Mandatory=$true)][string]$ValidationPath,
    [Parameter(Mandatory=$true)][string]$InstallerValidationPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$validation = Get-Content -LiteralPath $ValidationPath -Raw | ConvertFrom-Json
$hash = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash
if ($validation.status -ne 'PASS' -or $validation.dllSha256 -ne $hash -or !$validation.reproducible) {
    throw 'El binario no tiene validacion coincidente.'
}
$installerValidation = Get-Content -LiteralPath $InstallerValidationPath -Raw | ConvertFrom-Json
$installerHash = (Get-FileHash -LiteralPath (Join-Path $root 'distribution\Install.ps1')).Hash
if ($installerValidation.status -ne 'PASS' -or $installerValidation.dllSha256 -ne $hash -or
    $installerValidation.installerSha256 -ne $installerHash) {
    throw 'El instalador y esta DLL no tienen una prueba coincidente.'
}
foreach ($source in $validation.sources) {
    if ((Get-FileHash -LiteralPath (Join-Path $root $source.path) -Algorithm SHA256).Hash -ne $source.sha256) {
        throw "La fuente cambio desde la validacion: $($source.path)"
    }
}
$output = Join-Path $root 'build\dist'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stage = Join-Path $output ('package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($name in @('Darksiders2DLL.ini','Install.ps1','INSTALACION.md','SEGURIDAD.md')) {
    Copy-Item -LiteralPath (Join-Path $root "distribution\$name") -Destination (Join-Path $stage $name)
}
foreach ($name in @('THIRD_PARTY_NOTICES.md','CREDITS.md')) {
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination (Join-Path $stage $name)
}
Copy-Item -LiteralPath $DllPath -Destination (Join-Path $stage 'dinput8.dll')
$validation | Add-Member -NotePropertyName installer -NotePropertyValue @{
    status='PASS'; sha256=$installerHash; checks=$installerValidation.checks;
    realGameModified=$installerValidation.realGameModified
} -Force
$validation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'VALIDACION.json') -Encoding UTF8
$manifest = @{version='0.4.0'; dllSha256=$hash; configSha256=(Get-FileHash -LiteralPath (Join-Path $stage 'Darksiders2DLL.ini')).Hash}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8
$sums = Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name
}
$sums | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ASCII
# An explicit allowlist prevents copyrighted game assets, executables, PDBs or
# test fixtures from entering the distributable.
$allowed = @('dinput8.dll','Darksiders2DLL.ini','Install.ps1','INSTALACION.md','SEGURIDAD.md',
             'THIRD_PARTY_NOTICES.md','CREDITS.md','VALIDACION.json','manifest.json','SHA256SUMS.txt')
$files = @(Get-ChildItem -LiteralPath $stage -File)
if ($files.Count -ne $allowed.Count -or @($files | Where-Object Name -NotIn $allowed).Count) {
    throw 'Contenido inesperado en el paquete.'
}
$zip = Join-Path $output 'Darksiders2DLL-0.4.0-win64.zip'
Compress-Archive -LiteralPath $files.FullName -DestinationPath $zip -Force
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$zipHash  Darksiders2DLL-0.4.0-win64.zip" | Set-Content -LiteralPath "$zip.sha256" -Encoding ASCII
Write-Output "Paquete: $zip"
Write-Output "SHA256: $zipHash"
Write-Output "Carpeta: $stage"
