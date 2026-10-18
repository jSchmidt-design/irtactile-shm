cmake_minimum_required(VERSION 3.16)
project(irtactile_shm_tests LANGUAGES CXX)

# STANDALONE-configurable, like examples/: point CMake at this directory and
# it configures against include/ with nothing else.
#
#   cmake -S bindings/cpp/tests -B build-tests
#   cmake --build build-tests
#   ctest --test-dir build-tests
#
# Two test targets, both registered with CTest:
#
#   conformance_runner  - the C++ leg of the vector suite (conformance/
#     README.md): it drives Reader from manifest.json + the golden blobs, with
#     no test framework, so the file doubles as the reference harness for
#     other bindings.
#   stream_shm_tests    - the doctest format suite (stream/StreamShm_test.cpp),
#     covering the publish protocol, the addressing arithmetic and the
#     reader's forward-compatibility rules over a plain std::vector<uint8_t>.
#
# Both dependencies (nlohmann/json, doctest) are repo-local test tooling,
# fetched at configure time and never shipped with the binding.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET irtactile::stream-shm)
    add_library(irtactile_shm_common INTERFACE)
    target_include_directories(irtactile_shm_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(irtactile_shm_common INTERFACE cxx_std_17)

    add_library(irtactile_shm_stream INTERFACE)
    target_link_libraries(irtactile_shm_stream INTERFACE irtactile_shm_common)

    add_library(irtactile::shm-common ALIAS irtactile_shm_common)
    add_library(irtactile::stream-shm ALIAS irtactile_shm_stream)
endif()

if(NOT TARGET irtactile::telemetry-model)
    add_library(irtactile_telemetry_model INTERFACE)
    target_include_directories(irtactile_telemetry_model INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(irtactile_telemetry_model INTERFACE cxx_std_17)
    add_library(irtactile::telemetry-model ALIAS irtactile_telemetry_model)
endif()

if(NOT TARGET irtactile::telemetry-shm)
    add_library(irtactile_shm_telemetry INTERFACE)
    target_link_libraries(irtactile_shm_telemetry INTERFACE irtactile::shm-common irtactile::telemetry-model)
    add_library(irtactile::telemetry-shm ALIAS irtactile_shm_telemetry)
endif()

include(FetchContent)

# nlohmann/json: the conformance runner parses manifest.json with it.
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.12.0
        GIT_SHALLOW    TRUE)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# doctest: the format suite's framework. Guarded so a consumer that already
# fetched doctest (irTactile_Internal does, at the same tag) wins instead of
# colliding with a second copy.
if(NOT TARGET doctest::doctest)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON  CACHE BOOL "" FORCE)
    # doctest v2.4.11 declares cmake_minimum_required(VERSION 3.0); allow it to
    # configure under newer CMake that dropped compatibility with < 3.5.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(doctest)
endif()

enable_testing()

# --- conformance runner ------------------------------------------------------
add_executable(conformance_runner conformance_runner.cpp)
target_link_libraries(conformance_runner PRIVATE irtactile::stream-shm nlohmann_json::nlohmann_json)

add_test(
    NAME conformance-stream-shm-v1
    COMMAND conformance_runner ${CMAKE_CURRENT_SOURCE_DIR}/../../../conformance/stream-shm/v1/manifest.json
)

# --- doctest format suite ----------------------------------------------------
# Explicit source list, not GLOB, so a missing file fails loudly rather than
# silently dropping a test (same rule as irTactile_Internal's runner).
add_executable(stream_shm_tests
    doctest_main.cpp
    stream/StreamShm_test.cpp
)
target_link_libraries(stream_shm_tests PRIVATE irtactile::stream-shm doctest::doctest)

# doctest_discover_tests() registers each TEST_CASE as its own CTest test. Its
# module ships in the doctest checkout; when doctest came from elsewhere (an
# installed package, a consumer's own fetch) fall back to one test per binary.
if(DEFINED doctest_SOURCE_DIR AND EXISTS "${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
    list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
    include(doctest)
    doctest_discover_tests(stream_shm_tests)
else()
    add_test(NAME stream-shm-format COMMAND stream_shm_tests)
endif()

# --- channel model tests -----------------------------------------------------
add_executable(telemetry_model_tests
    doctest_main.cpp
    telemetry/ChannelModel_test.cpp
)
target_link_libraries(telemetry_model_tests PRIVATE irtactile::telemetry-model doctest::doctest)
if(DEFINED doctest_SOURCE_DIR AND EXISTS "${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
    doctest_discover_tests(telemetry_model_tests)
else()
    add_test(NAME telemetry-model COMMAND telemetry_model_tests)
endif()

# --- telemetry-shm v1 -------------------------------------------------------
add_executable(telemetry_shm_tests
    doctest_main.cpp
    telemetry/TelemetryShm_test.cpp
)
target_link_libraries(telemetry_shm_tests PRIVATE irtactile::telemetry-shm doctest::doctest)
if(DEFINED doctest_SOURCE_DIR AND EXISTS "${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
    doctest_discover_tests(telemetry_shm_tests)
else()
    add_test(NAME telemetry-shm COMMAND telemetry_shm_tests)
endif()

# The C++ leg of the telemetry-shm vector suite (conformance/telemetry-shm/v1).
add_executable(telemetry_conformance_runner telemetry_conformance_runner.cpp)
target_link_libraries(telemetry_conformance_runner PRIVATE irtactile::telemetry-shm nlohmann_json::nlohmann_json)
add_test(
    NAME conformance-telemetry-shm-v1
    COMMAND telemetry_conformance_runner ${CMAKE_CURRENT_SOURCE_DIR}/../../../conformance/telemetry-shm/v1/manifest.json
)

foreach(target conformance_runner stream_shm_tests telemetry_model_tests telemetry_shm_tests telemetry_conformance_runner)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
