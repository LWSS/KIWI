#pragma once
#include <stddef.h>
#include <stdint.h>
struct LinkerBspRenderGeometry;
struct LinkerBspRenderGroups;
struct LinkerBspRenderTrees;
struct LinkerBspRenderCells;
struct LinkerWorldSurfaceOrder;
struct GfxBrushModel;
struct GfxAabbTree;
struct LinkerWorldVisibility
{
    unsigned int staticSurfaceCountNoDecal, sortedIndexCount;
    GfxBrushModel *models;
    GfxAabbTree *trees;
    uint16_t *sortedSurfIndex;
};
// Owns copies of the models/trees and the original-order plus no-decal surface indices.
// Final world assembly must redirect cell tree pointers to these copies at the same array offsets.
// On success, updates only bit 1 of order->surfaces[].flags. Failure changes no input.
bool Linker_BuildWorldVisibility(const LinkerBspRenderGeometry *geometry, const LinkerBspRenderGroups *groups,
                                 const LinkerBspRenderTrees *trees, const LinkerBspRenderCells *cells,
                                 const uint16_t *materialRanks, unsigned int materialRankCount,
                                 LinkerWorldSurfaceOrder *order, LinkerWorldVisibility **visibility, char *error,
                                 size_t errorSize);
void Linker_FreeWorldVisibility(LinkerWorldVisibility *visibility);
