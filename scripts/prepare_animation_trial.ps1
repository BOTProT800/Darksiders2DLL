#requires -Version 5.1
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = '0.8.0-animation-trial'
$evidence = Join-Path $root 'build\validation-animation-trial'
$proof = Get-Content -LiteralPath (Join-Path $evidence 'VALIDACION.json') -Raw | ConvertFrom-Json
if ($proof.status -ne 'PASS' -or $proof.version -ne $version -or !$proof.reproducible) {
    throw "Primero ejecuta verify_release.ps1 -EvidenceName validation-animation-trial para $version."
}
foreach ($suite in @('tests-debug','tests-release','tests-observe')) {
    foreach ($name in @("test:$suite-animations", "test:$suite-animation-corpus")) {
        if ($proof.checks -notcontains $name) { throw "Falta evidencia: $name" }
    }
}
foreach ($source in $proof.sources) {
    if ((Get-FileHash -LiteralPath (Join-Path $root $source.path)).Hash -ne $source.sha256) {
        throw "El codigo cambio despues de la verificacion: $($source.path)"
    }
}
$dll = Join-Path $evidence 'release-a\out\dinput8.dll'
if ((Get-FileHash -LiteralPath $dll).Hash -ne $proof.dllSha256) { throw 'La DLL no coincide con la evidencia.' }
$trial = Join-Path $root 'build\animation-trial'
$destination = Join-Path $trial 'package'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$files = [ordered]@{
    'dinput8.dll' = $dll
    'offline_tests.exe' = (Join-Path $evidence 'tests-release\out\offline_tests.exe')
    'VALIDACION.json' = (Join-Path $evidence 'VALIDACION.json')
    'LEEME.md' = (Join-Path $root 'distribution\ANIMACIONES_PRUEBA.md')
    'CREDITS.md' = (Join-Path $root 'CREDITS.md')
    'THIRD_PARTY_NOTICES.md' = (Join-Path $root 'THIRD_PARTY_NOTICES.md')
    'check_animation.ps1' = (Join-Path $PSScriptRoot 'check_animation.ps1')
}
foreach ($entry in $files.GetEnumerator()) {
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $destination $entry.Key) -Force
}
$hashes = foreach ($name in ($files.Keys | Sort-Object)) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $destination $name)).Hash, $name
}
$hashes | Set-Content -LiteralPath (Join-Path $destination 'CHECKSUMS.sha256') -Encoding ASCII
# The probe changes one rotation residual of D_Idle.anm by +1 (at most 0.0099 degrees).
$probe = 'mods\anim_probe\media\characters\death\D_Idle.anm'
$source = Join-Path $root "build\animation-catalog\$probe"
if ((Get-FileHash -LiteralPath $source).Hash -ne '18669CCE074A036E4105EB66670E13C3CB12CD7AF42A2E367BB9C862353DDCE6') {
    throw 'La sonda D_Idle.anm no coincide con la evidencia de la fase C.'
}
$target = Join-Path $trial $probe
New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $target -Force
Write-Output "Preparado: $destination"
Write-Output 'El empaquetado no cambia la instalacion. Revisa con: scripts\manage_animation_trial.ps1 -Action Plan'
