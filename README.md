# CrowdySDK-Godot

Crowded Kingdoms for Godot 4.7: player accounts and realtime multiplayer, as a Godot addon built
on the CrowdyCPP C++ SDK.

```text
addons/crowdy_sdk/   The SDK. This folder is all a game needs.
native/              C++ source of the addon's library, its dependencies and build script.
tests/               Smoke test of the SDK.
demos/               Example games built on the SDK (moving to their own repository later).
project.godot        Opens this repository in Godot, to run the demos and tests.
```

## Add Crowdy to a game

1. Build the library once (below), or take `addons/crowdy_sdk/` from someone who has.
2. Copy `addons/crowdy_sdk/` into the game, at the same path, and commit it there.
3. In the game: Project Settings > Plugins > enable **Crowdy SDK**, then set `crowdy/api_url` and
   `crowdy/app_id` under Project Settings > General.

The game then uses the `Crowdy` autoload; see the [SDK README](addons/crowdy_sdk/README.md).
To update a game, build here and copy the folder again:

```powershell
robocopy addons\crowdy_sdk D:\path\to\game\addons\crowdy_sdk /MIR
```

## Work on the SDK

```powershell
git submodule update --init --recursive
.\native\build_windows.ps1 -VcpkgRoot D:\path\to\vcpkg
godot --headless --path . res://tests/smoke_test.tscn
```

Details in [native/README.md](native/README.md). Run the demos by opening this folder in Godot.
