#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Original,
    [Parameter(Mandatory=$true)][string]$Modified,
    [string]$Validator = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$Validator) {
    $Validator = Join-Path $PSScriptRoot 'offline_tests.exe'
    if (!(Test-Path -LiteralPath $Validator -PathType Leaf)) {
        $Validator = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\shape-trial\package\offline_tests.exe'
    }
}
$exe = (Resolve-Path -LiteralPath $Validator).ProviderPath
$before = (Resolve-Path -LiteralPath $Original).ProviderPath
$after = (Resolve-Path -LiteralPath $Modified).ProviderPath
& $exe '--model-check' $before $after
exit $LASTEXITCODE
