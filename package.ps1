# Builds the zip to send to friends: the DLL, the installer and the read-me. No compiler needed on
# their side. Usage: .\package.ps1   ->   dist\KenshiCoop.zip
$ErrorActionPreference = "Stop"
$dll = Join-Path $PSScriptRoot "build\bin\Release\KenshiCoop.dll"
if (-not (Test-Path $dll)) { throw "KenshiCoop.dll not found: build it first (.\build.ps1)" }

$stage = Join-Path $PSScriptRoot "dist\KenshiCoop"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll $stage
Copy-Item (Join-Path $PSScriptRoot "install.ps1") $stage
Copy-Item (Join-Path $PSScriptRoot "README.md") (Join-Path $stage "LISEZMOI.md")
Set-Content (Join-Path $stage "Installer.bat") -Encoding ascii -Value @(
    '@echo off',
    'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"',
    'pause')
Set-Content (Join-Path $stage "Desinstaller.bat") -Encoding ascii -Value @(
    '@echo off',
    'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall',
    'pause')

$zip = Join-Path $PSScriptRoot "dist\KenshiCoop.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "Package: $zip"
