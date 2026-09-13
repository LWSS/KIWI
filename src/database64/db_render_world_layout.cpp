#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include "db_render_world_layout.h"
#include <stdio.h>

bool DB64_GetWorldRuntimeLayout(const GfxWorld *world, DB64WorldRuntimeLayout *layout, char *error, size_t errorSize)
{
    if (!layout || (!error && errorSize))
    {
        return false;
    }
    *layout = {};
    if (errorSize)
    {
        error[0] = 0;
    }
    if (!world || world->dpvsPlanes.cellCount < 0 || world->dpvsPlanes.cellCount > 1024 || world->surfaceCount < 0 ||
        world->surfaceCount > 65536 || world->dpvs.staticSurfaceCount > (unsigned int)world->surfaceCount ||
        world->dpvs.staticSurfaceCountNoDecal > world->dpvs.staticSurfaceCount || world->dpvs.smodelCount > 65536 ||
        world->sunPrimaryLightIndex > 1 || world->primaryLightCount <= world->sunPrimaryLightIndex ||
        world->primaryLightCount > 255 || world->dpvsDyn.dynEntClientCount[0] > 65535 ||
        world->dpvsDyn.dynEntClientCount[1] > 65535)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid native render world visibility dimensions");
        }
        return false;
    }
    const unsigned int cells = world->dpvsPlanes.cellCount;
    const unsigned int lights = world->primaryLightCount - world->sunPrimaryLightIndex - 1;
    layout->cellBitsBytes = 16 * ((cells + 127) / 128);
    layout->surfaceVisWords = 4 * ((world->dpvs.staticSurfaceCount + 127) / 128);
    layout->smodelVisWords = 4 * ((world->dpvs.smodelCount + 127) / 128);
    layout->sceneEntCellWords = cells * 256;
    layout->cellCasterWords = cells * ((cells + 31) / 32);
    // Each non-sun primary light has one bit for each of 4096 scene entities.
    layout->primaryLightEntityShadowWords = lights * (4096 / 32);
    for (unsigned int i = 0; i < 2; ++i)
    {
        const unsigned int entities = world->dpvsDyn.dynEntClientCount[i];
        layout->dynEntWords[i] = (entities + 31) / 32;
        layout->dynEntVisBytes[i] = 32 * layout->dynEntWords[i];
        layout->dynEntCellWords[i] = cells * layout->dynEntWords[i];
        // Lights share one packed bit array; do not round each light's row separately.
        layout->primaryLightDynEntShadowWords[i] = (lights * entities + 31) / 32;
    }
    return true;
}

bool DB64_ValidateWorldRuntimeCounts(const GfxWorld *world, char *error, size_t errorSize)
{
    DB64WorldRuntimeLayout layout;
    if (!DB64_GetWorldRuntimeLayout(world, &layout, error, errorSize))
    {
        return false;
    }
    if (world->cellBitsCount != layout.cellBitsBytes || world->dpvs.surfaceVisDataCount != layout.surfaceVisWords ||
        world->dpvs.smodelVisDataCount != layout.smodelVisWords ||
        world->dpvsDyn.dynEntClientWordCount[0] != layout.dynEntWords[0] ||
        world->dpvsDyn.dynEntClientWordCount[1] != layout.dynEntWords[1])
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Native render world visibility counts disagree with their dimensions");
        }
        return false;
    }
    return true;
}
