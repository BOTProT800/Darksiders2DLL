# Compatibility entry point. The v6-only matrix is preserved as research text.
[CmdletBinding()]
param(
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe',
    [string]$GameDirectory = '',
    [string]$AssetSource = ''
)
& (Join-Path $PSScriptRoot 'verify_release.ps1') @PSBoundParameters
