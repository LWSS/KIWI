include(FetchContent)

##### Tracy #####
option ( TRACY_ENABLE "" ON )
option ( TRACY_ON_DEMAND "" ON )
option ( TRACY_ONLY_LOCALHOST "" ON )
# TracyETW.cpp mirrors 64-bit kernel event layouts with void* fields; its
# static_assert(sizeof(VSyncDPC) == 64) fails on this 32-bit target.  FORCE:
# pre-0.14 configures left this cached OFF, and option() never overrides a cache.
set ( TRACY_NO_SYSTEM_TRACING ON CACHE BOOL "" FORCE )
#option ( TRACY_FIBERS "" ON )

FetchContent_Declare (
	tracy
	GIT_REPOSITORY https://github.com/wolfpld/tracy.git
	GIT_TAG v0.14.0
	GIT_SHALLOW TRUE
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable ( tracy )

set_property(TARGET TracyClient PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

#Tracy is default off, but turned on by simply defining TRACY_ENABLE
set_property(TARGET TracyClient PROPERTY INTERFACE_COMPILE_DEFINITIONS TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_BINARY_DIR}/_deps/tracy-src/public)
target_link_libraries(${PROJECT_NAME} PUBLIC TracyClient)
#################
