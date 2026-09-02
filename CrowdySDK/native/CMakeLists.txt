cmake_minimum_required(VERSION 3.16)
project(CrowdyGodotExtension)

# Variables expected to be provided by the caller or environment
if(NOT DEFINED CROWDYCPP_DIR)
  # Default to the workspace third_party folder from the native subdir
  set(CROWDYCPP_DIR "${CMAKE_SOURCE_DIR}/../../../third_party/crowdycpp" CACHE PATH "Path to CrowdyCPP root (or submodule)")
endif()
if(NOT DEFINED GODOT_CPP_DIR)
  set(GODOT_CPP_DIR "${CMAKE_SOURCE_DIR}/../../../third_party/godotcpp" CACHE PATH "Path to godot-cpp bindings root")
endif()

message(STATUS "Using CROWDYCPP_DIR=${CROWDYCPP_DIR}")
message(STATUS "Using GODOT_CPP_DIR=${GODOT_CPP_DIR}")

add_subdirectory(src)
