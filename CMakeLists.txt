cmake_minimum_required(VERSION 3.16)

project(KIWI)

# Set the default C++ standard
set(CMAKE_CXX_STANDARD 20)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

# Set this to match the folder name in scripts/platform
# This will handle the path overrides if needed
set(KISAK_PLATFORM win32)

# Set Address Sanitizer
# NOTE: If you use this you will need to copy to the game's directory
# clang_rt.asan_dbg_dynamic-i386.dll and clang_rt.asan_dynamic-i386.dll from the MSVC C directory located below (or similar)
# C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86
set(USE_ASAN OFF)

# Set the gloabl directory variables
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
set(BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/scripts)
set(PLATFORM_DIR ${SCRIPTS_DIR}/platform)

# Architecture-neutral tool format contract.  CoD4Map (x86) and CoD4Rad
# (x64) consume this through separate build trees.
add_library(kiwi_cod4_bsp_format INTERFACE)
target_sources(kiwi_cod4_bsp_format INTERFACE
    "${SRC_DIR}/common/cod4_bsp_format.h"
)
target_include_directories(kiwi_cod4_bsp_format INTERFACE
    "${SRC_DIR}/common"
)
add_library(KIWI::cod4-bsp-format ALIAS kiwi_cod4_bsp_format)

# Keep every executable in the same per-configuration runtime folder.  The
# explicit generator expression also applies to nested x64 tool builds without
# Visual Studio appending a second configuration directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/$<CONFIG>")

# Include global scripts/tricks
include(${SCRIPTS_DIR}/extern/increment_build.cmake)

# Include platform overrides
include(${SCRIPTS_DIR}/platform_override.cmake)

# Log out the KISAK_PLATFORM
message("Building KISAK_PLATFORM: ${KISAK_PLATFORM}")

# The recovered game, Radiant, and CoD4Map targets use the original Win32 ABI.
# CoD4Rad is a standalone x64 port, so Visual Studio must generate it in a
# separate x64 build tree; a single CMake generator cannot mix architectures.
option(KIWI_BUILD_COD4RAD "Build or expose the CoD4Rad tool target" ON)
option(KIWI_COD4RAD_WITH_EMBREE "Build the optional Embree 4 ray-tracing extension" OFF)
set(COD4RAD_EMBREE_ROOT "$ENV{EMBREEDIR}" CACHE PATH "Root of an Embree 4 x64 Windows SDK")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(KIWI_BUILD_COD4RAD)
        add_subdirectory(${SCRIPTS_DIR}/cod4rad)
    else()
        message(STATUS "x64 configuration has KIWI_BUILD_COD4RAD disabled")
    endif()
else()
    add_subdirectory(${SCRIPTS_DIR}/mp)
    #add_subdirectory(${SCRIPTS_DIR}/sp) # Disabled SP in KIWI
    add_subdirectory(${SCRIPTS_DIR}/dedi)
    add_subdirectory(${SCRIPTS_DIR}/radiant)
    add_subdirectory(${SCRIPTS_DIR}/cod4map)

    # A Visual Studio solution has one generator architecture.  Keep the
    # native x64 compiler in its own build tree, but expose a utility project
    # in the main Win32 solution that configures and builds it transparently.
    if(KIWI_BUILD_COD4RAD AND CMAKE_GENERATOR MATCHES "^Visual Studio")
        include(${SCRIPTS_DIR}/cod4rad/cod4rad_files.cmake)
        set(_cod4rad_bridge_sources
            ${COD4RAD_SOURCES}
            ${COD4RAD_HEADERS}
            ${COD4RAD_ZLIB_SOURCES}
            ${COD4RAD_MINIZIP_SOURCES}
            ${COD4RAD_ASM_SOURCES}
            "${SRC_DIR}/cod4rad/embree_trace.cpp"
            "${SRC_DIR}/cod4rad/embree_trace_stub.c"
            "${SRC_DIR}/common/cod4_bsp_format.h"
        )

        add_custom_target(KIWI-cod4rad ALL
            COMMAND "${CMAKE_COMMAND}"
                -S "${CMAKE_SOURCE_DIR}"
                -B "${CMAKE_SOURCE_DIR}/build-cod4rad"
                -G "${CMAKE_GENERATOR}"
                -A x64
                -DKIWI_BUILD_COD4RAD:BOOL=ON
                -DKIWI_COD4RAD_WITH_EMBREE:BOOL=${KIWI_COD4RAD_WITH_EMBREE}
                "-DCOD4RAD_EMBREE_ROOT:PATH=${COD4RAD_EMBREE_ROOT}"
            COMMAND "${CMAKE_COMMAND}"
                --build "${CMAKE_SOURCE_DIR}/build-cod4rad"
                --config "$<CONFIG>"
                --target KIWI-cod4rad
            COMMENT "Building the x64 CoD4Rad tool"
            USES_TERMINAL
            VERBATIM
            SOURCES ${_cod4rad_bridge_sources}
        )
        set_target_properties(KIWI-cod4rad PROPERTIES FOLDER "Tools")
        source_group(TREE "${SRC_DIR}" PREFIX "src" FILES ${_cod4rad_bridge_sources})
    endif()
endif()
