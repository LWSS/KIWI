/*
 * bspfile.c — BSP file loading, writing, and entity parsing.
 */

#include "cod2rad64.h"

/* core geometry */
BspLeaf_disk_t  bspLeafs[MAX_MAP_LEAFS];
BspModel_t      bspModels[MAX_MAP_MODELS];
BspNode_disk_t  bspNodes[MAX_MAP_NODES];
BspPlane_disk_t bspPlanes[MAX_MAP_PLANES];

/* surfaces */
BspDrawVert_t  bspDrawVerts[MAX_MAP_DRAW_VERTS];
BspTriSoup_t   bspTriangles[MAX_MAP_TRISOUPS];
unsigned char  bspTriSoupPrimaryLightIndices[MAX_MAP_TRISOUPS];
unsigned short bspDrawIndexes[MAX_MAP_DRAW_INDEXES];

/* CoD4 primary lights are retained as native 128-byte disk records.  Rad
 * consumes the sun record and the per-surface indexes while the writer keeps
 * the input chunk byte-exact. */
BspPrimaryLight_t bspPrimaryLights[255];
int               numBSPPrimaryLights;
int               g_sunPrimaryLightIndex;

/* brushes */
BspBrushSide_t bspBrushSidesData[MAX_MAP_BRUSHSIDES];
BspBrush_t     bspBrushes[MAX_MAP_BRUSHES];
int            bspLeafBrushes[MAX_MAP_LEAFBRUSHES];
int            bspLeafSurfaces[MAX_MAP_LEAFSURFACES];

/* visibility / portals */
BspPortal_t bspPortals[MAX_MAP_PORTALS];
float       bspPortalVerts[MAX_MAP_PORTAL_VERTS][3];
char        bspVisBytes[MAX_MAP_VISIBILITY];

/* collision */
BspAabbTreeEntry_t      bspAabbTrees[MAX_MAP_AABBTREES];
BspCollisionBorder_t    bspCollisionBorders[MAX_MAP_COLLISION_BORDERS];
BspCollisionEdge_t      bspCollisionEdgeData[MAX_MAP_COLLISION_EDGES];
BspCollisionPart_t      bspCollisionParts[MAX_MAP_COLLISION_PARTS];
BspCollisionTri_t       bspCollisionTriData[MAX_MAP_COLLISION_TRIS];
BspCollisionVert_t      bspCollisionVerts[MAX_MAP_COLLISION_VERTS];
BspCollisionAabb_disk_t bspCollisionAABBs[MAX_MAP_COLLISION_AABBS];

/* lighting */
/* bspLightmapData and g_lightmapOutput are the same buffer in the binary.
 * Alias bspLightmapData to g_lightmapOutput. */
extern unsigned char g_lightmapOutput[];
#define bspLightmapData ((char *)g_lightmapOutput)

/* bspLightGridHash / bspLightGridColors are the same buffers as CalculateLightGrid's
 * g_gridSampleArray / g_gridColorEntries in the binary.
 * Alias to match — otherwise CalculateLightGrid writes into one buffer and
 * WriteBSPFile reads from another (empty) buffer. */
extern unsigned char g_gridSampleArray[];
extern unsigned char g_gridColorEntries[];
extern int g_gridSampleArrayCount;
extern int g_gridColorCount;
extern unsigned char g_cod4LightGridHeader[];
extern int g_cod4LightGridHeaderSize;
extern unsigned char g_cod4LightGridRows[];
extern int g_cod4LightGridRowsSize;
extern unsigned char g_cod4LightGridEntries[];
extern int g_cod4LightGridEntryCount;
extern unsigned char g_cod4LightGridColors[];
extern int g_cod4LightGridColorCount;
/* Data buffers and counts are shared — grid computation overwrites loaded data */
#define bspLightGridHash       ((BspLightGridEntry_t *)g_gridSampleArray)
#define bspLightGridColors     ((BspLightGridColor_t *)g_gridColorEntries)
#define numBSPLightGridHash    g_gridSampleArrayCount
#define numBSPLightGridColors  g_gridColorCount

/* shadows */
BspShadowCluster_disk_t  bspShadowClusters[MAX_MAP_SHADOW_CLUSTERS];
BspShadowSource_t        bspShadowSources[MAX_MAP_SHADOW_SOURCES];
BspShadowVert_t          bspShadowVerts[MAX_MAP_SHADOW_VERTS];
DiskShadowAabb_t         bspShadowData[MAX_MAP_SHADOW_AABBTREES];
#define SHADOW_INDEX_SENTINEL 1
static short             _bspShadowIndexBuf[MAX_MAP_SHADOW_INDEXES + SHADOW_INDEX_SENTINEL];
short                   *bspShadowIndexes = &_bspShadowIndexBuf[SHADOW_INDEX_SENTINEL];

/* occluders */
BspOccluder_t     bspOccluders[MAX_MAP_OCCLUDERS];
short             bspOccluderIndexes[MAX_MAP_OCCLUDER_INDEXES];
int               bspOccluderPlanes[MAX_MAP_OCCLUDER_PLANES];
BspOccluderEdge_t bspOccluderEdges[MAX_MAP_OCCLUDER_EDGES];

/* cells / cullgroups */
BspCell_t      bspCells[MAX_MAP_CELLS];
BspCullGroup_t bspCullGroups[MAX_MAP_CULLGROUPS];
int            bspCullGroupIndexes[MAX_MAP_CULLGROUPINDEXES];

/* entities / materials / misc */
Entity_t    g_entities[MAX_MAP_ENTITIES];
Dmaterial_t bspMaterials[MAX_MAP_MATERIALS];
char        bspEntData[MAX_MAP_ENTSTRING];
char        bspPaths[MAX_MAP_PATHS];

int  bspEntDataSize;
int  numBSPAabbTrees;
int  numBSPBrushes;
int  numBSPBrushSides;
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
int  numBSPLeafBrushes;
int  numBSPLeafs;
int  numBSPLeafSurfaces;
int  numBSPLightBytes;
int  numBSPMaterials;
int  numBSPModels;
int  numBSPNodes;
int  numBSPOccluderEdges;
int  numBSPOccluderIndexes;
int  numBSPOccluderPlanes;
int  numBSPOccluders;
int  numBSPPaths;
int  numBSPPlanes;
int  numBSPPortals;
int  numBSPPortalVerts;
int  numBSPShadowAabbTrees;
int  numBSPShadowClusters;
int  numBSPShadowIndices;
int  numBSPShadowSources;
int  numBSPShadowVerts;
int  numBSPTriSoups;
int  numBSPVisBytes;
int  num_entities;

static char s_assertDisable_SwapShortsBlock;
static char s_assertDisable_SwapShortsBlock_len;
static char s_assertDisable_SwapDrawSurfaces;
static char s_assertDisable_SwapDrawSurfaces_align;
static char s_assertDisable_SwapMaterials;
static char s_assertDisable_SwapMaterials_align;
static char s_assertDisable_SwapNodes;
static char s_assertDisable_SwapNodes_len;
static char s_assertDisable_SwapLeafs;
static char s_assertDisable_SwapLeafs_len;
static char s_assertDisable_SwapLeafBrushes;
static char s_assertDisable_SwapLeafBrushes_len;
static char s_assertDisable_SwapDrawVerts;
static char s_assertDisable_SwapDrawVerts_len;
static char s_assertDisable_SwapBrushes;
static char s_assertDisable_SwapBrushes_len;
static char s_assertDisable_SwapCollisionAabbTree;
static char s_assertDisable_SwapCollisionAabbTree_len;
static char s_assertDisable_SetBspFileExtensions;
static char s_assertDisable_SetBspFileExtensions_prt;
static char s_assertDisable_SetBspFileExtensions_poly;
static char s_assertDisable_GetBspFileExtension;
static char s_assertDisable_GetPolyFileExtension;

/* BSP lump struct definitions — from cod2map where sizes match,
   cod2rad-specific where they differ */

typedef struct {
    int data[2];
} bspBrushSide_t; /* 8 bytes */

typedef struct {
    short numSides;
    short shaderNum;
} bspBrush_t; /* 4 bytes */

typedef struct {
    int data[8];
} bspCullGroup_t; /* 32 bytes */

typedef struct {
    float xyz[3];
} bspShadowVert_t; /* 12 bytes */

typedef struct {
    int firstVert;
    int count;
} bspShadowCluster_t; /* 8 bytes */

typedef struct {
    int data[10];
} bspShadowAabb_t; /* 40 bytes */

typedef struct {
    int data[2];
} bspShadowSource_t; /* 8 bytes */

typedef struct {
    int data[3];
} bspPortalVert_t; /* 12 bytes */

typedef struct {
    int data[5];
} bspOccluder_t; /* 20 bytes */

typedef struct {
    int data[3];
} bspAabbTreeEntry_t; /* 12 bytes */

typedef struct {
    int data[13];
} bspCell_t; /* 52 bytes */

typedef struct {
    int data[4];
} bspPortal_t; /* 16 bytes */

/* Leaf brush index — 4 bytes (matches cod2map) */
typedef struct {
    int brushIndex;
} bspLeafBrush_t; /* 4 bytes */

typedef struct {
    float data[4];
} bspCollisionVert_t; /* 16 bytes */

/* Collision edge — 56 bytes (matches cod2map) */
typedef struct {
    int data[14];
} bspCollisionEdge_t; /* 56 bytes */

typedef struct {
    int data[18];
} bspCollisionTri_t; /* 72 bytes */

typedef struct {
    float data[7];
} bspCollisionBorder_t; /* 28 bytes */

typedef struct {
    int data[3];
} bspCollisionPart_t; /* 12 bytes */

typedef struct {
    int data[8];
} bspCollisionAabb_t; /* 32 bytes */

typedef struct {
    int colorData;
    unsigned char pad[2];
    unsigned short dirIndex;
} bspLightGridEntry_t; /* 8 bytes */

typedef struct {
    float rgb[3];
    float intensity[3];
} bspLightGridColor_t; /* 24 bytes */

typedef struct {
    int data[2];
} bspPath_t; /* 8 bytes */

typedef BspChunk_t bspChunk_t;
typedef BspFileHeader_t bspFileHeader_t;

/* CoD4's serialized surface is 24 bytes.  The rest of cod4rad still uses the
 * compact 16-byte CoD2 lighting view, so the loader converts the fields it
 * consumes and the writer preserves the original surface chunk. */
typedef struct {
    unsigned short materialIndex;
    unsigned char lightmapIndex;
    unsigned char reflectionProbeIndex;
    unsigned char primaryLightIndex;
    unsigned char castsSunShadow;
    unsigned char unused[2];
    int vertexLayerData;
    int firstVertex;
    unsigned short vertexCount;
    unsigned short indexCount;
    int firstIndex;
} cod4BspTriSoup_t;

typedef struct {
    float mins[3];
    float maxs[3];
    unsigned short firstTriSoup;
    unsigned short firstTriSoupUnlayered;
    unsigned short numTriSoups;
    unsigned short numTriSoupsUnlayered;
    int firstAABB;
    int numAABBs;
    int firstBrush;
    int numBrushes;
} cod4BspModel_t;

typedef char cod4rad_bsp_chunk_size_must_be_8[sizeof(bspChunk_t) == 8 ? 1 : -1];
typedef char cod4rad_tri_soup_size_must_be_24[sizeof(cod4BspTriSoup_t) == 24 ? 1 : -1];
typedef char cod4rad_model_size_must_be_48[sizeof(cod4BspModel_t) == 48 ? 1 : -1];

/* The loaded image remains resident until exit so unchanged/unknown CoD4
 * chunks can be copied byte-for-byte into the output BSP. */
static unsigned char *s_loadedBspImage;
static int s_loadedBspImageSize;
static const void *s_bspChunkData[BSP_CHUNK_LIMIT];

/*
================
SwapLongsBlock_generic

Byte-swaps an array of 32-bit values in place.
================
*/
void SwapLongsBlock_generic(void *data, int size)
{
    int count = size / 4;
    int *p = (int *)data;
    int i;

    for (i = 0; i < count; i++)
        p[i] = BigLong(p[i]);
}

/*
================
SwapShortsBlock

Byte-swaps an array of 16-bit values in place.
================
*/
void SwapShortsBlock(void *data, int len)
{
    int count;
    short *p;
    int i;

    Assert(data, s_assertDisable_SwapShortsBlock);
    Assert(len >= 0, s_assertDisable_SwapShortsBlock_len);

    count = len / 2;
    p = (short *)data;
    for (i = 0; i < count; i++)
        p[i] = BigShort(p[i]);
}

#define MAX_KEY 32
#define MAX_TOKEN_CHARS 1024
/* MAX_MAP_ENTITIES / MAX_MAP_ENTSTRING now in cod2rad64.h */

/* BSP lump limits — from cod2map.h */
#define MAX_MAP_MATERIALS         1024
#define MAX_MAP_LIGHTBYTES        0x5D00000
#define MAX_MAP_LIGHTGRID         0x400000
#define MAX_MAP_LIGHTGRIDCOLORS   0xFFFF
#define MAX_MAP_PLANES            524288
#define MAX_MAP_BRUSHSIDES        655360
#define MAX_MAP_BRUSHES           0x8000
#define MAX_MAP_TRISOUPS          0x8000
#define MAX_MAP_DRAW_VERTS        0x80000
#define MAX_MAP_DRAW_INDEXES      3145728
#define MAX_MAP_CULLGROUPS        2048
#define MAX_MAP_CULLGROUPINDEXES  4096
#define MAX_MAP_SHADOW_VERTS      0x80000
#define MAX_MAP_SHADOW_INDEXES    3145728
#define MAX_MAP_SHADOW_CLUSTERS   256
#define MAX_MAP_SHADOW_AABBTREES  0x20000
#define MAX_MAP_SHADOW_SOURCES    256
#define MAX_MAP_PORTAL_VERTS      0x4000
#define MAX_MAP_OCCLUDERS         4096
#define MAX_MAP_OCCLUDER_PLANES   0x8000
#define MAX_MAP_OCCLUDER_EDGES    0x10000
#define MAX_MAP_OCCLUDER_INDEXES  49152
#define MAX_MAP_AABBTREES         0x100000
#define MAX_MAP_CELLS             1024
#define MAX_MAP_PORTALS           2048
#define MAX_MAP_NODES             0x8000
#define MAX_MAP_LEAFS             0x8000
#define MAX_MAP_LEAFBRUSHES       0x40000
#define MAX_MAP_LEAFSURFACES      0x20000
#define MAX_MAP_COLLISION_VERTS   0x40000
#define MAX_MAP_COLLISION_EDGES   0x80000
#define MAX_MAP_COLLISION_TRIS    0x40000
#define MAX_MAP_COLLISION_BORDERS 0x20000
#define MAX_MAP_COLLISION_PARTS   0x20000
#define MAX_MAP_COLLISION_AABBS   0x40000
#define MAX_MAP_MODELS            1023
#define MAX_MAP_VISIBILITY        0x200000
#define MAX_MAP_PATHS             0x80000

/* epair_t is the same as KeyValuePair_t from cod2rad64.h */
typedef KeyValuePair_t epair_t;

/* Entity_t and g_entities come from cod2rad64.h */
extern char bspEntData[];
extern int bspEntDataSize;
extern int num_entities;

/*
================
ParseEpair

Parses a key-value pair from entity data.
================
*/
epair_t *ParseEpair(const char *key, char **parsePos)
{
    epair_t *ep;
    char *val;
    char *p;

    ep = malloc(sizeof(epair_t));
    ep->next = NULL;
    ep->key = NULL;
    ep->value = NULL;

    if (strlen(key) >= MAX_KEY - 1)
        Error("ParseEpair: token too long");
    if (key[strlen(key) - 2] == '\\')
        Error("ParseEpair: key '%s' ends with a '\\'\n", key);
    if (strchr(key, '\n') || strchr(key, '\r'))
        Error("ParseEpair: key '%s' contains a newline character\n", key);
    if (strchr(key, '"'))
        Error("ParseEpair: key '%s' contains a \" character, will cause parsing errors\n", key);

    ep->key = CopyStringInternal(key);

    val = Com_ParseOnLine(parsePos);
    if (strlen(val) >= MAX_TOKEN_CHARS - 1)
        Error("ParseEpair: token too long");
    if (val[strlen(val) - 2] == '\\')
        Error("ParseEpair: value '%s' ends with a '\\'\n", val);
    if (strchr(val, '\n') || strchr(val, '\r'))
        Error("ParseEpair: value '%s' contains a newline character (use of '\\' at end of value?)\n", val);
    if (strchr(val, '"'))
        Error("ParseEpair: value '%s' contains a \" character, will cause parsing errors\n", val);

    ep->value = CopyStringInternal(val);

    /* strip trailing whitespace from key */
    for (p = ep->key + strlen(ep->key) - 1; p >= ep->key; p--)
    {
        if (*p > ' ')
            break;
        *p = 0;
    }

    /* strip trailing whitespace from value */
    for (p = ep->value + strlen(ep->value) - 1; p >= ep->value; p--)
    {
        if (*p > ' ')
            break;
        *p = 0;
    }

    return ep;
}

/*
================
ParseEntities

Parses all entities from bspEntData string.
Entity_t parsing is inlined — no separate ParseEntity_t function.
================
*/
int ParseEntities(void)
{
    char *parsePos;
    char *token;
    epair_t *ep;

    num_entities = 0;
    Com_BeginParseSession("LUMP_ENTITIES");
    parsePos = bspEntData;

    while (1)
    {
        token = Com_Parse(&parsePos);
        if (!*token)
            break;

        if (strcmp(token, "{"))
            Error("ParseEntity_t: { not found");

        if (num_entities == MAX_MAP_ENTITIES)
            Error("num_entities == MAX_MAP_ENTITIES");

        {
            int entIdx = num_entities++;

            while (1)
            {
                token = Com_Parse(&parsePos);
                if (!*token)
                    Error("ParseEntity_t: EOF without closing brace");
                if (!strcmp(token, "}"))
                    break;

                ep = ParseEpair(token, &parsePos);
                ep->next = g_entities[entIdx].keyValues;
                g_entities[entIdx].keyValues = ep;
            }
        }
    }

    return 0;
}

/*
================
SetKeyValue

Sets a key-value pair on an entity, replacing existing or adding new.
================
*/
void SetKeyValue(Entity_t *entity, const char *key, const char *value)
{
    epair_t *ep;

    for (ep = entity->keyValues; ep; ep = ep->next)
    {
        if (!strcmp(ep->key, key))
        {
            free(ep->value);
            ep->value = CopyStringInternal(value);
            return;
        }
    }

    ep = malloc(sizeof(epair_t));
    ep->key = CopyStringInternal(key);
    ep->value = CopyStringInternal(value);
    ep->next = entity->keyValues;
    entity->keyValues = ep;
}

/*
================
UnparseEntities

Serializes all entities back to BSP entity data string.
================
*/
static char s_assertDisable_UnparseEntities;

void UnparseEntities(void)
{
    char *end;
    Entity_t *ent;
    epair_t *ep;
    char *p;
    int i;
    char line[2048];
    char keyBuf[1024];
    char valueBuf[1024];

    end = bspEntData;
    bspEntData[0] = 0;

    for (i = 0; i < num_entities; i++)
    {
        ent = &g_entities[i];
        if (!ent->keyValues)
            continue;

        strcat(end, "{\n");
        end += 2;

        for (ep = ent->keyValues; ep; ep = ep->next)
        {
            strcpy(keyBuf, ep->key);
            for (p = keyBuf + strlen(keyBuf) - 1; p >= keyBuf; p--)
            {
                if (*p > ' ')
                    break;
                *p = 0;
            }

            strcpy(valueBuf, ep->value);
            for (p = valueBuf + strlen(valueBuf) - 1; p >= valueBuf; p--)
            {
                if (*p > ' ')
                    break;
                *p = 0;
            }

            sprintf(line, "\"%s\" \"%s\"\n", keyBuf, valueBuf);
            strcat(end, line);
            end += strlen(line);
        }

        strcat(end, "}\n");
        end += 2;

        if (end > bspEntData + MAX_MAP_ENTSTRING)
            Error("Entity_t string buffer overflow.  This is caused by too many entities and/or too many key pairs.  Max may need to be increased.");
    }

    bspEntDataSize = (int)(end - bspEntData + 1);
    Assert(end == bspEntData + strlen(bspEntData), s_assertDisable_UnparseEntities);
}

static size_t BspPaddedLength(unsigned int length)
{
    return ((size_t)length + 3u) & ~(size_t)3u;
}

/* Return a tagged CoD4 chunk by type.  Payloads begin after the compact
 * 12 + 8 * chunkCount header and are stored sequentially with 4-byte pads. */
static const unsigned char *GetBspLump(
    const bspFileHeader_t *header,
    int lumpIdx,
    int elemSize,
    int maxBytes,
    int *elementCount)
{
    size_t offset;
    unsigned int i;

    *elementCount = 0;
    offset = 12u + 8u * (size_t)header->chunkCount;

    for (i = 0; i < header->chunkCount; i++)
    {
        unsigned int length = header->chunks[i].length;

        if ((int)header->chunks[i].type == lumpIdx)
        {
            if (elemSize <= 0 || length % (unsigned int)elemSize)
                Error("LoadBspFile: lump %i has odd size", lumpIdx);
            if (length > (unsigned int)maxBytes)
                Error("LoadBspFile: buffer for lump %i is too small (%i < %u)", lumpIdx, maxBytes, length);
            if (offset + length > (size_t)s_loadedBspImageSize)
                Error("LoadBspFile: lump %i extends past end of file", lumpIdx);

            *elementCount = (int)(length / (unsigned int)elemSize);
            return s_loadedBspImage + offset;
        }

        offset += BspPaddedLength(length);
    }

    return NULL;
}

/*
================
CopyLump

Copies a tagged CoD4 BSP chunk into a destination buffer.
Returns number of elements copied.
================
*/
int CopyLump(bspFileHeader_t *header, int lumpIdx, void *dest, int elemSize, int maxBytes)
{
    int count;
    const unsigned char *source = GetBspLump(header, lumpIdx, elemSize, maxBytes, &count);

    if (count)
        memcpy(dest, source, (size_t)count * elemSize);
    return count;
}

static int LoadCod4TriSoups(bspFileHeader_t *header)
{
    int count;
    int i;
    const cod4BspTriSoup_t *source = (const cod4BspTriSoup_t *)GetBspLump(
        header,
        LUMP_TRIANGLES,
        sizeof(cod4BspTriSoup_t),
        MAX_MAP_TRISOUPS * sizeof(cod4BspTriSoup_t),
        &count);

    for (i = 0; i < count; i++)
    {
        bspTriangles[i].materialIndex = source[i].materialIndex;
        bspTriangles[i].lightmapIndex = source[i].lightmapIndex;
        bspTriangles[i].firstVertex = source[i].firstVertex;
        bspTriangles[i].vertexCount = source[i].vertexCount;
        bspTriangles[i].indexCount = source[i].indexCount;
        bspTriangles[i].firstIndex = source[i].firstIndex;
        bspTriSoupPrimaryLightIndices[i] = source[i].primaryLightIndex;
    }

    return count;
}

static int LoadCod4Models(bspFileHeader_t *header)
{
    int count;
    int i;
    const cod4BspModel_t *source = (const cod4BspModel_t *)GetBspLump(
        header,
        LUMP_MODELS,
        sizeof(cod4BspModel_t),
        MAX_MAP_MODELS * sizeof(cod4BspModel_t),
        &count);

    for (i = 0; i < count; i++)
    {
        memcpy(bspModels[i].mins, source[i].mins, sizeof(bspModels[i].mins));
        memcpy(bspModels[i].maxs, source[i].maxs, sizeof(bspModels[i].maxs));
        bspModels[i].firstTriSoup = source[i].firstTriSoup;
        bspModels[i].numTriSoups = source[i].numTriSoups;
        bspModels[i].firstAABB = source[i].firstAABB;
        bspModels[i].numAABBs = source[i].numAABBs;
        bspModels[i].firstBrush = source[i].firstBrush;
        bspModels[i].numBrushes = source[i].numBrushes;
    }

    return count;
}

/*
================
LoadBspFile

Loads a BSP file from disk into global arrays.
================
*/
void LoadBspFile(const char *filename)
{
    bspFileHeader_t *header;
    int fileSize;
    size_t endOffset;
    unsigned int i;
    unsigned int j;

    if (s_loadedBspImage)
    {
        free(s_loadedBspImage);
        s_loadedBspImage = NULL;
        s_loadedBspImageSize = 0;
    }

    fileSize = LoadFile(filename, (void **)&header);
    if (fileSize < 0)
        Error("Could not load file '%s'\n", filename);
    if (fileSize < 12)
        Error("%s is too small to be an IBSP file", filename);
    if (header->magic != BSP_IDENT)
        Error("%s is not a IBSP file", filename);
    if (header->version != BSP_VERSION)
        Error("%s is version %i, not %i", filename, header->version, BSP_VERSION);
    if (header->chunkCount > BSP_CHUNK_LIMIT)
        Error("%s has %u BSP chunks; maximum is %u", filename, header->chunkCount, BSP_CHUNK_LIMIT);

    endOffset = 12u + 8u * (size_t)header->chunkCount;
    if (endOffset > (size_t)fileSize)
        Error("%s has a truncated BSP chunk directory", filename);

    for (i = 0; i < header->chunkCount; i++)
    {
        for (j = 0; j < i; j++)
        {
            if (header->chunks[j].type == header->chunks[i].type)
                Error("%s contains duplicate BSP chunk type %u", filename, header->chunks[i].type);
        }

        endOffset += BspPaddedLength(header->chunks[i].length);
        if (endOffset > (size_t)fileSize)
            Error("%s has a BSP chunk extending past end of file", filename);
    }

    s_loadedBspImage = (unsigned char *)header;
    s_loadedBspImageSize = fileSize;

    numBSPMaterials       = CopyLump(header, LUMP_MATERIALS,           bspMaterials,          sizeof(bspMaterials[0]),          sizeof(bspMaterials));
    numBSPLightBytes      = CopyLump(header, LUMP_LIGHTBYTES,          bspLightmapData,       1,                                MAX_MAP_LIGHTBYTES);
    /* bspLightGridHash/Colors are aliased pointers to g_gridSampleArray/g_gridColorEntries
     * (see top of file); sizeof(bspLightGridHash) gives 8 (pointer size), not array bytes.
     * Use the explicit MAX sizes instead. */
    numBSPLightGridHash   = CopyLump(header, LUMP_LIGHTGRIDENTRIES,    bspLightGridHash,      sizeof(BspLightGridEntry_t),      MAX_MAP_LIGHTGRID * (unsigned long long)sizeof(BspLightGridEntry_t));
    numBSPLightGridColors = CopyLump(header, LUMP_LIGHTGRIDCOLORS,     bspLightGridColors,    sizeof(BspLightGridColor_t),      MAX_MAP_LIGHTGRIDCOLORS * (unsigned long long)sizeof(BspLightGridColor_t));
    numBSPPlanes          = CopyLump(header, LUMP_PLANES,              bspPlanes,             sizeof(bspPlanes[0]),             sizeof(bspPlanes));
    numBSPBrushSides      = CopyLump(header, LUMP_BRUSHSIDES,          bspBrushSidesData,     sizeof(bspBrushSidesData[0]),     sizeof(bspBrushSidesData));
    numBSPBrushes         = CopyLump(header, LUMP_BRUSHES,             bspBrushes,            sizeof(bspBrushes[0]),            sizeof(bspBrushes));
    numBSPTriSoups        = LoadCod4TriSoups(header);
    numBSPPrimaryLights   = CopyLump(header, LUMP_PRIMARY_LIGHTS,       bspPrimaryLights,      sizeof(bspPrimaryLights[0]),      sizeof(bspPrimaryLights));
    g_sunPrimaryLightIndex = numBSPPrimaryLights > 1 && bspPrimaryLights[1].type == 1 ? 1 : 0;
    numBSPDrawVerts       = CopyLump(header, LUMP_DRAWVERTS,           bspDrawVerts,          sizeof(bspDrawVerts[0]),          sizeof(bspDrawVerts));
    numBSPDrawIndexes     = CopyLump(header, LUMP_DRAWINDICES,         bspDrawIndexes,        sizeof(bspDrawIndexes[0]),        sizeof(bspDrawIndexes));
    numBSPCullGroups      = CopyLump(header, LUMP_CULLGROUPS,          bspCullGroups,         sizeof(bspCullGroups[0]),         sizeof(bspCullGroups));
    numBSPCullGroupIndexes= CopyLump(header, LUMP_CULLGROUPINDICES,    bspCullGroupIndexes,   sizeof(bspCullGroupIndexes[0]),   sizeof(bspCullGroupIndexes));
    numBSPShadowVerts     = 0;
    numBSPShadowIndices   = 0;
    numBSPShadowClusters  = 0;
    numBSPShadowAabbTrees = 0;
    numBSPShadowSources   = 0;
    numBSPPortalVerts     = CopyLump(header, LUMP_PORTALVERTS,         bspPortalVerts,        sizeof(bspPortalVerts[0]),        sizeof(bspPortalVerts));
    numBSPOccluders       = 0;
    numBSPOccluderPlanes  = 0;
    numBSPOccluderEdges   = 0;
    numBSPOccluderIndexes = 0;
    numBSPAabbTrees       = CopyLump(header, LUMP_AABBTREES,           bspAabbTrees,          sizeof(bspAabbTrees[0]),          sizeof(bspAabbTrees));
    numBSPCells           = 0; /* CoD4 cells are 112 bytes; preserve the chunk opaquely. */
    numBSPPortals         = CopyLump(header, LUMP_PORTALS,             bspPortals,            sizeof(bspPortals[0]),            sizeof(bspPortals));
    numBSPNodes           = CopyLump(header, LUMP_NODES,               bspNodes,              sizeof(bspNodes[0]),              sizeof(bspNodes));
    numBSPLeafs           = 0; /* CoD4 leafs are 24 bytes and unused by lighting. */
    numBSPLeafBrushes     = CopyLump(header, LUMP_LEAFBRUSHES,         bspLeafBrushes,        sizeof(bspLeafBrushes[0]),        sizeof(bspLeafBrushes));
    numBSPLeafSurfaces    = CopyLump(header, LUMP_LEAFSURFACES,        bspLeafSurfaces,       sizeof(bspLeafSurfaces[0]),       sizeof(bspLeafSurfaces));
    numBSPCollisionVerts  = 0; /* CoD4 collision data has compact disk layouts. */
    numBSPCollisionEdges  = 0;
    numBSPCollisionTris   = 0;
    numBSPCollisionBorders= CopyLump(header, LUMP_COLLISIONBORDERS,    bspCollisionBorders,   sizeof(bspCollisionBorders[0]),   sizeof(bspCollisionBorders));
    numBSPCollisionParts  = CopyLump(header, LUMP_COLLISIONPARTITIONS, bspCollisionParts,     sizeof(bspCollisionParts[0]),     sizeof(bspCollisionParts));
    numBSPCollisionAABBs  = CopyLump(header, LUMP_COLLISIONAABBS,      bspCollisionAABBs,     sizeof(bspCollisionAABBs[0]),     sizeof(bspCollisionAABBs));
    numBSPModels          = LoadCod4Models(header);
    numBSPVisBytes        = CopyLump(header, LUMP_VISIBILITY,          bspVisBytes,           1,                                sizeof(bspVisBytes));
    bspEntDataSize        = CopyLump(header, LUMP_ENTITIES,            bspEntData,            1,                                sizeof(bspEntData));
    numBSPPaths           = 0; /* Native CoD4 uses a newer opaque path format. */

    if (!bspEntDataSize)
        Error("%s has no entity chunk", filename);
    if (bspEntData[bspEntDataSize - 1] != '\0')
    {
        if (bspEntDataSize >= (int)sizeof(bspEntData))
            Error("%s has an unterminated entity chunk", filename);
        bspEntData[bspEntDataSize] = '\0';
    }
}

static char bspFileExtension[10];
static char prtFileExtension[10];
static char polyFileExtension[10];

/*
================
SetBspFileExtensions

Builds file extension strings from root (e.g. ".d3dbsp").
================
*/
void SetBspFileExtensions(const char *root)
{
    Assert(strlen(root) + 3 < 10, s_assertDisable_SetBspFileExtensions);
    Assert(strlen(root) + 3 < 10, s_assertDisable_SetBspFileExtensions_prt);
    Assert(strlen(root) + 4 < 10, s_assertDisable_SetBspFileExtensions_poly);

    sprintf(bspFileExtension, ".%sbsp", root);
    sprintf(prtFileExtension, ".%sprt", root);
    sprintf(polyFileExtension, ".%spoly", root);
}

/*
================
GetBspFileExtension

Returns the BSP file extension string.
================
*/
const char *GetBspFileExtension(void)
{
    Assert(bspFileExtension[0], s_assertDisable_GetBspFileExtension);
    return bspFileExtension;
}

/*
================
GetPolyFileExtension

Returns the poly file extension string.
================
*/
const char *GetPolyFileExtension(void)
{
    Assert(polyFileExtension[0], s_assertDisable_GetPolyFileExtension);
    return polyFileExtension;
}

/*
================
AddLump

Registers a non-empty tagged CoD4 chunk for the writer.
================
*/
void AddLump(FILE *fp, bspFileHeader_t *header, int lumpIdx, void *data, int length)
{
    unsigned int i;

    (void)fp;
    if (length <= 0)
        return;
    if (header->chunkCount >= BSP_CHUNK_LIMIT)
        Error("WriteBspFile: exceeded BSP chunk limit of %u", BSP_CHUNK_LIMIT);

    for (i = 0; i < header->chunkCount; i++)
    {
        if ((int)header->chunks[i].type == lumpIdx)
            Error("WriteBspFile: duplicate BSP chunk type %i", lumpIdx);
    }

    header->chunks[header->chunkCount].type = (unsigned int)lumpIdx;
    header->chunks[header->chunkCount].length = (unsigned int)length;
    s_bspChunkData[header->chunkCount] = data;
    header->chunkCount++;
}

static int FindLoadedChunkIndex(int lumpIdx)
{
    const bspFileHeader_t *header = (const bspFileHeader_t *)s_loadedBspImage;
    unsigned int i;

    for (i = 0; i < header->chunkCount; i++)
    {
        if ((int)header->chunks[i].type == lumpIdx)
            return (int)i;
    }
    return -1;
}

static const unsigned char *GetLoadedChunkData(int chunkIndex)
{
    const bspFileHeader_t *header = (const bspFileHeader_t *)s_loadedBspImage;
    size_t offset = 12u + 8u * (size_t)header->chunkCount;
    int i;

    for (i = 0; i < chunkIndex; i++)
        offset += BspPaddedLength(header->chunks[i].length);
    return s_loadedBspImage + offset;
}

static void SelectOutputChunk(
    int lumpIdx,
    const void *inputData,
    int inputLength,
    const void **outputData,
    int *outputLength)
{
    *outputData = inputData;
    *outputLength = inputLength;

    switch (lumpIdx)
    {
        case LUMP_LIGHTBYTES:
            *outputData = bspLightmapData;
            *outputLength = numBSPLightBytes;
            break;
        case LUMP_LIGHTGRIDENTRIES:
            /* A map can legitimately yield no usable grid points (for
             * example when its vis-cache is absent and none of its static
             * models load).  Do not turn a valid CoD4 grid into an absent
             * chunk in that case: retail retains usable grid data, and the
             * untouched input is a safer fallback than an empty table. */
            if (g_cod4LightGridEntryCount != 0)
            {
                *outputData = g_cod4LightGridEntries;
                *outputLength = g_cod4LightGridEntryCount * 4;
            }
            else if (numBSPLightGridHash != 0 || inputLength == 0)
            {
                *outputData = bspLightGridHash;
                *outputLength = numBSPLightGridHash * (int)sizeof(bspLightGridHash[0]);
            }
            break;
        case LUMP_LIGHTGRIDCOLORS:
            if (g_cod4LightGridColorCount != 0)
            {
                *outputData = g_cod4LightGridColors;
                *outputLength = g_cod4LightGridColorCount * 168;
            }
            else if (numBSPLightGridColors != 0 || inputLength == 0)
            {
                *outputData = bspLightGridColors;
                *outputLength = numBSPLightGridColors * (int)sizeof(bspLightGridColors[0]);
            }
            break;
        case LUMP_LIGHTGRIDHEADER:
            if (g_cod4LightGridHeaderSize != 0)
            {
                *outputData = g_cod4LightGridHeader;
                *outputLength = g_cod4LightGridHeaderSize;
            }
            break;
        case LUMP_LIGHTGRIDROWS:
            if (g_cod4LightGridRowsSize != 0)
            {
                *outputData = g_cod4LightGridRows;
                *outputLength = g_cod4LightGridRowsSize;
            }
            break;
        case LUMP_ENTITIES:
            *outputData = bspEntData;
            *outputLength = bspEntDataSize;
            break;
        default:
            break;
    }
}

/*
================
WriteBspFile

Writes the BSP file to disk from global arrays.
================
*/
#if 0 /* CoD2 fixed-directory writer retained only as provenance. */
int WriteBspFile_Cod2(const char *filename, int swapFlag)
{
    FILE *fp;
    bspFileHeader_t header;

    if (numBSPNodes > 0x7FFF)
        Error("numnodes is %d, exceeds limit of %d.\n", numBSPNodes, 0x7FFF);

    /* Pre-write byte-swap:
     * - PC (swapFlag=0): in-memory data is LE, file must be LE → no swap needed
     * - Xbox (swapFlag=1): in-memory data is LE, file must be BE → swap LE→BE
     * Orig's condition here is `if (!swapFlag)` which triggers the swap on PC;
     * that corrupts every passthrough lump because our SwapBSPFile
     * unconditionally byte-swaps longs. Matching orig's observed output
     * (which preserves pristine lumps bit-exact on PC) requires skipping
     * the swap for PC. */
    if (swapFlag)
        SwapBSPFile(swapFlag);

    memset(&header, 0, sizeof(header));

    fp = fopen(filename, "wb");
    if (!fp)
    {
        /* check if file is read-only */
        unsigned int attrs = GetFileAttributesA(filename);
        Error("could not open '%s' for writing\n", filename);

        if (attrs != 0xFFFFFFFF && (attrs & 1)) /* FILE_ATTRIBUTE_READONLY */
        {
            char msgText[1024];
            sprintf(msgText, "could not open '%s' for writing; replace?", filename);
            int mbResult = MessageBoxA(NULL, msgText, "OUTPUT FILE IS READ ONLY", 0x21); /* MB_OKCANCEL | MB_ICONEXCLAMATION */
            if (mbResult != 1) /* IDOK */
                return 0;
            SetFileAttributesA(filename, attrs & ~1); /* clear readonly */
            fp = fopen(filename, "wb");
            if (!fp)
                Error("could not open '%s' for writing\n", filename);
        }
    }

    header.magic = BigLong(BSP_IDENT);
    header.version = BigLong(BSP_VERSION);
    SafeWrite(fp, &header, sizeof(header));

    AddLump(fp, &header, LUMP_MATERIALS,           bspMaterials,          sizeof(bspMaterials[0]) * numBSPMaterials);
    AddLump(fp, &header, LUMP_LIGHTBYTES,          bspLightmapData,       numBSPLightBytes);
    AddLump(fp, &header, LUMP_LIGHTGRIDENTRIES,    bspLightGridHash,      sizeof(bspLightGridHash[0]) * numBSPLightGridHash);
    AddLump(fp, &header, LUMP_LIGHTGRIDCOLORS,     bspLightGridColors,    sizeof(bspLightGridColors[0]) * numBSPLightGridColors);
    AddLump(fp, &header, LUMP_PLANES,              bspPlanes,             sizeof(bspPlanes[0]) * numBSPPlanes);
    AddLump(fp, &header, LUMP_BRUSHSIDES,          bspBrushSidesData,     sizeof(bspBrushSidesData[0]) * numBSPBrushSides);
    AddLump(fp, &header, LUMP_BRUSHES,             bspBrushes,            sizeof(bspBrushes[0]) * numBSPBrushes);
    AddLump(fp, &header, LUMP_TRIANGLES,           bspTriangles,          sizeof(bspTriangles[0]) * numBSPTriSoups);
    AddLump(fp, &header, LUMP_DRAWVERTS,           bspDrawVerts,          sizeof(bspDrawVerts[0]) * numBSPDrawVerts);
    AddLump(fp, &header, LUMP_DRAWINDICES,         bspDrawIndexes,        sizeof(bspDrawIndexes[0]) * numBSPDrawIndexes);
    AddLump(fp, &header, LUMP_CULLGROUPS,          bspCullGroups,         sizeof(bspCullGroups[0]) * numBSPCullGroups);
    AddLump(fp, &header, LUMP_CULLGROUPINDICES,    bspCullGroupIndexes,   sizeof(bspCullGroupIndexes[0]) * numBSPCullGroupIndexes);
    AddLump(fp, &header, LUMP_OBSOLETE_1,          bspShadowVerts,        sizeof(bspShadowVerts[0]) * numBSPShadowVerts);
    AddLump(fp, &header, LUMP_OBSOLETE_2,          bspShadowIndexes,      sizeof(bspShadowIndexes[0]) * numBSPShadowIndices);
    AddLump(fp, &header, LUMP_OBSOLETE_3,          bspShadowClusters,     sizeof(bspShadowClusters[0]) * numBSPShadowClusters);
    AddLump(fp, &header, LUMP_OBSOLETE_4,          bspShadowData,         sizeof(bspShadowData[0]) * numBSPShadowAabbTrees);
    AddLump(fp, &header, LUMP_OBSOLETE_5,          bspShadowSources,      sizeof(bspShadowSources[0]) * numBSPShadowSources);
    AddLump(fp, &header, LUMP_PORTALVERTS,         bspPortalVerts,        sizeof(bspPortalVerts[0]) * numBSPPortalVerts);
    AddLump(fp, &header, LUMP_OCCLUDER,            bspOccluders,          sizeof(bspOccluders[0]) * numBSPOccluders);
    AddLump(fp, &header, LUMP_OCCLUDERPLANES,      bspOccluderPlanes,     sizeof(bspOccluderPlanes[0]) * numBSPOccluderPlanes);
    AddLump(fp, &header, LUMP_OCCLUDEREDGES,       bspOccluderEdges,      sizeof(bspOccluderEdges[0]) * numBSPOccluderEdges);
    AddLump(fp, &header, LUMP_OCCLUDERINDICES,     bspOccluderIndexes,    sizeof(bspOccluderIndexes[0]) * numBSPOccluderIndexes);
    AddLump(fp, &header, LUMP_AABBTREES,           bspAabbTrees,          sizeof(bspAabbTrees[0]) * numBSPAabbTrees);
    AddLump(fp, &header, LUMP_CELLS,               bspCells,              sizeof(bspCells[0]) * numBSPCells);
    AddLump(fp, &header, LUMP_PORTALS,             bspPortals,            sizeof(bspPortals[0]) * numBSPPortals);
    AddLump(fp, &header, LUMP_NODES,               bspNodes,              sizeof(bspNodes[0]) * numBSPNodes);
    AddLump(fp, &header, LUMP_LEAFS,               bspLeafs,              sizeof(bspLeafs[0]) * numBSPLeafs);
    AddLump(fp, &header, LUMP_LEAFBRUSHES,         bspLeafBrushes,        sizeof(bspLeafBrushes[0]) * numBSPLeafBrushes);
    AddLump(fp, &header, LUMP_LEAFSURFACES,        bspLeafSurfaces,       sizeof(bspLeafSurfaces[0]) * numBSPLeafSurfaces);
    AddLump(fp, &header, LUMP_COLLISIONVERTS,      bspCollisionVerts,     sizeof(bspCollisionVerts[0]) * numBSPCollisionVerts);
    AddLump(fp, &header, LUMP_COLLISIONEDGES,      bspCollisionEdgeData,  sizeof(bspCollisionEdgeData[0]) * numBSPCollisionEdges);
    AddLump(fp, &header, LUMP_COLLISIONTRIS,       bspCollisionTriData,   sizeof(bspCollisionTriData[0]) * numBSPCollisionTris);
    AddLump(fp, &header, LUMP_COLLISIONBORDERS,    bspCollisionBorders,   sizeof(bspCollisionBorders[0]) * numBSPCollisionBorders);
    AddLump(fp, &header, LUMP_COLLISIONPARTITIONS, bspCollisionParts,     sizeof(bspCollisionParts[0]) * numBSPCollisionParts);
    AddLump(fp, &header, LUMP_COLLISIONAABBS,      bspCollisionAABBs,     sizeof(bspCollisionAABBs[0]) * numBSPCollisionAABBs);
    AddLump(fp, &header, LUMP_MODELS,              bspModels,             sizeof(bspModels[0]) * numBSPModels);
    AddLump(fp, &header, LUMP_VISIBILITY,          bspVisBytes,           numBSPVisBytes);
    AddLump(fp, &header, LUMP_ENTITIES,            bspEntData,            bspEntDataSize);
    if (numBSPPaths > 0)
        AddLump(fp, &header, LUMP_PATHCONNECTIONS, bspPaths,              sizeof(bspPaths[0]) * numBSPPaths);

    /* if swapFlag was 0 (no pre-swap), byte-swap the header entries now */
    if (swapFlag)
    {
        int *headerInts = (int *)&header;
        int j;
        Swap_Init_BigEndian();
        for (j = 0; j < (int)(sizeof(header) / sizeof(int)); j++)
            headerInts[j] = LongSwap(headerInts[j]);
        Swap_Init();
    }

    fseek(fp, 0, SEEK_SET);
    SafeWrite(fp, &header, sizeof(header));
    return fclose(fp);
}
#endif

int WriteBspFile(const char *filename, int swapFlag)
{
    FILE *fp;
    bspFileHeader_t header;
    const bspFileHeader_t *inputHeader;
    unsigned char consumed[BSP_CHUNK_LIMIT];
    unsigned int chunkIndex;
    unsigned int orderIndex;
    static const unsigned char padding[3] = { 0, 0, 0 };
    static const int nativeChunkOrder[] = {
        LUMP_MATERIALS,
        LUMP_LIGHTBYTES,
        LUMP_LIGHTGRIDHEADER,
        LUMP_LIGHTGRIDROWS,
        LUMP_LIGHTGRIDENTRIES,
        LUMP_LIGHTGRIDCOLORS,
        LUMP_PLANES,
        LUMP_BRUSHSIDES,
        LUMP_BRUSHSIDEEDGECOUNTS,
        LUMP_BRUSHEDGES,
        LUMP_BRUSHES,
        LUMP_TRIANGLES,
        LUMP_DRAWVERTS,
        LUMP_VERTEX_LAYER_DATA,
        LUMP_DRAWINDICES,
        LUMP_CULLGROUPS,
        LUMP_CULLGROUPINDICES,
        LUMP_PORTALVERTS,
        LUMP_AABBTREES,
        LUMP_CELLS,
        LUMP_PORTALS,
        LUMP_NODES,
        LUMP_LEAFS,
        LUMP_LEAFBRUSHES,
        LUMP_LEAFSURFACES,
        LUMP_COLLISIONVERTS,
        LUMP_COLLISIONTRIS,
        LUMP_COLLISIONEDGEWALKABLE,
        LUMP_COLLISIONBORDERS,
        LUMP_COLLISIONPARTITIONS,
        LUMP_COLLISIONAABBS,
        LUMP_MODELS,
        LUMP_VISIBILITY,
        LUMP_ENTITIES,
        LUMP_PRIMARY_LIGHTS,
        LUMP_LIGHTREGIONS,
        LUMP_LIGHTREGION_HULLS,
        LUMP_LIGHTREGION_AXES,
        LUMP_UNLAYERED_TRIANGLES,
        LUMP_UNLAYERED_DRAWVERTS,
        LUMP_UNLAYERED_DRAWINDICES,
        LUMP_UNLAYERED_CULLGROUPS,
        LUMP_UNLAYERED_AABBTREES,
        LUMP_PATHCONNECTIONS,
        LUMP_REFLECTION_PROBES
    };

    if (numBSPNodes > 0x7FFF)
        Error("numnodes is %d, exceeds limit of %d.\n", numBSPNodes, 0x7FFF);
    if (!s_loadedBspImage)
        Error("WriteBspFile: no input BSP is loaded");

    /* CoD4 v22 is a little-endian tagged stream.  Platform conversion of
     * relevant payloads happens before this serialization boundary. */
    (void)swapFlag;
    inputHeader = (const bspFileHeader_t *)s_loadedBspImage;
    memset(&header, 0, sizeof(header));
    memset(s_bspChunkData, 0, sizeof(s_bspChunkData));
    memset(consumed, 0, sizeof(consumed));
    header.magic = BSP_IDENT;
    header.version = BSP_VERSION;

    /* Match retail chunk ordering and replace only generated lighting/entity
     * payloads.  All other CoD4 data remains byte-for-byte opaque. */
    for (orderIndex = 0; orderIndex < sizeof(nativeChunkOrder) / sizeof(nativeChunkOrder[0]); orderIndex++)
    {
        int lumpIdx = nativeChunkOrder[orderIndex];
        int inputIndex = FindLoadedChunkIndex(lumpIdx);
        const void *inputData = NULL;
        const void *outputData;
        int inputLength = 0;
        int outputLength;

        if (inputIndex >= 0)
        {
            consumed[inputIndex] = 1;
            inputData = GetLoadedChunkData(inputIndex);
            inputLength = (int)inputHeader->chunks[inputIndex].length;
        }

        SelectOutputChunk(lumpIdx, inputData, inputLength, &outputData, &outputLength);
        AddLump(NULL, &header, lumpIdx, (void *)outputData, outputLength);
    }

    /* Preserve obsolete or future chunks not present in the known order. */
    for (chunkIndex = 0; chunkIndex < inputHeader->chunkCount; chunkIndex++)
    {
        if (!consumed[chunkIndex])
        {
            AddLump(
                NULL,
                &header,
                (int)inputHeader->chunks[chunkIndex].type,
                (void *)GetLoadedChunkData((int)chunkIndex),
                (int)inputHeader->chunks[chunkIndex].length);
        }
    }

    fp = fopen(filename, "wb");
    if (!fp)
    {
        unsigned int attrs = GetFileAttributesA(filename);
        char msgText[1024];
        int mbResult;

        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_READONLY))
            Error("could not open '%s' for writing\n", filename);

        sprintf(msgText, "could not open '%s' for writing; replace read-only file?", filename);
        mbResult = MessageBoxA(NULL, msgText, "OUTPUT FILE IS READ ONLY", MB_OKCANCEL | MB_ICONEXCLAMATION);
        if (mbResult != IDOK)
            return mbResult;
        SetFileAttributesA(filename, attrs & ~FILE_ATTRIBUTE_READONLY);
        fp = fopen(filename, "wb");
        if (!fp)
            Error("could not open '%s' for writing\n", filename);
    }

    SafeWrite(fp, &header, 12 + 8 * header.chunkCount);
    for (chunkIndex = 0; chunkIndex < header.chunkCount; chunkIndex++)
    {
        unsigned int length = header.chunks[chunkIndex].length;
        unsigned int padLength = (0u - length) & 3u;

        SafeWrite(fp, (void *)s_bspChunkData[chunkIndex], length);
        if (padLength)
            SafeWrite(fp, (void *)padding, padLength);
    }

    return fclose(fp);
}

/*
================
SwapDrawSurfaces

Byte-swaps draw surface entries (40 bytes each).
Layout: L+0, S+4, byte+6, L+8, [L+12 conditional], L+16..36.
================
*/
void SwapDrawSurfaces(void *data, int len)
{
    int count, i;
    unsigned int *p;

    Assert(data, s_assertDisable_SwapDrawSurfaces);
    count = len / 40;
    Assert(len == count * 40, s_assertDisable_SwapDrawSurfaces_align);

    for (i = 0; i < count; i++)
    {
        p = (unsigned int *)((char *)data + i * 40 + 24);
        *(p - 6) = BigLong(*(p - 6));                  /* +0: long */
        *((short *)p - 10) = BigShort(*((short *)p - 10)); /* +4: short */
        if (!*((char *)p - 18))                         /* +6: byte flag */
        {
            *(p - 4) = BigLong(*(p - 4));              /* +8: long */
            *(p - 3) = BigLong(*(p - 3));              /* +12: long */
        }
        else
        {
            *(p - 4) = BigLong(*(p - 4));              /* +8: long */
        }
        *(p - 2) = BigLong(*(p - 2));                  /* +16: long */
        *(p - 1) = BigLong(*(p - 1));                  /* +20: long */
        *p       = BigLong(*p);                         /* +24: long */
        p[1]     = BigLong(p[1]);                       /* +28: long */
        p[2]     = BigLong(p[2]);                       /* +32: long */
        p[3]     = BigLong(p[3]);                       /* +36: long */
    }
}

/*
================
SwapMaterials

Byte-swaps material entries (72 bytes each).
Layout: char[64]+0, L+64, L+68.
================
*/
void SwapMaterials(void *data, int len)
{
    int count, i;
    unsigned int *p;

    Assert(data, s_assertDisable_SwapMaterials);
    count = len / 72;
    Assert(len == count * 72, s_assertDisable_SwapMaterials_align);

    for (i = 0; i < count; i++)
    {
        p = (unsigned int *)((char *)data + i * 72 + 68);
        *(p - 1) = BigLong(*(p - 1));                  /* +64: long */
        *p       = BigLong(*p);                         /* +68: long */
    }
}

/*
================
SwapNodes

Byte-swaps BSP node entries (16 bytes each).
Layout: S+0, S+2, L+4, S+8, S+10, L+12.
================
*/
void SwapNodes(void *data, int len)
{
    int count, i;
    short *p;

    Assert(data, s_assertDisable_SwapNodes);
    Assert(len >= 0, s_assertDisable_SwapNodes_len);
    count = len / 16;

    for (i = 0; i < count; i++)
    {
        p = (short *)((char *)data + i * 16 + 4);
        *(p - 2)       = BigShort(*(p - 2));           /* +0: short */
        *(p - 1)       = BigShort(*(p - 1));           /* +2: short */
        *(int *)p      = BigLong(*(int *)p);            /* +4: long */
        p[2]           = BigShort(p[2]);                /* +8: short */
        p[3]           = BigShort(p[3]);                /* +10: short */
        *((int *)p + 2)= BigLong(*((int *)p + 2));     /* +12: long */
    }
}

/*
 * BSP lump structs for byte-swapping.
 */
typedef struct BspLeaf { int a; short b; short c; int d; int e; short f; short pad; } BspLeaf;
typedef struct BspLeafBrush { short a; short pad; int b; int c; } BspLeafBrush;
typedef struct BspDrawVert { int a; int b; int c; int d; int e; int f; short g; short h; int j; } BspDrawVert;
typedef struct BspBrush { int a; unsigned char skip[2]; short b; } BspBrush;

/*
================
SwapLeafs
================
*/
void SwapLeafs(void *data, int len)
{
    int count, i;

    Assert(data, s_assertDisable_SwapLeafs);
    Assert(len >= 0, s_assertDisable_SwapLeafs_len);
    count = len / 20;

    for (i = 0; i < count; i++)
    {
        BspLeaf *leaf = (BspLeaf *)data + i;
        leaf->a = BigLong(leaf->a);
        leaf->b = BigShort(leaf->b);
        leaf->c = BigShort(leaf->c);
        leaf->d = BigLong(leaf->d);
        leaf->e = BigLong(leaf->e);
        leaf->f = BigShort(leaf->f);
    }
}

/*
================
SwapLeafBrushes

Byte-swaps leaf brush entries (12 bytes each).
Layout: S+0, L+4, L+8.
================
*/
void SwapLeafBrushes(void *data, int len)
{
    int count, i;

    Assert(data, s_assertDisable_SwapLeafBrushes);
    Assert(len >= 0, s_assertDisable_SwapLeafBrushes_len);
    count = len / 12;

    for (i = 0; i < count; i++)
    {
        BspLeafBrush *lb = (BspLeafBrush *)data + i;
        lb->a = BigShort(lb->a);
        lb->b = BigLong(lb->b);
        lb->c = BigLong(lb->c);
    }
}

/*
================
SwapDrawVerts

Byte-swaps draw vertex entries (32 bytes each).
Layout: L×6 at +0..+20, S+24, S+26, L+28.
================
*/
void SwapDrawVerts(void *data, int len)
{
    int count, i;

    Assert(data, s_assertDisable_SwapDrawVerts);
    Assert(len >= 0, s_assertDisable_SwapDrawVerts_len);
    count = len / 32;

    for (i = 0; i < count; i++)
    {
        BspDrawVert *dv = (BspDrawVert *)data + i;
        dv->a = BigLong(dv->a);
        dv->b = BigLong(dv->b);
        dv->c = BigLong(dv->c);
        dv->d = BigLong(dv->d);
        dv->e = BigLong(dv->e);
        dv->f = BigLong(dv->f);
        dv->g = BigShort(dv->g);
        dv->h = BigShort(dv->h);
        dv->j = BigLong(dv->j);
    }
}

/*
================
SwapBrushes

Byte-swaps brush entries (8 bytes each).
Layout: L+0, [2 unswapped bytes +4], S+6.
================
*/
void SwapBrushes(void *data, int len)
{
    int count, i;

    Assert(data, s_assertDisable_SwapBrushes);
    Assert(len >= 0, s_assertDisable_SwapBrushes_len);
    count = len / 8;

    for (i = 0; i < count; i++)
    {
        BspBrush *br = (BspBrush *)data + i;
        br->a = BigLong(br->a);
        br->b = BigShort(br->b);
    }
}

/*
================
SwapCollisionAabbTree

Byte-swaps collision AABB tree — variable-length nested structure.
Header: L+0, S+4(count). Per outer entry: S(subcount), then
per sub-entry: S + L (6 bytes each).
================
*/
void SwapCollisionAabbTree(void *data, int len, int preSwap)
{
    short *cursor;
    short outerCount, innerCount;
    int i, j;

    Assert(data, s_assertDisable_SwapCollisionAabbTree);
    Assert(len >= 0, s_assertDisable_SwapCollisionAabbTree_len);

    *(int *)data = BigLong(*(int *)data);                /* +0: long */

    cursor = (short *)data + 2;                          /* +4 */
    if (preSwap)
    {
        outerCount = BigShort(*cursor);
        *cursor = outerCount;
    }
    else
    {
        outerCount = *cursor;
        *cursor = BigShort(*cursor);
    }
    cursor++;                                            /* +6 */

    for (i = 0; i < outerCount; i++)
    {
        if (preSwap)
        {
            innerCount = BigShort(*cursor);
            *cursor = innerCount;
        }
        else
        {
            innerCount = *cursor;
            *cursor = BigShort(*cursor);
        }
        cursor++;

        for (j = 0; j < innerCount; j++)
        {
            *cursor = BigShort(*cursor);                /* short */
            *(int *)(cursor + 1) = BigLong(*(int *)(cursor + 1)); /* long */
            cursor += 3;                                 /* advance 6 bytes */
        }
    }
}

/*
================
SwapLumpData

Dispatch swap handler based on lump type.
================
*/
static char s_assertDisable_SwapLumpData;

void SwapLumpData(int lumpIdx, void *data, int maxCount, int count, int elemSize, int swapFlag)
{
    int dataSize;

    Assert(count >= 0, s_assertDisable_SwapLumpData);

    if (!count)
        return;
    if (count > maxCount)
        Error("%i > %i\n", count, maxCount);

    dataSize = elemSize * count;
    Assert(dataSize >= 0, s_assertDisable_SwapLumpData);
    if (dataSize <= 0)
        return;

    switch (lumpIdx)
    {
        case LUMP_MATERIALS:           SwapMaterials(data, dataSize); break;
        case LUMP_LIGHTBYTES:          return; /* byte data */
        case LUMP_LIGHTGRIDENTRIES:    SwapBrushes(data, dataSize); break;
        case LUMP_LIGHTGRIDCOLORS:     return; /* byte data */
        case LUMP_PLANES:              SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_BRUSHSIDES:          SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_BRUSHES:             SwapShortsBlock(data, dataSize); break;
        case LUMP_TRIANGLES:           SwapNodes(data, dataSize); break;
        case LUMP_DRAWVERTS:           SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_DRAWINDICES:         SwapShortsBlock(data, dataSize); break;
        case LUMP_CULLGROUPS:          SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_CULLGROUPINDICES:    SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_OBSOLETE_1:          SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_OBSOLETE_2:          SwapShortsBlock(data, dataSize); break;
        case LUMP_OBSOLETE_3:          SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_OBSOLETE_4:          SwapDrawSurfaces(data, dataSize); break;
        case LUMP_OBSOLETE_5:          SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_PORTALVERTS:         SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_OCCLUDER:            SwapLeafs(data, dataSize); break;
        case LUMP_OCCLUDERPLANES:      SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_OCCLUDEREDGES:       return; /* byte data */
        case LUMP_OCCLUDERINDICES:     SwapShortsBlock(data, dataSize); break;
        case LUMP_AABBTREES:           SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_CELLS:               SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_PORTALS:             SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_NODES:               SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_LEAFS:               SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_LEAFBRUSHES:         SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_LEAFSURFACES:        SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_COLLISIONVERTS:      SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_COLLISIONEDGES:      SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_COLLISIONTRIS:       SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_COLLISIONBORDERS:    SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_COLLISIONPARTITIONS: SwapLeafBrushes(data, dataSize); break;
        case LUMP_COLLISIONAABBS:      SwapDrawVerts(data, dataSize); break;
        case LUMP_MODELS:              SwapLongsBlock_generic(data, dataSize); break;
        case LUMP_VISIBILITY:
        {
            int *visHeader = (int *)data;
            visHeader[0] = BigLong(visHeader[0]);
            visHeader[1] = BigLong(visHeader[1]);
            break;
        }
        case LUMP_ENTITIES:            return; /* string data */
        case LUMP_PATHCONNECTIONS:     SwapCollisionAabbTree(data, dataSize, swapFlag); break;
        default:
            Com_Error(0, "SwapLumpData: Unknown lump type");
            break;
    }
}

/*
================
SwapBSPFile

Swap all BSP lumps for endian conversion.
================
*/
void SwapBSPFile(int swapFlag)
{
    Swap_Init();

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
    SwapLumpData(LUMP_COLLISIONEDGES,      bspCollisionEdgeData,  MAX_MAP_COLLISION_EDGES,   numBSPCollisionEdges,   sizeof(bspCollisionEdgeData[0]), swapFlag);
    SwapLumpData(LUMP_COLLISIONTRIS,       bspCollisionTriData,   MAX_MAP_COLLISION_TRIS,    numBSPCollisionTris,    sizeof(bspCollisionTriData[0]),  swapFlag);
    SwapLumpData(LUMP_COLLISIONBORDERS,    bspCollisionBorders,   MAX_MAP_COLLISION_BORDERS, numBSPCollisionBorders, sizeof(bspCollisionBorders[0]),  swapFlag);
    SwapLumpData(LUMP_COLLISIONPARTITIONS, bspCollisionParts,     MAX_MAP_COLLISION_PARTS,   numBSPCollisionParts,   sizeof(bspCollisionParts[0]),    swapFlag);
    SwapLumpData(LUMP_COLLISIONAABBS,      bspCollisionAABBs,     MAX_MAP_COLLISION_AABBS,   numBSPCollisionAABBs,   sizeof(bspCollisionAABBs[0]),    swapFlag);
    SwapLumpData(LUMP_MODELS,              bspModels,             MAX_MAP_MODELS,            numBSPModels,           sizeof(bspModels[0]),            swapFlag);
    SwapLumpData(LUMP_VISIBILITY,          bspVisBytes,           MAX_MAP_VISIBILITY,        numBSPVisBytes,         sizeof(bspVisBytes[0]),          swapFlag);
    SwapLumpData(LUMP_ENTITIES,            bspEntData,            MAX_MAP_ENTSTRING,         bspEntDataSize,         sizeof(bspEntData[0]),           swapFlag);
    SwapLumpData(LUMP_PATHCONNECTIONS,     bspPaths,              MAX_MAP_PATHS,             numBSPPaths,            sizeof(bspPaths[0]),             swapFlag);

    Swap_Init_BigEndian();
}
