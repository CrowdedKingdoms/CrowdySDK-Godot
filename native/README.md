# Crowdy SDK: native library

The C++ GDExtension behind `addons/crowdy_sdk`. Games never need this folder: they use the built
library in `addons/crowdy_sdk/bin/`. Change things here only to change what the SDK can do.

```text
native/
├── src/                 The extension (C++). One class per file pair; see below.
├── third_party/
│   ├── crowdycpp/       CrowdyCPP, the Crowded Kingdoms C++ SDK (submodule).
│   └── godotcpp/        godot-cpp, Godot's C++ bindings (submodule).
├── CMakeLists.txt       Builds godot-cpp, CrowdyCPP and the extension in one build tree.
├── build_windows.ps1    Configures and builds into build/, output into addons/crowdy_sdk/bin/.
└── build/               Build tree (not committed).
```

## Build (Windows)

Visual Studio 2022 or newer with the C++ workload, and vcpkg with libcurl and OpenSSL:

```powershell
git submodule update --init --recursive
vcpkg install curl openssl --triplet x64-windows
.\native\build_windows.ps1 -VcpkgRoot D:\path\to\vcpkg
godot --headless --path . res://tests/smoke_test.tscn
```

`-Config RelWithDebInfo` adds debug symbols; `-Clean` deletes the build tree first (needed after
changing `-VcpkgRoot` or moving the submodules). Restart the Godot editor after a build: it does
not reload the library while running. The extension targets the Godot 4.7 API
(`GODOTCPP_API_VERSION` in `CMakeLists.txt`, `compatibility_minimum` in the `.gdextension`).

## Source files

| File | Godot class or role |
|---|---|
| `register_types.cpp` | Entry point: registers the classes below. |
| `crowdy_identity.*` | `CrowdyIdentity`: sign-in, the gates a new account passes, app-token mint. |
| `crowdy_game_session.*` | `CrowdyGameSession`: the realtime session (CrowdyCPP's `startGameSession` and `WorldSession`) as signals. |
| `crowdy_request.*`, `crowdy_result.*` | `CrowdyRequest` (awaitable) and `CrowdyResult`; `PendingRequests` tracks a class's open requests. |
| `crowdy_app_token.*` | `CrowdyAppToken`: the opaque token passed from identity to session. |
| `async_host_election.*` | Host election that does not block the main thread. |
| `background_call.h` | Runs one blocking call (connecting) on its own thread. |
| `client_config.*` | Transport and timeout settings every CrowdyCPP client uses. |
| `conversions.*` | Strings, bytes, chunks, actor ids and error codes between CrowdyCPP and Godot. |

## Threading rules

- Scripts only run on the main thread. CrowdyCPP delivers callbacks from `CrowdyClient::poll()` and
  `WorldSession::tick()`, which the classes call from their own `poll()`.
- Signals raised inside a CrowdyCPP callback are queued and emitted after it returns, so a script
  may call any method from a handler, `stop()` included.
- Closures handed to CrowdyCPP capture plain C++ values (a request id, `this`), never Godot
  objects: CrowdyCPP may copy or destroy them on its own threads.
- Blocking calls run in a `BackgroundCall` on copies of what they need. No thread is detached:
  destroying the owner waits for the call, so nothing runs after Godot unloads the library.
