#ifndef KIWI_COD4_BSP_FORMAT_H
#define KIWI_COD4_BSP_FORMAT_H

#include <stdint.h>

/* Architecture-neutral contract shared by the CoD4 map and radiosity tools. */
#define BSP_IDENT          0x50534249u /* 'IBSP' little-endian */
#define BSP_IDENT_SWAP     0x49425350u /* 'IBSP' big-endian */
#define BSP_VERSION        22
#define BSP_CHUNK_LIMIT    100
#define BSP_LUMP_COUNT     55

typedef enum LumpType_e {
    LUMP_MATERIALS              = 0,
    LUMP_LIGHTBYTES             = 1,
    LUMP_LIGHTGRIDENTRIES       = 2,
    LUMP_LIGHTGRIDCOLORS        = 3,
    LUMP_PLANES                 = 4,
    LUMP_BRUSHSIDES             = 5,
    LUMP_BRUSHSIDEEDGECOUNTS    = 6,
    LUMP_BRUSHEDGES             = 7,
    LUMP_BRUSHES                = 8,
    LUMP_TRIANGLES              = 9,
    LUMP_DRAWVERTS              = 10,
    LUMP_DRAWINDICES            = 11,
    LUMP_CULLGROUPS             = 12,
    LUMP_CULLGROUPINDICES       = 13,
    LUMP_OBSOLETE_1             = 14,
    LUMP_OBSOLETE_2             = 15,
    LUMP_OBSOLETE_3             = 16,
    LUMP_OBSOLETE_4             = 17,
    LUMP_OBSOLETE_5             = 18,
    LUMP_PORTALVERTS            = 19,
    LUMP_OBSOLETE_6             = 20,
    LUMP_OBSOLETE_7             = 21,
    LUMP_OBSOLETE_8             = 22,
    LUMP_OBSOLETE_9             = 23,
    LUMP_AABBTREES              = 24,
    LUMP_CELLS                  = 25,
    LUMP_PORTALS                = 26,
    LUMP_NODES                  = 27,
    LUMP_LEAFS                  = 28,
    LUMP_LEAFBRUSHES            = 29,
    LUMP_LEAFSURFACES           = 30,
    LUMP_COLLISIONVERTS         = 31,
    LUMP_COLLISIONTRIS          = 32,
    LUMP_COLLISIONEDGEWALKABLE  = 33,
    LUMP_COLLISIONBORDERS       = 34,
    LUMP_COLLISIONPARTITIONS    = 35,
    LUMP_COLLISIONAABBS         = 36,
    LUMP_MODELS                 = 37,
    LUMP_VISIBILITY             = 38,
    LUMP_ENTITIES               = 39,
    LUMP_PATHCONNECTIONS        = 40,
    LUMP_REFLECTION_PROBES      = 41,
    LUMP_VERTEX_LAYER_DATA      = 42,
    LUMP_PRIMARY_LIGHTS         = 43,
    LUMP_LIGHTGRIDHEADER        = 44,
    LUMP_LIGHTGRIDROWS          = 45,
    LUMP_OBSOLETE_10            = 46,
    LUMP_UNLAYERED_TRIANGLES    = 47,
    LUMP_UNLAYERED_DRAWVERTS    = 48,
    LUMP_UNLAYERED_DRAWINDICES  = 49,
    LUMP_UNLAYERED_CULLGROUPS   = 50,
    LUMP_UNLAYERED_AABBTREES    = 51,
    LUMP_LIGHTREGIONS           = 52,
    LUMP_LIGHTREGION_HULLS      = 53,
    LUMP_LIGHTREGION_AXES       = 54,
    LUMP_COUNT                  = BSP_LUMP_COUNT,

    /* Compatibility names used by recovered legacy byte-swap paths. */
    LUMP_OCCLUDER               = LUMP_OBSOLETE_6,
    LUMP_OCCLUDERPLANES         = LUMP_OBSOLETE_7,
    LUMP_OCCLUDEREDGES          = LUMP_OBSOLETE_8,
    LUMP_OCCLUDERINDICES        = LUMP_OBSOLETE_9,
    LUMP_COLLISIONEDGES         = LUMP_COLLISIONEDGEWALKABLE
} LumpType_t;

typedef struct BspChunk_s {
    uint32_t type;
    uint32_t length;
} BspChunk_t;

/* The serialized header uses only 12 + 8 * chunkCount bytes.  The fixed
 * capacity here mirrors the native in-memory writer representation. */
typedef struct BspFileHeader_s {
    uint32_t magic;
    uint32_t version;
    uint32_t chunkCount;
    BspChunk_t chunks[BSP_CHUNK_LIMIT];
} BspFileHeader_t;

typedef char cod4_bsp_chunk_size_must_be_8[sizeof(BspChunk_t) == 8 ? 1 : -1];
typedef char cod4_bsp_header_size_must_be_0x32c[sizeof(BspFileHeader_t) == 0x32C ? 1 : -1];

#endif
