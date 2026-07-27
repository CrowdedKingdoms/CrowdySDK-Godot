# CrowdyGodotDemo

Minimal Godot 4 project skeleton to start integrating CrowdyCPP with Godot.

Next steps:

- Implement a GDExtension wrapper that exposes CrowdyCPP APIs to Godot and place built binaries under addons/CrowdyCPP/bin/.
- Or implement a pure-GDScript high-level port for non-performance-critical features.
- Copy operations/ (GraphQL files) from the CrowdyCPP repo into this project for use by the GraphQL client.

Notes:

- CrowdyCPP must be built or wrapped as a GDExtension; you cannot simply reference the C++ sources with a manifest. Each update requires rebuilding native binaries.
