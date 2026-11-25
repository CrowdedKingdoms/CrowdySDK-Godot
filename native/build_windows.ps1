<#
.SYNOPSIS
Builds the Crowdy SDK GDExtension for Windows into addons/crowdy_sdk/bin.

.DESCRIPTION
Configures and builds native/ with CMake. godot-cpp and CrowdyCPP are built from the
third_party submodules into the same build tree. libcurl and OpenSSL come from vcpkg
(x64-windows): vcpkg install curl openssl --triplet x64-windows

.PARAMETER VcpkgRoot
The vcpkg installation that provides curl and openssl. Defaults to $env:VCPKG_ROOT.

.PARAMETER Config
Release, or RelWithDebInfo to debug the extension with symbols.

.PARAMETER Clean
Deletes the build directory first. Needed after changing VcpkgRoot.

.EXAMPLE
.\native\build_windows.ps1 -VcpkgRoot D:\Workspace\vcpkg
#>
param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [ValidateSet("Release", "RelWithDebInfo")]
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$buildDir = Join-Path $PSScriptRoot "build"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

$configureArgs = @("-S", $PSScriptRoot, "-B", $buildDir)
if ($VcpkgRoot) {
    $toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $toolchain)) {
        throw "No vcpkg toolchain at $toolchain"
    }
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
}

cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed."
}

cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed."
}

Write-Host "Built into $(Resolve-Path (Join-Path $PSScriptRoot '..\addons\crowdy_sdk\bin'))" -ForegroundColor Green
