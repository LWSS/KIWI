#include <universal/q_shared.h>
#include <gfx_d3d/r_gfx.h>
#include "native_light_grid.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool GridEntry(const GfxLightGrid *grid, const int *position, const GfxLightGridEntry **entry)
{
    *entry = NULL;
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (position[i] < grid->mins[i] || position[i] > grid->maxs[i])
        {
            return true;
        }
    }
    const unsigned int rowIndex = position[grid->rowAxis] - grid->mins[grid->rowAxis];
    if (grid->rowDataStart[rowIndex] == UINT16_MAX)
    {
        return true;
    }
    size_t offset = (size_t)grid->rowDataStart[rowIndex] * 4;
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
    const int col = position[grid->colAxis] - row.colStart, z = position[2] - row.zStart;
    if (col < 0 || col >= row.colCount || z < 0 || z >= row.zCount)
    {
        return true;
    }
    unsigned int columns = 0, firstEntry = row.firstEntry;
    while (columns < row.colCount)
    {
        if (offset > grid->rawRowDataSize || grid->rawRowDataSize - offset < 2)
        {
            return false;
        }
        const unsigned int count = grid->rawRowData[offset], height = grid->rawRowData[offset + 1];
        const unsigned int runSize = height ? 3 + (row.zCount > 255) : 2;
        if (!count || count > row.colCount - columns || runSize > grid->rawRowDataSize - offset ||
            count * height > grid->entryCount - firstEntry)
        {
            return false;
        }
        unsigned int base = 0;
        if (height)
        {
            base = grid->rawRowData[offset + 2];
            if (row.zCount > 255)
            {
                base |= (unsigned int)grid->rawRowData[offset + 3] << 8;
            }
            if (base + height > row.zCount)
            {
                return false;
            }
        }
        if ((unsigned int)col < columns + count)
        {
            if (height && (unsigned int)z >= base && (unsigned int)z - base < height)
            {
                *entry = &grid->entries[firstEntry + ((unsigned int)col - columns) * height + (unsigned int)z - base];
            }
            return true;
        }
        firstEntry += count * height;
        columns += count;
        offset += runSize;
    }
    return false;
}

static bool SampleGrid(const GfxLightGrid *grid, const float *position, unsigned int primaryLightCount,
                       LinkerGridSightTrace trace, void *context, uint8_t *primaryLight)
{
    if (!grid || !position || !primaryLight || !primaryLightCount || primaryLightCount > 255 ||
        grid->sunPrimaryLightIndex > 1 || grid->sunPrimaryLightIndex >= primaryLightCount || grid->rowAxis > 1 ||
        grid->colAxis > 1 || grid->rowAxis == grid->colAxis || !grid->rowDataStart ||
        (grid->rawRowDataSize && !grid->rawRowData) || (grid->entryCount && !grid->entries))
    {
        return false;
    }
    int pos[3];
    double lerp[3];
    bool outside = false;
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(position[i]) || grid->mins[i] > grid->maxs[i] || grid->maxs[i] >= (i == 2 ? 4096 : 8192))
        {
            return false;
        }
        const double coordinate = ((double)position[i] + 131072) / (i == 2 ? 64 : 32);
        if (coordinate < (double)grid->mins[i] - 1 || coordinate >= (double)grid->maxs[i] + 1)
        {
            outside = true;
        }
        // Bound the conversion even when a finite input is far outside the world's grid.
        const double integral = floor(coordinate);
        pos[i] = integral >= -1 && integral <= 8192 ? (int)integral : -2;
        lerp[i] = coordinate - integral;
    }
    if (outside)
    {
        *primaryLight = (uint8_t)grid->sunPrimaryLightIndex;
        return true;
    }
    const GfxLightGridEntry *entries[8] = {};
    bool honorSuppression = false;
    double bestWeight = 0;
    uint8_t selected = 0;
    for (unsigned int corner = 0; corner < 8; ++corner)
    {
        int point[3] = {pos[0], pos[1], pos[2]};
        const unsigned int rowBit = (corner >> 2) & 1, colBit = (corner >> 1) & 1, zBit = corner & 1;
        point[grid->rowAxis] += rowBit;
        point[grid->colAxis] += colBit;
        point[2] += zBit;
        if (!GridEntry(grid, point, &entries[corner]))
        {
            return false;
        }
        const GfxLightGridEntry *entry = entries[corner];
        const double weight = (rowBit ? lerp[grid->rowAxis] : 1 - lerp[grid->rowAxis]) *
                              (colBit ? lerp[grid->colAxis] : 1 - lerp[grid->colAxis]) * (zBit ? lerp[2] : 1 - lerp[2]);
        if (!entry)
        {
            continue;
        }
        if (entry->primaryLightIndex != 255 && entry->primaryLightIndex >= primaryLightCount)
        {
            return false;
        }
        if (weight < 0.001f)
        {
            entries[corner] = NULL;
            continue;
        }
        bool visible = true;
        if (entry->needsTrace & (1 << corner))
        {
            if (!trace)
            {
                return false;
            }
            float end[3];
            double direction[3], lengthSquared = 0;
            for (unsigned int i = 0; i < 3; ++i)
            {
                end[i] = (float)(point[i] * (i == 2 ? 64 : 32) - 131072);
                direction[i] = (double)position[i] - end[i];
                lengthSquared += direction[i] * direction[i];
            }
            const double nudge = lengthSquared > 0 ? 0.01 / sqrt(lengthSquared) : 0;
            for (unsigned int i = 0; i < 3; ++i)
            {
                end[i] = (float)(end[i] + direction[i] * nudge);
            }
            if (!trace(position, end, &visible, context))
            {
                return false;
            }
        }
        if (!visible && honorSuppression)
        {
            entries[corner] = NULL;
            continue;
        }
        if (visible && !honorSuppression)
        {
            honorSuppression = true;
            bestWeight = weight;
            selected = entry->primaryLightIndex;
            for (unsigned int i = 0; i < corner; ++i)
            {
                entries[i] = NULL;
            }
            continue;
        }
        const uint8_t light = entry->primaryLightIndex;
        if (!selected || (light && (selected == 255 || (light != 255 && weight > bestWeight))))
        {
            bestWeight = weight;
            selected = light;
        }
    }
    if (selected == 255)
    {
        selected = (uint8_t)grid->sunPrimaryLightIndex;
    }
    for (unsigned int i = 0; i < 8; ++i)
    {
        if (entries[i] && entries[i]->primaryLightIndex != 255 &&
            (!entries[i]->primaryLightIndex || entries[i]->primaryLightIndex == selected))
        {
            *primaryLight = selected;
            return true;
        }
    }
    *primaryLight = (uint8_t)grid->sunPrimaryLightIndex;
    return true;
}

bool Linker_SamplePrimaryLightGrid(const GfxLightGrid *grid, const float *position, unsigned int primaryLightCount,
                                   LinkerGridSightTrace trace, void *context, uint8_t *primaryLight, char *error,
                                   size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    uint8_t selected = 0;
    if (!primaryLight || !SampleGrid(grid, position, primaryLightCount, trace, context, &selected))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid light-grid sample, row data or missing/failed visibility trace");
        }
        return false;
    }
    *primaryLight = selected;
    return true;
}
