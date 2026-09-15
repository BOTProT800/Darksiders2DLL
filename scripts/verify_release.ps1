#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe',
    [string]$GameDirectory = '',
    [string]$AssetSource = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$evidence = Join-Path $root 'build\validation-release-0.4'
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$checks = [System.Collections.Generic.List[string]]::new()
function Build([string]$Name, [string]$Project, [string]$Configuration, [string[]]$Properties = @()) {
    $out = (Join-Path $evidence "$Name\out").Replace('\','/') + '/'
    $obj = (Join-Path $evidence "$Name\obj").Replace('\','/') + '/'
    $parts = @($MSBuild, (Join-Path $root $Project), '/t:Rebuild', "/p:Configuration=$Configuration",
        '/p:Platform=x64', "/p:OutDir=$out", "/p:IntDir=$obj", '/p:BuildProjectReferences=false',
        '/m', '/v:minimal', "/flp:logfile=$(Join-Path $evidence "$Name.build.log");verbosity=normal") + $Properties
    foreach ($part in $parts) {
        if ($part -match '["&|<>^%!\r\n]') { throw 'Caracter de comando no admitido.' }
    }
    $command = 'set PATH=& ' + (($parts | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    & $env:ComSpec /d /s /c $command | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Compilacion fallida: $Name" }
    $checks.Add("build:$Name")
    return $out
}
function Run([string]$Name, [string]$Exe, [string[]]$Arguments = @()) {
    & $Exe @Arguments | Tee-Object -FilePath (Join-Path $evidence "$Name.run.log") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Prueba fallida: $Name" }
    $checks.Add("test:$Name")
}
function Installation-State {
    if (!$GameDirectory) { return $null }
    return @{ proxy=(Get-FileHash -LiteralPath (Join-Path $GameDirectory 'dinput8.dll')).Hash;
        upaks=@(Get-ChildItem -LiteralPath (Join-Path $GameDirectory 'media') -Filter '*.upak' -File |
            Sort-Object Name | ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) }
}
$before = Installation-State
if ($before) { $before | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'installation-before.json') }
$arguments = @()
if ($AssetSource) {
    $arguments += $AssetSource
    if ($GameDirectory) { $arguments += (Join-Path $GameDirectory 'Darksiders2.exe'); $arguments += (Join-Path $GameDirectory 'mods') }
}
foreach ($test in @(@('tests-debug','Debug','true'), @('tests-release','Release','true'), @('tests-observe','Release','false'))) {
    $dir = Build $test[0] 'Darksiders2DLL.Tests.vcxproj' $test[1] @("/p:TestGeneralResolverWrite=$($test[2])")
    Run $test[0] (Join-Path $dir 'offline_tests.exe') $arguments
}
$a = Build 'release-a' 'Darksiders2DLL.vcxproj' 'Release'
$b = Build 'release-b' 'Darksiders2DLL.vcxproj' 'Release'
$debug = Build 'debug' 'Darksiders2DLL.vcxproj' 'Debug'
$hash = (Get-FileHash -LiteralPath (Join-Path $a 'dinput8.dll')).Hash
if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $b 'dinput8.dll')).Hash) { throw 'Release no reproducible.' }
$checks.Add('release:byte-identical')
$smoke = Build 'smoke' 'Darksiders2DLL.ProxySmoke.vcxproj' 'Release'
foreach ($variant in @(@('release-a',$a), @('release-b',$b), @('debug',$debug))) {
    Run "smoke-$($variant[0])" (Join-Path $smoke 'proxy_smoke.exe') @((Join-Path $variant[1] 'dinput8.dll'))
}
$bytes = [IO.File]::ReadAllBytes((Join-Path $a 'dinput8.dll'))
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
if ([BitConverter]::ToUInt16($bytes, $pe+4) -ne 0x8664) { throw 'DLL no x64.' }
$characteristics = [BitConverter]::ToUInt16($bytes, $pe+24+70)
if (($characteristics -band 0x4160) -ne 0x4160) { throw 'Faltan ASLR/DEP/CFG/high entropy.' }
$text = [Text.Encoding]::Unicode.GetString($bytes)
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
foreach ($forbidden in @('first_test_mod','ui_hudicon_passiveability_improved_agility','debug_general_dds_write_prototype','D3DX11CreateTextureFromMemory')) {
    if ($text.Contains($forbidden) -or $ascii.Contains($forbidden)) { throw "Dependencia retirada encontrada: $forbidden" }
}
if (!$text.Contains('GENERAL_DDS_OVERRIDE_HIT')) { throw 'Falta el cargador general.' }
$checks.Add('release:PE-hardening-and-no-special-case')
$sources = @(Get-ChildItem -LiteralPath $root -File | Where-Object Extension -In @('.cpp','.h','.vcxproj','.def','.json'))
$sources += @(Get-ChildItem -LiteralPath (Join-Path $root 'tests') -File | Where-Object Extension -In @('.cpp','.h'))
$sourceHashes = @($sources | Sort-Object FullName | ForEach-Object {
    @{path=$_.FullName.Substring($root.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
if ($before) {
    $after = Installation-State
    $after | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'installation-after.json')
    if ($before.proxy -ne $after.proxy -or (Compare-Object $before.upaks $after.upaks)) { throw 'La instalacion cambio durante las pruebas.' }
    $checks.Add('game:proxy-and-upak-metadata-unchanged')
}
$result = @{status='PASS'; version='0.4.0'; utc=[DateTime]::UtcNow.ToString('o'); dllSha256=$hash;
    reproducible=$true; dllCharacteristics=('0x{0:X4}' -f $characteristics); checks=@($checks.ToArray());
    sources=$sourceHashes; inGameValidated=$false }
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'VALIDACION.json') -Encoding UTF8
Write-Output "PASS: $hash"
