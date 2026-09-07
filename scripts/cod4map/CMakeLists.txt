message("----- KIWI cod4map -----")

project(KIWI-cod4map LANGUAGES C ASM_MASM)

include(cod4map_files.cmake)

# The imported reconstruction is C code.  Keep the recovered CoD4/PDB .cpp
# filenames for TU tracking while the baseline still requires C language
# semantics (tentative definitions and implicit void-pointer conversions).
set(COD4MAP_C_SOURCES ${COD4MAP_SOURCES})
list(FILTER COD4MAP_C_SOURCES INCLUDE REGEX "\\.cpp$")
set_source_files_properties(${COD4MAP_C_SOURCES} PROPERTIES LANGUAGE C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "KIWI-cod4map is an x64 tool; configure with -A x64")
endif()

add_executable(${PROJECT_NAME}
    ${COD4MAP_SOURCES}
    ${COD4MAP_ZLIB}
    ${COD4MAP_ASM_SOURCES}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32
    _CONSOLE
    DETERMINISTIC_SORT
)

# cod4map is always optimized (/O2); MSVC's default Debug /RTC1 is incompatible with /O2, so strip it (directory-scoped).
# Sources here are forced to LANGUAGE C, so /RTC1 comes from the C flags (strip both to be safe).
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/O2>
    $<$<COMPILE_LANGUAGE:C,CXX>:/Ot>
    $<$<COMPILE_LANGUAGE:C,CXX>:/Ob2>
    $<$<COMPILE_LANGUAGE:C,CXX>:/Oi>
    $<$<COMPILE_LANGUAGE:C,CXX>:/Oy>
    $<$<COMPILE_LANGUAGE:C,CXX>:/fp:precise>
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3>
    $<$<COMPILE_LANGUAGE:C,CXX>:/Z7>
)

set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${SRC_DIR}/cod4map
    ${SRC_DIR}/cod4map/libs/minizip
    ${DEPS_DIR}
    ${DEPS_DIR}/zlib
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    KIWI::cod4-bsp-format
    kernel32.lib
    user32.lib
    gdi32.lib
)

target_link_options(${PROJECT_NAME} PRIVATE
    /machine:x64
    # The large static compiler pools require a full link. Incremental Debug
    # images can link successfully but fail Windows image validation at launch.
    /INCREMENTAL:NO
    /STACK:8388608
    "$<$<CONFIG:Release>:/DEBUG>"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "KIWI-cod4map"
    VS_DEBUGGER_WORKING_DIRECTORY
        "F:/SteamLibrary/steamapps/common/Call of Duty 4"
)

source_group(TREE ${SRC_DIR}/cod4map PREFIX "src/cod4map" FILES
    ${COD4MAP_SOURCES}
)
source_group(TREE ${DEPS_DIR}/zlib PREFIX "deps/zlib" FILES
    ${COD4MAP_ZLIB}
)
