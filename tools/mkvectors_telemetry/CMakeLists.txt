cmake_minimum_required(VERSION 3.16)
project(irtactile_shm_mkvectors_telemetry LANGUAGES CXX)

# mkvectors_telemetry - emits conformance/telemetry-shm/v1 (golden sections +
# manifest.json) from the producer side of the C++ binding. Standalone:
#
#   cmake -S tools/mkvectors_telemetry -B build-mkvt
#   cmake --build build-mkvt --config Release
#   build-mkvt/Release/mkvectors_telemetry conformance/telemetry-shm/v1
#
# The output is committed; regenerating and diffing is the drift check. The
# expectations in the manifest are written by hand in the generator source
# (an independent opinion), the section bytes come from the C++ producer.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BINDING ${CMAKE_CURRENT_SOURCE_DIR}/../../bindings/cpp)
add_library(irtactile_shm_telemetry_headers INTERFACE)
target_include_directories(irtactile_shm_telemetry_headers INTERFACE ${BINDING}/include)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(mkvectors_telemetry mkvectors_telemetry.cpp)
target_include_directories(mkvectors_telemetry PRIVATE ${BINDING}/tests)
target_link_libraries(mkvectors_telemetry PRIVATE irtactile_shm_telemetry_headers nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(mkvectors_telemetry PRIVATE /W4 /permissive-)
else()
    target_compile_options(mkvectors_telemetry PRIVATE -Wall -Wextra)
endif()
