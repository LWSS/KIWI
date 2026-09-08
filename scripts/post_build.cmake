# [POST_BUILD] Copy over MILES dependency.
# KISAK (miles9): Remove obsolete 7.2e plugins before copying the 9.3b runtime.
# Every target (mp/sp/dedi) cleans the same bin/<config> dir; remove -f tolerates
# files already removed by another target. Only obsolete files are removed.
# NOTE: mssmp3.asi is NOT obsolete - the 9.3b MP3 add-on ships one in deps/msslib/dlls/miles
# (the 7.2e one gets overwritten by the copy below, so it needs no removal here).
# (`-E remove` rather than `-E rm`: the latter needs CMake 3.17, the project minimum is 3.16.)
add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove -f
        "${BIN_DIR}/${CMAKE_BUILD_TYPE}/miles/mssvoice.asi"
        "${BIN_DIR}/${CMAKE_BUILD_TYPE}/miles/milesEq.flt"
        "${BIN_DIR}/${CMAKE_BUILD_TYPE}/miles/milesEq.flt.orig"
        COMMENT "REMOVING OBSOLETE MILES PLUGINS"
        VERBATIM
)
# copy_directory_if_different, NOT copy_directory: every target (mp/sp/dedi) runs this
# into the same bin/<config> dir, so unconditional copies race each other during parallel
# builds (and fail outright if the game is running with the DLLs loaded). Skipping
# unchanged files avoids the write entirely.
add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        ${MILES_RUNTIME_DIR}
        ${BIN_DIR}/${CMAKE_BUILD_TYPE}
        COMMENT "COPYING MILES DEPENDENCIES"
)
# [POST_BUILD] Copy over steam depdendency
add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${DEPS_DIR}/steamsdk/steam_api.dll
        ${BIN_DIR}/${CMAKE_BUILD_TYPE}
        COMMENT "COPYING STEAM DEPENDENCIES"
)
