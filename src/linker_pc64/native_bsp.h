#pragma once
#include <stddef.h>
#include <stdint.h>
#include <xanim/xanim.h>
#include <game/g_bsp.h>

struct LinkerMapWorlds
{
    MapEnts entities;
    ComWorld common;
    GameWorldMp multiplayer;
    char *name;
    char *lightNames;
};
struct DynEntityCreateParams;
// Caller owns the returned array; source dependency names are retained for compilation.
bool Linker_ImportDynamicEntityParams(const void *bsp, size_t size, DynEntityCreateParams **entities,
                                      unsigned int *count, char *error, size_t errorSize);

// Imports the pointer-bearing world records from a KIWI BSP v22. Render and
// collision worlds are separate import stages. The input bytes are not retained.
bool Linker_ImportMapWorlds(const void *bsp, size_t size, const char *assetName, LinkerMapWorlds **worlds, char *error,
                            size_t errorSize);
void Linker_FreeMapWorlds(LinkerMapWorlds *worlds);

struct SunLightParseParams;
struct GfxLight;
bool Linker_ImportSunSettings(const void *bsp, size_t size, SunLightParseParams *params, GfxLight *light, char *error,
                              size_t errorSize);

struct clipMap_t;
struct LinkerBspStaticModel
{
    char model[1024];
    float origin[3], angles[3], scale;
    unsigned int entityIndex;
    uint8_t flags, primaryLightIndex, groundLighting[4];
    bool hasGroundLighting;
};
// Preserves renderer entities purged from MapEnts. Caller releases placements with free().
bool Linker_ImportStaticModelPlacements(const void *bsp, size_t size, unsigned int sunIndex,
                                        unsigned int primaryLightCount, LinkerBspStaticModel **placements,
                                        unsigned int *count, char *error, size_t errorSize);
// Collision import stage: triangles, partitions, AABBs, materials, planes and brushes.
// Nodes, leaves and submodels include spatial brush subdivision and the box hull.
// Static and dynamic model dependencies remain a separate import stage.
bool Linker_ImportCollisionGeometry(const void *bsp, size_t size, clipMap_t **map, char *error, size_t errorSize);
void Linker_FreeCollisionGeometry(clipMap_t *map);

struct GfxWorldVertex;
struct GfxSurface;
struct GfxBrushModel;
struct GfxCullGroup;
struct LinkerBspRenderGroups
{
    unsigned int modelCount, cullGroupCount;
    GfxBrushModel *models;
    GfxCullGroup *cullGroups;
};
// Surface ranges use the original BSP ordering; world assembly applies the final sorting permutation.
bool Linker_ImportRenderGroups(const void *bsp, size_t size, bool layered, unsigned int surfaceCount,
                               LinkerBspRenderGroups **groups, char *error, size_t errorSize);
void Linker_FreeRenderGroups(LinkerBspRenderGroups *groups);
struct LinkerBspRenderGeometry
{
    unsigned int vertexCount, surfaceCount, indexCount, materialCount, layerDataSize;
    GfxWorldVertex *vertices;
    GfxSurface *surfaces;
    uint16_t *indices;
    uint16_t *materialIndices;
    dmaterial_t *materials;
    uint8_t *layerData;
};
// Material/image references and lightmap atlas transforms are assigned during world assembly.
bool Linker_ImportRenderGeometry(const void *bsp, size_t size, bool layered, LinkerBspRenderGeometry **geometry,
                                 char *error, size_t errorSize);
void Linker_FreeRenderGeometry(LinkerBspRenderGeometry *geometry);
struct Material;
// Binds compiled materials in BSP material-table order, after validating every layer-data span.
// The material objects remain owned by the caller. Failure leaves surface material pointers unchanged.
bool Linker_BindRenderMaterials(LinkerBspRenderGeometry *geometry, Material *const *materials,
                                unsigned int materialCount, char *error, size_t errorSize);

struct GfxAabbTree;
struct LinkerBspRenderTrees
{
    unsigned int treeCount;
    GfxAabbTree *trees;
    uint8_t *roots;
};
bool Linker_ImportRenderTrees(const void *bsp, size_t size, bool layered, const LinkerBspRenderGeometry *geometry,
                              LinkerBspRenderTrees **trees, char *error, size_t errorSize);
void Linker_FreeRenderTrees(LinkerBspRenderTrees *trees);

struct GfxCell;
struct GfxPortal;
struct LinkerBspRenderCells
{
    unsigned int cellCount, portalCount, vertexCount, planeCount, cullIndexCount;
    GfxCell *cells;
    GfxPortal *portals;
    float (*vertices)[3];
    cplane_s *planes;
    int *cullIndices;
};
// Owns the cell/portal data; cell tree pointers borrow the supplied render trees.
bool Linker_ImportRenderCells(const void *bsp, size_t size, bool layered, const LinkerBspRenderTrees *trees,
                              const LinkerBspRenderGroups *groups, LinkerBspRenderCells **cells, char *error,
                              size_t errorSize);
void Linker_FreeRenderCells(LinkerBspRenderCells *cells);
// Builds the renderer's uint16_t node stream, collapsing subtrees contained in a single cell.
// The returned allocation belongs to the caller and is released with free().
bool Linker_ImportRenderNodes(const void *bsp, size_t size, unsigned int cellCount, unsigned int planeCount,
                              uint16_t **nodes, unsigned int *wordCount, char *error, size_t errorSize);

struct GfxLightGrid;
// Imports v22 light grid data; light regions are attached during world assembly.
bool Linker_ImportLightGrid(const void *bsp, size_t size, unsigned int sunPrimaryLightIndex,
                            unsigned int primaryLightCount, GfxLightGrid **grid, char *error, size_t errorSize);
void Linker_FreeLightGrid(GfxLightGrid *grid);

struct GfxLightmapArray;
struct LinkerBspLightmaps
{
    unsigned int count;
    GfxLightmapArray *lightmaps;
};
// Keeps BSP tile indices and UVs unchanged. Light-definition attenuation is applied during world assembly.
bool Linker_ImportLightmaps(const void *bsp, size_t size, const char *mapName, const LinkerBspRenderGeometry *geometry,
                            LinkerBspLightmaps **lightmaps, char *error, size_t errorSize);
void Linker_FreeLightmaps(LinkerBspLightmaps *lightmaps);

struct GfxReflectionProbe;
struct ColorCorrectionData;
// Parses reflections/reflections.csv; the returned table is released with free().
bool Linker_ParseReflectionCorrections(const void *data, size_t size, ColorCorrectionData **corrections,
                                       unsigned int *count, char *error, size_t errorSize);
struct LinkerBspReflectionProbes
{
    unsigned int count;
    GfxReflectionProbe *probes;
};
// Includes the engine's default probe at index zero. A null correction table uses identity levels/gamma/saturation.
bool Linker_ImportReflectionProbes(const void *bsp, size_t size, const char *mapName,
                                   const ColorCorrectionData *corrections, unsigned int correctionCount,
                                   LinkerBspReflectionProbes **probes, char *error, size_t errorSize);
void Linker_FreeReflectionProbes(LinkerBspReflectionProbes *probes);

struct GfxLightRegion;
struct GfxLightRegionHull;
struct GfxLightRegionAxis;
struct LinkerBspLightRegions
{
    unsigned int regionCount, hullCount, axisCount;
    bool present;
    GfxLightRegion *regions;
    GfxLightRegionHull *hulls;
    GfxLightRegionAxis *axes;
};
bool Linker_ImportLightRegions(const void *bsp, size_t size, unsigned int primaryLightCount,
                               LinkerBspLightRegions **regions, char *error, size_t errorSize);
void Linker_FreeLightRegions(LinkerBspLightRegions *regions);
