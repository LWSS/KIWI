#pragma once
#include "native_bsp.h"
#include <stddef.h>
struct LinkerCompiledMaterial;
struct LinkerWorldSurfaceOrder;
struct LinkerWorldVisibility;
struct GfxLightDef;
struct GfxShadowGeometry;
struct LinkerCompiledModel;
struct GfxStaticModelDrawInst;
struct GfxStaticModelInst;
struct LinkerModelCells;
struct LinkerModelTrees;
struct LinkerAssembledWorld;
struct LinkerCompiledSun;
struct WorldEffectCompileContext;
struct LinkerWorldSource
{
    LinkerMapWorlds *metadata;
    clipMap_t *collision;
    LinkerBspRenderGeometry *geometry;
    LinkerBspRenderGroups *groups;
    LinkerBspRenderTrees *trees;
    LinkerBspRenderCells *cells;
    LinkerBspLightmaps *lightmaps;
    LinkerBspReflectionProbes *probes;
    LinkerBspLightRegions *regions;
    GfxLightGrid *lightGrid;
    uint16_t *nodes;
    unsigned int nodeWordCount, materialCount, sunPrimaryLightIndex;
    LinkerCompiledMaterial **materials;
    LinkerWorldSurfaceOrder *surfaceOrder;
    LinkerWorldVisibility *visibility;
    SunLightParseParams *sunParse;
    GfxLight *sunLight;
    unsigned int lightDefCount;
    GfxLightDef **lightDefs;
    GfxShadowGeometry *surfaceShadows;
    unsigned int staticModelPlacementCount;
    LinkerBspStaticModel *staticModelPlacements;
    ScriptStringList modelStrings;
    unsigned int modelCount;
    LinkerCompiledModel **models;
    XModel **staticModelAssets;
    GfxStaticModelDrawInst *modelDrawInstances;
    GfxStaticModelInst *modelInstances;
    LinkerModelCells *modelCells;
    LinkerModelTrees *modelTrees;
    LinkerAssembledWorld *assembled;
    LinkerCompiledSun *sunEffects;
    DynEntityCreateParams *dynamicEntities;
    unsigned int dynamicEntityCount;
    PhysPreset **dynamicPresets;
    XModelPieces **dynamicPieces;
    WorldEffectCompileContext *effectContext;
};
// Compiles and owns the BSP render inputs and their material/image dependencies.
// Final renderer assembly, model ordering/cache allocation and sun flare assets remain separate steps.
bool Linker_CompileWorldSource(const char *root, const char *bspName, bool layered, LinkerWorldSource **world,
                               char *error, size_t errorSize);
void Linker_FreeWorldSource(LinkerWorldSource *world);
XModel *Linker_CompileWorldModel(const char *root, LinkerWorldSource *world, const char *name,
                                char *error, size_t errorSize);
const FxEffectDef *Linker_CompileWorldEffect(const char *root, LinkerWorldSource *world, const char *name,
                                            char *error, size_t errorSize);
