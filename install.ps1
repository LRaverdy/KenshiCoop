# Installs KenshiCoop into a Kenshi folder: copies the DLL and registers it as an Ogre plugin.
# Usage: .\install.ps1 [-KenshiDir "C:\...\Kenshi"] [-Uninstall]
param(
    [string]$KenshiDir = "",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"

# Find Kenshi in the Steam libraries (it may live on another drive) when no folder is given.
function Find-Kenshi {
    $steam = $null
    try { $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath } catch {}
    $roots = @()
    if ($steam) { $roots += ($steam -replace '/', '\') }
    $roots += "${env:ProgramFiles(x86)}\Steam"
    foreach ($root in $roots | Select-Object -Unique) {
        $vdf = Join-Path $root "steamapps\libraryfolders.vdf"
        $libs = @($root)
        if (Test-Path $vdf) {
            # paths are written with doubled backslashes in the .vdf
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\\\', '\') }
        }
        foreach ($lib in $libs | Select-Object -Unique) {
            $dir = Join-Path $lib "steamapps\common\Kenshi"
            if (Test-Path (Join-Path $dir "kenshi_x64.exe")) { return $dir }
        }
    }
    return $null
}
if (-not $KenshiDir) { $KenshiDir = Find-Kenshi }
if (-not $KenshiDir) { throw "Kenshi not found in the Steam libraries: run .\install.ps1 -KenshiDir 'X:\...\Kenshi'" }
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
