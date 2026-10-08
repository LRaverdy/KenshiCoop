# Builds KenshiCoop (Release, x64) with the Visual Studio 2022 toolchain.
param([string]$Config = "Release", [switch]$Asan)
$ErrorActionPreference = "Stop"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio 2022 with the C++ workload is required" }
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$build = Join-Path $PSScriptRoot ("build" + ($(if ($Asan) { "-asan" } else { "" })))
$asanFlag = $(if ($Asan) { "ON" } else { "OFF" })
& $cmake -S $PSScriptRoot -B $build -G "Visual Studio 17 2022" -A x64 "-DKC_ASAN=$asanFlag" | Out-Host
if ($LASTEXITCODE) { throw "cmake configure failed" }
& $cmake --build $build --config $Config --parallel | Out-Host
if ($LASTEXITCODE) { throw "build failed" }
