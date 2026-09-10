cmake_minimum_required(VERSION 3.16)

project(KIWI)

# Compiler detection above determines the target pointer size, including Win32
# generators running on a 64-bit host.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    project(KIWI64)
endif()

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

# Architecture-neutral file format contract shared by the native x64 tools.
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
if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/$<CONFIG>")
endif()

# Include global scripts/tricks
include(${SCRIPTS_DIR}/extern/increment_build.cmake)

# Include platform overrides
include(${SCRIPTS_DIR}/platform_override.cmake)

# Log out the KISAK_PLATFORM
message("Building KISAK_PLATFORM: ${KISAK_PLATFORM}")

# Fastfile deserialization still uses the original 32-bit database layout.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_raw_default ON)
else()
    set(_raw_default OFF)
endif()
option(KIWI_RAW_ONLY "Build the game with raw assets and no fastfile loader" ${_raw_default})
option(KIWI_BUILD_GAME "Expose game targets for the selected architecture" ON)
if(KIWI_BUILD_GAME AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT KIWI_RAW_ONLY)
    message(FATAL_ERROR "The x64 game requires KIWI_RAW_ONLY until the fastfile database is ported.")
endif()
if(KIWI_BUILD_GAME)
    add_subdirectory(${SCRIPTS_DIR}/mp)
    #add_subdirectory(${SCRIPTS_DIR}/sp) # Disabled SP in KIWI
    add_subdirectory(${SCRIPTS_DIR}/dedi)
endif()

# Editor and offline tool targets are independent of the game target.
option(KIWI_BUILD_RADIANT "Build the Radiant editor" ON)
if(KIWI_BUILD_RADIANT)
    add_subdirectory(${SCRIPTS_DIR}/radiant)
endif()

option(KIWI_BUILD_COD4MAP "Build or expose the CoD4Map tool target" ON)
option(KIWI_BUILD_COD4RAD "Build or expose the CoD4Rad tool target" ON)
option(KIWI_COD4RAD_WITH_EMBREE "Build the optional Embree 4 ray-tracing extension" OFF)
set(COD4RAD_EMBREE_ROOT "$ENV{EMBREEDIR}" CACHE PATH "Root of an Embree 4 x64 Windows SDK")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(KIWI_BUILD_COD4MAP)
        add_subdirectory(${SCRIPTS_DIR}/cod4map)
    endif()
    if(KIWI_BUILD_COD4RAD)
        add_subdirectory(${SCRIPTS_DIR}/cod4rad)
    endif()
else()
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        foreach(_tool cod4map cod4rad)
            string(TOUPPER "${_tool}" _tool_upper)
            if(KIWI_BUILD_${_tool_upper})
                include(${SCRIPTS_DIR}/${_tool}/${_tool}_files.cmake)
                set(_tool_sources ${${_tool_upper}_SOURCES} ${${_tool_upper}_HEADERS}
                    ${${_tool_upper}_ASM_SOURCES} ${${_tool_upper}_ZLIB_SOURCES}
                    ${${_tool_upper}_MINIZIP_SOURCES} ${${_tool_upper}_EMBREE_SOURCES})
                add_custom_target(KIWI-${_tool} ALL
                    COMMAND "${CMAKE_COMMAND}"
                        -S "${CMAKE_SOURCE_DIR}"
                        -B "${CMAKE_BINARY_DIR}/x64/${_tool}"
                        -G "${CMAKE_GENERATOR}"
                        -A x64
                        -DKIWI_BUILD_GAME:BOOL=OFF
                        -DKIWI_BUILD_RADIANT:BOOL=OFF
                        -DKIWI_BUILD_COD4MAP:BOOL=$<STREQUAL:${_tool},cod4map>
                        -DKIWI_BUILD_COD4RAD:BOOL=$<STREQUAL:${_tool},cod4rad>
                        -DKIWI_COD4RAD_WITH_EMBREE:BOOL=${KIWI_COD4RAD_WITH_EMBREE}
                        "-DCOD4RAD_EMBREE_ROOT:PATH=${COD4RAD_EMBREE_ROOT}"
                        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_$<UPPER_CASE:$<CONFIG>>:PATH=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                    COMMAND "${CMAKE_COMMAND}"
                        --build "${CMAKE_BINARY_DIR}/x64/${_tool}"
                        --config "$<CONFIG>"
                        --target KIWI-${_tool}
                    COMMENT "Building the x64 ${_tool} tool"
                    USES_TERMINAL
                    VERBATIM
                    SOURCES ${_tool_sources}
                )
                set_target_properties(KIWI-${_tool} PROPERTIES FOLDER "Tools")
                source_group(TREE "${SRC_DIR}" PREFIX "src" FILES ${_tool_sources})
            endif()
        endforeach()
    endif()
endif()

# KIWI (2026-09-10): the editor launches cod4map / cod4rad, so building (F5) the editor
# must bring the tools up to date first.  add_dependencies becomes a VS project reference
# in the .sln, which "Build startup project" honours; it applies to the real x64 tool
# targets and to the x86 custom targets alike (both are named KIWI-<tool>).
if(TARGET KIWI-Radiant)
    foreach(_tool cod4map cod4rad)
        if(TARGET KIWI-${_tool})
            add_dependencies(KIWI-Radiant KIWI-${_tool})
        endif()
    endforeach()
endif()
