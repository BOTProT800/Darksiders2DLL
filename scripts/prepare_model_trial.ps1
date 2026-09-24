#requires -Version 5.1
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$evidence = Join-Path $root 'build\validation-shape-trial'
$proof = Get-Content -LiteralPath (Join-Path $evidence 'VALIDACION.json') -Raw | ConvertFrom-Json
if ($proof.status -ne 'PASS' -or $proof.version -ne '0.5.1-shape-trial' -or !$proof.reproducible) {
    throw 'Primero ejecuta verify_release.ps1 -EvidenceName validation-shape-trial para 0.5.1-shape-trial.'
}
foreach ($name in @('test:tests-debug-models','test:tests-release-models','test:tests-observe-models')) {
    if ($proof.checks -notcontains $name) { throw "Falta evidencia: $name" }
}
foreach ($source in $proof.sources) {
    if ((Get-FileHash -LiteralPath (Join-Path $root $source.path)).Hash -ne $source.sha256) {
        throw "El codigo cambio despues de la verificacion: $($source.path)"
    }
}
$dll = Join-Path $evidence 'release-a\out\dinput8.dll'
if ((Get-FileHash -LiteralPath $dll).Hash -ne $proof.dllSha256) { throw 'La DLL no coincide con la evidencia.' }
$destination = Join-Path $root 'build\shape-trial\package'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$files = [ordered]@{
    'dinput8.dll' = $dll
    'offline_tests.exe' = (Join-Path $evidence 'tests-release\out\offline_tests.exe')
    'VALIDACION.json' = (Join-Path $evidence 'VALIDACION.json')
    'LEEME.md' = (Join-Path $root 'distribution\MODELOS_PRUEBA.md')
    'CREDITS.md' = (Join-Path $root 'CREDITS.md')
    'THIRD_PARTY_NOTICES.md' = (Join-Path $root 'THIRD_PARTY_NOTICES.md')
    'check_model.ps1' = (Join-Path $PSScriptRoot 'check_model.ps1')
}
foreach ($entry in $files.GetEnumerator()) {
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $destination $entry.Key) -Force
}
@'
; Experimental 0.5.1-shape-trial. See LEEME.md before enabling model writes.
[loader]
enabled=true
mode=override
models=off
'@ | Set-Content -LiteralPath (Join-Path $destination 'Darksiders2DLL.ini') -Encoding ASCII
$names = @($files.Keys) + 'Darksiders2DLL.ini'
$hashes = foreach ($name in ($names | Sort-Object)) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $destination $name)).Hash, $name
}
$hashes | Set-Content -LiteralPath (Join-Path $destination 'CHECKSUMS.sha256') -Encoding ASCII
Write-Output "Preparado: $destination"
Write-Output 'El empaquetado no cambia la instalacion. La prueba visual de death_head.2 esta documentada en research/MODEL_TRIAL.md.'
