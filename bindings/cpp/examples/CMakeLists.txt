cmake_minimum_required(VERSION 3.16)
project(irtactile_shm_examples LANGUAGES CXX)

# STANDALONE build of the examples.
#
# This file is deliberately usable on its own -- point CMake at this directory
# and it configures with NO irTactile include paths, no nlohmann, no util and
# no portaudio. That is the check that keeps the portable headers honest: the
# format header must not acquire an irTactile include, an nlohmann type or a
# <windows.h> dependency during development.
#
#   cmake -S examples -B build-examples && cmake --build build-examples
#
# reader_min builds ANYWHERE (Linux/GCC, Linux/Clang, macOS, Windows). That
# leg is the one that fails when the split erodes -- if the only example used
# the Win32 Client, a stray #include <windows.h> migrating up into
# stream/stream.h would compile clean forever.

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

add_executable(reader_min stream/reader_min.cpp)
target_link_libraries(reader_min PRIVATE irtactile::stream-shm)

if(MSVC)
    target_compile_options(reader_min PRIVATE /W4 /permissive-)
else()
    target_compile_options(reader_min PRIVATE -Wall -Wextra)
endif()

# The Win32 leg. Windows only, by construction.
if(WIN32)
    add_executable(reader_win32 stream/reader_win32.cpp)
    target_link_libraries(reader_win32 PRIVATE irtactile::stream-shm)
    if(MSVC)
        target_compile_options(reader_win32 PRIVATE /W4 /permissive-)
    endif()

    # The GDI waveform viewer: a WinMain GUI app, human-run against a live
    # producer. No add_test -- it is not CI-runnable.
    add_executable(viewer_win32 stream/viewer_win32.cpp)
    target_link_libraries(viewer_win32 PRIVATE irtactile::stream-shm)
    set_target_properties(viewer_win32 PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_compile_options(viewer_win32 PRIVATE /W4 /permissive-)
    endif()

    # The telemetry-shm demo producer.
    add_subdirectory(telemetry_demo_producer)
endif()

enable_testing()
add_test(NAME reader_min COMMAND reader_min)
