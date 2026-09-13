#pragma once
#include <universal/q_shared.h>
#include <gfx_d3d/r_gfx.h>
#include <limits.h>

static inline bool DB64_ValidLightGridHeader(const GfxLightGrid *grid, unsigned int primaryLightCount)
{
    if (!grid || !primaryLightCount || primaryLightCount > 255 || grid->rowAxis > 1 || grid->colAxis > 1 ||
        grid->rowAxis == grid->colAxis || grid->sunPrimaryLightIndex > 1 ||
        grid->sunPrimaryLightIndex >= primaryLightCount || !grid->rowDataStart ||
        grid->entryCount > INT_MAX / sizeof(GfxLightGridEntry) || grid->rawRowDataSize > 262144 || (!!grid->rawRowData != (grid->rawRowDataSize != 0)) ||
        (!!grid->entries != (grid->entryCount != 0)) || !grid->colors || !grid->colorCount || grid->colorCount > 65536)
    {
        return false;
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (grid->mins[i] > grid->maxs[i] || grid->maxs[i] >= (i == 2 ? 4096 : 8192))
        {
            return false;
        }
    }
    return true;
}

void DB64_LoadRenderLightGrid(GfxLightGrid *grid, unsigned int primaryLightCount, bool atStreamStart);

// Validate all rows, including rows no static model happens to sample.
static inline bool DB64_ValidLightGridData(const GfxLightGrid *grid, unsigned int primaryLightCount)
{
    if (!DB64_ValidLightGridHeader(grid, primaryLightCount))
    {
        return false;
    }
    const unsigned int rowCount = grid->maxs[grid->rowAxis] - grid->mins[grid->rowAxis] + 1;
    for (unsigned int i = 0; i < grid->entryCount; ++i)
    {
        if (grid->entries[i].colorsIndex >= grid->colorCount ||
            (grid->entries[i].primaryLightIndex != 255 && grid->entries[i].primaryLightIndex >= primaryLightCount))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < rowCount; ++i)
    {
        if (grid->rowDataStart[i] == UINT16_MAX)
        {
            continue;
        }
        size_t offset = (size_t)grid->rowDataStart[i] * 4;
        if (offset > grid->rawRowDataSize || sizeof(GfxLightGridRow) > grid->rawRowDataSize - offset)
        {
            return false;
        }
        GfxLightGridRow row;
        memcpy(&row, grid->rawRowData + offset, sizeof(GfxLightGridRow));
        offset += sizeof(GfxLightGridRow);
        if (!row.colCount || !row.zCount || row.colStart < grid->mins[grid->colAxis] ||
            (unsigned int)row.colStart + row.colCount > (unsigned int)grid->maxs[grid->colAxis] + 1 ||
            row.zStart < grid->mins[2] || (unsigned int)row.zStart + row.zCount > (unsigned int)grid->maxs[2] + 1 ||
            row.firstEntry >= grid->entryCount)
        {
            return false;
        }
        unsigned int columns = 0, entry = row.firstEntry;
        while (columns < row.colCount)
        {
            if (offset > grid->rawRowDataSize || grid->rawRowDataSize - offset < 2)
            {
                return false;
            }
            const unsigned int count = grid->rawRowData[offset], height = grid->rawRowData[offset + 1];
            const unsigned int runSize = height ? 3 + (row.zCount > 255) : 2;
            if (!count || count > row.colCount - columns || runSize > grid->rawRowDataSize - offset ||
                count * height > grid->entryCount - entry)
            {
                return false;
            }
            if (height)
            {
                unsigned int base = grid->rawRowData[offset + 2];
                if (row.zCount > 255)
                {
                    base |= (unsigned int)grid->rawRowData[offset + 3] << 8;
                }
                if (base + height > row.zCount)
                {
                    return false;
                }
            }
            entry += count * height;
            columns += count;
            offset += runSize;
        }
    }
    return true;
}
