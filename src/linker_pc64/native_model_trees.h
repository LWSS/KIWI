#pragma once
#include <stddef.h>
struct GfxAabbTree;
struct GfxStaticModelInst;
struct LinkerBspRenderTrees;
struct LinkerBspRenderCells;
struct LinkerModelCells;
struct LinkerModelTrees
{
    unsigned int treeCount, cellCount;
    GfxAabbTree *trees;
    unsigned int *cellRoots, *cellTreeCounts;
};
// Copies the finalized surface trees, inserts model lists, and flattens each cell's subtree contiguously.
// sourceTrees is the original array used by cell pointers; surfaceTrees has the same original indexing.
bool Linker_BuildModelTrees(const LinkerBspRenderTrees *sourceTrees, const GfxAabbTree *surfaceTrees,
                            const LinkerBspRenderCells *cells, const LinkerModelCells *membership,
                            const GfxStaticModelInst *models, LinkerModelTrees **result, char *error, size_t errorSize);
void Linker_FreeModelTrees(LinkerModelTrees *trees);
