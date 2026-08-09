cmake_minimum_required(VERSION 3.16)
project(irtactile_shm_mkvectors LANGUAGES CXX)

# mkvectors - emits the conformance vectors (conformance/<api>/vN/) from the
# producer side of the C++ binding. Standalone-configurable, like examples/:
#
#   cmake -S tools/mkvectors -B build-mkvectors
#   cmake --build build-mkvectors
#   ./build-mkvectors/mkvectors conformance/stream-shm/v1
#
# The output is committed; regenerating and diffing is the drift check.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET irtactile::stream-shm)
    add_library(irtactile_shm_common INTERFACE)
    target_include_directories(irtactile_shm_common INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../bindings/cpp/include)
    target_compile_features(irtactile_shm_common INTERFACE cxx_std_17)

    add_library(irtactile_shm_stream INTERFACE)
    target_link_libraries(irtactile_shm_stream INTERFACE irtactile_shm_common)

    add_library(irtactile::shm-common ALIAS irtactile_shm_common)
    add_library(irtactile::stream-shm ALIAS irtactile_shm_stream)
endif()

# mkvectors is repo-local tooling (never shipped to a consumer), so it takes
# a real dependency: nlohmann/json for the manifest, fetched at configure time.
include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(mkvectors mkvectors.cpp)
target_link_libraries(mkvectors PRIVATE irtactile::stream-shm nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(mkvectors PRIVATE /W4 /permissive-)
else()
    target_compile_options(mkvectors PRIVATE -Wall -Wextra)
endif()
