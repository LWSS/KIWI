# KIWI model importer core: the converter (from obj2xmodel), source loading, InstaMAT
# texture variants, collision hulls, recipes and the build driver. Compiled into
# KIWI-modelimport (CLI + MCP server) and KIWI-Radiant's Model Import window.
set(MODELIMPORT_CORE_SOURCES
    "${SRC_DIR}/radiant/modelimport/cod4_iwi.cpp"
    "${SRC_DIR}/radiant/modelimport/cod4_iwi.h"
    "${SRC_DIR}/radiant/modelimport/cod4_material.cpp"
    "${SRC_DIR}/radiant/modelimport/cod4_material.h"
    "${SRC_DIR}/radiant/modelimport/xmodel_write.cpp"
    "${SRC_DIR}/radiant/modelimport/xmodel_write.h"
    "${SRC_DIR}/radiant/modelimport/ki_pieces.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_pieces.h"
    "${SRC_DIR}/radiant/modelimport/ki_convert.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_convert.h"
    "${SRC_DIR}/radiant/modelimport/ki_mesh.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_mesh.h"
    "${SRC_DIR}/radiant/modelimport/ki_textures.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_textures.h"
    "${SRC_DIR}/radiant/modelimport/ki_collision.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_collision.h"
    "${SRC_DIR}/radiant/modelimport/ki_recipe.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_recipe.h"
    "${SRC_DIR}/radiant/modelimport/ki_build.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_build.h"
    "${SRC_DIR}/radiant/modelimport/ki_xmodel_read.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_xmodel_read.h"
    "${SRC_DIR}/radiant/modelimport/ki_autocollide.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_autocollide.h"
    "${SRC_DIR}/radiant/modelimport/ki_pick.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_pick.h"
    "${SRC_DIR}/radiant/modelimport/ki_job.cpp"
    "${SRC_DIR}/radiant/modelimport/ki_job.h"
    "${SRC_DIR}/radiant/deps/mikktspace/mikktspace.c"
    "${SRC_DIR}/radiant/deps/mikktspace/mikktspace.h"
)

# The core's third-party code lives beside it in src/radiant/deps:
# stb_image + stb_dxt (stb/), nlohmann json (nlohmann/json.hpp) and the
# MikkTSpace tangent generator (mikktspace/, zlib licence, plain C).
set(MODELIMPORT_DEPS_INCLUDE_DIRS
    "${SRC_DIR}/radiant/deps/stb"
    "${SRC_DIR}/radiant/deps"
)
set(MODELIMPORT_INCLUDE_DIRS
    "${SRC_DIR}/radiant/modelimport"
    ${MODELIMPORT_DEPS_INCLUDE_DIRS}
)
