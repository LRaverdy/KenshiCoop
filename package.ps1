# Builds the zip to send to friends: the DLL, the installer and the read-me. No compiler needed on
# their side. Usage: .\package.ps1   ->   dist\KenshiCoop.zip
$ErrorActionPreference = "Stop"
$dll = Join-Path $PSScriptRoot "build\bin\Release\KenshiCoop.dll"
if (-not (Test-Path $dll)) { throw "KenshiCoop.dll not found: build it first (.\build.ps1)" }

$stage = Join-Path $PSScriptRoot "dist\KenshiCoop"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll $stage
# plain batch installers (no PowerShell, no execution-policy bypass: antivirus heuristics flag those)
Copy-Item (Join-Path $PSScriptRoot "tools\Installer.bat") $stage
Copy-Item (Join-Path $PSScriptRoot "tools\Desinstaller.bat") $stage
Copy-Item (Join-Path $PSScriptRoot "README.md") (Join-Path $stage "LISEZMOI.md")

$zip = Join-Path $PSScriptRoot "dist\KenshiCoop.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "Package: $zip"
