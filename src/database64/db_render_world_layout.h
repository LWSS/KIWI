#pragma once
#include <stddef.h>
struct GfxWorld;
struct DB64WorldRuntimeLayout
{
    unsigned int cellBitsBytes, surfaceVisWords, smodelVisWords;
    unsigned int sceneEntCellWords, cellCasterWords;
    unsigned int primaryLightEntityShadowWords;
    unsigned int dynEntWords[2], dynEntVisBytes[2], dynEntCellWords[2];
    unsigned int primaryLightDynEntShadowWords[2];
};
// Reads only scalar fields: usable before serialized pointers have been resolved.
bool DB64_GetWorldRuntimeLayout(const GfxWorld *world, DB64WorldRuntimeLayout *layout, char *error, size_t errorSize);
bool DB64_ValidateWorldRuntimeCounts(const GfxWorld *world, char *error, size_t errorSize);
