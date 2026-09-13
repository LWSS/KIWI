#pragma once
#include <stddef.h>
#include <stdint.h>
struct cplane_s;
struct GfxStaticModelInst;
struct LinkerModelCells
{
    unsigned int modelCount, cellCount, wordsPerModel;
    uint32_t *bits;
};
// Cell membership comes from the BSP node stream, with axial splitting to avoid adding unrelated cells.
bool Linker_BuildModelCells(const uint16_t *nodes, unsigned int nodeWordCount, const cplane_s *planes,
                            unsigned int planeCount, unsigned int cellCount, const GfxStaticModelInst *models,
                            unsigned int modelCount, LinkerModelCells **membership, char *error, size_t errorSize);
void Linker_FreeModelCells(LinkerModelCells *membership);
struct LinkerBspRenderCells;
struct LinkerBspReflectionProbes;
struct GfxStaticModelDrawInst;
// Uses the model's bounds center and the containing cell's probe list; solid-space centers search all authored probes.
bool Linker_AssignModelReflectionProbes(const uint16_t *nodes, unsigned int nodeWordCount,
                                        const LinkerBspRenderCells *cells, const LinkerBspReflectionProbes *probes,
                                        const GfxStaticModelInst *models, unsigned int modelCount,
                                        GfxStaticModelDrawInst *draw, char *error, size_t errorSize);
