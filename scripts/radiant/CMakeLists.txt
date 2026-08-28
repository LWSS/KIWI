message("----- KIWI Radiant -----")

# No MFC: plain Win32 + kisak_mfc_shim.h (CString/CStringList/CMap/CFile compat types).

project(KIWI-Radiant)

# Phase 2: engine file sets (UNIVERSAL, XANIM, etc.) from common_files.cmake
include(${SCRIPTS_DIR}/common_files.cmake)

# Editor file list + radiant-specific engine subset
include(radiant_files.cmake)

# DXSDK — same pattern as pre_build.cmake
if (DEFINED CICD)
    if (NOT DEFINED DXSDK_DIR)
        message(FATAL_ERROR "DXSDK_DIR must be set for CI builds (-DDXSDK_DIR=...)")
    endif()
    set(DXSDK_INC_DIR ${DXSDK_DIR}/include)
    set(DXSDK_LIB_DIR ${DXSDK_DIR}/${CMAKE_BUILD_TYPE}/lib/x86)
else()
    set(DXSDK_DIR $ENV{DXSDK_DIR})
    set(DXSDK_INC_DIR ${DXSDK_DIR}/include)
    set(DXSDK_LIB_DIR ${DXSDK_DIR}/lib/x86)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(D3DX_LIB d3dx9d.lib)
else()
    set(D3DX_LIB d3dx9.lib)
endif()

# Dear ImGui (docking branch) — deps/imgui, pinned in deps/imgui/VERSION.txt.
# UI-rework Phase 2: compiled into the target; win32 + dx9 backends match the
# renderer bridge. These TUs do NOT include stdafx.h (no afx ordering constraint).
set(IMGUI_SRCS
    ${DEPS_DIR}/imgui/imgui.cpp
    ${DEPS_DIR}/imgui/imgui_draw.cpp
    ${DEPS_DIR}/imgui/imgui_tables.cpp
    ${DEPS_DIR}/imgui/imgui_widgets.cpp
    ${DEPS_DIR}/imgui/imgui_demo.cpp
    ${DEPS_DIR}/imgui/backends/imgui_impl_win32.cpp
    ${DEPS_DIR}/imgui/backends/imgui_impl_dx9.cpp
)

# WIN32 = GUI subsystem (not console)
# Engine subset: UNIVERSAL (leaf math/string/FS), COMMON (brush_edges), XANIM (model preview),
# RADIANT_SCRIPT (scr_memorytree + scr_stringlist), RADIANT_PHYSICS (physpreset + ode mass/matrix),
# RADIANT_QCOMMON (com_bsp + cmd), RADIANT_GFX_D3D (filtered: game-only files excluded).
# WIN32 group: added only as link errors demand it (Radiant has its own Sys_* in win_qe3.cpp).
add_executable(${PROJECT_NAME} WIN32
    ${RADIANT_SRCS}
    "${SRC_DIR}/radiant/kiwi_grass.h"
    "${SRC_DIR}/radiant/kiwi_grass.cpp"
    ${UNIVERSAL}
    ${COMMON}
    ${XANIM}
    ${ZLIB}
    ${RADIANT_SCRIPT}
    ${RADIANT_PHYSICS}
    ${RADIANT_QCOMMON}
    ${RADIANT_GFX_D3D}
    ${IMGUI_SRCS}
)

# Tracy — same FetchContent client the mp/sp/dedi targets use.
include(${SCRIPTS_DIR}/extern/tracy.cmake)

# KISAK_RADIANT guards all editor additions.
# _MBCS / not UNICODE: the binary was built ANSI; all decompiled code is char*.
target_compile_definitions(${PROJECT_NAME} PUBLIC
    KISAK_RADIANT
    # TRANSITIONAL: deleted once the last `#ifdef KISAK_NO_MFC` guard leaves src/radiant.
    KISAK_NO_MFC
    WIN32
    _WINDOWS
    _MBCS
    # Radiant edits SP maps; use SP entity count so q_shared.h MAX_GENTITIES resolves
    MAX_GENTITIES=2176
    # Radiant always profiles; profile.h #errors on TRACY_ENABLE without TRACY_ON_DEMAND.
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_ONLY_LOCALHOST
)

# Compile flags — match the SP/MP builds (/MT static CRT, as the original binary used)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT /O2 /Ot /MP4 /W3 /Z7 /permissive-")
set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG}   /MTd /Od /MP4 /W3 /Z7 /permissive-")

# Debug builds compile the per-vertex / per-triangle render TUs at /O2.  cl.exe rejects
# /RTC1 together with /O2 (D8016) and CMake cannot remove a flag per source, so /RTC1 is
# stripped from this directory's Debug flags.  -DKIWI_RADIANT_DEBUG_FAST_HOT=OFF restores both.
option(KIWI_RADIANT_DEBUG_FAST_HOT
       "Radiant Debug: compile the proven-hot render TUs at /O2 (requires dropping /RTC1)" ON)

if(KIWI_RADIANT_DEBUG_FAST_HOT)
    # Directory-scoped: scripts/mp, scripts/dedi and scripts/cod4map are separate
    # add_subdirectory() scopes and keep their own flags untouched.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")

    # Leaf per-element loops only.  Editor LOGIC (brush.cpp, camwnd.cpp, entity.cpp,
    # pmesh.cpp) stays at /Od so it remains steppable.
    set(KIWI_RADIANT_DEBUG_HOT_SRCS
        "${SRC_DIR}/radiant/r_ed_scene.cpp"
        "${SRC_DIR}/radiant/kiwi_instcache.cpp"
        "${SRC_DIR}/radiant/kiwi_modelcache.cpp"
        "${SRC_DIR}/radiant/shadowvolume.cpp"
        "${SRC_DIR}/radiant/kiwi_shadowcache.cpp"
        "${SRC_DIR}/radiant/kiwi_lightcache.cpp"
        "${SRC_DIR}/radiant/kiwi_walkcache.cpp"
        "${SRC_DIR}/radiant/kiwi_surfcache.cpp"
        "${SRC_DIR}/gfx_d3d/rb_backend.cpp"
        "${SRC_DIR}/gfx_d3d/r_xsurface.cpp"
        "${SRC_DIR}/gfx_d3d/r_shade.cpp"
        "${SRC_DIR}/universal/com_math.cpp"
    )
    # Per-source options are appended AFTER the configuration flags, so /O2 wins.
    set_source_files_properties(${KIWI_RADIANT_DEBUG_HOT_SRCS} PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Debug>:/O2;/Ob2;/Oi;/Ot>")
endif()

# Warnings as Errors
target_compile_options(${PROJECT_NAME} PRIVATE /we4700) # Uninit'd variable used in logic

# Static MSVC runtime
set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# x86 / Win32
set(CMAKE_GENERATOR_PLATFORM "Win32")
target_link_options(${PROJECT_NAME} PRIVATE /machine:x86)

# mp_backlot exhausts the 2 GB user address space without this (the 128 KB malloc in
# XModelReadSurface_BuildCollisionTree fails).  Radiant target only.
target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)


# PDB for Release
target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG>")
target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/OPT:REF>")
target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/OPT:ICF>")

# Include paths
target_include_directories(${PROJECT_NAME} PUBLIC ${SRC_DIR})
target_include_directories(${PROJECT_NAME} PUBLIC ${DEPS_DIR})
# imgui backends do `#include "imgui.h"` — the imgui root must be on the path.
target_include_directories(${PROJECT_NAME} PUBLIC ${DEPS_DIR}/imgui)
target_include_directories(${PROJECT_NAME} PUBLIC ${DXSDK_INC_DIR})

# Library search paths
target_link_directories(${PROJECT_NAME} PUBLIC ${DXSDK_LIB_DIR})

# Link libraries — no Miles/Bink/Steam (Radiant doesn't use them)
target_link_libraries(${PROJECT_NAME} PUBLIC
    d3d9.lib
    ${D3DX_LIB}
    ws2_32.lib
    gdiplus.lib
    winmm.lib
    comctl32.lib
    kernel32.lib
    user32.lib
    gdi32.lib
    winspool.lib
    comdlg32.lib
    advapi32.lib
    shell32.lib
    ole32.lib
    oleaut32.lib
    uuid.lib
    odbc32.lib
    odbccp32.lib
    dxguid.lib
    ddraw.lib
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${BIN_DIR}/$<CONFIG>"
)

# VS Solution Explorer folders mirroring the on-disk tree (src/… and deps/…).
# source_group(TREE) requires every listed file to live under its root, hence the split.
source_group(TREE ${SRC_DIR} PREFIX "src" FILES
    ${RADIANT_SRCS}
    ${UNIVERSAL}
    ${COMMON}
    ${XANIM}
    ${RADIANT_SCRIPT}
    ${RADIANT_PHYSICS}
    ${RADIANT_QCOMMON}
    ${RADIANT_GFX_D3D}
)
source_group(TREE ${DEPS_DIR} PREFIX "deps" FILES ${ZLIB} ${IMGUI_SRCS})
