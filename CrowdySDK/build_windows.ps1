param(
	[string]$Root = "",
	[string]$Arch = "x64",                    # e.g. x64 or Win32 — empty leaves CMake to choose
	[string]$Generator = "",               # optional CMake generator (e.g. 'Visual Studio 18 2026')
	[string]$Toolchain = "",               # optional CMake toolchain file (vcpkg)
	[string]$CrowdyWithCurl = "ON",
	[string]$CrowdyWithOpenSSL = "ON"
)

Write-Host "Building CrowdyCPP GDExtension (Windows)"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrEmpty($Root)) {
	# Default to two levels up from the script directory (the project root)
	$root = (Resolve-Path ($scriptDir)).Path
} else {
	$root = (Resolve-Path $Root).Path
}
Write-Host "Workspace root: $root"

$godot_cpp_dir = Join-Path $root "third_party\godotcpp"
$crowdycpp_dir = Join-Path $root "third_party\crowdycpp"
$ext_native = Join-Path $root "native"

# Autodetect Visual Studio when a VS generator was not provided or when the
# requested Visual Studio generator is not available on this machine. We use
# vswhere.exe when present to detect an installed VS instance. If none is
# found fall back to the NMake generator which previously worked on this
# environment.
function Detect-VisualStudioGenerator() {
	param([string]$requested)
	$vswhere = "$Env:ProgramFiles(x86)\Microsoft Visual Studio\Installer\vswhere.exe"
	if (-Not (Test-Path $vswhere)) {
		# try ProgramFiles as well
		$vswhere = "$Env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
	}

	# Keep requested generator as-is; we will map detected VS major versions to
	# the corresponding generator string. Older CMake may not support newer
	# generator names — detection below will choose the closest supported one.

	if (-Not (Test-Path $vswhere)) {
		if (-Not [string]::IsNullOrEmpty($requested)) { return $requested }
		Write-Host "vswhere.exe not found; falling back to 'NMake Makefiles' generator." -ForegroundColor Yellow
		return 'NMake Makefiles'
	}

	try {
		$version = & "$vswhere" -latest -products * -requires Microsoft.Component.MSBuild -property installationVersion 2>$null
	} catch {
		$version = $null
	}

	if ([string]::IsNullOrEmpty($version)) {
		if (-Not [string]::IsNullOrEmpty($requested)) { return $requested }
		Write-Host "No Visual Studio instances found by vswhere; using 'NMake Makefiles'" -ForegroundColor Yellow
		return 'NMake Makefiles'
	}

	$major = 0
	if ($version -match '^(\d+)') { $major = [int]$Matches[1] }
	switch ($major) {
		{ $_ -ge 18 } { return 'Visual Studio 18 2026' }
		17 { return 'Visual Studio 17 2022' }
		16 { return 'Visual Studio 16 2019' }
		15 { return 'Visual Studio 15 2017' }
		default { return 'NMake Makefiles' }
	}
}

function SanitizeName($s) {
	if ([string]::IsNullOrEmpty($s)) { return "default" }
	return ($s -replace '[^A-Za-z0-9_.-]', '_')
}

# If the user passed a generator try to validate it; otherwise detect one.
if (-Not [string]::IsNullOrEmpty($Generator)) {
	if ($Generator -like 'Visual Studio*') {
		$detected = Detect-VisualStudioGenerator $Generator
		if ($detected -ne $Generator) {
			Write-Host "Requested generator '$Generator' is not available; falling back to '$detected'" -ForegroundColor Yellow
			$Generator = $detected
		}
	}
} else {
	$Generator = Detect-VisualStudioGenerator $Generator
	Write-Host "Autodetected CMake generator: $Generator"
}

# Use a single 'build' directory for each component. This keeps layout simple
# and avoids generator-specific folder names.
$godot_build_dir = Join-Path $godot_cpp_dir "build"
$crowdy_build_dir = Join-Path $crowdycpp_dir "build"
$ext_build = Join-Path $ext_native "build"

function Run-CmakeBuild($src, $buildDir, $target = $null, $configureOptions = "") {
	if (-Not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }
	Push-Location $buildDir
	$genPrefix = ""
	if (-Not [string]::IsNullOrEmpty($Generator)) { $genPrefix = "-G `"$Generator`"" }
	$cfgCmd = "cmake $genPrefix -S `"$src`" -B `"$buildDir`" -DCMAKE_BUILD_TYPE=Release $configureOptions"
	Write-Host "Configuring: $cfgCmd"
	iex $cfgCmd
	if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $src" }
	if ($target) {
		cmake --build $buildDir --config Release --target $target -j
	} else {
		cmake --build $buildDir --config Release -j
	}
	if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $src" }
	Pop-Location
}

# Cleaning is handled by clean_builds.ps1; do not remove build dirs here.
# Configure godot-cpp to use the dynamic MSVC runtime (MultiThreadedDLL) so it
# matches vcpkg / CrowdyCPP built libraries. Pass optional Arch/Toolchain when set.
$godotConfigureOpts = ""
if (-Not [string]::IsNullOrEmpty($Arch)) { $godotConfigureOpts += " -A $Arch" }
if (-Not [string]::IsNullOrEmpty($Toolchain)) { $godotConfigureOpts += " -DCMAKE_TOOLCHAIN_FILE=`"$Toolchain`"" }
$godotConfigureOpts += " -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"
try {
	Run-CmakeBuild $godot_cpp_dir $godot_build_dir "godot-cpp" $godotConfigureOpts
} catch {
	# If Visual Studio generator was selected but CMake couldn't find an
	# instance (common when not running from a VS developer shell), fall
	# back to NMake which worked previously in this environment.
	if ($Generator -like 'Visual Studio*') {
		Write-Host "Visual Studio generator '$Generator' failed to configure. Falling back to 'NMake Makefiles'." -ForegroundColor Yellow
		$Generator = 'NMake Makefiles'
		Write-Host "Falling back to a generator-specific build directory for '$Generator'" -ForegroundColor Yellow
		# Use the common 'build' directories for the fallback generator as well
		$godot_build_dir = Join-Path $godot_cpp_dir "build"
		$crowdy_build_dir = Join-Path $crowdycpp_dir "build"
		$ext_build = Join-Path $ext_native "build"
		# NMake does not accept the -A <arch> option; strip it from the configure options
		$retryOptions = $godotConfigureOpts -replace ' -A \S+',''
		Run-CmakeBuild $godot_cpp_dir $godot_build_dir "godot-cpp" $retryOptions
	} else {
		throw
	}
}

Write-Host "Building CrowdyCPP library (may require libcurl/OpenSSL installed)..."
$crowdyConfigureOpts = ""
if (-Not [string]::IsNullOrEmpty($Arch)) { $crowdyConfigureOpts += " -A $Arch" }
$crowdyConfigureOpts += " -DCROWDY_WITH_CURL=$CrowdyWithCurl -DCROWDY_WITH_OPENSSL=$CrowdyWithOpenSSL"
if (-Not [string]::IsNullOrEmpty($Toolchain)) { $crowdyConfigureOpts += " -DCMAKE_TOOLCHAIN_FILE=`"$Toolchain`"" }
$crowdyConfigureOpts += " -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"

try {
	Run-CmakeBuild $crowdycpp_dir $crowdy_build_dir $null $crowdyConfigureOpts
} catch {
	throw
}

Write-Host "Building the GDExtension native module..."
if (-Not (Test-Path $ext_build)) { New-Item -ItemType Directory -Path $ext_build | Out-Null }
$extConfigureOpts = ""
if (-Not [string]::IsNullOrEmpty($Arch)) { $extConfigureOpts += " -A $Arch" }
if (-Not [string]::IsNullOrEmpty($Toolchain)) { $extConfigureOpts += " -DCMAKE_TOOLCHAIN_FILE=`"$Toolchain`"" }
$extConfigureOpts += " -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"

# Pass absolute paths for the dependency dirs
$absCrowdy = (Resolve-Path $crowdycpp_dir).Path
$absGodotCpp = (Resolve-Path $godot_cpp_dir).Path
$extConfigureOpts += " -DCROWDYCPP_DIR=`"$absCrowdy`" -DGODOT_CPP_DIR=`"$absGodotCpp`""
Run-CmakeBuild $ext_native $ext_build $null $extConfigureOpts

Write-Host "Build complete. The extension DLL should be in CrowdySDK/bin/Windows/x86_64/"