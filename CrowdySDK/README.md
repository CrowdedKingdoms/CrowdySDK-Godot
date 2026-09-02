CrowdyCPP Godot GDExtension
===========================

This addon provides a small GDExtension wrapper around the CrowdyCPP native
SDK so GDScript can call Crowdy services through a native C++ bridge.

Provided bindings (GDScript-callable on the CrowdyNative singleton/class):
- ping() -> "pong"
- graphql_query(management_url, query) -> raw JSON result (synchronous)
- dev_login(management_url, email) -> JSON with token + user info (DEV only)
- request_login_link(management_url, email, redirect_uri) -> JSON result

Building
--------

The repository includes CrowdyCPP and godot-cpp under third_party/. To build
on Windows use the provided PowerShell script:

  powershell -ExecutionPolicy Bypass -File addons/CrowdyCPP/build_windows.ps1

What the script does:
- Builds godot-cpp (bindings)
- Builds CrowdyCPP (the native SDK)
- Builds the GDExtension and places the DLL in addons/CrowdyCPP/bin/Windows/x86_64/

Dependencies
------------
CrowdyCPP by default uses libcurl and OpenSSL for HTTP and crypto. On Windows
install libcurl and OpenSSL (e.g. via vcpkg) or set CMake options to use
alternative providers. If CMake cannot find libcurl/OpenSSL, disable
CROWDY_WITH_CURL and CROWDY_WITH_OPENSSL when configuring CrowdyCPP, but some
auth flows may require the HTTP transport.

If you run into build errors, build each component (godot-cpp, CrowdyCPP) by
hand using the cmake commands in the script and check for missing native
dependencies.
