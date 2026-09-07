message("----- KIWI CoD4Rad -----")

project(KIWI-cod4rad LANGUAGES C CXX ASM_MASM)

if(NOT MSVC)
    message(FATAL_ERROR "KIWI-cod4rad currently requires the Microsoft toolchain")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "KIWI-cod4rad is an x64 port; configure this target with -A x64")
endif()

include(cod4rad_files.cmake)

if(KIWI_COD4RAD_WITH_EMBREE)
    if(NOT COD4RAD_EMBREE_ROOT)
        message(FATAL_ERROR "KIWI_COD4RAD_WITH_EMBREE requires COD4RAD_EMBREE_ROOT or EMBREEDIR")
    endif()

    set(_cod4rad_embree_include "${COD4RAD_EMBREE_ROOT}/include")
    set(_cod4rad_embree_lib "${COD4RAD_EMBREE_ROOT}/lib/embree4.lib")
    set(_cod4rad_tbb_lib "${COD4RAD_EMBREE_ROOT}/lib/tbb.lib")

    foreach(_required_file
        "${_cod4rad_embree_include}/embree4/rtcore.h"
        "${_cod4rad_embree_lib}"
        "${_cod4rad_tbb_lib}"
        "${COD4RAD_EMBREE_ROOT}/bin/embree4.dll"
        "${COD4RAD_EMBREE_ROOT}/bin/tbb12.dll"
        "${COD4RAD_EMBREE_ROOT}/bin/tbbmalloc.dll"
    )
        if(NOT EXISTS "${_required_file}")
            message(FATAL_ERROR "Incomplete Embree SDK: missing ${_required_file}")
        endif()
    endforeach()

    list(APPEND COD4RAD_SOURCES "${SRC_DIR}/cod4rad/embree_trace.cpp")
else()
    list(APPEND COD4RAD_SOURCES "${SRC_DIR}/cod4rad/embree_trace_stub.c")
endif()

# MSVC's default Debug /RTC1 setting cannot be combined with the recovered
# tool's always-optimized build.  Keep this override local to CoD4Rad.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

add_executable(${PROJECT_NAME}
    ${COD4RAD_SOURCES}
    ${COD4RAD_HEADERS}
    ${COD4RAD_ZLIB_SOURCES}
    ${COD4RAD_MINIZIP_SOURCES}
    ${COD4RAD_ASM_SOURCES}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CONSOLE
    _CRT_SECURE_NO_WARNINGS
    COD4RAD_WITH_EMBREE=$<BOOL:${KIWI_COD4RAD_WITH_EMBREE}>
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/O2>
    $<$<COMPILE_LANGUAGE:C,CXX>:/fp:strict>
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3>
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
)

set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SRC_DIR}/cod4rad"
    "${SRC_DIR}/cod4rad/zlib"
    "${SRC_DIR}/cod4rad/minizip"
    "${SRC_DIR}/common"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    KIWI::cod4-bsp-format
    kernel32.lib
    user32.lib
    gdi32.lib
    advapi32.lib
    winmm.lib
)

if(KIWI_COD4RAD_WITH_EMBREE)
    target_include_directories(${PROJECT_NAME} PRIVATE "${_cod4rad_embree_include}")
    target_link_libraries(${PROJECT_NAME} PRIVATE "${_cod4rad_embree_lib}" "${_cod4rad_tbb_lib}")

    foreach(_runtime_dll embree4.dll tbb12.dll tbbmalloc.dll)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${COD4RAD_EMBREE_ROOT}/bin/${_runtime_dll}"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${_runtime_dll}"
            VERBATIM
        )
    endforeach()
endif()

target_link_options(${PROJECT_NAME} PRIVATE
    /machine:x64
    /STACK:8388608
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "KIWI-cod4rad"
    FOLDER "Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "F:/SteamLibrary/steamapps/common/Call of Duty 4"
)

source_group(TREE "${SRC_DIR}" PREFIX "src" FILES
    ${COD4RAD_SOURCES}
    ${COD4RAD_HEADERS}
    ${COD4RAD_ZLIB_SOURCES}
    ${COD4RAD_MINIZIP_SOURCES}
    ${COD4RAD_ASM_SOURCES}
)
