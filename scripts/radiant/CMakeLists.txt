message("----- KIWI Radiant -----")

# Phase 4 / U-RIP: MFC is DELETED from the tree — there is no MFC shell and no
# CMAKE_MFC_FLAG any more. The editor is plain Win32 + kisak_mfc_shim.h (which
# supplies the CString/CStringList/CMap/CFile compat types the faithful port uses).
# /MT stays (static CRT, set below) — that is independent of MFC.

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

# KISAK_RADIANT guards all editor additions.
# _MBCS / not UNICODE: the binary was built ANSI; all decompiled code is char*.
target_compile_definitions(${PROJECT_NAME} PUBLIC
    KISAK_RADIANT
    # KISAK_NO_MFC is now UNCONDITIONAL and TRANSITIONAL: MFC is gone, but U-RIP-2 is
    # still stripping the last `#ifdef KISAK_NO_MFC` guards out of src/radiant. The
    # define keeps those already-stripped-elsewhere TUs on the (only) Win32 path until
    # the final guard is removed — at which point this line gets deleted too.
    KISAK_NO_MFC
    WIN32
    _WINDOWS
    _MBCS
    # Radiant edits SP maps; use SP entity count so q_shared.h MAX_GENTITIES resolves
    MAX_GENTITIES=2176
)

# Compile flags — match the SP/MP builds (/MT static CRT, as the original binary used)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT /O2 /Ot /MP4 /W3 /Z7 /permissive-")
set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG}   /MTd /Od /MP4 /W3 /Z7 /permissive-")

# Warnings as Errors
target_compile_options(${PROJECT_NAME} PRIVATE /we4700) # Uninit'd variable used in logic

# Static MSVC runtime
set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# x86 / Win32
set(CMAKE_GENERATOR_PLATFORM "Win32")
target_link_options(${PROJECT_NAME} PRIVATE /machine:x86)


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
