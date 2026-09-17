cmake_minimum_required(VERSION 3.16)

project(CrowdyGodotExtension LANGUAGES CXX)

# =============================================================================
# Paths
# =============================================================================

# Expected layout:
#
# CrowdySDK-Godot/
# ├── CrowdySDK/
# │   ├── native/
# │   │   ├── CMakeLists.txt
# │   │   ├── src/
# │   │   │   └── CrowdyNative.cpp
# │   │   └── build/
# │   └── bin/
# │       └── Windows/
# │           └── x86_64/
# │               └── CrowdyCPP.dll
# │
# └── third_party/
#     ├── crowdycpp/
#     └── godotcpp/
#
# CMAKE_CURRENT_SOURCE_DIR = .../CrowdySDK/native
# Therefore ../../../third_party = workspace/third_party.

if(NOT DEFINED CROWDYCPP_DIR)
    set(
        CROWDYCPP_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/crowdycpp"
        CACHE PATH
        "Path to CrowdyCPP source"
    )
endif()

if(NOT DEFINED GODOT_CPP_DIR)
    set(
        GODOT_CPP_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/godotcpp"
        CACHE PATH
        "Path to godot-cpp source"
    )
endif()

message(STATUS "============================================================")
message(STATUS "CrowdyGodotExtension")
message(STATUS "============================================================")
message(STATUS "CROWDYCPP_DIR  = ${CROWDYCPP_DIR}")
message(STATUS "GODOT_CPP_DIR  = ${GODOT_CPP_DIR}")
message(STATUS "CMAKE_GENERATOR = ${CMAKE_GENERATOR}")
message(STATUS "CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")
message(STATUS "============================================================")


# =============================================================================
# Build configuration
# =============================================================================

# CrowdyCPP requires C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Make the runtime choice visible to all subprojects before their targets
# are created.
#
# This is important for MSVC because CrowdyCPP, godot-cpp, vcpkg libraries,
# and the extension must agree on the CRT.
if(MSVC)
    set(
        CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL"
        CACHE STRING
        "MSVC runtime library"
        FORCE
    )
endif()

find_package(Threads REQUIRED)


# =============================================================================
# Validate dependencies
# =============================================================================

if(NOT EXISTS "${CROWDYCPP_DIR}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "CrowdyCPP source was not found at:\n"
        "  ${CROWDYCPP_DIR}\n\n"
        "Set CROWDYCPP_DIR to the CrowdyCPP source directory."
    )
endif()

if(NOT EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "godot-cpp source was not found at:\n"
        "  ${GODOT_CPP_DIR}\n\n"
        "Set GODOT_CPP_DIR to the godot-cpp source directory."
    )
endif()


# =============================================================================
# Third-party libraries
# =============================================================================

message(STATUS "")
message(STATUS "Configuring godot-cpp...")
message(STATUS "")

add_subdirectory(
    "${GODOT_CPP_DIR}"
    "${CMAKE_BINARY_DIR}/godotcpp_build"
    EXCLUDE_FROM_ALL
)

message(STATUS "")
message(STATUS "Configuring CrowdyCPP...")
message(STATUS "")

add_subdirectory(
    "${CROWDYCPP_DIR}"
    "${CMAKE_BINARY_DIR}/crowdycpp_build"
    EXCLUDE_FROM_ALL
)


# =============================================================================
# GDExtension
# =============================================================================

set(CROWDY_GDEXTENSION_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CrowdyNative.cpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CrowdyAuth.cpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CrowdyReplication.cpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CrowdySerialization.cpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CrowdyTransport.cpp" )

add_library(
    crowdy_gdextension
    SHARED
    ${CROWDY_GDEXTENSION_SOURCES}
)

target_compile_features(
    crowdy_gdextension
    PUBLIC
    cxx_std_20
)

target_include_directories(
    crowdy_gdextension
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${GODOT_CPP_DIR}/include"
        "${CROWDYCPP_DIR}/include"
)

target_link_libraries(
    crowdy_gdextension
    PRIVATE
        godot-cpp
        crowdy
)


# =============================================================================
# MSVC runtime for the extension
# =============================================================================

if(MSVC)
    set_target_properties(
        crowdy_gdextension
        PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()


# =============================================================================
# Output
# =============================================================================

# native/
#   CMakeLists.txt
#
# ../bin/Windows/x86_64/
#
# Therefore:
#   native/../bin/Windows/x86_64
#
set(
    CROWDY_OUTPUT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../bin/Windows/x86_64"
)

file(
    MAKE_DIRECTORY
    "${CROWDY_OUTPUT_DIR}"
)

set_target_properties(
    crowdy_gdextension
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CrowdyCPP"

        RUNTIME_OUTPUT_DIRECTORY
            "${CROWDY_OUTPUT_DIR}"

        RUNTIME_OUTPUT_DIRECTORY_DEBUG
            "${CROWDY_OUTPUT_DIR}"

        RUNTIME_OUTPUT_DIRECTORY_RELEASE
            "${CROWDY_OUTPUT_DIR}"

        LIBRARY_OUTPUT_DIRECTORY
            "${CROWDY_OUTPUT_DIR}"

        LIBRARY_OUTPUT_DIRECTORY_DEBUG
            "${CROWDY_OUTPUT_DIR}"

        LIBRARY_OUTPUT_DIRECTORY_RELEASE
            "${CROWDY_OUTPUT_DIR}"

        ARCHIVE_OUTPUT_DIRECTORY
            "${CROWDY_OUTPUT_DIR}"

        ARCHIVE_OUTPUT_DIRECTORY_DEBUG
            "${CROWDY_OUTPUT_DIR}"

        ARCHIVE_OUTPUT_DIRECTORY_RELEASE
            "${CROWDY_OUTPUT_DIR}"
)


# =============================================================================
# Copy runtime dependencies from vcpkg
# =============================================================================

if(
    DEFINED CMAKE_TOOLCHAIN_FILE
    AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake"
)

    message(STATUS "")
    message(STATUS "vcpkg toolchain detected:")
    message(STATUS "  ${CMAKE_TOOLCHAIN_FILE}")

    # .../vcpkg/scripts/buildsystems/vcpkg.cmake
    get_filename_component(
        _vcpkg_scripts_dir
        "${CMAKE_TOOLCHAIN_FILE}"
        DIRECTORY
    )

    # .../vcpkg/scripts
    get_filename_component(
        _vcpkg_root
        "${_vcpkg_scripts_dir}"
        DIRECTORY
    )

    # Prefer the explicitly selected triplet.
    if(DEFINED VCPKG_TARGET_TRIPLET)

        set(
            _vcpkg_triplet
            "${VCPKG_TARGET_TRIPLET}"
        )

    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)

        set(
            _vcpkg_triplet
            "x64-windows"
        )

    else()

        set(
            _vcpkg_triplet
            "x86-windows"
        )

    endif()

    set(
        _vcpkg_bin_dir
        "${_vcpkg_root}/installed/${_vcpkg_triplet}/bin"
    )

    message(STATUS "vcpkg triplet: ${_vcpkg_triplet}")
    message(STATUS "vcpkg bin dir: ${_vcpkg_bin_dir}")


    # -------------------------------------------------------------------------
    # Runtime DLL list
    # -------------------------------------------------------------------------

    set(_dlls_to_copy)


    if(CROWDY_WITH_CURL)

        list(
            APPEND
            _dlls_to_copy

            "${_vcpkg_bin_dir}/libcurl.dll"
            "${_vcpkg_bin_dir}/z.dll"
        )

    endif()


    if(CROWDY_WITH_OPENSSL)

        file(
            GLOB
            _openssl_dlls

            "${_vcpkg_bin_dir}/libssl*.dll"
            "${_vcpkg_bin_dir}/libcrypto*.dll"
        )

        list(
            APPEND
            _dlls_to_copy
            ${_openssl_dlls}
        )

    endif()


    # -------------------------------------------------------------------------
    # Copy DLLs after building the extension
    # -------------------------------------------------------------------------

    foreach(_src_dll IN LISTS _dlls_to_copy)

        if(EXISTS "${_src_dll}")

            add_custom_command(
                TARGET crowdy_gdextension
                POST_BUILD

                COMMAND
                    "${CMAKE_COMMAND}"
                    -E
                    copy_if_different
                    "${_src_dll}"
                    "${CROWDY_OUTPUT_DIR}/"

                COMMENT
                    "Copying runtime dependency ${_src_dll}"
            )

        else()

            message(
                STATUS
                "Runtime DLL not found, skipping: ${_src_dll}"
            )

        endif()

    endforeach()

endif()


# =============================================================================
# Build information
# =============================================================================

message(STATUS "")
message(STATUS "============================================================")
message(STATUS "CrowdyGodotExtension configuration complete")
message(STATUS "============================================================")
message(STATUS "Extension target : crowdy_gdextension")
message(STATUS "Output name      : CrowdyCPP")
message(STATUS "Output directory : ${CROWDY_OUTPUT_DIR}")
message(STATUS "C++ standard     : C++20")

if(MSVC)
    message(STATUS "MSVC runtime     : MultiThreadedDLL (/MD)")
endif()

message(STATUS "============================================================")