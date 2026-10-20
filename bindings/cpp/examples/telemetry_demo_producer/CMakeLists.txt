cmake_minimum_required(VERSION 3.16)
project(irtactile_telemetry_demo_producer LANGUAGES CXX)

# The basic telemetry-shm v1 producer: publishes a few std.* channels, a
# local channel and an event, and prints the return region. Windows only (named
# sections). Also configurable on its own:
#
#   cmake -S bindings/cpp/examples/telemetry_demo_producer -B build-demo
#   cmake --build build-demo --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET irtactile::telemetry-shm)
    add_library(irtactile_shm_common INTERFACE)
    target_include_directories(irtactile_shm_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
    target_compile_features(irtactile_shm_common INTERFACE cxx_std_17)
    add_library(irtactile_channels INTERFACE)
    target_link_libraries(irtactile_channels INTERFACE irtactile_shm_common)
    add_library(irtactile_shm_telemetry INTERFACE)
    target_link_libraries(irtactile_shm_telemetry INTERFACE irtactile_shm_common irtactile_channels)
    add_library(irtactile::telemetry-shm ALIAS irtactile_shm_telemetry)
endif()

add_executable(telemetry_demo_producer telemetry_demo.cpp)
target_link_libraries(telemetry_demo_producer PRIVATE irtactile::telemetry-shm)
if(MSVC)
    target_compile_options(telemetry_demo_producer PRIVATE /W4 /permissive-)
endif()
