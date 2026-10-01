# KIWI-modelimport.exe: the model importer's command line and MCP server (x64 tool).
include(${SCRIPTS_DIR}/modelimport/modelimport_files.cmake)

add_executable(KIWI-modelimport
    "${SRC_DIR}/radiant/modelimport/ki_main.cpp"
    ${MODELIMPORT_CORE_SOURCES}
)
target_include_directories(KIWI-modelimport PRIVATE ${MODELIMPORT_INCLUDE_DIRS})
target_compile_definitions(KIWI-modelimport PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(KIWI-modelimport PRIVATE /W3 /utf-8 /MP /permissive- /bigobj)
endif()
set_property(TARGET KIWI-modelimport PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(KIWI-modelimport PROPERTIES WIN32_EXECUTABLE FALSE)
