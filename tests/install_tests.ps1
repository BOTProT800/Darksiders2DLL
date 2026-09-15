#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameExecutable,
    [Parameter(Mandatory=$true)][string]$DllPath,
    [string]$PackageDirectory = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $root ('build\installer-tests-' + [guid]::NewGuid().ToString('N'))
$game = Join-Path $testRoot 'game'
$package = Join-Path $testRoot 'package'
New-Item -ItemType Directory -Path $game,$package | Out-Null
Copy-Item -LiteralPath $GameExecutable -Destination (Join-Path $game 'Darksiders2.exe')
if ($PackageDirectory) {
    foreach ($name in @('Install.ps1','dinput8.dll','Darksiders2DLL.ini','manifest.json')) {
        Copy-Item -LiteralPath (Join-Path $PackageDirectory $name) -Destination (Join-Path $package $name)
    }
} else {
    Copy-Item -LiteralPath (Join-Path $root 'distribution\Install.ps1') -Destination $package
    Copy-Item -LiteralPath (Join-Path $root 'distribution\Darksiders2DLL.ini') -Destination $package
    Copy-Item -LiteralPath $DllPath -Destination (Join-Path $package 'dinput8.dll')
    @{version='0.4.0'; dllSha256=(Get-FileHash -LiteralPath $DllPath).Hash;
      configSha256=(Get-FileHash -LiteralPath (Join-Path $package 'Darksiders2DLL.ini')).Hash} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
}
$checks = [System.Collections.Generic.List[string]]::new()
function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Invoke-Installer([string]$Name, [bool]$Success, [string]$Action='Install', [string]$Replace='', [string]$Directory=$game) {
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $package 'Install.ps1'),'-GameDirectory',$Directory,'-Action',$Action)
    if ($Replace) { $arguments += @('-ReplaceSha256',$Replace) }
    # Bypass is scoped to this trusted test child, never persisted or shipped as
    # part of the installer. All modifications stay in this workspace fixture.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & powershell.exe @arguments 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    $output | Out-String | Set-Content -LiteralPath (Join-Path $testRoot "$Name.log")
    Require (($code -eq 0) -eq $Success) "Unexpected installer result: $Name (exit $code). See $testRoot"
    $checks.Add($Name)
}
$dll = Join-Path $game 'dinput8.dll'
$config = Join-Path $game 'Darksiders2DLL.ini'
$backup = Join-Path $game 'dinput8.dll.ds2-backup'
$mod = Join-Path $game 'mods\any_name\media\sentinel.txt'
$upak = Join-Path $game 'media\media.upak'
New-Item -ItemType Directory -Path (Split-Path $mod),(Split-Path $upak) | Out-Null
[IO.File]::WriteAllText($mod, 'USER MOD DATA')
[IO.File]::WriteAllText($upak, 'PACKAGE MUST NOT CHANGE')
[IO.File]::WriteAllText($config, "[loader]`nenabled=false`nmode=observe`n")
$preservedConfig = (Get-FileHash -LiteralPath $config).Hash
$preservedMod = (Get-FileHash -LiteralPath $mod).Hash
$preservedUpak = (Get-FileHash -LiteralPath $upak).Hash

Invoke-Installer 'fresh-install' $true
Require ((Get-FileHash -LiteralPath $dll).Hash -eq (Get-FileHash -LiteralPath $DllPath).Hash) 'Wrong installed DLL'
Invoke-Installer 'verify' $true 'Verify'
Invoke-Installer 'duplicate-refused' $false
Invoke-Installer 'fresh-uninstall' $true 'Uninstall'
Require (!(Test-Path -LiteralPath $dll)) 'Uninstall left DLL'

[IO.File]::WriteAllBytes($dll, [byte[]](1,2,3,4,5))
$old = (Get-FileHash -LiteralPath $dll).Hash
Invoke-Installer 'unknown-proxy-refused' $false
Invoke-Installer 'wrong-replace-hash-refused' $false 'Install' ('0' * 64)
Invoke-Installer 'backup-install' $true 'Install' $old
Require ((Get-FileHash -LiteralPath $backup).Hash -eq $old) 'Backup is not original'
[IO.File]::WriteAllBytes($dll, [byte[]](9,9,9))
Invoke-Installer 'modified-dll-uninstall-refused' $false 'Uninstall'
Copy-Item -LiteralPath $DllPath -Destination $dll -Force
[IO.File]::WriteAllBytes($backup, [byte[]](8,8,8))
Invoke-Installer 'modified-backup-refused' $false 'Uninstall'
[IO.File]::WriteAllBytes($backup, [byte[]](1,2,3,4,5))
Invoke-Installer 'backup-restored' $true 'Uninstall'
Require ((Get-FileHash -LiteralPath $dll).Hash -eq $old) 'Uninstall did not restore old proxy'
Remove-Item -LiteralPath $dll

$packageDll = Join-Path $package 'dinput8.dll'
[IO.File]::WriteAllBytes($packageDll, [byte[]](0,1,2))
Invoke-Installer 'tampered-package-refused' $false
Require (!(Test-Path -LiteralPath $dll)) 'Tampered package installed DLL'
Copy-Item -LiteralPath $DllPath -Destination $packageDll -Force

$lock = [IO.File]::Open((Join-Path $game 'Darksiders2.exe'),'Open','Read','Read')
try { Invoke-Installer 'busy-executable-refused' $false } finally { $lock.Dispose() }
$redirect = Join-Path $testRoot 'redirect'
New-Item -ItemType Junction -Path $redirect -Target $game | Out-Null
Invoke-Installer 'junction-directory-refused' $false 'Install' '' $redirect

$wrongGame = Join-Path $testRoot 'wrong-game'
New-Item -ItemType Directory -Path $wrongGame | Out-Null
[IO.File]::WriteAllText((Join-Path $wrongGame 'Darksiders2.exe'), 'not supported')
Invoke-Installer 'unsupported-game-refused' $false 'Install' '' $wrongGame
Require (!(Test-Path -LiteralPath (Join-Path $wrongGame 'dinput8.dll'))) 'Unsupported game modified'
Require ((Get-FileHash -LiteralPath $config).Hash -eq $preservedConfig) 'User INI changed'
Require ((Get-FileHash -LiteralPath $mod).Hash -eq $preservedMod) 'User mods changed'
Require ((Get-FileHash -LiteralPath $upak).Hash -eq $preservedUpak) 'Package changed'
# Exercise the actual documented repeated-script workflow in one PS session,
# with a missing INI so the default-configuration creation is covered too.
Remove-Item -LiteralPath $config
$sameSession = Join-Path $testRoot 'same-session.ps1'
@'
param([string]$Installer,[string]$Game)
$ErrorActionPreference = 'Stop'
& $Installer -GameDirectory $Game
& $Installer -GameDirectory $Game -Action Verify
& $Installer -GameDirectory $Game -Action Uninstall
'@ | Set-Content -LiteralPath $sameSession -Encoding ASCII
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $sameSession -Installer (Join-Path $package 'Install.ps1') -Game $game
Require ($LASTEXITCODE -eq 0) 'Repeated installer invocation failed'
Require ((Test-Path -LiteralPath $config) -and !(Test-Path -LiteralPath $dll)) 'Default INI creation/uninstall failed'
$checks.Add('same-session-install-verify-uninstall')
$checks.Add('default-ini-created-and-preserved')
$result = @{status='PASS'; dllSha256=(Get-FileHash -LiteralPath $DllPath).Hash;
    installerSha256=(Get-FileHash -LiteralPath (Join-Path $package 'Install.ps1')).Hash;
    checks=$checks.ToArray(); fixture=$testRoot; realGameModified=$false}
$result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $testRoot 'installer-validation.json') -Encoding UTF8
Write-Output "installer_tests: PASS ($($checks.Count) cases). $testRoot"
