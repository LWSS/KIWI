message("----- KIWI cod4map -----")

project(KIWI-cod4map)

include(cod4map_files.cmake)

# The imported reconstruction is C code.  Keep the recovered CoD4/PDB .cpp
# filenames for TU tracking while the baseline still requires C language
# semantics (tentative definitions and implicit void-pointer conversions).
set(COD4MAP_C_SOURCES ${COD4MAP_SOURCES})
list(FILTER COD4MAP_C_SOURCES INCLUDE REGEX "\\.cpp$")
set_source_files_properties(${COD4MAP_C_SOURCES} PROPERTIES LANGUAGE C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "KIWI-cod4map currently requires an x86/Win32 build")
endif()

add_executable(${PROJECT_NAME}
    ${COD4MAP_SOURCES}
    ${COD4MAP_ZLIB}
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
    /O2
    /Ot
    /Ob2
    /Oi
    /Oy
    /fp:precise
    /arch:IA32
    /W3
    /Z7
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
    /machine:x86
    /STACK:8388608
    /DYNAMICBASE:NO
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
