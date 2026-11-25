# 3.25 for add_subdirectory(SYSTEM): warnings are reported for this extension's code only.
cmake_minimum_required(VERSION 3.25)

project(CrowdySDKGodot LANGUAGES CXX)

# =============================================================================
# Inputs
# =============================================================================

set(CROWDYCPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crowdycpp" CACHE PATH "CrowdyCPP source directory")
set(GODOT_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/godotcpp" CACHE PATH "godot-cpp source directory")
set(CROWDY_SDK_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../addons/crowdy_sdk/bin" CACHE PATH
    "Where the extension library and the libraries it loads are placed")

# Keep in sync with compatibility_minimum in addons/crowdy_sdk/crowdy_sdk.gdextension.
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version the extension is built against")

foreach(dependency_dir IN ITEMS "${CROWDYCPP_DIR}" "${GODOT_CPP_DIR}")
    if(NOT EXISTS "${dependency_dir}/CMakeLists.txt")
        message(FATAL_ERROR "Missing ${dependency_dir}. Run: git submodule update --init --recursive")
    endif()
endforeach()

# =============================================================================
# Dependencies
# =============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Every library linked into the extension must use the same C runtime. vcpkg's x64-windows
# libcurl and OpenSSL use the dynamic one, and godot-cpp defaults to the static one.
set(GODOTCPP_USE_STATIC_CPP OFF CACHE BOOL "" FORCE)

# Only the CrowdyCPP library itself.
set(CROWDY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CROWDY_BUILD_E2E OFF CACHE BOOL "" FORCE)
set(CROWDY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CROWDY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory("${GODOT_CPP_DIR}" godot-cpp EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory("${CROWDYCPP_DIR}" crowdycpp EXCLUDE_FROM_ALL SYSTEM)

# =============================================================================
# Extension
# =============================================================================

add_library(crowdy_sdk SHARED
    src/async_host_election.cpp
    src/client_config.cpp
    src/conversions.cpp
    src/crowdy_app_token.cpp
    src/crowdy_game_session.cpp
    src/crowdy_identity.cpp
    src/crowdy_request.cpp
    src/crowdy_result.cpp
    src/register_types.cpp
)

target_link_libraries(crowdy_sdk PRIVATE godot::cpp crowdy)

if(MSVC)
    target_compile_options(crowdy_sdk PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(crowdy_sdk PRIVATE -Wall -Wextra)
endif()

# Named the way Godot names extension binaries: <name>.<platform>.<arch>.
string(TOLOWER "${CMAKE_SYSTEM_NAME}" platform_name)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(arch_name "x86_64")
else()
    set(arch_name "x86_32")
endif()

set_target_properties(crowdy_sdk PROPERTIES
    PREFIX ""
    OUTPUT_NAME "crowdy_sdk.${platform_name}.${arch_name}"
    # The generator expression stops multi-config generators adding a Release/ subfolder.
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CROWDY_SDK_OUTPUT_DIR}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CROWDY_SDK_OUTPUT_DIR}>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pdb"
)

# The libraries the extension loads at run time (libcurl, OpenSSL and theirs) go next to it,
# where Windows finds them. addons/crowdy_sdk/crowdy_sdk.gdextension lists them for exports.
if(WIN32)
    add_custom_command(TARGET crowdy_sdk POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:crowdy_sdk>" "${CROWDY_SDK_OUTPUT_DIR}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()
