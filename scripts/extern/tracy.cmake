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

# Tracy's own set_option() puts TRACY_ENABLE / TRACY_ON_DEMAND / TRACY_ONLY_LOCALHOST on
# TracyClient's PUBLIC interface when the options above are ON.  The old form of this line
# OVERWROTE that interface list with just the two secondary flags, so every consumer
# except Radiant (which re-defines TRACY_ENABLE itself) compiled profile.h's
# "Disable Profiling without Tracy" branch: KIWI-mp / sp / dedi linked the client but
# every zone and frame mark was a no-op and nothing ever showed up.  APPEND keeps
# TRACY_ENABLE; on-demand mode costs nothing until a profiler connects.
set_property(TARGET TracyClient APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_BINARY_DIR}/_deps/tracy-src/public)
target_link_libraries(${PROJECT_NAME} PUBLIC TracyClient)
#################
