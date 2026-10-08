# Installs KenshiCoop into a Kenshi folder: copies the DLL and registers it as an Ogre plugin.
# Usage: .\install.ps1 [-KenshiDir "C:\...\Kenshi"] [-Uninstall]
param(
    [string]$KenshiDir = "${env:ProgramFiles(x86)}\Steam\steamapps\common\Kenshi",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $KenshiDir "kenshi_x64.exe"
$cfg = Join-Path $KenshiDir "Plugins_x64.cfg"
if (-not (Test-Path $exe)) { throw "kenshi_x64.exe not found in '$KenshiDir' (use -KenshiDir)" }
if (Get-Process kenshi_x64 -ErrorAction SilentlyContinue) { throw "Close Kenshi first." }

$lines = Get-Content $cfg
$entry = "Plugin=KenshiCoop"
if ($Uninstall) {
    $lines | Where-Object { $_.Trim() -ne $entry } | Set-Content $cfg -Encoding ascii
    Remove-Item (Join-Path $KenshiDir "KenshiCoop.dll") -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $KenshiDir "KenshiCoop.pdb") -ErrorAction SilentlyContinue
    Write-Host "KenshiCoop removed (KenshiCoop.ini and logs were kept)."
    return
}

$dll = Join-Path $PSScriptRoot "build\bin\Release\KenshiCoop.dll"
if (-not (Test-Path $dll)) { $dll = Join-Path $PSScriptRoot "KenshiCoop.dll" }
if (-not (Test-Path $dll)) { throw "KenshiCoop.dll not found: build it first (.\build.ps1)" }

$backup = "$cfg.before-kenshicoop"
if (-not (Test-Path $backup)) { Copy-Item $cfg $backup }
Copy-Item $dll $KenshiDir -Force
$pdb = [IO.Path]::ChangeExtension($dll, ".pdb")
if (Test-Path $pdb) { Copy-Item $pdb $KenshiDir -Force }
if (-not ($lines | Where-Object { $_.Trim() -eq $entry })) {
    ($lines + $entry) | Set-Content $cfg -Encoding ascii
}
Write-Host "KenshiCoop installed in $KenshiDir"
Write-Host "Settings: $(Join-Path $KenshiDir 'KenshiCoop.ini') (created on first launch)"
