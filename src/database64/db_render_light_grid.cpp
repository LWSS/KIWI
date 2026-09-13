#include <universal/q_shared.h>
#include <gfx_d3d/r_gfx.h>
#include "database.h"
#include "db_render_light_grid.h"
#include <limits.h>

static void *ReadArray(size_t count, size_t stride, unsigned int alignment)
{
    if (count > INT_MAX / stride)
    {
        Com_Error(ERR_DROP, "Native light-grid array exceeds stream limits");
    }
    void *array = DB_AllocStreamPos(alignment);
    Load_Stream(true, (uint8_t *)array, count * stride);
    return array;
}

void DB64_LoadRenderLightGrid(GfxLightGrid *grid, unsigned int primaryLightCount, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)grid, sizeof(GfxLightGrid));
    if (!DB64_ValidLightGridHeader(grid, primaryLightCount))
    {
        Com_Error(ERR_DROP, "Invalid native light-grid header");
    }
    const unsigned int rowCount = grid->maxs[grid->rowAxis] - grid->mins[grid->rowAxis] + 1;
    grid->rowDataStart = (uint16_t *)ReadArray(rowCount, sizeof(uint16_t), 1);
    if (grid->rawRowData)
    {
        grid->rawRowData = (uint8_t *)ReadArray(grid->rawRowDataSize, sizeof(uint8_t), 0);
    }
    if (grid->entries)
    {
        grid->entries = (GfxLightGridEntry *)ReadArray(grid->entryCount, sizeof(GfxLightGridEntry), 15);
    }
    grid->colors = (GfxLightGridColors *)ReadArray(grid->colorCount, sizeof(GfxLightGridColors), 15);
    if (!DB64_ValidLightGridData(grid, primaryLightCount))
    {
        Com_Error(ERR_DROP, "Invalid native light-grid row data or entry reference");
    }
}
