#pragma once
#include <stddef.h>
struct ComWorld;
struct LinkerBspLightRegions;
struct LinkerWorldSurfaceOrder;
struct GfxShadowGeometry;
struct GfxStaticModelDrawInst;
struct GfxStaticModelInst;
// Selects local lights by lowest-LOD vertex voting, preserving baked ground lighting.
// Unlit models remain zero; the light-grid sun fallback must run before final serialization.
bool Linker_AssignModelLocalLights(const ComWorld *common, const LinkerBspLightRegions *regions,
                                   const GfxStaticModelInst *instances, unsigned int modelCount,
                                   GfxStaticModelDrawInst *draw, char *error, size_t errorSize);
// Replaces only model lists, preserving surface lists. Call again after changing light assignments or model order.
bool Linker_BuildModelShadows(GfxShadowGeometry *shadows, unsigned int lightCount, const GfxStaticModelDrawInst *draw,
                              unsigned int modelCount, char *error, size_t errorSize);
// Builds sorted surface references for non-sun primary lights. Static-model lists are attached separately.
bool Linker_BuildSurfaceShadows(const ComWorld *common, unsigned int sunIndex, const LinkerBspLightRegions *regions,
                                const LinkerWorldSurfaceOrder *order, GfxShadowGeometry **shadows, char *error,
                                size_t errorSize);
void Linker_FreeSurfaceShadows(GfxShadowGeometry *shadows, unsigned int lightCount);
