#pragma once
#include <stddef.h>
#include <stdint.h>

struct LinkerBspRenderGeometry;
struct LinkerBspRenderGroups;
struct GfxSurface;

struct LinkerWorldSurfaceOrder
{
    unsigned int surfaceCount, staticSurfaceCount;
    unsigned int litSurfsBegin, litSurfsEnd, decalSurfsBegin, decalSurfsEnd;
    unsigned int emissiveSurfsBegin, emissiveSurfsEnd;
    GfxSurface *surfaces;
    uint16_t *materialIndices;
    uint16_t *originalToSorted;
};

// Material ranks must come from the zone's material ordering. They are not pointer values.
// Only model zero is sorted. Tree/cull-group ranges continue to index originalToSorted;
// submodel ranges continue to index surfaces directly. All input data remains unchanged.
// The no-decal index suffix is constructed separately after decal-layer classification.
bool Linker_SortWorldSurfaces(const LinkerBspRenderGeometry *geometry, const LinkerBspRenderGroups *groups,
                              const uint16_t *materialRanks, unsigned int materialRankCount,
                              LinkerWorldSurfaceOrder **order, char *error, size_t errorSize);
void Linker_FreeWorldSurfaceOrder(LinkerWorldSurfaceOrder *order);
