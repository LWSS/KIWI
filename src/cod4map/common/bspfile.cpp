/*
bspfile.c — BSP file format I/O

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

/* core geometry */
BspLeaf_disk_t  bspLeafs[MAX_MAP_LEAFS];
BspModel_t      bspModels[MAX_MAP_MODELS];
BspNode_disk_t  bspNodes[MAX_MAP_NODES];
BspPlane_disk_t bspPlanes[MAX_MAP_PLANES];

/* surfaces */
BspDrawVert_t  bspDrawVerts[MAX_MAP_DRAW_VERTS];
BspDrawVert_t  g_drawVertexBuf[(MAX_MAP_DRAW_VERTS + 1)];
BspTriSoup_t   bspTriangles[MAX_MAP_TRISOUPS];
#define bspTriSoupData (&g_drawVertexBuf[1])
unsigned short bspDrawIndexes[MAX_MAP_DRAW_INDEXES];

/* CoD4 emits the same surface pipeline into two independent output families.
   The second family is the unlayered draw data referenced by lumps 47-51. */
BspDrawVert_t  bspUnlayeredDrawVerts[MAX_MAP_DRAW_VERTS];
BspDrawVert_t  g_unlayeredDrawVertexBuf[(MAX_MAP_DRAW_VERTS + 1)];
BspTriSoup_t   bspUnlayeredTriangles[MAX_MAP_TRISOUPS];
unsigned short bspUnlayeredDrawIndexes[MAX_MAP_DRAW_INDEXES];

/* brushes */
BspBrushSide_t bspBrushSidesData[MAX_MAP_BRUSHSIDES];
unsigned char  bspBrushSideEdgeCounts[MAX_MAP_BRUSHSIDES];
unsigned char  bspBrushEdges[MAX_MAP_BRUSHEDGES];
BspBrush_t     bspBrushes[MAX_MAP_BRUSHES];
int            bspLeafBrushes[MAX_MAP_LEAFBRUSHES];
int            bspLeafSurfaces[MAX_MAP_LEAFSURFACES];

/* visibility / portals */
BspPortal_t bspPortals[MAX_MAP_PORTALS];
vec3_t      bspPortalVerts[MAX_MAP_PORTAL_VERTS];
char        bspVisBytes[MAX_MAP_VISIBILITY];

/* collision */
BspAabbTreeEntry_t   bspAabbTrees[MAX_MAP_AABBTREES];
BspAabbTreeEntry_t   bspUnlayeredAabbTrees[MAX_MAP_AABBTREES];
BspCollisionBorder_t bspCollisionBorders[MAX_MAP_COLLISION_BORDERS];
BspCollisionEdge_t   bspCollisionEdgeData[MAX_MAP_COLLISION_EDGES];
BspCollisionPart_t   bspCollisionParts[MAX_MAP_COLLISION_PARTS];
BspCollisionTri_t    bspCollisionTriData[MAX_MAP_COLLISION_TRIS];
BspCollisionVert_t   bspCollisionVerts[MAX_MAP_COLLISION_VERTS];
unsigned char         bspCollisionEdgeWalkable[((3 * MAX_MAP_COLLISION_TRIS + 7) / 8 + 3) & ~3];
CollisionAabbTree_t  bspCollisionAABBs[MAX_MAP_COLLISION_AABBS];

/* lighting */
BspLightGridColor_t bspLightGridColors[MAX_MAP_LIGHTGRIDCOLORS];
BspLightGridEntry_t bspLightGridHash[MAX_MAP_LIGHTGRID];
BspLight_t          bspLights[MAX_MAP_LIGHTS];

/* shadows */
BspShadowCluster_disk_t  bspShadowClusters[MAX_MAP_SHADOW_CLUSTERS];
BspShadowSource_t        bspShadowSources[MAX_MAP_SHADOW_SOURCES];
BspShadowVert_t          bspShadowVerts[MAX_MAP_SHADOW_VERTS];
DiskShadowAabb_t         bspShadowData[MAX_MAP_SHADOW_AABBTREES];
#define SHADOW_INDEX_SENTINEL 1
static short             _bspShadowIndexBuf[MAX_MAP_SHADOW_INDEXES + SHADOW_INDEX_SENTINEL];
short                   *bspShadowIndexes = &_bspShadowIndexBuf[SHADOW_INDEX_SENTINEL];

/* occluders */
BspOccluder_t bspOccluders[MAX_MAP_OCCLUDERS];
short         bspOccluderIndexes[MAX_MAP_OCCLUDER_INDEXES];
int           bspOccluderPlanes[MAX_MAP_OCCLUDER_PLANES];

/* cells / cullgroups */
BspCell_t      bspCells[MAX_MAP_CELLS];
BspCullGroup_t bspCullGroups[MAX_MAP_CULLGROUPS];
BspCullGroup_t bspUnlayeredCullGroups[MAX_MAP_CULLGROUPS];
int            bspCullGroupIndexes[MAX_MAP_CULLGROUPINDEXES];

/* entities / materials / misc */
Entity_t    g_entities[MAX_MAP_ENTITIES];
Dmaterial_t bspMaterials[MAX_MAP_MATERIALS];
char        bspEntData[MAX_MAP_ENTSTRING];
char        bspPaths[MAX_MAP_PATHS];
char        g_bspSwapPath[MAX_OS_PATH];
char        g_largeBuf[LARGE_BUF_SIZE];

/* Variable-sized CoD4 light-grid header.  The initial 22-byte value is the
   one-row empty grid emitted by the original compiler before light is built. */
unsigned char bspLightGridHeader[16404];
int           bspLightGridHeaderSize = 22;

static void *s_bspChunkData[BSP_CHUNK_LIMIT];
static int s_loadedLumpLengths[LUMP_COUNT];

enum { MAX_BSP_PRIMARY_LIGHTS = 255 };

/* The compiler retains all three current-format light-region chunks when it
   reads an existing BSP.  Hulls and axes are opaque to the current map build
   path, so keep their native-sized disk payloads verbatim. */
static unsigned char s_bspLightRegionHulls[155040];
static unsigned char s_bspLightRegionAxes[326400];
static unsigned char s_bspLightGridRows[0x40000];
static unsigned char s_bspVertexLayerData[0x200000];
static int s_numBspLightRegions;
static int s_numBspLightRegionHulls;
static int s_numBspLightRegionAxes;
static int s_numBspLightGridRows;
static int s_numBspVertexLayerData;
static int s_loadedLightRegionChunks;
static int s_loadedLightGridRows;
static int s_loadedVertexLayerData;

/* Native 0x449C40 publishes the final type-0 context's byte count to the
   BSP writer.  Keep that output-owned lifecycle separate from the legacy
   loaded-BSP retention path above. */
void BeginBspVertexLayerData(void)
{
  s_numBspVertexLayerData = 0;
  s_loadedVertexLayerData = 1;
}

unsigned char *AllocBspVertexLayerData(unsigned int byteCount)
{
  unsigned char *result;

  if (byteCount > sizeof(s_bspVertexLayerData)
      || (unsigned int)s_numBspVertexLayerData > sizeof(s_bspVertexLayerData) - byteCount)
    Com_Error("MAX_BSP_VERTEX_LAYER_DATA (%i) exceeded\n", (int)sizeof(s_bspVertexLayerData));

  result = &s_bspVertexLayerData[s_numBspVertexLayerData];
  s_numBspVertexLayerData += (int)byteCount;
  s_loadedVertexLayerData = 1;
  return result;
}

unsigned int GetBspVertexLayerDataCount(void)
{
  return (unsigned int)s_numBspVertexLayerData;
}

/* Native 0x435C30 serializes generated packed K-DOPs into these same three
   BSP chunks.  Keep loaded chunks untouched until this explicit build path
   replaces them. */
void BeginBspPrimaryLightRegionEmission(void)
{
  /* 0x435C30 emits exactly one region-count byte for every primary light,
     including zero for sun/omni entries.  Keep that generated byte count
     distinct from the hull and axis counts reset below. */
  s_numBspLightRegions = numBspPrimaryLights;
  s_numBspLightRegionHulls = 0;
  s_numBspLightRegionAxes = 0;
  s_loadedLightRegionChunks = 1;
}

void AppendBspPrimaryLightRegionHull(const void *packedHull)
{
  int axisCount;

  memcpy(&axisCount, (const char *)packedHull + 72, sizeof(axisCount));
  if (s_numBspLightRegionHulls >= (int)(sizeof(s_bspLightRegionHulls) / 76)
      || axisCount < 0
      || axisCount > (int)(sizeof(s_bspLightRegionAxes) / 20) - s_numBspLightRegionAxes)
    Com_Error("MAX_LIGHTREGION_HULLS");

  memcpy(s_bspLightRegionHulls + 76 * s_numBspLightRegionHulls++, packedHull, 76);
  memcpy(s_bspLightRegionAxes + 20 * s_numBspLightRegionAxes, (const char *)packedHull + 76, 20 * axisCount);
  s_numBspLightRegionAxes += axisCount;
}

int  bspEntDataSize;
char Buffer[16];
char g_polyFileExt[16];
char g_prtFileExt[16];
int  numBSPAabbTrees;
int  numBSPUnlayeredAabbTrees;
int  numBSPBrushes;
int  numBSPBrushSides;
int  numBSPBrushEdges;
int  numBSPCells;
int  numBSPCollisionAABBs;
int  numBSPCollisionBorders;
int  numBSPCollisionEdges;
int  numBSPCollisionParts;
int  numBSPCollisionTris;
int  numBSPCollisionVerts;
int  numBSPCullGroupIndexes;
int  numBSPCullGroups;
int  numBSPDrawIndexes;
int  numBSPDrawVerts;
int  numBSPDrawVertsEmitted;
int  numBSPUnlayeredDrawIndexes;
int  numBSPUnlayeredDrawVerts;
int  numBSPUnlayeredDrawVertsEmitted;
int  numBSPLeafBrushes;
int  numBSPLeafs;
int  numBSPLeafSurfaces;
int  numBSPLightBytes;
int  numBSPLightGridColors;
int  numBSPLightGridHash;
int  numBSPLights;
int  numBSPMaterials;
int  numBSPModels;
int  numBSPNodes;
int  numBSPOccluderEdges;
int  numBSPOccluderIndexes;
int  numBSPOccluderPlanes;
int  numBSPOccluders;
int  numBSPPlanes;
int  numBSPPortals;
int  numBSPPortalVerts;
int  numBSPShadowAabbTrees;
int  numBSPShadowClusters;
int  numBSPShadowIndices;
int  numBSPShadowSources;
int  numBSPShadowVerts;
int  numBSPTriSoups;
int  numBSPUnlayeredTriSoups;
int  numBSPVisBytes;
int  num_entities;

char s_assertDisable_GetBSPFileExtension;
char s_assertDisable_GetPRTFileExtension;
char s_assertDisable_GetPolyFileExtension;
char s_assertDisable_SetBSPFileExtensions;
char s_assertDisable_SetBSPFileExtensions;
char s_assertDisable_SetBSPFileExtensions;
char s_assertDisable_SwapCollisionAabbLumpData;
char s_assertDisable_SwapCollisionAabbLumpData;
char s_assertDisable_SwapCollisionPartitionLumpData;
char s_assertDisable_SwapCollisionPartitionLumpData;
char s_assertDisable_SwapDrawSurfLumpData;
char s_assertDisable_SwapDrawSurfLumpData_0;
char s_assertDisable_SwapLightGridEntryLumpData;
char s_assertDisable_SwapLightGridEntryLumpData;
char s_assertDisable_SwapLumpData;
char s_assertDisable_SwapLumpData;
char s_assertDisable_SwapLumpData;
char s_assertDisable_SwapLumpData;
char s_assertDisable_SwapMaterialLumpData;
char s_assertDisable_SwapMaterialLumpData;
char s_assertDisable_SwapOccluderLumpData;
char s_assertDisable_SwapOccluderLumpData;
char s_assertDisable_SwapShortBlock;
char s_assertDisable_SwapShortBlock_0;
char s_assertDisable_SwapTriangleLumpData;
char s_assertDisable_SwapTriangleLumpData;
char s_assertDisable_SwapVisData;
char s_assertDisable_SwapVisData;
char s_assertDisable_UnparseEntities;


/*
================
HandlePlatformOption

Parse -platform argument and set target platform.
================
*/
int HandlePlatformOption(int argc, char **argv)
{
  unsigned int i;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }
  SetTargetPlatformByName(argv[1]);
  return 2;
}

/*
================
SwapBlock

Swap all 32-bit values in a block
================
*/
int SwapBlock(int size, void *block)
{
  unsigned int *p = block;
  int count;
  int i;
  int last;

  last = size;
  count = size >> 2;
  for ( i = 0; i < count; i++ )
  {
    last = BigLong(p[i]);
    p[i] = last;
  }
  return last;
}

/*
================
SwapShortBlock

Swap all 16-bit values in a block.
================
*/
void SwapShortBlock(int size, unsigned short *data)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapShortBlock);
  Assert(size >= 0, s_assertDisable_SwapShortBlock_0);

  count = size >> 1;
  for ( i = 0; i < count; i++ )
    data[i] = BigShort(data[i]);
}

/*
================
SwapDrawSurfLumpData

Byte-swap draw surface lump entries.
================
*/
void SwapDrawSurfLumpData(BspTriSoup_disk_t *data, unsigned int lumpSize)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapDrawSurfLumpData);
  count = lumpSize / sizeof(*data);
  Assert(lumpSize % sizeof(*data) == 0, s_assertDisable_SwapDrawSurfLumpData_0);

  /* swap each entry's fields */
  for ( i = 0; i < count; i++ )
  {
    /* material and lightmap */
    data[i].materialSortKey = BigLong(data[i].materialSortKey);
    data[i].lightmapIndex = BigShort(data[i].lightmapIndex);

    /* vertex/index ranges — patches also need numVerts swapped */
    data[i].firstVert = BigLong(data[i].firstVert);
    if ( !data[i].surfType )
      data[i].numVerts = BigLong(data[i].numVerts);
    data[i].firstIndex = BigLong(data[i].firstIndex);
    data[i].numIndices = BigLong(data[i].numIndices);

    /* buffer references */
    data[i].vertBufIndex = BigLong(data[i].vertBufIndex);
    data[i].vertBufFirstVert = BigLong(data[i].vertBufFirstVert);
    data[i].idxBufIndex = BigLong(data[i].idxBufIndex);
    data[i].idxBufFirstIdx = BigLong(data[i].idxBufFirstIdx);
  }
}

/*
================
SwapMaterialLumpData

Byte-swap material lump entries.
================
*/
void SwapMaterialLumpData(Dmaterial_t *data, unsigned int lumpSize)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapMaterialLumpData);
  count = lumpSize / sizeof(Dmaterial_t);
  Assert(lumpSize % sizeof(Dmaterial_t) == 0, s_assertDisable_SwapMaterialLumpData);

  /* only surfaceFlags and contentFlags need swapping — name is a char array */
  for ( i = 0; i < count; i++ )
  {
    data[i].surfaceFlags = BigLong(data[i].surfaceFlags);
    data[i].contentFlags = BigLong(data[i].contentFlags);
  }
}

/*
================
SwapTriangleLumpData

Byte-swap triangle soup lump entries.
================
*/
int SwapTriangleLumpData(BspTriSoup_t *data, int lumpSize)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapTriangleLumpData);
  Assert(lumpSize >= 0, s_assertDisable_SwapTriangleLumpData);

  count = lumpSize / sizeof(*data);

  /* swap each entry's fields */
  for ( i = 0; i < count; i++ )
  {
    data[i].materialIndex = BigShort(data[i].materialIndex);
    data[i].vertexLayerData = BigLong(data[i].vertexLayerData);
    data[i].firstVertex = BigLong(data[i].firstVertex);
    data[i].vertexCount = BigShort(data[i].vertexCount);
    data[i].indexCount = BigShort(data[i].indexCount);
    data[i].firstIndex = BigLong(data[i].firstIndex);
  }
  return count;
}

/*
================
SwapOccluderLumpData

Byte-swap occluder lump entries.
================
*/
int SwapOccluderLumpData(BspOccluder_t *data, int lumpSize)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapOccluderLumpData);
  Assert(lumpSize >= 0, s_assertDisable_SwapOccluderLumpData);

  count = lumpSize / sizeof(*data);

  /* swap each entry's fields */
  for ( i = 0; i < count; i++ )
  {
    data[i].startPlanes  = BigLong(data[i].startPlanes);
    data[i].numNewPlanes = BigShort(data[i].numNewPlanes);
    data[i].numNewEdges  = BigShort(data[i].numNewEdges);
    data[i].startEdges   = BigLong(data[i].startEdges);
    data[i].startVerts   = BigLong(data[i].startVerts);
    data[i].numNewVerts  = BigShort(data[i].numNewVerts);
  }
  return count;
}

/*
================
SwapCollisionPartitionLumpData

Byte-swap collision partition lump entries.
================
*/
int SwapCollisionPartitionLumpData(BspCollisionPart_t *data, int lumpSize)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapCollisionPartitionLumpData);
  Assert(lumpSize >= 0, s_assertDisable_SwapCollisionPartitionLumpData);

  count = lumpSize / sizeof(*data);

  /* swap int fields — byte fields don't need swapping */
  for ( i = 0; i < count; i++ )
  {
    data[i].reserved0 = BigShort(data[i].reserved0);
    data[i].firstTri = BigLong(data[i].firstTri);
    data[i].firstBorder = BigLong(data[i].firstBorder);
  }
  return count;
}

/*
================
SwapCollisionAabbLumpData

Byte-swap collision AABB lump entries.
================
*/
int SwapCollisionAabbLumpData(BspCollisionAabb_disk_t *data, int lumpSize)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapCollisionAabbLumpData);
  Assert(lumpSize >= 0, s_assertDisable_SwapCollisionAabbLumpData);

  count = lumpSize / sizeof(*data);

  /* swap all fields */
  for ( i = 0; i < count; i++ )
  {
    /* bounds */
    data[i].mins[0] = (float)BigLong((int)data[i].mins[0]);
    data[i].mins[1] = (float)BigLong((int)data[i].mins[1]);
    data[i].mins[2] = (float)BigLong((int)data[i].mins[2]);
    data[i].maxs[0] = (float)BigLong((int)data[i].maxs[0]);
    data[i].maxs[1] = (float)BigLong((int)data[i].maxs[1]);
    data[i].maxs[2] = (float)BigLong((int)data[i].maxs[2]);

    /* child and partition references */
    data[i].firstChild = BigShort(data[i].firstChild);
    data[i].childCount = BigShort(data[i].childCount);
    data[i].firstPartition = BigLong(data[i].firstPartition);
  }
  return count;
}

/*
================
SwapLightGridEntryLumpData

Byte-swap light grid entries.
NOTE: parameter order is swapped vs other swap functions (size first, data second).
================
*/
void SwapLightGridEntryLumpData(int size, BspLightGridEntry_t *data)
{
  int count;
  int i;

  Assert(data != NULL, s_assertDisable_SwapLightGridEntryLumpData);
  Assert(size >= 0, s_assertDisable_SwapLightGridEntryLumpData);

  count = size / sizeof(*data);

  /* swap int and short fields — byte data stays as-is */
  for ( i = 0; i < count; i++ )
  {
    data[i].colorData = BigLong(data[i].colorData);
    data[i].dirIndex = BigShort(data[i].dirIndex);
  }
}

/*
================
SwapVisData

Byte-swap visibility data — variable-length structure with per-cluster portal lists.
================
*/
int SwapVisData(BspVisHeader_t *vis, int size, int swapFlag)
{
  short numClusters, portalCount = 0;
  union { unsigned short *s; BspVisPortal_t *p; } cur;
  int i, j;

  Assert(vis != NULL, s_assertDisable_SwapVisData);
  Assert(size >= 0, s_assertDisable_SwapVisData);

  /* swap header */
  vis->numClusters = BigLong(vis->numClusters);
  if ( swapFlag )
    numClusters = BigShort(vis->numPortals);
  else
    numClusters = vis->numPortals;
  vis->numPortals = BigShort(vis->numPortals);

  /* walk per-cluster portal lists */
  cur.s = vis->portalData;
  for ( i = 0; i < numClusters; i++ )
  {
    /* read portal count before swapping */
    if ( swapFlag )
      portalCount = BigShort(*cur.s);
    else
      portalCount = *cur.s;
    *cur.s = BigShort(*cur.s);
    cur.s++;

    /* swap each packed portal entry */
    for ( j = 0; j < portalCount; j++ )
    {
      cur.p[j].index = BigShort(cur.p[j].index);
      cur.p[j].offset = BigLong(cur.p[j].offset);
    }
    cur.p += portalCount;
  }
  return portalCount;
}

/*
================
SwapLumpData

Dispatch byte-swapping for a BSP lump based on lump index.
================
*/
void SwapLumpData(int lumpIdx, void *data, int maxCount, int count, int elemSize, int swapFlag)
{
  int dataSize;

  Assert(count >= 0, s_assertDisable_SwapLumpData);
  Assert(elemSize > 0, s_assertDisable_SwapLumpData);

  if ( !count )
    return;
  if ( count > maxCount )
    Com_Error("%i > %i\n", count, maxCount);

  dataSize = elemSize * count;
  Assert(data != NULL, s_assertDisable_SwapLumpData);
  if ( dataSize <= 0 )
  {
    Assert(dataSize == 0, s_assertDisable_SwapLumpData);
    return;
  }

  /* dispatch to lump-specific swap handler */
  switch ( lumpIdx )
  {
    case LUMP_MATERIALS:           SwapMaterialLumpData(data, dataSize); break;
    case LUMP_LIGHTBYTES:          return;  /* byte data — no swap needed */
    case LUMP_LIGHTGRIDENTRIES:    SwapLightGridEntryLumpData(dataSize, data); break;
    case LUMP_LIGHTGRIDCOLORS:     return;  /* byte data — no swap needed */
    case LUMP_PLANES:              SwapBlock(dataSize, data); break;
    case LUMP_BRUSHSIDES:          SwapBlock(dataSize, data); break;
    case LUMP_BRUSHES:             SwapShortBlock(dataSize, data); break;
    case LUMP_TRIANGLES:           SwapTriangleLumpData(data, dataSize); break;
    case LUMP_DRAWVERTS:           SwapBlock(dataSize, data); break;
    case LUMP_DRAWINDICES:         SwapShortBlock(dataSize, data); break;
    case LUMP_CULLGROUPS:          SwapBlock(dataSize, data); break;
    case LUMP_CULLGROUPINDICES:    SwapBlock(dataSize, data); break;
    case LUMP_OBSOLETE_1:          SwapBlock(dataSize, data); break;
    case LUMP_OBSOLETE_2:          SwapShortBlock(dataSize, data); break;
    case LUMP_OBSOLETE_3:          SwapBlock(dataSize, data); break;
    case LUMP_OBSOLETE_4:          SwapDrawSurfLumpData(data, dataSize); break;
    case LUMP_OBSOLETE_5:          SwapBlock(dataSize, data); break;
    case LUMP_PORTALVERTS:         SwapBlock(dataSize, data); break;
    case LUMP_OCCLUDER:            SwapOccluderLumpData(data, dataSize); break;
    case LUMP_OCCLUDERPLANES:      SwapBlock(dataSize, data); break;
    case LUMP_OCCLUDEREDGES:       return;  /* byte data — no swap needed */
    case LUMP_OCCLUDERINDICES:     SwapShortBlock(dataSize, data); break;
    case LUMP_AABBTREES:           SwapBlock(dataSize, data); break;
    case LUMP_CELLS:               SwapBlock(dataSize, data); break;
    case LUMP_PORTALS:             SwapBlock(dataSize, data); break;
    case LUMP_NODES:               SwapBlock(dataSize, data); break;
    case LUMP_LEAFS:               SwapBlock(dataSize, data); break;
    case LUMP_LEAFBRUSHES:         SwapBlock(dataSize, data); break;
    case LUMP_LEAFSURFACES:        SwapBlock(dataSize, data); break;
    case LUMP_COLLISIONVERTS:      SwapBlock(dataSize, data); break;
    case LUMP_COLLISIONEDGEWALKABLE: return;  /* packed bits */
    case LUMP_COLLISIONTRIS:       SwapShortBlock(dataSize, data); break;
    case LUMP_COLLISIONBORDERS:    SwapBlock(dataSize, data); break;
    case LUMP_COLLISIONPARTITIONS: SwapCollisionPartitionLumpData(data, dataSize); break;
    case LUMP_COLLISIONAABBS:      SwapCollisionAabbLumpData(data, dataSize); break;
    case LUMP_MODELS:              SwapBlock(dataSize, data); break;
    case LUMP_VISIBILITY:
    {
      /* visibility header is just 2 ints */
      int *visHeader = data;
      visHeader[0] = BigLong(visHeader[0]);
      visHeader[1] = BigLong(visHeader[1]);
      break;
    }
    case LUMP_ENTITIES:            return;  /* string data — no swap needed */
    case LUMP_PATHCONNECTIONS:     SwapVisData(data, dataSize, swapFlag); break;
    default:
      Com_Error("SwapLumpData: Unknown lump type");
      break;
  }
}

/*
================
SwapBSPFile

Byte-swap all lumps in a loaded BSP file.
================
*/
int SwapBSPFile(int swapFlag)
{
  Swap_Init();

  /*           lump                      data                   maxCount                   count                   elemSize                                 */
  SwapLumpData(LUMP_MATERIALS,           bspMaterials,          MAX_MAP_MATERIALS,         numBSPMaterials,        sizeof(bspMaterials[0]),         swapFlag);
  SwapLumpData(LUMP_LIGHTBYTES,          bspLightmapData,       MAX_MAP_LIGHTBYTES,        numBSPLightBytes,       sizeof(bspLightmapData[0]),      swapFlag);
  SwapLumpData(LUMP_LIGHTGRIDENTRIES,    bspLightGridHash,      MAX_MAP_LIGHTGRID,         numBSPLightGridHash,    sizeof(bspLightGridHash[0]),     swapFlag);
  SwapLumpData(LUMP_LIGHTGRIDCOLORS,     bspLightGridColors,    MAX_MAP_LIGHTGRIDCOLORS,   numBSPLightGridColors,  sizeof(bspLightGridColors[0]),   swapFlag);
  SwapLumpData(LUMP_PLANES,              bspPlanes,             MAX_MAP_PLANES,            numBSPPlanes,           sizeof(bspPlanes[0]),            swapFlag);
  SwapLumpData(LUMP_BRUSHSIDES,          bspBrushSidesData,     MAX_MAP_BRUSHSIDES,        numBSPBrushSides,       sizeof(bspBrushSidesData[0]),    swapFlag);
  SwapLumpData(LUMP_BRUSHES,             bspBrushes,            MAX_MAP_BRUSHES,           numBSPBrushes,          sizeof(bspBrushes[0]),           swapFlag);
  SwapLumpData(LUMP_TRIANGLES,           bspTriangles,          MAX_MAP_TRISOUPS,          numBSPTriSoups,         sizeof(bspTriangles[0]),         swapFlag);
  SwapLumpData(LUMP_DRAWVERTS,           bspDrawVerts,          MAX_MAP_DRAW_VERTS,        numBSPDrawVerts,        sizeof(bspDrawVerts[0]),         swapFlag);
  SwapLumpData(LUMP_DRAWINDICES,         bspDrawIndexes,        MAX_MAP_DRAW_INDEXES,      numBSPDrawIndexes,      sizeof(bspDrawIndexes[0]),       swapFlag);
  SwapLumpData(LUMP_CULLGROUPS,          bspCullGroups,         MAX_MAP_CULLGROUPS,        numBSPCullGroups,       sizeof(bspCullGroups[0]),        swapFlag);
  SwapLumpData(LUMP_CULLGROUPINDICES,    bspCullGroupIndexes,   MAX_MAP_CULLGROUPINDEXES,  numBSPCullGroupIndexes, sizeof(bspCullGroupIndexes[0]),  swapFlag);
  SwapLumpData(LUMP_OBSOLETE_1,          bspShadowVerts,        MAX_MAP_SHADOW_VERTS,      numBSPShadowVerts,      sizeof(bspShadowVerts[0]),       swapFlag);
  SwapLumpData(LUMP_OBSOLETE_2,          bspShadowIndexes,      MAX_MAP_SHADOW_INDEXES,    numBSPShadowIndices,    sizeof(bspShadowIndexes[0]),     swapFlag);
  SwapLumpData(LUMP_OBSOLETE_3,          bspShadowClusters,     MAX_MAP_SHADOW_CLUSTERS,   numBSPShadowClusters,   sizeof(bspShadowClusters[0]),    swapFlag);
  SwapLumpData(LUMP_OBSOLETE_4,          bspShadowData,         MAX_MAP_SHADOW_AABBTREES,  numBSPShadowAabbTrees,  sizeof(bspShadowData[0]),        swapFlag);
  SwapLumpData(LUMP_OBSOLETE_5,          bspShadowSources,      MAX_MAP_SHADOW_SOURCES,    numBSPShadowSources,    sizeof(bspShadowSources[0]),     swapFlag);
  SwapLumpData(LUMP_PORTALVERTS,         bspPortalVerts,        MAX_MAP_PORTAL_VERTS,      numBSPPortalVerts,      sizeof(bspPortalVerts[0]),       swapFlag);
  SwapLumpData(LUMP_OCCLUDER,            bspOccluders,          MAX_MAP_OCCLUDERS,         numBSPOccluders,        sizeof(bspOccluders[0]),         swapFlag);
  SwapLumpData(LUMP_OCCLUDERPLANES,      bspOccluderPlanes,     MAX_MAP_OCCLUDER_PLANES,   numBSPOccluderPlanes,   sizeof(bspOccluderPlanes[0]),    swapFlag);
  SwapLumpData(LUMP_OCCLUDEREDGES,       bspOccluderEdges,      MAX_MAP_OCCLUDER_EDGES,    numBSPOccluderEdges,    sizeof(bspOccluderEdges[0]),     swapFlag);
  SwapLumpData(LUMP_OCCLUDERINDICES,     bspOccluderIndexes,    MAX_MAP_OCCLUDER_INDEXES,  numBSPOccluderIndexes,  sizeof(bspOccluderIndexes[0]),   swapFlag);
  SwapLumpData(LUMP_AABBTREES,           bspAabbTrees,          MAX_MAP_AABBTREES,         numBSPAabbTrees,        sizeof(bspAabbTrees[0]),         swapFlag);
  SwapLumpData(LUMP_CELLS,               bspCells,              MAX_MAP_CELLS,             numBSPCells,            sizeof(bspCells[0]),             swapFlag);
  SwapLumpData(LUMP_PORTALS,             bspPortals,            MAX_MAP_PORTALS,           numBSPPortals,          sizeof(bspPortals[0]),           swapFlag);
  SwapLumpData(LUMP_NODES,               bspNodes,              MAX_MAP_NODES,             numBSPNodes,            sizeof(bspNodes[0]),             swapFlag);
  SwapLumpData(LUMP_LEAFS,               bspLeafs,              MAX_MAP_LEAFS,             numBSPLeafs,            sizeof(bspLeafs[0]),             swapFlag);
  SwapLumpData(LUMP_LEAFBRUSHES,         bspLeafBrushes,        MAX_MAP_LEAFBRUSHES,       numBSPLeafBrushes,      sizeof(bspLeafBrushes[0]),       swapFlag);
  SwapLumpData(LUMP_LEAFSURFACES,        bspLeafSurfaces,       MAX_MAP_LEAFSURFACES,      numBSPLeafSurfaces,     sizeof(bspLeafSurfaces[0]),      swapFlag);
  SwapLumpData(LUMP_COLLISIONVERTS,      bspCollisionVerts,     MAX_MAP_COLLISION_VERTS,   numBSPCollisionVerts,   sizeof(bspCollisionVerts[0]),    swapFlag);
  SwapLumpData(LUMP_COLLISIONTRIS,       bspCollisionTriData,   MAX_MAP_COLLISION_TRIS,    numBSPCollisionTris,    sizeof(bspCollisionTriData[0]),  swapFlag);
  SwapLumpData(LUMP_COLLISIONBORDERS,    bspCollisionBorders,   MAX_MAP_COLLISION_BORDERS, numBSPCollisionBorders, sizeof(bspCollisionBorders[0]),  swapFlag);
  SwapLumpData(LUMP_COLLISIONPARTITIONS, bspCollisionParts,     MAX_MAP_COLLISION_PARTS,   numBSPCollisionParts,   sizeof(bspCollisionParts[0]),    swapFlag);
  SwapLumpData(LUMP_COLLISIONAABBS,      bspCollisionAABBs,     MAX_MAP_COLLISION_AABBS,   numBSPCollisionAABBs,   sizeof(bspCollisionAABBs[0]),    swapFlag);
  SwapLumpData(LUMP_MODELS,              bspModels,             MAX_MAP_MODELS,            numBSPModels,           sizeof(bspModels[0]),            swapFlag);
  SwapLumpData(LUMP_VISIBILITY,          bspVisBytes,           MAX_MAP_VISIBILITY,        numBSPVisBytes,         sizeof(bspVisBytes[0]),          swapFlag);
  SwapLumpData(LUMP_ENTITIES,            bspEntData,            MAX_MAP_ENTSTRING,         bspEntDataSize,         sizeof(bspEntData[0]),           swapFlag);
  SwapLumpData(LUMP_PATHCONNECTIONS,     bspPaths,              MAX_MAP_PATHS,             numBSPPaths,            sizeof(bspPaths[0]),             swapFlag);

  return Swap_InitByteSwap();
}

/*
================
SwapAndLoadLump

Swap and load a single BSP lump from file header
================
*/
void SwapAndLoadLump(BspFileHeader_t *header, int lumpIdx, int elemSize, int swapFlag)
{
  unsigned int offset = 12 + 8 * header->chunkCount;
  unsigned int i;

  for ( i = 0; i < header->chunkCount; ++i )
  {
    if ( header->chunks[i].type == lumpIdx )
    {
      const unsigned int lumpSize = header->chunks[i].length;
      if ( lumpSize % elemSize )
        Com_Error("LoadBspFile: odd lump size");
      SwapLumpData(lumpIdx, (byte *)header + offset, lumpSize / elemSize,
                   lumpSize / elemSize, elemSize, swapFlag);
      return;
    }
    offset += (header->chunks[i].length + 3) & ~3u;
  }
}

/*
================
CopyLump

Copy a BSP lump from file into destination buffer
================
*/
static const byte *GetBspLump(BspFileHeader_t *header, int lumpIdx, int elemSize, int maxSize, int *elementCount)
{
  unsigned int offset = 12 + 8 * header->chunkCount;
  unsigned int i;

  *elementCount = 0;
  for ( i = 0; i < header->chunkCount; ++i )
  {
    if ( header->chunks[i].type == lumpIdx )
    {
      const unsigned int length = header->chunks[i].length;
      if ( length % elemSize )
        Com_Error("LoadBspFile: lump %i has odd size", lumpIdx);
      if ( length > (unsigned int)maxSize )
        Com_Error("LoadBspFile: buffer for lump %i is too small (%i < %i)", lumpIdx, maxSize, length);
      *elementCount = length / elemSize;
      return (const byte *)header + offset;
    }
    offset += (header->chunks[i].length + 3) & ~3u;
  }
  return NULL;
}

int CopyLump(BspFileHeader_t *header, int lumpIdx, void *dest, int elemSize, int maxSize)
{
  int elementCount;
  const byte *source = GetBspLump(header, lumpIdx, elemSize, maxSize, &elementCount);

  if ( elementCount )
    memcpy(dest, source, elemSize * elementCount);
  return elementCount;
}

static void ConvertLegacyMaterialNames(void)
{
  int materialIndex;

  for ( materialIndex = 0; materialIndex < numBSPMaterials; ++materialIndex )
  {
    char *name = bspMaterials[materialIndex].material;
    int from;
    int to;

    if ( name[0] != '*' )
      continue;

    from = 0;
    to = 0;
    do
    {
      do
        name[++to] = name[++from];
      while ( isdigit((unsigned char)name[from]) );

      to += name[from] == 'n';
      from += 7;
      Assert(name[from] == '_' || name[from] == '\0', s_assertDisable_SwapLumpData);
      name[to] = name[from];
    }
    while ( name[to] );
  }
}

static int ConvertLegacyTriSoups(BspFileHeader_t *header, int sourceElemSize, BspTriSoup_t *dest, int maxSize)
{
  const byte *source;
  int triCount;
  int triIndex;

  source = GetBspLump(header, LUMP_TRIANGLES, sourceElemSize, maxSize, &triCount);
  if ( !triCount )
    return 0;

  for ( triIndex = 0; triIndex < triCount; ++triIndex )
  {
    const byte *legacyTri = source + sourceElemSize * triIndex;
    BspTriSoup_t *tri = &dest[triIndex];

    tri->materialIndex = *(const unsigned short *)(legacyTri + 0);
    tri->lightmapIndex = legacyTri[2];
    tri->reflectionProbeIndex = legacyTri[3];
    tri->primaryLightIndex = 0;
    tri->castsSunShadow = 1;
    if ( sourceElemSize == 20 )
    {
      tri->vertexLayerData = *(const int *)(legacyTri + 4);
      tri->firstVertex = *(const int *)(legacyTri + 8);
      tri->vertexCount = *(const unsigned short *)(legacyTri + 12);
      tri->indexCount = *(const unsigned short *)(legacyTri + 14);
      tri->firstIndex = *(const int *)(legacyTri + 16);
    }
    else
    {
      tri->vertexLayerData = 0;
      tri->firstVertex = *(const int *)(legacyTri + 4);
      tri->vertexCount = *(const unsigned short *)(legacyTri + 8);
      tri->indexCount = *(const unsigned short *)(legacyTri + 10);
      tri->firstIndex = *(const int *)(legacyTri + 12);
    }
  }
  return triCount;
}

static int ConvertLegacyCells(BspFileHeader_t *header, int sourceElemSize)
{
  const byte *source;
  int cellCount;
  int cellIndex;

  source = GetBspLump(header, LUMP_CELLS, sourceElemSize, sizeof(bspCells), &cellCount);
  if ( !cellCount )
    return 0;

  for ( cellIndex = 0; cellIndex < cellCount; ++cellIndex )
  {
    const byte *legacyCell = source + sourceElemSize * cellIndex;
    BspCell_t *cell = &bspCells[cellIndex];

    memcpy(cell->mins, legacyCell, sizeof(cell->mins));
    memcpy(cell->maxs, legacyCell + 12, sizeof(cell->maxs));
    if ( sourceElemSize == 44 )
    {
      cell->aabbTreeIndexPair[0] = *(const unsigned short *)(legacyCell + 24);
      cell->aabbTreeIndexPair[1] = *(const unsigned short *)(legacyCell + 26);
    }
    else
    {
      const int aabbTreeIndex = *(const int *)(legacyCell + 24);
      Assert(aabbTreeIndex == (unsigned short)aabbTreeIndex, s_assertDisable_SwapLumpData);
      cell->aabbTreeIndexPair[0] = (unsigned short)aabbTreeIndex;
      cell->aabbTreeIndexPair[1] = 0xFFFF;
    }
    cell->firstPortal = *(const int *)(legacyCell + 28);
    cell->portalCount = *(const int *)(legacyCell + 32);
    cell->firstCullGroup = *(const int *)(legacyCell + 36);
    cell->cullGroupCount = *(const int *)(legacyCell + 40);
    memset(cell->reflectionProbes, 0, sizeof(cell->reflectionProbes));
    cell->reflectionProbeCount = 1;
  }
  return cellCount;
}

static int ConvertLegacyLeafs(BspFileHeader_t *header)
{
  const byte *source;
  int leafCount;
  int leafIndex;

  source = GetBspLump(header, LUMP_LEAFS, 36, sizeof(bspLeafs), &leafCount);
  if ( !leafCount )
    return 0;

  for ( leafIndex = 0; leafIndex < leafCount; ++leafIndex )
  {
    const int *legacyLeaf = (const int *)(source + 36 * leafIndex);
    BspLeaf_disk_t *leaf = &bspLeafs[leafIndex];

    leaf->cluster = legacyLeaf[0];
    leaf->firstCollisionAABB = legacyLeaf[2];
    leaf->numCollisionAABBs = legacyLeaf[3];
    leaf->firstLeafBrush = legacyLeaf[4];
    leaf->numLeafBrushes = legacyLeaf[5];
    leaf->cellnum = legacyLeaf[6];
  }
  return leafCount;
}

static void ShiftLegacyPrimaryLights(unsigned char *lights, int lightCount)
{
  int lightIndex;
  int triIndex;

  for ( lightIndex = lightCount; lightIndex; --lightIndex )
  {
    memcpy(lights + 96 * lightIndex, lights + 96 * (lightIndex - 1), 96);
    ++lights[96 * lightIndex + 3];
  }
  memset(lights, 0, 128);
  Assert(lights[3] == 0, s_assertDisable_SwapLumpData);
  for ( triIndex = 0; triIndex < numBSPTriSoups; ++triIndex )
  {
    if ( bspTriangles[triIndex].primaryLightIndex == 255 )
      bspTriangles[triIndex].primaryLightIndex = 0;
    else
      ++bspTriangles[triIndex].primaryLightIndex;
  }
}

static int ConvertLegacyPrimaryLights(BspFileHeader_t *header, unsigned int version)
{
  unsigned char legacyLights[(MAX_BSP_PRIMARY_LIGHTS + 1) * 96];
  int lightCount;
  int lightIndex;

  lightCount = CopyLump(header, LUMP_PRIMARY_LIGHTS, legacyLights, 96, MAX_BSP_PRIMARY_LIGHTS * 96);
  if ( version <= 14 )
  {
    ShiftLegacyPrimaryLights(legacyLights, lightCount);
    ++lightCount;
  }

  for ( lightIndex = 0; lightIndex < lightCount; ++lightIndex )
  {
    const unsigned char *legacyLight = legacyLights + 96 * lightIndex;
    DiskPrimaryLight_t *light = &bspPrimaryLights[lightIndex];

    light->type = legacyLight[3];
    light->canUseShadowMap = 0;
    memcpy(light->color, legacyLight + 4, sizeof(light->color));
    memcpy(light->dir, legacyLight + 16, sizeof(light->dir));
    memcpy(light->origin, legacyLight + 28, sizeof(light->origin));
    light->radius = *(const float *)(legacyLight + 40);
    light->cosHalfFovOuter = *(const float *)(legacyLight + 44);
    light->cosHalfFovInner = *(const float *)(legacyLight + 48);
    light->exponent = *(const int *)(legacyLight + 52);
    light->rotationLimit = 1.0f;
    light->translationLimit = 0.0f;
    strcpy(light->defName, (const char *)(legacyLight + 56));
  }
  return lightCount;
}

static void LoadLegacyReflectionProbes(BspFileHeader_t *header)
{
  const byte *source;
  int probeCount;
  int probeIndex;

  source = GetBspLump(header, LUMP_REFLECTION_PROBES, 131076, 4 * 131076, &probeCount);
  if ( (unsigned int)probeCount > MAX_BSP_PRIMARY_LIGHTS )
    Com_Error("Map has too many reflection probes %d > %d", probeCount, MAX_BSP_PRIMARY_LIGHTS);
  for ( probeIndex = 0; probeIndex < probeCount; ++probeIndex )
  {
    const byte *legacyProbe = source + 131076 * probeIndex;
    memcpy(bspReflectionProbes[probeIndex].origin, legacyProbe, 12);
    memcpy(bspReflectionProbes[probeIndex].pixels, legacyProbe + 12, sizeof(bspReflectionProbes[probeIndex].pixels));
    bspReflectionProbes[probeIndex].colorCorrectionFilename[0] = '\0';
  }
  numBspReflectionProbes = probeCount;
}

/*
================
LoadExistingBSPReflectionProbes

CoD4 0x40A320 loads an existing BSP only far enough to preserve its
reflection-probe lump.  It deliberately does not populate the ordinary BSP
globals: those counters and arrays belong to the new compile which follows.
================
*/
void LoadExistingBSPReflectionProbes(char *filename)
{
  BspFileHeader_t *header;
  int fileSize;
  unsigned int version;

  fileSize = LoadFile(filename, (void **)&header);
  if ( fileSize < 0 )
  {
    numBspReflectionProbes = 0;
    return;
  }

  if ( header->magic != BSP_IDENT )
    Com_Error("%s is not a IBSP file", filename);

  version = header->version;
  if ( version < 6 )
    Com_Error("%s is version %i, but %i is the oldest supported version", filename, version, 6);
  if ( version > BSP_VERSION )
    Com_Error("%s is version %i, but the compiler uses older version %i", filename, version, BSP_VERSION);
  if ( header->chunkCount > BSP_CHUNK_LIMIT ||
       fileSize < 12 + 8 * (int)header->chunkCount )
    Com_Error("%s has an invalid BSP chunk directory", filename);

  if ( version > 11 )
  {
    numBspReflectionProbes = CopyLump(
      header,
      LUMP_REFLECTION_PROBES,
      bspReflectionProbes,
      sizeof(bspReflectionProbes[0]),
      sizeof(bspReflectionProbes));
  }
  else if ( version > 7 )
  {
    LoadLegacyReflectionProbes(header);
  }
  else
  {
    numBspReflectionProbes = 0;
  }

  free(header);
}

/*
================
LoadBSPFile

Load a BSP file from disk into memory
================
*/
static void LoadBSPFileWithLegacy(char *filename, int allowLegacy)
{
  BspFileHeader_t *header;
  int fileSize;
  unsigned int version;

  fileSize = LoadFile(filename, (void **)&header);
  if ( fileSize < 0 )
    Com_Error("Could not load file '%s'\n", filename);

  /* verify BSP magic */
  if ( header->magic != BSP_IDENT )
    Com_Error("%s is not a IBSP file", filename);

  version = header->version;
  if ( version < 6 )
    Com_Error("%s is version %i, but %i is the oldest supported version", filename, version, 6);
  if ( version > BSP_VERSION )
    Com_Error("%s is version %i, but the compiler uses older version %i", filename, version, BSP_VERSION);
  if ( version != BSP_VERSION && !allowLegacy )
    Com_Error("bsp is version %i, not %i - recompile to fix", version, BSP_VERSION);
  if ( header->chunkCount > BSP_CHUNK_LIMIT ||
       fileSize < 12 + 8 * (int)header->chunkCount )
    Com_Error("%s has an invalid BSP chunk directory", filename);

  memset(s_loadedLumpLengths, 0, sizeof(s_loadedLumpLengths));
  for ( unsigned int chunkIndex = 0; chunkIndex < header->chunkCount; ++chunkIndex )
  {
    const int lumpType = (int)header->chunks[chunkIndex].type;
    if ( lumpType >= 0 && lumpType < LUMP_COUNT )
      s_loadedLumpLengths[lumpType] = header->chunks[chunkIndex].length;
  }

  /*                                        lump                      dest                   elemSize                          maxBytes                    */
  numBSPMaterials = CopyLump(header, LUMP_MATERIALS, bspMaterials, sizeof(bspMaterials[0]), sizeof(bspMaterials));
  if ( version < 10 )
    ConvertLegacyMaterialNames();

  if ( version < 7 )
    numBSPLightBytes = 0;
  else
    numBSPLightBytes = CopyLump(header, LUMP_LIGHTBYTES, bspLightmapData, sizeof(bspLightmapData[0]), sizeof(bspLightmapData));

  if ( version >= 16 )
  {
    bspLightGridHeaderSize = CopyLump(header, LUMP_LIGHTGRIDHEADER, bspLightGridHeader, 1, sizeof(bspLightGridHeader));
    s_numBspLightGridRows = CopyLump(header, LUMP_LIGHTGRIDROWS, s_bspLightGridRows, 1, sizeof(s_bspLightGridRows));
    s_loadedLightGridRows = 1;
    numBSPLightGridHash = CopyLump(header, LUMP_LIGHTGRIDENTRIES, bspLightGridHash, sizeof(bspLightGridHash[0]), sizeof(bspLightGridHash));
    numBSPLightGridColors = CopyLump(header, LUMP_LIGHTGRIDCOLORS, bspLightGridColors, sizeof(bspLightGridColors[0]), sizeof(bspLightGridColors));
  }
  else
  {
    bspLightGridHeaderSize = 0;
    s_numBspLightGridRows = 0;
    s_loadedLightGridRows = 0;
    numBSPLightGridHash = 0;
    numBSPLightGridColors = 0;
  }
  numBSPPlanes           = CopyLump(header, LUMP_PLANES,              bspPlanes,             sizeof(bspPlanes[0]),             sizeof(bspPlanes));
  numBSPBrushSides       = CopyLump(header, LUMP_BRUSHSIDES,          bspBrushSidesData,     sizeof(bspBrushSidesData[0]),     sizeof(bspBrushSidesData));
  if ( numBSPBrushSides != CopyLump(header, LUMP_BRUSHSIDEEDGECOUNTS, bspBrushSideEdgeCounts, 1, sizeof(bspBrushSideEdgeCounts)) )
    Com_Error("Number of brush side edge counts does not equal the number of brush sides");
  numBSPBrushEdges       = CopyLump(header, LUMP_BRUSHEDGES,          bspBrushEdges,         1, sizeof(bspBrushEdges));
  numBSPBrushes          = CopyLump(header, LUMP_BRUSHES,             bspBrushes,            sizeof(bspBrushes[0]),            sizeof(bspBrushes));
  if ( version > 12 )
    numBSPTriSoups = CopyLump(header, LUMP_TRIANGLES, bspTriangles, sizeof(bspTriangles[0]), sizeof(bspTriangles));
  else if ( version > 8 )
    numBSPTriSoups = ConvertLegacyTriSoups(header, 20, bspTriangles, sizeof(bspTriangles));
  else
    numBSPTriSoups = ConvertLegacyTriSoups(header, 16, bspTriangles, sizeof(bspTriangles));
  numBSPUnlayeredTriSoups = CopyLump(header, LUMP_UNLAYERED_TRIANGLES, bspUnlayeredTriangles, sizeof(bspUnlayeredTriangles[0]), sizeof(bspUnlayeredTriangles));
  numBSPDrawVerts        = CopyLump(header, LUMP_DRAWVERTS,           bspDrawVerts,          sizeof(bspDrawVerts[0]),          sizeof(bspDrawVerts));
  numBSPUnlayeredDrawVerts = CopyLump(header, LUMP_UNLAYERED_DRAWVERTS, bspUnlayeredDrawVerts, sizeof(bspUnlayeredDrawVerts[0]), sizeof(bspUnlayeredDrawVerts));
  numBSPDrawIndexes      = CopyLump(header, LUMP_DRAWINDICES,         bspDrawIndexes,        sizeof(bspDrawIndexes[0]),        sizeof(bspDrawIndexes));
  numBSPUnlayeredDrawIndexes = CopyLump(header, LUMP_UNLAYERED_DRAWINDICES, bspUnlayeredDrawIndexes, sizeof(bspUnlayeredDrawIndexes[0]), sizeof(bspUnlayeredDrawIndexes));
  numBSPCullGroups       = CopyLump(header, LUMP_CULLGROUPS,          bspCullGroups,         sizeof(bspCullGroups[0]),         sizeof(bspCullGroups));
  {
    const int unlayeredCullGroupCount = CopyLump(
      header,
      LUMP_UNLAYERED_CULLGROUPS,
      bspUnlayeredCullGroups,
      sizeof(bspUnlayeredCullGroups[0]),
      sizeof(bspUnlayeredCullGroups));
    Assert(
      unlayeredCullGroupCount == numBSPCullGroups || unlayeredCullGroupCount == 0,
      s_assertDisable_SwapLumpData);
  }
  numBSPCullGroupIndexes = CopyLump(header, LUMP_CULLGROUPINDICES,    bspCullGroupIndexes,   sizeof(bspCullGroupIndexes[0]),   sizeof(bspCullGroupIndexes));
  numBSPShadowVerts = 0;
  numBSPShadowIndices = 0;
  numBSPShadowClusters = 0;
  numBSPShadowAabbTrees = 0;
  numBSPShadowSources = 0;
  numBSPPortalVerts      = CopyLump(header, LUMP_PORTALVERTS,         bspPortalVerts,        sizeof(bspPortalVerts[0]),        sizeof(bspPortalVerts));
  numBSPOccluders = 0;
  numBSPOccluderPlanes = 0;
  numBSPOccluderEdges = 0;
  numBSPOccluderIndexes = 0;
  numBSPAabbTrees        = CopyLump(header, LUMP_AABBTREES,           bspAabbTrees,          sizeof(bspAabbTrees[0]),          sizeof(bspAabbTrees));
  numBSPUnlayeredAabbTrees = CopyLump(header, LUMP_UNLAYERED_AABBTREES, bspUnlayeredAabbTrees, sizeof(bspUnlayeredAabbTrees[0]), sizeof(bspUnlayeredAabbTrees));
  if ( version > 21 )
    numBSPCells = CopyLump(header, LUMP_CELLS, bspCells, sizeof(bspCells[0]), sizeof(bspCells));
  else if ( version > 14 )
    numBSPCells = ConvertLegacyCells(header, 44);
  else
    numBSPCells = ConvertLegacyCells(header, 52);
  numBSPPortals          = CopyLump(header, LUMP_PORTALS,             bspPortals,            sizeof(bspPortals[0]),            sizeof(bspPortals));
  numBSPNodes            = CopyLump(header, LUMP_NODES,               bspNodes,              sizeof(bspNodes[0]),              sizeof(bspNodes));
  if ( version > 14 )
    numBSPLeafs = CopyLump(header, LUMP_LEAFS, bspLeafs, sizeof(bspLeafs[0]), sizeof(bspLeafs));
  else
    numBSPLeafs = ConvertLegacyLeafs(header);
  numBSPLeafBrushes      = CopyLump(header, LUMP_LEAFBRUSHES,         bspLeafBrushes,        sizeof(bspLeafBrushes[0]),        sizeof(bspLeafBrushes));
  numBSPLeafSurfaces     = CopyLump(header, LUMP_LEAFSURFACES,        bspLeafSurfaces,       sizeof(bspLeafSurfaces[0]),       sizeof(bspLeafSurfaces));
  numBSPCollisionVerts   = CopyLump(header, LUMP_COLLISIONVERTS,      bspCollisionVerts,     sizeof(bspCollisionVerts[0]),     sizeof(bspCollisionVerts));
  numBSPCollisionEdges   = CopyLump(header, LUMP_COLLISIONEDGEWALKABLE, bspCollisionEdgeWalkable, 1, sizeof(bspCollisionEdgeWalkable));
  numBSPCollisionTris    = CopyLump(header, LUMP_COLLISIONTRIS,       bspCollisionTriData,   sizeof(bspCollisionTriData[0]),   sizeof(bspCollisionTriData));
  numBSPCollisionBorders = CopyLump(header, LUMP_COLLISIONBORDERS,    bspCollisionBorders,   sizeof(bspCollisionBorders[0]),   sizeof(bspCollisionBorders));
  numBSPCollisionParts   = CopyLump(header, LUMP_COLLISIONPARTITIONS, bspCollisionParts,     sizeof(bspCollisionParts[0]),     sizeof(bspCollisionParts));
  numBSPCollisionAABBs   = CopyLump(header, LUMP_COLLISIONAABBS,      bspCollisionAABBs,     sizeof(bspCollisionAABBs[0]),     sizeof(bspCollisionAABBs));
  numBSPModels           = CopyLump(header, LUMP_MODELS,              bspModels,             sizeof(bspModels[0]),             sizeof(bspModels));
  numBSPVisBytes         = CopyLump(header, LUMP_VISIBILITY,          bspVisBytes,           sizeof(bspVisBytes[0]),           sizeof(bspVisBytes));
  bspEntDataSize         = CopyLump(header, LUMP_ENTITIES,            bspEntData,            sizeof(bspEntData[0]),            sizeof(bspEntData));
  numBSPPaths            = CopyLump(header, LUMP_PATHCONNECTIONS,     bspPaths,              sizeof(bspPaths[0]),              sizeof(bspPaths));
  if ( version > 16 )
    numBspPrimaryLights = CopyLump(header, LUMP_PRIMARY_LIGHTS, bspPrimaryLights, sizeof(bspPrimaryLights[0]), sizeof(bspPrimaryLights));
  else if ( version > 13 )
    numBspPrimaryLights = ConvertLegacyPrimaryLights(header, version);
  else
  {
    numBspPrimaryLights = 2;
    memset(bspPrimaryLights, 0, 2 * sizeof(bspPrimaryLights[0]));
    bspPrimaryLights[1].type = 1;
    bspPrimaryLights[1].dir[2] = 1.0f;
  }

  s_numBspLightRegions = CopyLump(header, LUMP_LIGHTREGIONS, bspLightRegions, sizeof(bspLightRegions[0]), sizeof(bspLightRegions));
  s_numBspLightRegionHulls = CopyLump(header, LUMP_LIGHTREGION_HULLS, s_bspLightRegionHulls, 76, sizeof(s_bspLightRegionHulls));
  s_numBspLightRegionAxes = CopyLump(header, LUMP_LIGHTREGION_AXES, s_bspLightRegionAxes, 20, sizeof(s_bspLightRegionAxes));
  s_loadedLightRegionChunks = 1;

  if ( version > 11 )
    numBspReflectionProbes = CopyLump(header, LUMP_REFLECTION_PROBES, bspReflectionProbes, sizeof(bspReflectionProbes[0]), sizeof(bspReflectionProbes));
  else if ( version > 7 )
    LoadLegacyReflectionProbes(header);
  else
    numBspReflectionProbes = 0;

  s_numBspVertexLayerData = CopyLump(header, LUMP_VERTEX_LAYER_DATA, s_bspVertexLayerData, 1, sizeof(s_bspVertexLayerData));
  s_loadedVertexLayerData = 1;

  free(header);
}

void LoadBSPFile(char *filename)
{
  LoadBSPFileWithLegacy(filename, 0);
}

/*
================
AddLump

Add a lump to the BSP file being written
================
*/
void AddLump(FILE *fp, BspFileHeader_t *header, int lumpIdx, void *data, int length)
{
  unsigned int i;
  const LumpType_t type = (LumpType_t)lumpIdx;
  (void)fp;

  Assert(header->chunkCount < BSP_CHUNK_LIMIT, s_assertDisable_SwapLumpData);
  if ( !length )
    return;

  for ( i = 0; i < header->chunkCount; ++i )
    Assert(header->chunks[i].type != type, s_assertDisable_SwapLumpData);

  header->chunks[header->chunkCount].type = type;
  header->chunks[header->chunkCount].length = length;
  s_bspChunkData[header->chunkCount] = data;
  ++header->chunkCount;
}

unsigned int GetCollisionEdgeWalkableSize(int collisionTriCount)
{
  return (((3 * collisionTriCount + 7) >> 3) + 3) & ~3u;
}

unsigned int GetLightGridRowCount(void)
{
  const unsigned short *bounds = (const unsigned short *)bspLightGridHeader;
  const unsigned int rowAxis = *(const unsigned int *)&bspLightGridHeader[12];
  Assert(rowAxis < 3, s_assertDisable_SwapLumpData);
  return (unsigned int)bounds[rowAxis + 3] + 1u - bounds[rowAxis];
}

unsigned int GetLightGridHeaderSize(void)
{
  return 20 + 2 * GetLightGridRowCount();
}

void ValidateBspTriangleData(
  const BspTriSoup_t *triSoups,
  unsigned int triSoupCount,
  unsigned int diskVertCount,
  const unsigned short *diskIndices,
  unsigned int diskIndexCount)
{
  unsigned int triSoupIter;

  for ( triSoupIter = 0; triSoupIter < triSoupCount; ++triSoupIter )
  {
    const BspTriSoup_t *triSoup = &triSoups[triSoupIter];
    unsigned int indexIter;

    Assert(triSoup->firstVertex + triSoup->vertexCount <= diskVertCount,
           s_assertDisable_SwapLumpData);
    Assert(triSoup->firstIndex + triSoup->indexCount <= diskIndexCount,
           s_assertDisable_SwapLumpData);
    for ( indexIter = 0; indexIter < triSoup->indexCount; ++indexIter )
    {
      Assert(diskIndices[triSoup->firstIndex + indexIter] < triSoup->vertexCount,
             s_assertDisable_SwapLumpData);
    }
  }
}

/*
================
WriteBSPFile

Write the BSP file to disk
================
*/
int WriteBSPFile(const char *filename, int swapFlag)
{
  FILE *fp;
  unsigned int fileAttrs;
  int result;
  BspFileHeader_t header;
  char text[MAX_OS_PATH];
  unsigned int chunkIndex;
  int lightRegionCount;
  int lightRegionHullCount;
  int lightRegionAxisCount;
  int lightGridRowCount;
  int vertexLayerDataCount;
  static const unsigned char padding[3] = { 0, 0, 0 };

  if ( numBSPNodes > SHRT_MAX )
    Com_Error(
      "numnodes is %d, exceeds limit of %d.\n"
      "Blocksize is a possible cause, try increasing it (0 will set it as large as possible).\n"
      "Also, you might try making geometry detail.\n",
      numBSPNodes, SHRT_MAX);

  ValidateBspTriangleData(
    bspTriangles,
    numBSPTriSoups,
    numBSPDrawVerts,
    bspDrawIndexes,
    numBSPDrawIndexes);
  ValidateBspTriangleData(
    bspUnlayeredTriangles,
    numBSPUnlayeredTriSoups,
    numBSPUnlayeredDrawVerts,
    bspUnlayeredDrawIndexes,
    numBSPUnlayeredDrawIndexes);

  (void)swapFlag;
  memset(&header, 0, sizeof(header));
  memset(s_bspChunkData, 0, sizeof(s_bspChunkData));

  /* open output file */
  fp = fopen(filename, "wb");
  if ( !fp )
  {
    fileAttrs = GetFileAttributesA(filename);
    if ( fileAttrs == INVALID_FILE_ATTRIBUTES || !(fileAttrs & FILE_ATTRIBUTE_READONLY) )
      Com_Error("could not open '%s' for writing\n", filename);

    /* prompt to replace read-only file */
    sprintf(text, "could not open '%s' for writing; replace read-only file?", filename);
    result = MessageBoxA(GetActiveWindow(), text, "OUTPUT FILE IS READ ONLY", MB_OKCANCEL | MB_ICONEXCLAMATION);
    if ( result == IDOK )
    {
      SetFileAttributesA(filename, fileAttrs & ~FILE_ATTRIBUTE_READONLY);
      fp = fopen(filename, "wb");
      if ( !fp )
        Com_Error("could not open '%s' for writing\n", filename);
    }
    else
    {
      return result;
    }
  }
  if ( fp )
  {
    header.magic = BSP_IDENT;
    header.version = BSP_VERSION;
    /* Light-region entries are cell-owned.  A map that stops before world
       processing can retain the sentinel primary light but has no cells; the
       native writer omits LUMP_LIGHTREGIONS in that case. */
    lightRegionCount = s_loadedLightRegionChunks
        ? s_numBspLightRegions
        : (numBSPCells ? numBspPrimaryLights : 0);
    lightRegionHullCount = s_loadedLightRegionChunks ? s_numBspLightRegionHulls : 0;
    lightRegionAxisCount = s_loadedLightRegionChunks ? s_numBspLightRegionAxes : 0;
    lightGridRowCount = s_loadedLightGridRows ? s_numBspLightGridRows : 0;
    vertexLayerDataCount = s_loadedVertexLayerData ? s_numBspVertexLayerData : 0;

    /* The original registers chunks in this order and omits zero lengths. */
    AddLump(fp, &header, LUMP_MATERIALS,           bspMaterials,          sizeof(bspMaterials[0])         * numBSPMaterials);
    AddLump(fp, &header, LUMP_LIGHTBYTES,          bspLightmapData,       sizeof(bspLightmapData[0])      * numBSPLightBytes);
    if ( bspLightGridHeaderSize == 22 )
    {
      bspLightGridHeader[16] = 1; /* colAxis */
      bspLightGridHeader[20] = 0xFF;
      bspLightGridHeader[21] = 0xFF; /* no row data */
    }
    bspLightGridHeaderSize = GetLightGridHeaderSize();
    AddLump(fp, &header, LUMP_LIGHTGRIDHEADER,     bspLightGridHeader,    bspLightGridHeaderSize);
    AddLump(fp, &header, LUMP_LIGHTGRIDROWS,       s_bspLightGridRows,    lightGridRowCount);
    AddLump(fp, &header, LUMP_LIGHTGRIDENTRIES,    bspLightGridHash,      sizeof(bspLightGridHash[0])     * numBSPLightGridHash);
    AddLump(fp, &header, LUMP_LIGHTGRIDCOLORS,     bspLightGridColors,    sizeof(bspLightGridColors[0])   * numBSPLightGridColors);
    AddLump(fp, &header, LUMP_PLANES,              bspPlanes,             sizeof(bspPlanes[0])            * numBSPPlanes);
    AddLump(fp, &header, LUMP_BRUSHSIDES,          bspBrushSidesData,     sizeof(bspBrushSidesData[0])    * numBSPBrushSides);
    AddLump(fp, &header, LUMP_BRUSHSIDEEDGECOUNTS, bspBrushSideEdgeCounts, numBSPBrushSides);
    AddLump(fp, &header, LUMP_BRUSHEDGES,          bspBrushEdges,          numBSPBrushEdges);
    AddLump(fp, &header, LUMP_BRUSHES,             bspBrushes,            sizeof(bspBrushes[0])           * numBSPBrushes);
    AddLump(fp, &header, LUMP_TRIANGLES,           bspTriangles,          sizeof(bspTriangles[0])         * numBSPTriSoups);
    AddLump(fp, &header, LUMP_DRAWVERTS,           bspDrawVerts,          sizeof(bspDrawVerts[0])         * numBSPDrawVerts);
    AddLump(fp, &header, LUMP_VERTEX_LAYER_DATA,   s_bspVertexLayerData,  vertexLayerDataCount);
    AddLump(fp, &header, LUMP_DRAWINDICES,         bspDrawIndexes,        sizeof(bspDrawIndexes[0])       * numBSPDrawIndexes);
    AddLump(fp, &header, LUMP_CULLGROUPS,          bspCullGroups,         sizeof(bspCullGroups[0])        * numBSPCullGroups);
    AddLump(fp, &header, LUMP_CULLGROUPINDICES,    bspCullGroupIndexes,   sizeof(bspCullGroupIndexes[0])  * numBSPCullGroupIndexes);
    AddLump(fp, &header, LUMP_PORTALVERTS,         bspPortalVerts,        sizeof(bspPortalVerts[0])       * numBSPPortalVerts);
    AddLump(fp, &header, LUMP_AABBTREES,           bspAabbTrees,          sizeof(bspAabbTrees[0])         * numBSPAabbTrees);
    AddLump(fp, &header, LUMP_CELLS,               bspCells,              sizeof(bspCells[0])             * numBSPCells);
    AddLump(fp, &header, LUMP_PORTALS,             bspPortals,            sizeof(bspPortals[0])           * numBSPPortals);
    AddLump(fp, &header, LUMP_NODES,               bspNodes,              sizeof(bspNodes[0])             * numBSPNodes);
    AddLump(fp, &header, LUMP_LEAFS,               bspLeafs,              sizeof(bspLeafs[0])             * numBSPLeafs);
    AddLump(fp, &header, LUMP_LEAFBRUSHES,         bspLeafBrushes,        sizeof(bspLeafBrushes[0])       * numBSPLeafBrushes);
    AddLump(fp, &header, LUMP_LEAFSURFACES,        bspLeafSurfaces,       sizeof(bspLeafSurfaces[0])      * numBSPLeafSurfaces);
    AddLump(fp, &header, LUMP_COLLISIONVERTS,      bspCollisionVerts,     sizeof(bspCollisionVerts[0])    * numBSPCollisionVerts);
    AddLump(fp, &header, LUMP_COLLISIONTRIS,       bspCollisionTriData,   sizeof(bspCollisionTriData[0]) * numBSPCollisionTris);
    AddLump(fp, &header, LUMP_COLLISIONEDGEWALKABLE, bspCollisionEdgeWalkable, GetCollisionEdgeWalkableSize(numBSPCollisionTris));
    AddLump(fp, &header, LUMP_COLLISIONBORDERS,    bspCollisionBorders,   sizeof(bspCollisionBorders[0])  * numBSPCollisionBorders);
    AddLump(fp, &header, LUMP_COLLISIONPARTITIONS, bspCollisionParts,     sizeof(bspCollisionParts[0])    * numBSPCollisionParts);
    AddLump(fp, &header, LUMP_COLLISIONAABBS,      bspCollisionAABBs,     sizeof(bspCollisionAABBs[0])    * numBSPCollisionAABBs);
    AddLump(fp, &header, LUMP_MODELS,              bspModels,             sizeof(bspModels[0])            * numBSPModels);
    AddLump(fp, &header, LUMP_VISIBILITY,          bspVisBytes,           sizeof(bspVisBytes[0])          * numBSPVisBytes);
    AddLump(fp, &header, LUMP_ENTITIES,            bspEntData,            sizeof(bspEntData[0])           * bspEntDataSize);
    AddLump(fp, &header, LUMP_PRIMARY_LIGHTS,      bspPrimaryLights,      sizeof(bspPrimaryLights[0])     * numBspPrimaryLights);
    AddLump(fp, &header, LUMP_LIGHTREGIONS,        bspLightRegions,       lightRegionCount);
    AddLump(fp, &header, LUMP_LIGHTREGION_HULLS,   s_bspLightRegionHulls, 76 * lightRegionHullCount);
    AddLump(fp, &header, LUMP_LIGHTREGION_AXES,    s_bspLightRegionAxes,  20 * lightRegionAxisCount);
    AddLump(fp, &header, LUMP_UNLAYERED_TRIANGLES, bspUnlayeredTriangles, sizeof(bspUnlayeredTriangles[0]) * numBSPUnlayeredTriSoups);
    AddLump(fp, &header, LUMP_UNLAYERED_DRAWVERTS, bspUnlayeredDrawVerts, sizeof(bspUnlayeredDrawVerts[0]) * numBSPUnlayeredDrawVerts);
    AddLump(fp, &header, LUMP_UNLAYERED_DRAWINDICES, bspUnlayeredDrawIndexes, sizeof(bspUnlayeredDrawIndexes[0]) * numBSPUnlayeredDrawIndexes);
    AddLump(fp, &header, LUMP_UNLAYERED_CULLGROUPS, bspUnlayeredCullGroups, sizeof(bspUnlayeredCullGroups[0]) * numBSPCullGroups);
    AddLump(fp, &header, LUMP_UNLAYERED_AABBTREES, bspUnlayeredAabbTrees, sizeof(bspUnlayeredAabbTrees[0]) * numBSPUnlayeredAabbTrees);
    if ( numBSPPaths > 0 )
      AddLump(fp, &header, LUMP_PATHCONNECTIONS,   bspPaths,              sizeof(bspPaths[0])             * numBSPPaths);
    AddLump(fp, &header, LUMP_REFLECTION_PROBES,   bspReflectionProbes,   sizeof(bspReflectionProbes[0])  * numBspReflectionProbes);

    SafeWrite(fp, &header, 12 + 8 * header.chunkCount);
    for ( chunkIndex = 0; chunkIndex < header.chunkCount; ++chunkIndex )
    {
      const unsigned int length = header.chunks[chunkIndex].length;
      const unsigned int padLength = (0u - length) & 3u;
      SafeWrite(fp, s_bspChunkData[chunkIndex], length);
      if ( padLength )
        SafeWrite(fp, (void *)padding, padLength);
    }
    return fclose(fp);
  }
  return 0;
}

/*
================
PrintBSPLumpSize

Print a single BSP lump's name, count, and size
================
*/
int PrintBSPLumpSize(const char *name, int count, int size, int maxSize, float pctMultiplier)
{
  char buf[16];
  float limitPct;

  buf[0] = '\0';
  if ( count >= 0 )
    _itoa(count, buf, 10);
  limitPct = maxSize ? (float)((double)size / (double)maxSize * 100.0) : 0.0f;
  if ( limitPct > 75.0f )
    printf("^1");
  printf("%6.2f%% ", limitPct);
  return printf("%6s %-18s %8i B %6.0f KB %5.1f%%\n",
    buf, name, size, size / 1024.0, pctMultiplier * size);
}

/*
================
ParseEPair

Parse a key-value epair from entity text.
Allocates a structure: [0]=next ptr, [1]=key string, [2]=value string.
================
*/
Epair_t *ParseEPair(char *str, char **parsePos)
{
  Epair_t *ep;
  char *val;
  char *p;

  ep = malloc(sizeof(*ep));
  ep->next = NULL;
  ep->key = NULL;
  ep->value = NULL;

  /* validate key */
  if ( strlen(str) >= MAX_KEY - 1 )
    Com_Error("ParseEpair: token too long");
  if ( str[strlen(str) - 2] == '\\' )
    Com_Error("ParseEpair: key '%s' ends with a '\\'\n", str);
  if ( strchr(str, '\n') || strchr(str, '\r') )
    Com_Error("ParseEpair: key '%s' contains a newline character\n", str);
  if ( strchr(str, '"') )
    Com_Error("ParseEpair: key '%s' contains a \" character, will cause parsing errors\n", str);
  ep->key = CopyString(str);

  /* validate value */
  val = COM_ParseExt(parsePos);
  if ( strlen(val) >= MAX_TOKEN_CHARS - 1 )
    Com_Error("ParseEpair: token too long");
  if ( val[strlen(val) - 2] == '\\' )
    Com_Error("ParseEpair: value '%s' ends with a '\\'\n", val);
  if ( strchr(val, '\n') || strchr(val, '\r') )
    Com_Error("ParseEpair: value '%s' contains a newline character (use of '\\' at end of value?)\n", val);
  if ( strchr(val, '"') )
    Com_Error("ParseEpair: value '%s' contains a \" character, will cause parsing errors\n", val);
  ep->value = CopyString(val);

  /* strip trailing whitespace from key */
  for ( p = (char *)&ep->key[strlen(ep->key) - 1]; p >= (char *)ep->key; *p-- = '\0' )
  {
    if ( *p > ' ' )
      break;
  }

  /* strip trailing whitespace from value */
  for ( p = (char *)&ep->value[strlen(ep->value) - 1]; p >= (char *)ep->value; *p-- = '\0' )
  {
    if ( *p > ' ' )
      break;
  }
  return ep;
}

/*
================
ParseEntity

Parse a single entity from BSP entity data
================
*/
int ParseEntity(char **parsePos)
{
  char *token;
  Epair_t *ep;
  Entity_t *ent;

  token = COM_Parse(parsePos);
  if ( !*token )
    return 0;
  if ( strcmp(token, "{") )
    Com_Error("ParseEntity: { not found");
  if ( num_entities == MAX_MAP_ENTITIES )
    Com_Error("MAX_MAP_ENTITIES exceeded");

  ent = &g_entities[num_entities];
  num_entities++;

  /* parse key-value pairs until closing brace */
  while ( 1 )
  {
    token = COM_Parse(parsePos);
    if ( !*token )
      Com_Error("ParseEntity: EOF without closing brace");
    if ( !strcmp(token, "}") )
      break;
    ep = ParseEPair(token, parsePos);
    ep->next = ent->epairs;
    ent->epairs = ep;
  }
  return 1;
}

/*
================
ParseEntities

Parse all entities from BSP entity data string
================
*/
int ParseEntities()
{
  char *parsePos;

  num_entities = 0;
  Com_BeginParseSession("LUMP_ENTITIES");
  parsePos = bspEntData;
  while ( ParseEntity(&parsePos) )
    ;
  Com_EndParseSession();
  return 0;
}

/*
================
UnparseEntities

Serialize all entities back to BSP entity data string
================
*/
int UnparseEntities()
{
  char *end;
  Entity_t *ent;
  Epair_t *ep;
  char *p;
  int i;
  
  /* contiguous buffer: line(2048) + key(1024) + value(1024) = 4096 bytes
     trim loops use cross-array pointer math — layout must not change */
  char buf[MAXPRINTMSG];
  #define lineBuf  buf[0]
  #define linePad  ((char *)&buf[1])
  #define keyBuf   (&buf[2048])
  #define valueBuf (&buf[3072])

  end = bspEntData;
  bspEntData[0] = '\0';

  for ( i = 0; i < num_entities; i++ )
  {
    ent = &g_entities[i];
    if ( !ent->epairs )
      continue;

    /* open entity block */
    strcat(end, "{\n");
    end += 2;

    /* write key-value pairs */
    for ( ep = ent->epairs; ep; ep = ep->next )
    {
      /* copy and trim key */
      strcpy(keyBuf, ep->key);
      for ( p = &linePad[strlen(keyBuf) + 2046]; p >= keyBuf; *p-- = '\0' )
      {
        if ( *p > ' ' )
          break;
      }

      /* copy and trim value */
      strcpy(valueBuf, ep->value);
      for ( p = &keyBuf[strlen(valueBuf) + 1023]; p >= valueBuf; *p-- = '\0' )
      {
        if ( *p > ' ' )
          break;
      }

      sprintf(&lineBuf, "\"%s\" \"%s\"\n", keyBuf, valueBuf);
      strcat(end, &lineBuf);
      end = &end[&linePad[strlen(&lineBuf)] - linePad];
    }

    /* close entity block */
    strcat(end, "}\n");
    end += 2;
    Assert(end == &bspEntData[strlen(bspEntData)], s_assertDisable_UnparseEntities);
    if ( end > bspEntData + MAX_MAP_ENTSTRING )
      Com_Error("Entity string buffer overflow.  This is caused by too many entities and/or too many key pairs."
                      "  Max may need to be increased.");
  }
  bspEntDataSize = (int)(end - bspEntData + 1);
  return num_entities;
}
#undef lineBuf
#undef linePad
#undef keyBuf
#undef valueBuf

/*
================
UnparseEntitiesWithOrigins

Serialize entities back to BSP data, injecting "origin" keys for brush models
================
*/
static int ParseEntityInto(char **parsePos, Entity_t *entities, int *entityCount)
{
  char *token;
  Epair_t *ep;
  Entity_t *entity;

  token = COM_Parse(parsePos);
  if ( !*token )
    return 0;
  if ( strcmp(token, "{") )
    Com_Error("ParseEntity: { not found");
  if ( *entityCount == MAX_MAP_ENTITIES )
    Com_Error("MAX_MAP_ENTITIES exceeded");

  entity = &entities[(*entityCount)++];
  while ( 1 )
  {
    token = COM_Parse(parsePos);
    if ( !*token )
      Com_Error("ParseEntity: EOF without closing brace");
    if ( !strcmp(token, "}") )
      return 1;
    ep = ParseEPair(token, parsePos);
    ep->next = entity->epairs;
    entity->epairs = ep;
  }
}

static void ParseEntitiesInto(char *text, Entity_t *entities, int *entityCount)
{
  char *parsePos = text;

  *entityCount = 0;
  Com_BeginParseSession("LUMP_ENTITIES");
  while ( ParseEntityInto(&parsePos, entities, entityCount) )
    ;
  Com_EndParseSession();
}

static Entity_t *FindMatchingOldMiscModel(Entity_t *oldEntities, int oldEntityCount, Entity_t *newEntity)
{
  const char *origin = ValueForKey(newEntity, "origin");
  const char *model = ValueForKey(newEntity, "model");
  int entityIndex;

  if ( !*origin || !*model )
    return NULL;
  for ( entityIndex = 0; entityIndex < oldEntityCount; ++entityIndex )
  {
    Entity_t *oldEntity = &oldEntities[entityIndex];
    if ( !strcmp(ValueForKey(oldEntity, "classname"), "misc_model")
      && !strcmp(ValueForKey(oldEntity, "origin"), origin)
      && !strcmp(ValueForKey(oldEntity, "model"), model) )
      return oldEntity;
  }
  return NULL;
}

int UnparseEntitiesWithOrigins()
{
  Entity_t *ent;
  Epair_t *ep, *newEp;
  char *p;
  char *modelLine, *afterLine;
  char *end, *writePos;
  int i;
  
  /* contiguous buffer: line(2304) + key(1024) + value(1024) = 4352 bytes
     trim loops use cross-array pointer math — layout must not change */
  char buf[4352];
  #define line   buf
  #define key2   (&buf[2304])
  #define value2 (&buf[3328])
  char origin[MAX_OS_PATH_SHORT];
  char originBuf[32];
  Entity_t oldEntities[MAX_MAP_ENTITIES];
  int oldEntityCount;

  ParseEntitiesInto(bspEntData, oldEntities, &oldEntityCount);
  for ( i = 0; i < num_entities; ++i )
  {
    Entity_t *oldEntity;
    const char *value;

    ent = &g_entities[i];
    for ( ep = ent->epairs; ep; ep = ep->next )
    {
      if ( !strcmp(ep->key, "classname") && !strcmp(ep->value, "misc_model") )
      {
        oldEntity = FindMatchingOldMiscModel(oldEntities, oldEntityCount, ent);
        if ( oldEntity )
        {
          value = ValueForKey(oldEntity, "gndLt");
          if ( *value )
            SetKeyValue(ent, "gndLt", value);
        }
      }
      if ( !strcmp(ep->key, "model") && ep->value[0] == '*' )
      {
        oldEntity = FindEntityByKeyValue(oldEntities, oldEntityCount, "model", ep->value);
        if ( oldEntity )
        {
          value = ValueForKey(oldEntity, "origin");
          if ( *value )
            SetKeyValue(ent, "origin", value);
        }
      }
    }
  }
  return UnparseEntities();

  /* pass 1: find brush model entities and inject "origin" key */
  for ( i = 0; i < num_entities; i++ )
  {
    ent = &g_entities[i];
    if ( !ent->epairs )
      continue;

    /* find the "model" "*N" epair */
    for ( ep = ent->epairs; ep; ep = ep->next )
    {
      if ( !strstr(ep->key, "model") || !strstr(ep->value, "*") )
        continue;

      /* trim key */
      strcpy(value2, ep->key);
      for ( p = &key2[strlen(value2) + 1023]; p >= value2; *p-- = '\0' )
      {
        if ( *p > ' ' )
          break;
      }

      /* trim value */
      strcpy(key2, ep->value);
      for ( p = &line[strlen(key2) + 2303]; p >= key2; *p-- = '\0' )
      {
        if ( *p > ' ' )
          break;
      }

      /* find this key-value pair in the entity string */
      sprintf(origin, "\"%s\" \"%s\"\n", value2, key2);
      modelLine = strstr(bspEntData, origin);
      if ( modelLine )
        break;
    }

    if ( ep )
    {
      /* extract origin value from the line after the model key */
      memset(originBuf, 0, sizeof(originBuf));
      afterLine = &modelLine[strlen(origin) + 1];
      if ( *afterLine != '"' )
      {
        int idx = 0;
        do
        {
          originBuf[idx] = *afterLine;
          ++afterLine;
          ++idx;
          line[idx + 2048] = *afterLine;
        }
        while ( *afterLine != '"' );
      }

      /* inject origin epair */
      newEp = malloc(sizeof(*newEp));
      newEp->next = ent->epairs->next;
      ent->epairs->next = newEp;
      newEp->key = CopyString("origin");
      newEp->value = CopyString(originBuf);
    }
  }

  /* pass 2: serialize all entities back to string */
  end = bspEntData;
  bspEntData[0] = '\0';

  for ( i = 0; i < num_entities; i++ )
  {
    ent = &g_entities[i];
    if ( !ent->epairs )
      continue;

    /* open entity block */
    strcat(end, "{\n");
    writePos = end + 2;

    /* write key-value pairs */
    for ( ep = ent->epairs; ep; ep = ep->next )
    {
      /* trim key */
      strcpy(value2, ep->key);
      for ( p = &key2[strlen(value2) + 1023]; p >= value2; *p-- = '\0' )
      {
        if ( *p > ' ' )
          break;
      }

      /* trim value */
      strcpy(key2, ep->value);
      for ( p = &line[strlen(key2) + 2303]; p >= key2; *p-- = '\0' )
      {
        if ( *p > ' ' )
          break;
      }

      sprintf(line, "\"%s\" \"%s\"\n", value2, key2);
      strcat(writePos, line);
      writePos += strlen(line);
    }

    /* close entity block */
    strcat(writePos, "}\n");
    end = writePos + 2;
    if ( end > bspEntData + MAX_MAP_ENTSTRING )
      Com_Error("Entity string buffer overflow.  This is caused by too many entities and/or too many key pairs."
                      "  Max may need to be increased.");
  }

  bspEntDataSize = (int)(end - bspEntData + 1);
  return num_entities;
}
#undef line
#undef key2
#undef value2

Epair_t *EPairList_SetKeyValue(Epair_t *epairs, const char *key, const char *value)
{
  Epair_t *ep;

  for ( ep = epairs; ep; ep = ep->next )
  {
    if ( !_stricmp(ep->key, key) )
    {
      free((char *)ep->value);
      ep->value = _strdup(value);
      return epairs;
    }
  }

  ep = malloc(sizeof(*ep));
  ep->next = epairs;
  ep->key = _strdup(key);
  ep->value = _strdup(value);
  return ep;
}

int EPairList_HasKey(const Epair_t *epairs, const char *key)
{
  const Epair_t *ep;

  for ( ep = epairs; ep; ep = ep->next )
  {
    if ( !_stricmp(ep->key, key) )
      return 1;
  }
  return 0;
}

const char *EPairList_ValueForKey(const Epair_t *epairs, const char *key)
{
  const Epair_t *ep;

  for ( ep = epairs; ep; ep = ep->next )
  {
    if ( !_stricmp(ep->key, key) )
      return ep->value;
  }
  return g_emptyString;
}

Entity_t *FindEntityByKeyValue(
    Entity_t *entities,
    int entityCount,
    const char *key,
    const char *value)
{
  Epair_t *epair;
  int entityIndex;

  Assert(entities, s_assertDisable_UnparseEntities);
  Assert(key, s_assertDisable_UnparseEntities);
  Assert(value, s_assertDisable_UnparseEntities);

  for ( entityIndex = 0; entityIndex < entityCount; ++entityIndex )
  {
    for ( epair = entities[entityIndex].epairs; epair; epair = epair->next )
    {
      if ( !strcmp(epair->key, key) && !strcmp(epair->value, value) )
        return &entities[entityIndex];
    }
  }
  return NULL;
}

void DeleteKey(Entity_t *entity, const char *key)
{
  Epair_t **link;
  Epair_t *epair;

  for ( link = &entity->epairs; *link; link = &epair->next )
  {
    epair = *link;
    if ( !_stricmp(epair->key, key) )
    {
      *link = epair->next;
      free((char *)epair->key);
      free((char *)epair->value);
      free(epair);
      return;
    }
  }
}

/*
================
SetKeyValue

Set a key-value pair on an entity, replacing existing or creating new.
CoD4 treats entity keys case-insensitively.
================
*/
void SetKeyValue(Entity_t *entity, const char *key, const char *value)
{
  entity->epairs = EPairList_SetKeyValue(entity->epairs, key, value);
}

/*
================
ValueForKey

Get the string value for a key on an entity
================
*/
const char *ValueForKey(const Entity_t *entity, const char *key)
{
  return EPairList_ValueForKey(entity->epairs, key);
}

/*
================
FloatForKey

Get a float value for a key on an entity.
================
*/
double FloatForKey(Entity_t *ent, const char *key)
{
  return atof(ValueForKey(ent, key));
}

int IntForKey(const Entity_t *entity, const char *key)
{
  return atoi(ValueForKey(entity, key));
}

/*
================
GetVectorForKey

Parse a vector from an entity's key-value pair.
================
*/
float *GetVectorForKey(Entity_t *entity, const char *key, float *out)
{
  const char *val;
  int n;

  val = ValueForKey(entity, key);
  out[0] = 0.0f;
  out[1] = 0.0f;
  out[2] = 0.0f;

  n = sscanf(val, "%f %f %f", &out[0], &out[1], &out[2]);

  /* warn if incomplete vector */
  if ( n == 1 || n == 2 )
    printf(
      "WARNING: entity %i: key '%s' with value '%s', when it should be a vector; treating as '%g %g %g'\n",
      (int)(entity - g_entities), key, val, out[0], out[1], out[2]);
  return out;
}

/*
================
LoadAndWriteBSPFile

Load a BSP file and immediately write it back out (for format conversion)
================
*/
int LoadAndWriteBSPFile(char *inputPath, const char *lpFileName, int swapFlag)
{
  LoadBSPFileWithLegacy(inputPath, 1);
  return WriteBSPFile(lpFileName, swapFlag);
}

/*
================
SetBSPFileExtensions

Set BSP/PRT/poly file extensions based on root string (e.g., "d" -> ".dbsp", ".dprt", ".dpoly")
================
*/
int SetBSPFileExtensions(const char *root)
{
  Assert(strlen(root) + 3 < 10, s_assertDisable_SetBSPFileExtensions);
  Assert(strlen(root) + 3 < 10, s_assertDisable_SetBSPFileExtensions);
  Assert(strlen(root) + 4 < 10, s_assertDisable_SetBSPFileExtensions);
  sprintf(Buffer, ".%sbsp", root);
  sprintf(g_prtFileExt, ".%sprt", root);
  return sprintf(g_polyFileExt, ".%spoly", root);
}

/*
================
GetBSPFileExtension / GetPRTFileExtension / GetPolyFileExtension

MUST NOT be inlined — MSVC /O2 inlines into callers with local Buffer[1024],
shadowing the global Buffer[16]. Generated from macro matching original pattern.
================
*/
#define DEFINE_GET_FILE_EXT(funcName, extVar, assertStr, assertLine) \
char *funcName() \
{ \
  Assert(extVar[0], s_assertDisable_##funcName); \
  return extVar; \
}

DEFINE_GET_FILE_EXT(GetBSPFileExtension,  Buffer,         "bspFileExtension[0]",  1623)
DEFINE_GET_FILE_EXT(GetPRTFileExtension,  g_prtFileExt,   "prtFileExtension[0]",  1630)
DEFINE_GET_FILE_EXT(GetPolyFileExtension, g_polyFileExt,  "polyFileExtension[0]", 1637)

#undef DEFINE_GET_FILE_EXT

/*
================
PrintBSPFileSizes

Print sizes of all BSP lumps
================
*/
int PrintBSPFileSizes(int totalSize)
{
  float pct;
  int size;

  if ( !num_entities )
    ParseEntities();
  pct = 100.0f / totalSize;

  printf("\n");
  printf(" Limit%%  Count Lump                    Bytes Kilobytes   BSP%%\n");
  printf("------------------------------------------------------------\n");

  PrintBSPLumpSize("models", numBSPModels, s_loadedLumpLengths[LUMP_MODELS], 196560, pct);
  PrintBSPLumpSize("materials", numBSPMaterials, s_loadedLumpLengths[LUMP_MATERIALS], 88128, pct);
  PrintBSPLumpSize("brushes", numBSPBrushes, s_loadedLumpLengths[LUMP_BRUSHES], 0x20000, pct);
  PrintBSPLumpSize("brushsides", numBSPBrushSides, s_loadedLumpLengths[LUMP_BRUSHSIDES], 0x500000, pct);
  PrintBSPLumpSize("planes", numBSPPlanes, s_loadedLumpLengths[LUMP_PLANES], 0x100000, pct);
  PrintBSPLumpSize("entdata", num_entities, s_loadedLumpLengths[LUMP_ENTITIES], 0x1000000, pct);
  printf("\n");

  PrintBSPLumpSize("nodes", numBSPNodes, s_loadedLumpLengths[LUMP_NODES], 0x120000, pct);
  PrintBSPLumpSize("leafs", numBSPLeafs, s_loadedLumpLengths[LUMP_LEAFS], 0xC0000, pct);
  PrintBSPLumpSize("leafbrushes", numBSPLeafBrushes, s_loadedLumpLengths[LUMP_LEAFBRUSHES], 0x100000, pct);
  PrintBSPLumpSize("leafsurfaces", numBSPLeafSurfaces, s_loadedLumpLengths[LUMP_LEAFSURFACES], 0x80000, pct);
  PrintBSPLumpSize("collisionverts", numBSPCollisionVerts, s_loadedLumpLengths[LUMP_COLLISIONVERTS], 0xC0000, pct);
  PrintBSPLumpSize("collisiontris", numBSPCollisionTris, s_loadedLumpLengths[LUMP_COLLISIONTRIS], 0xC0000, pct);
  size = s_loadedLumpLengths[LUMP_COLLISIONEDGEWALKABLE];
  PrintBSPLumpSize("collisionedgewalk", 3 * numBSPCollisionTris, size,
    GetCollisionEdgeWalkableSize(MAX_MAP_COLLISION_TRIS), pct);
  PrintBSPLumpSize("collisionborders", numBSPCollisionBorders, s_loadedLumpLengths[LUMP_COLLISIONBORDERS], 0xE0000, pct);
  PrintBSPLumpSize("collisionparts", numBSPCollisionParts, s_loadedLumpLengths[LUMP_COLLISIONPARTITIONS], 0x180000, pct);
  PrintBSPLumpSize("collisionaabbs", numBSPCollisionAABBs, s_loadedLumpLengths[LUMP_COLLISIONAABBS], 0x800000, pct);

  PrintBSPLumpSize("layered verts", numBSPDrawVerts, s_loadedLumpLengths[LUMP_DRAWVERTS], 0x2200000, pct);
  PrintBSPLumpSize("layered data", numBSPDrawVerts, s_loadedLumpLengths[LUMP_VERTEX_LAYER_DATA], 0x200000, pct);
  PrintBSPLumpSize("simple verts", numBSPUnlayeredDrawVerts, s_loadedLumpLengths[LUMP_UNLAYERED_DRAWVERTS], 0x2640000, pct);
  PrintBSPLumpSize("layered indexes", numBSPDrawIndexes, s_loadedLumpLengths[LUMP_DRAWINDICES], 0x200000, pct);
  PrintBSPLumpSize("simple indexes", numBSPUnlayeredDrawIndexes, s_loadedLumpLengths[LUMP_UNLAYERED_DRAWINDICES], 0x240000, pct);
  PrintBSPLumpSize("layered tri soups", numBSPTriSoups, s_loadedLumpLengths[LUMP_TRIANGLES], 0xC0000, pct);
  PrintBSPLumpSize("simple tri soups", numBSPUnlayeredTriSoups, s_loadedLumpLengths[LUMP_UNLAYERED_TRIANGLES], 0xC0000, pct);
  PrintBSPLumpSize("lightmaps", numBSPLightBytes / LIGHTMAP_BYTES, s_loadedLumpLengths[LUMP_LIGHTBYTES], 0x5D00000, pct);
  PrintBSPLumpSize("light grid header", 1, s_loadedLumpLengths[LUMP_LIGHTGRIDHEADER], 0, pct);
  PrintBSPLumpSize("light grid rows", GetLightGridRowCount(), s_loadedLumpLengths[LUMP_LIGHTGRIDROWS], 0x40000, pct);
  PrintBSPLumpSize("light grid points", numBSPLightGridHash, s_loadedLumpLengths[LUMP_LIGHTGRIDENTRIES], 0x400000, pct);
  PrintBSPLumpSize("light grid colors", numBSPLightGridColors, s_loadedLumpLengths[LUMP_LIGHTGRIDCOLORS], 0xA7FF58, pct);
  PrintBSPLumpSize("visibility", -1, s_loadedLumpLengths[LUMP_VISIBILITY], 0x200000, pct);
  PrintBSPLumpSize("portalverts", numBSPPortalVerts, s_loadedLumpLengths[LUMP_PORTALVERTS], 0x30000, pct);
  PrintBSPLumpSize("layered aabbtrees", numBSPAabbTrees, s_loadedLumpLengths[LUMP_AABBTREES], 0x60000, pct);
  PrintBSPLumpSize("simple aabbtrees", numBSPUnlayeredAabbTrees, s_loadedLumpLengths[LUMP_UNLAYERED_AABBTREES], 0x60000, pct);
  PrintBSPLumpSize("cells", numBSPCells, s_loadedLumpLengths[LUMP_CELLS], 0x1C000, pct);
  PrintBSPLumpSize("portals", numBSPPortals, s_loadedLumpLengths[LUMP_PORTALS], 0x8000, pct);
  PrintBSPLumpSize("cullgroups", numBSPCullGroups, s_loadedLumpLengths[LUMP_CULLGROUPS], 0x10000, pct);
  PrintBSPLumpSize("cullgroupindexes", numBSPCullGroupIndexes, s_loadedLumpLengths[LUMP_CULLGROUPINDICES], 0x4000, pct);
  size = s_loadedLumpLengths[LUMP_REFLECTION_PROBES];
  PrintBSPLumpSize("reflection_probes", size / sizeof(bspReflectionProbes[0]), size, 0x1FE43BC, pct);
  size = s_loadedLumpLengths[LUMP_PRIMARY_LIGHTS];
  PrintBSPLumpSize("primary lights", size / sizeof(bspPrimaryLights[0]), size, 32640, pct);
  size = s_loadedLumpLengths[LUMP_LIGHTREGIONS];
  PrintBSPLumpSize("light regions", size, size, 255, pct);
  size = s_loadedLumpLengths[LUMP_LIGHTREGION_HULLS];
  PrintBSPLumpSize("light region hulls", size / 76, size, 155040, pct);
  size = s_loadedLumpLengths[LUMP_LIGHTREGION_AXES];
  PrintBSPLumpSize("light region axes", size / 20, size, 326400, pct);
  printf("\n");
  size = s_loadedLumpLengths[LUMP_PATHCONNECTIONS];
  return PrintBSPLumpSize("paths", size != 0, size, 0x200000, pct);
}
