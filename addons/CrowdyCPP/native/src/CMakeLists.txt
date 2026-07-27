cmake_minimum_required(VERSION 3.16)

project(crowdy_gdextension)

find_package(Threads REQUIRED)

# Ensure MSVC uses the dynamic CRT (/MD) for consistency with vcpkg and CrowdyCPP.
# Set as CACHE so subdirectories (godot-cpp, CrowdyCPP) pick up the value before
# their targets are created.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "Use dynamic CRT (/MD/MDd) for MSVC")
endif()

if(NOT EXISTS "${CROWDYCPP_DIR}/CMakeLists.txt")
  message(WARNING "CrowdyCPP source not found at ${CROWDYCPP_DIR}. You should add CrowdyCPP as a submodule at addons/CrowdyCPP/crowdycpp or set -DCROWDYCPP_DIR=...")
endif()

if(NOT EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
  message(WARNING "godot-cpp bindings not found at ${GODOT_CPP_DIR}. Please clone godot-cpp and build its headers/libraries before building this extension.")
endif()

set(SRC_FILES
  CrowdyNative.cpp
)

add_library(crowdy_gdextension SHARED ${SRC_FILES})

# Include paths: godot-cpp and CrowdyCPP (user must provide)
target_include_directories(crowdy_gdextension PRIVATE
  ${GODOT_CPP_DIR}/include
  ${CROWDYCPP_DIR}/include
)

# Link to CrowdyCPP as a subdirectory or external library if available.
if(EXISTS "${CROWDYCPP_DIR}/CMakeLists.txt")
  add_subdirectory(${CROWDYCPP_DIR} crowdycpp_build EXCLUDE_FROM_ALL)
  # Expect CrowdyCPP to produce a library named CrowdyCPP or similar. User may need to adjust this.
	# CrowdyCPP exposes a target named 'crowdy' (and related component targets).
  target_link_libraries(crowdy_gdextension PRIVATE crowdy)
endif()

if(EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
  # Build/link godot-cpp as a subdirectory so the godot-cpp target is available
  add_subdirectory(${GODOT_CPP_DIR} godotcpp_build EXCLUDE_FROM_ALL)
  target_link_libraries(crowdy_gdextension PRIVATE godot-cpp)
endif()

set_target_properties(crowdy_gdextension PROPERTIES
  PREFIX ""
  OUTPUT_NAME "CrowdyCPP"
)

# Require C++17 for the extension (godot-cpp and CrowdyCPP use C++17 features)
## CrowdyCPP requires C++20; build the extension with C++20 so headers compile
set_target_properties(crowdy_gdextension PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
## Advertise the requirement via compile features for CMake
target_compile_features(crowdy_gdextension PUBLIC cxx_std_20)

if(MSVC)
  # Match godot-cpp's static CRT (MT) to avoid runtime-library mismatch at link time.
  # libgodot-cpp in this workspace is built with MT_StaticRelease; ensure the
  # extension uses the same MSVC runtime setting.
	# To match the CrowdyCPP and vcpkg-built libraries (which typically use the
  # dynamic CRT), prefer the dynamic runtime. If you rebuilt godot-cpp with a
  # different runtime, adjust this to MultiThreaded instead.
  set_target_properties(crowdy_gdextension PROPERTIES
	MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
  )
endif()

# Place built runtime into the addon bin path expected by the .gdextension
# Use CMAKE_CURRENT_SOURCE_DIR and climb to addons/CrowdyCPP/bin/Windows/x86_64
set(CROWDY_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../bin/Windows/x86_64")
# Ensure directory exists at configure time
file(MAKE_DIRECTORY "${CROWDY_OUTPUT_DIR}")
set_target_properties(crowdy_gdextension PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CROWDY_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CROWDY_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CROWDY_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CROWDY_OUTPUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CROWDY_OUTPUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CROWDY_OUTPUT_DIR}"
)

# Post-build: copy runtime DLL dependencies from vcpkg installed bin dir (if
# a vcpkg toolchain was used) so Godot can load the extension without extra PATH tweaks.
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
  # vcpkg toolchain path is .../vcpkg/scripts/buildsystems/vcpkg.cmake
  get_filename_component(_vcpkg_scripts_dir ${CMAKE_TOOLCHAIN_FILE} DIRECTORY)
  get_filename_component(_vcpkg_root ${_vcpkg_scripts_dir} DIRECTORY)
  # Determine triplet (prefer VCPKG_TARGET_TRIPLET if set)
  if(DEFINED VCPKG_TARGET_TRIPLET)
	set(_vcpkg_triplet "${VCPKG_TARGET_TRIPLET}")
  else()
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	  set(_vcpkg_triplet "x64-windows")
	else()
	  set(_vcpkg_triplet "x86-windows")
	endif()
  endif()
  set(_vcpkg_bin_dir "${_vcpkg_root}/installed/${_vcpkg_triplet}/bin")

  set(_dlls_to_copy)
  if(CROWDY_WITH_CURL)
	list(APPEND _dlls_to_copy "${_vcpkg_bin_dir}/libcurl.dll")
	list(APPEND _dlls_to_copy "${_vcpkg_bin_dir}/z.dll")
  endif()
  if(CROWDY_WITH_OPENSSL)
	# glob OpenSSL runtimes (libssl-*.dll, libcrypto-*.dll)
	file(GLOB _openssl_dlls "${_vcpkg_bin_dir}/libssl*.dll" "${_vcpkg_bin_dir}/libcrypto*.dll")
	list(APPEND _dlls_to_copy ${_openssl_dlls})
  endif()

  if(_dlls_to_copy)
	foreach(_src_dll IN LISTS _dlls_to_copy)
	  if(EXISTS "${_src_dll}")
		add_custom_command(TARGET crowdy_gdextension POST_BUILD
		  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src_dll}" "${CROWDY_OUTPUT_DIR}/"
		  COMMENT "Copying runtime dependency ${_src_dll} -> ${CROWDY_OUTPUT_DIR}"
		)
	  endif()
	endforeach()
  endif()
endif()
