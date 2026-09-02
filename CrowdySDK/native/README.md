# CrowdyCPP GDExtension native wrapper

This folder contains a skeleton GDExtension wrapper for CrowdyCPP. It is
intended as a starting point to expose CrowdyCPP APIs to Godot.

Windows build steps (developer machine)
1. Ensure you have Visual Studio with C++ and CMake installed and on PATH.
2. Clone the CrowdyCPP repo as a submodule (from the CrowdyGodotDemo root):
   git submodule update --init --recursive
   (this will populate addons/CrowdyCPP/crowdycpp)
3. Obtain godot-cpp headers/libraries and place them somewhere (recommended: addons/CrowdyCPP/godot-cpp).
   Follow https://github.com/godotengine/godot-cpp to build the C++ bindings.
4. Create a build directory and run CMake from the native folder:
   mkdir build && cd build
   cmake -DCROWDYCPP_DIR="${CROWDY_SUBMODULE_PATH}" -DGODOT_CPP_DIR="<path-to-godot-cpp>" ..
   cmake --build . --config Release
5. Copy the produced CrowdyCPP.dll into addons/CrowdyCPP/bin/Windows/x86_64/CrowdyCPP.dll
6. Launch Godot and enable the .gdextension plugin or just use the class from GDScript.

Notes
- The provided C++ sources are a minimal skeleton. To expose full CrowdyCPP
  functionality you will need to implement bridging functions that convert
  Godot types (String, Dictionary) to std::string and native structures used
  by CrowdyCPP, manage lifetime (unique_ptr/shared_ptr), and expose async
  callback wiring (CrowdyClient::poll can be used to deliver async callbacks
  into the game thread).
- Every update to CrowdyCPP or the wrapper requires rebuilding the native
  binary. Consider automating this in CI to produce cross-platform binaries.
