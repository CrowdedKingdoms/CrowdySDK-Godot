param(
	[string]$Root = "",
	[switch]$Godot,
	[switch]$Crowdy,
	[switch]$All
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrEmpty($Root)) {
	# Default to the project root (two levels up from the addon folder)
	$root = (Resolve-Path (Join-Path $scriptDir "..\..") ).Path
} else {
	$root = (Resolve-Path $Root).Path
}

Write-Host "Workspace root: $root"

$godot_build_dir = Join-Path $root "third_party\godotcpp\build"
$crowdy_build_dir = Join-Path $root "third_party\crowdycpp\build"
$ext_build_dir = Join-Path $root "addons\CrowdyCPP\native\build"

# If no specific target flags were given, clean all
if (-Not ($Godot.IsPresent -or $Crowdy.IsPresent -or $All.IsPresent)) {
	$Extension = $true
}

# -All overrides everything
if ($All) {
    $Godot = $true
    $Crowdy = $true
    $Extension = $true
}

function SafeRemove($path) {
	if (Test-Path $path) {
		Write-Host "Removing: $path"
		try {
			Remove-Item -Recurse -Force $path -ErrorAction Stop
		} catch {
			Write-Host "ERROR: Failed to remove $path : $_" -ForegroundColor Red
			throw
		}
	} else {
		Write-Host "Not found (skipping): $path"
	}
}

if ($Godot) { SafeRemove $godot_build_dir }
if ($Crowdy) { SafeRemove $crowdy_build_dir }
if ($Extension) { SafeRemove $ext_build_dir }

Write-Host "Clean complete."
