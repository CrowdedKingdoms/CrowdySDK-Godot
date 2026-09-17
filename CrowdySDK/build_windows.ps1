param(
    [string]$Root = "",
    [string]$Arch = "x64",
    [string]$Generator = "",
    [string]$Toolchain = "",
    [string]$CrowdyWithCurl = "ON",
    [string]$CrowdyWithOpenSSL = "ON"
)

Write-Host "Building CrowdyCPP GDExtension (Windows)"

# =============================================================================
# Paths
# =============================================================================

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrEmpty($Root)) {
    $root = (Resolve-Path (Join-Path $scriptDir "..")).Path
}
else {
    $root = (Resolve-Path $Root).Path
}

Write-Host "Workspace root: $root"

$ext_native = Join-Path $root "CrowdySDK\native"
$ext_build = Join-Path $ext_native "build"

$godot_cpp_dir = Join-Path $root "third_party\godotcpp"
$crowdycpp_dir = Join-Path $root "third_party\crowdycpp"

# =============================================================================
# Validate paths
# =============================================================================

if (-Not (Test-Path $ext_native)) {
    throw "CrowdySDK/native directory not found: $ext_native"
}

if (-Not (Test-Path $godot_cpp_dir)) {
    throw "godot-cpp directory not found: $godot_cpp_dir"
}

if (-Not (Test-Path $crowdycpp_dir)) {
    throw "CrowdyCPP directory not found: $crowdycpp_dir"
}

# =============================================================================
# Visual Studio generator detection
# =============================================================================

function Detect-VisualStudioGenerator {
    param(
        [string]$Requested
    )

    $vswhere = "$Env:ProgramFiles(x86)\Microsoft Visual Studio\Installer\vswhere.exe"

    if (-Not (Test-Path $vswhere)) {
        $vswhere = "$Env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
    }

    if (-Not (Test-Path $vswhere)) {
        if (-Not [string]::IsNullOrEmpty($Requested)) {
            return $Requested
        }

        Write-Host "vswhere.exe not found; falling back to 'NMake Makefiles' generator." -ForegroundColor Yellow
        return "NMake Makefiles"
    }

    try {
        $version = & "$vswhere" `
            -latest `
            -products * `
            -requires Microsoft.Component.MSBuild `
            -property installationVersion `
            2>$null
    }
    catch {
        $version = $null
    }

    if ([string]::IsNullOrEmpty($version)) {
        if (-Not [string]::IsNullOrEmpty($Requested)) {
            return $Requested
        }

        Write-Host "No Visual Studio instances found by vswhere; using 'NMake Makefiles'." -ForegroundColor Yellow
        return "NMake Makefiles"
    }

    $major = 0

    if ($version -match "^(\d+)") {
        $major = [int]$Matches[1]
    }

    switch ($major) {
        { $_ -ge 18 } {
            return "Visual Studio 18 2026"
        }

        17 {
            return "Visual Studio 17 2022"
        }

        16 {
            return "Visual Studio 16 2019"
        }

        15 {
            return "Visual Studio 15 2017"
        }

        default {
            return "NMake Makefiles"
        }
    }
}

# =============================================================================
# Select generator
# =============================================================================

if (-Not [string]::IsNullOrEmpty($Generator)) {
    if ($Generator -like "Visual Studio*") {
        $detected = Detect-VisualStudioGenerator $Generator

        if ($detected -ne $Generator) {
            Write-Host "Requested generator '$Generator' is not available; falling back to '$detected'." -ForegroundColor Yellow
            $Generator = $detected
        }
    }
}
else {
    $Generator = Detect-VisualStudioGenerator ""
    Write-Host "Autodetected CMake generator: $Generator"
}

# =============================================================================
# Build helper
# =============================================================================

function Run-CmakeBuild {
    param(
        [string]$SourceDir,
        [string]$BuildDir,
        [string]$ConfigureOptions = ""
    )

    if (-Not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    }

    Push-Location $BuildDir

    try {
        $genPrefix = ""

        if (-Not [string]::IsNullOrEmpty($Generator)) {
            $genPrefix = "-G `"$Generator`""
        }

        # ---------------------------------------------------------------------
        # Configure
        # ---------------------------------------------------------------------

        $cfgCmd = "cmake $genPrefix -S `"$SourceDir`" -B `"$BuildDir`" $ConfigureOptions"

        Write-Host ""
        Write-Host "============================================================"
        Write-Host "CMake configure"
        Write-Host "============================================================"
        Write-Host $cfgCmd
        Write-Host ""

        iex $cfgCmd

        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed."
        }

        # ---------------------------------------------------------------------
        # Build
        # ---------------------------------------------------------------------

        Write-Host ""
        Write-Host "============================================================"
        Write-Host "CMake build"
        Write-Host "============================================================"
        Write-Host ""

        cmake --build $BuildDir --config Release -j

        if ($LASTEXITCODE -ne 0) {
            throw "CMake build failed."
        }
    }
    finally {
        Pop-Location
    }
}

# =============================================================================
# Configure options
# =============================================================================

$configureOpts = ""

# Visual Studio generators support -A.
# NMake does not.
if (-Not [string]::IsNullOrEmpty($Arch) -and $Generator -notlike "NMake*") {
    $configureOpts += " -A $Arch"
}

# vcpkg toolchain
if (-Not [string]::IsNullOrEmpty($Toolchain)) {
    $absToolchain = (Resolve-Path $Toolchain).Path
    $configureOpts += " -DCMAKE_TOOLCHAIN_FILE=`"$absToolchain`""
}

# MSVC runtime
$configureOpts += " -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"

# Godot API version
$configureOpts += " -DGODOTCPP_API_VERSION=4-7"

# CrowdyCPP features
$configureOpts += " -DCROWDY_WITH_CURL=$CrowdyWithCurl"
$configureOpts += " -DCROWDY_WITH_OPENSSL=$CrowdyWithOpenSSL"

# Explicit dependency paths
$absCrowdy = (Resolve-Path $crowdycpp_dir).Path
$absGodotCpp = (Resolve-Path $godot_cpp_dir).Path

$configureOpts += " -DCROWDYCPP_DIR=`"$absCrowdy`""
$configureOpts += " -DGODOT_CPP_DIR=`"$absGodotCpp`""

# =============================================================================
# Print configuration
# =============================================================================

Write-Host ""
Write-Host "============================================================"
Write-Host "Build configuration"
Write-Host "============================================================"
Write-Host "Workspace       : $root"
Write-Host "Native source   : $ext_native"
Write-Host "Build directory : $ext_build"
Write-Host "CrowdyCPP       : $absCrowdy"
Write-Host "godot-cpp       : $absGodotCpp"
Write-Host "Generator       : $Generator"
Write-Host "Architecture    : $Arch"
Write-Host "Toolchain       : $Toolchain"
Write-Host "Curl            : $CrowdyWithCurl"
Write-Host "OpenSSL         : $CrowdyWithOpenSSL"
Write-Host "Godot API       : 4-7"
Write-Host "MSVC runtime    : MultiThreadedDLL (/MD)"
Write-Host "============================================================"
Write-Host ""

# =============================================================================
# Build
# =============================================================================

try {
    Run-CmakeBuild $ext_native $ext_build $configureOpts
}
catch {
    if ($Generator -like "Visual Studio*") {
        Write-Host ""
        Write-Host "Visual Studio generator '$Generator' failed." -ForegroundColor Yellow
        Write-Host "Falling back to 'NMake Makefiles'." -ForegroundColor Yellow

        $Generator = "NMake Makefiles"

        # NMake does not support -A.
        $retryOptions = $configureOpts -replace ' -A \S+', ''

        Run-CmakeBuild $ext_native $ext_build $retryOptions
    }
    else {
        throw
    }
}

# =============================================================================
# Complete
# =============================================================================

$outputDir = Join-Path $root "CrowdySDK\bin\Windows\x86_64"
$outputDll = Join-Path $outputDir "CrowdyCPP.dll"

Write-Host ""
Write-Host "============================================================"
Write-Host "Build complete"
Write-Host "============================================================"

if (Test-Path $outputDll) {
    Write-Host "Extension DLL:"
    Write-Host "  $outputDll" -ForegroundColor Green
}
else {
    Write-Host "WARNING: Expected extension DLL was not found:" -ForegroundColor Yellow
    Write-Host "  $outputDll"
}

Write-Host "============================================================"