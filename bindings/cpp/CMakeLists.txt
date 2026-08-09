cmake_minimum_required(VERSION 3.16)
project(irtactile_shm_cpp LANGUAGES CXX)

# The irTactile shared-memory contracts: header-only, zero dependencies.
#
# INTERFACE, not STATIC: there is nothing to compile and nothing to link. No
# nlohmann, no <windows.h> in the portable headers, no OS outside win32.h.
# That is what lets a consumer add_subdirectory() this and be done.
#
# Targets:
#   irtactile::shm-common   - common/atomic.h (+ future common/version.h)
#   irtactile::stream-shm   - stream/*, depends on shm-common
#   irtactile::shm          - umbrella convenience target
# A telemetry-shm target joins here once that contract lands; it will depend
# on shm-common exactly like stream-shm does.

add_library(irtactile_shm_common INTERFACE)
target_include_directories(irtactile_shm_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(irtactile_shm_common INTERFACE cxx_std_17)

add_library(irtactile_shm_stream INTERFACE)
target_link_libraries(irtactile_shm_stream INTERFACE irtactile_shm_common)

add_library(irtactile_shm INTERFACE)
target_link_libraries(irtactile_shm INTERFACE irtactile_shm_common irtactile_shm_stream)

add_library(irtactile::shm-common ALIAS irtactile_shm_common)
add_library(irtactile::stream-shm ALIAS irtactile_shm_stream)
add_library(irtactile::shm        ALIAS irtactile_shm)

# Make the headers visible in IDE project trees; they are the whole deliverable.
if(MSVC)
    add_custom_target(irtactile_shm_headers SOURCES
        include/irtactile/shm/common/atomic.h
        include/irtactile/shm/stream/layout.h
        include/irtactile/shm/stream/reader.h
        include/irtactile/shm/stream/win32.h
        include/irtactile/shm/stream.h
    )
    set_target_properties(irtactile_shm_headers PROPERTIES FOLDER "irtactile-shm")
endif()

# Tests: the conformance runner (tests/conformance_runner.cpp, see
# conformance/README.md) and the doctest format suite
# (tests/stream/StreamShm_test.cpp). Both are on by default for a standalone
# checkout and opt-in for consumers; they fetch nlohmann/json and doctest at
# configure time, so the repo tests itself with no external checkout.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(irtactile_shm_build_tests_default ON)
else()
    set(irtactile_shm_build_tests_default OFF)
endif()
option(IRTACTILE_SHM_BUILD_TESTS "Build the conformance runner and the doctest format suite" ${irtactile_shm_build_tests_default})
if(IRTACTILE_SHM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
