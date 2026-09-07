/* -------------------------------------------------------------------------------

cod2rad64.h — Master header for CoD2 radiosity light compiler (x64)

Reverse engineered from cod2rad64_original.exe.
All struct offsets and function signatures verified against LST.

------------------------------------------------------------------------------- */



/* Marker */
#ifndef COD2RAD64_H
#define COD2RAD64_H



/* -------------------------------------------------------------------------------

Dependencies

------------------------------------------------------------------------------- */

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <float.h>
#include <ctype.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include "../common/cod4_bsp_format.h"
/* crt_math_patches.h kept but not wired — original_powf's _original_log2 lacks the 128-entry table and regresses diff */



/* -------------------------------------------------------------------------------

Base Types

------------------------------------------------------------------------------- */

typedef float vec_t;
typedef vec_t vec2_t[2];
typedef vec_t vec3_t[3];
typedef vec_t vec4_t[4];

typedef unsigned char byte;
typedef int qboolean;
#define qtrue  1
#define qfalse 0



/* -------------------------------------------------------------------------------

Constants

------------------------------------------------------------------------------- */

#define MAX_QPATH                       64
#define MAX_OSPATH                      256
#define MAX_VERTS_PER_POLY              10
#define MAX_THREADS                     32
#define MAX_FILE_HANDLES                64
#define MAX_IWD_FILES                   1024
#define MAX_WORLD_COORD                 131072.0f
#define MIN_WORLD_COORD                 -131072.0f
#define POLY_SLOT_STRIDE                112
#define NUM_NATIVE_CMDLINE_SWITCHES     22
#define NUM_EXTENSION_CMDLINE_SWITCHES  10
#define NUM_CMDLINE_SWITCHES            (NUM_NATIVE_CMDLINE_SWITCHES + NUM_EXTENSION_CMDLINE_SWITCHES)
#define LIGHTMAP_DATA_SIZE              0x800000
#define LIGHTMAP_NONE                   0x1F

/* Surface flags — from cod2map.h */
#define SURF_NODAMAGE       0x00000001
#define SURF_SLICK           0x00000002
#define SURF_SKY             0x00000004
#define SURF_LADDER          0x00000008
#define SURF_NOIMPACT        0x00000010
#define SURF_NOMARKS         0x00000020
#define SURF_NODRAW          0x00000080
#define SURF_NOLIGHTMAP      0x00000400
#define SURF_POINTLIGHT      0x00000800
#define SURF_NOSTEPS         0x00002000
#define SURF_NONSOLID        0x00004000
#define SURF_NOCASTSHADOW    0x00040000

/* Surface type values — stored in MaterialDef.surfaceType byte, masked with 0x70 */
#define SURFTYPE_MASK        0x70
#define SURFTYPE_PLANAR      0x00    /* brush face */
#define SURFTYPE_PATCH       0x10    /* patch/curve */
#define SURFTYPE_TRISOUP     0x20    /* triangle soup (terrain) */

/* Dvar type constants — from binary assert strings */
#define DVAR_TYPE_BOOL       0
#define DVAR_TYPE_FLOAT      1
#define DVAR_TYPE_VEC2       2
#define DVAR_TYPE_VEC3       3
#define DVAR_TYPE_VEC4       4
#define DVAR_TYPE_INT        5
#define DVAR_TYPE_ENUM       6
#define DVAR_TYPE_STRING     7
#define DVAR_TYPE_COLOR      8

/* Dvar flag constants — from binary assert strings */
#define DVAR_SERVERINFO      0x0004
#define DVAR_SYSTEMINFO      0x0008
#define DVAR_INIT            0x0010
#define DVAR_LATCH           0x0020
#define DVAR_ROM             0x0040
#define DVAR_CHEAT           0x0080
#define DVAR_CHANGEABLE_RESET 0x1000
#define DVAR_SYS_EXTERNAL    0x4000
#define DVAR_SYS_MASK        0x7000

/* Content flags — from cod2map.h */
#define CONTENTS_SOLID          0x00000001
#define CONTENTS_FOLIAGE        0x00000002
#define CONTENTS_NONCOLLIDING   0x00000004
#define CONTENTS_SKY            0x00000800
#define CONTENTS_CLIPSHOT       0x00002000
#define CONTENTS_TELEPORTER     0x00040000
#define CONTENTS_NODROP         0x80000000
#define LMAP_STRIDE                     512   /* row stride shared by primary and secondary lightmaps */

/* transfer pool constants */
#define TRANSFERS_PER_BLOCK             15
#define TRANSFER_BLOCK_SIZE             0xF8
#define TRANSFER_POOL_COUNT             0x2B
#define TRANSFER_POOL_SIZE              0x29B0
#define TRANSFER_POOL_LINK_OFFSET       0x29A8

/* dobj constants */
#define DOBJ_MAX_PARTS                  128
#define DOBJ_MAX_SUBMODELS              8
#define DOBJ_MAX_PART_BITS              4

/* collision constants */
#define COLL_BOUNDS_MARGIN              0.125f
#define COLL_NORMAL_EPSILON             0.001f

/* memory tree constants */
#define MEMORY_NODE_BITS                16
#define MEMORY_NODE_COUNT               0x10000
#define MT_NODE_SIZE                    8

/* DEG2RAD as float — original binary uses 0x3C8EFA35 */
#define DEG2RAD                         0.017453292f

/* NaN check macros */
#define IS_NAN(x)           (((*(unsigned int *)&(x)) & 0x7F800000) == 0x7F800000)
#define IS_NAN_FLOAT(x)     ((*(int *)&(x) & 0x7F800000) == 0x7F800000)

/* vector macros — pure float, NO double promotion (verified against LST) */
#define DotProduct(a,b)         ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define DotProduct2D(a,b)       ((a)[0]*(b)[0]+(a)[1]*(b)[1])
#define VectorCopy(a,b)         ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2])
#define VectorSubtract(a,b,c)   ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
#define VectorAdd(a,b,c)        ((c)[0]=(a)[0]+(b)[0],(c)[1]=(a)[1]+(b)[1],(c)[2]=(a)[2]+(b)[2])
#define VectorScale(a,b,c)      ((c)[0]=(b)*(a)[0],(c)[1]=(b)*(a)[1],(c)[2]=(b)*(a)[2])
#define VectorMA(a,b,c,d)       ((d)[0]=(a)[0]+(b)*(c)[0],(d)[1]=(a)[1]+(b)*(c)[1],(d)[2]=(a)[2]+(b)*(c)[2])



/* -------------------------------------------------------------------------------

Struct Definitions — trace_types.h

------------------------------------------------------------------------------- */

/*
 * MaterialInfo_t — CoD4 material file header, 40 bytes.
 * CoD4 removed the CoD2 hash/sorted indices, shifting all later fields.
 */
typedef struct MaterialInfo_s
{
    int name;                       /* +0x00: offset to name string */
    int referenceImageName;         /* +0x04: offset to reference image name */
    unsigned char gameFlags;        /* +0x08: game flags */
    unsigned char sortKey;          /* +0x09: sort key */
    unsigned char texAtlasRows;     /* +0x0A: texture atlas row count */
    unsigned char texAtlasCols;     /* +0x0B: texture atlas column count */
    float maxDeformMove;            /* +0x0C: max deform movement */
    unsigned char deformFlags;      /* +0x10: deform flags */
    unsigned char usage;            /* +0x11: usage type */
    unsigned short toolFlags;       /* +0x12: tool flags */
    int locale;                     /* +0x14: locale flags */
    unsigned short autoTexScaleW;   /* +0x18: auto texture scale width */
    unsigned short autoTexScaleH;   /* +0x1A: auto texture scale height */
    float tessSize;                 /* +0x1C: tessellation size */
    int surfaceFlags;               /* +0x20: surface flags */
    int contents;                   /* +0x24: content flags */
} MaterialInfo_t;

/*
 * MaterialDef — material/surface definition, 128 bytes.
 * Layout verified from cod2rad LST LoadMaterial 0x4177B0..0x41785F.
 * Field names si->width / si->height from binary assert strings.
 * Offsets [84] and [104] are never written — real alignment gaps.
 */
typedef struct MaterialDef_s {
    char            name[64];         /* [0]   material name (inline strcpy) */
    int             contents;         /* [64]  content flags (fileData+0x24) */
    union {
        int         surfaceFlags;     /* [68]  surface flags (fileData+0x28) */
        int         flags;            /* [68]  alias used by compile.c */
    };
    union {
        unsigned short toolFlagsWord; /* [72]  word from fileData+0x16 */
        unsigned char  surfaceType;   /* [72]  alias: low byte, SURFTYPE_MASK user */
    };
    unsigned char   _pad4A[2];        /* [74]  alignment */
    int             unused76;         /* [76]  (fileData[0x0C] >> 1) & 1 — set, never read */
    int             unused80;         /* [80]  fileData[0x16] & 1 — set, never read */
    int             _pad54;           /* [84] */
    float           subdivisions;     /* [88]  tessellation size (0 → default) */
    int             reserved92;       /* [92]  always 0 */
    union {
        struct {
            int     globalTexture;    /* [96]  legacy material flag */
            int     unused100;        /* [100] legacy cache field */
        };
        void       *colorMask;        /* [96]  quarter-resolution RGB reflectivity */
    };
    int             _pad68;           /* [104] */
    int             surfFlags_bit7;   /* [108] (contents >> 7) & 1 — cod2map name */
    int             width;            /* [112] alpha mask width — assert si->width */
    int             height;           /* [116] alpha mask height — assert si->height */
    void           *extraData;        /* [120] Material_LoadAlphaImage bitmap pointer */
} MaterialDef_t;

/*
 * TraceHitResult — result from a ray/box trace, returned by CM_TraceRay.
 * fraction(+0x10), surface(+0x50).
 */
typedef struct TraceHitResult_s {
    unsigned char pad00[16];    /* [0] */
    float         fraction;     /* [16] trace hit fraction */
    unsigned char pad14[60];    /* [20] */
    MaterialDef_t *surface;     /* [80] hit surface material */
} TraceHitResult_t;



/* -------------------------------------------------------------------------------

Struct Definitions — dobj_types.h

------------------------------------------------------------------------------- */

/*
 * ScriptStringRef — MT node header for interned strings, 4 bytes + variable string.
 */
typedef struct ScriptStringRef
{
    unsigned char len;              /* +0x00: string length */
    unsigned char user;             /* +0x01: user flags */
    unsigned short refCount;        /* +0x02: reference count */
    char str[1];                    /* +0x04: string data (variable length) */
} ScriptStringRef;

/*
 * PlatformEntry_t — 40 bytes per entry. Entry in the target platform table.
 * Layout verified against binary .rdata at 0x45A0C8.
 */
typedef struct PlatformEntry_s {
    const char  *name;              /* [0]  platform name ("xenon", "pc") */
    long long    platformId;        /* [8]  platform ID (0=xenon, 1=pc) */
    const char  *nameAlt;           /* [16] duplicate name pointer */
    const char  *materialDirectory; /* [24] "materials" */
    long long    bigEndian;         /* [32] big-endian flag (1 for xenon) */
} PlatformEntry_t;

/*
 * DObjAnimMat — 32 bytes. Bone transform matrix.
 */
typedef struct DObjAnimMat_s {
    float quat[4];         /* [0]  rotation quaternion */
    float trans[3];        /* [16] translation */
    float transWeight;     /* [28] 2.0 / quatLenSq */
} DObjAnimMat_t;

/*
 * DObjSkelMat — 64 bytes. 4x4 skeleton transform (3x3 axis + translation).
 */
typedef struct DObjSkelMat_s {
    float axis[3][4];      /* [0]  3 rows of vec4 (axis[i][3] = 0) */
    float trans[4];        /* [48] translation (trans[3] = 1) */
} DObjSkelMat_t;

/*
 * DSkelPartBits_s — 48 bytes. Bone processing bits.
 */
typedef struct DSkelPartBits_s {
    int anim[4];           /* [0]  bones with animation bits set */
    int control[4];        /* [16] bones with control bits set */
    int skel[4];           /* [32] bones with skel bits set */
} DSkelPartBits_t;

/*
 * DSkel_t — 48 bytes header + variable-length mat array.
 * Field names from LST assert strings ("skel->animPartBits",
 * "skel->skelPartBits") and offsets from DObjCalcSkel at 0x41D350.
 */
typedef struct DSkel_s {
    int           animPartBits[4];    /* [0]  anim bits per 32 bones */
    int           controlPartBits[4]; /* [16] control bits per 32 bones */
    int           skelPartBits[4];    /* [32] skel bits per 32 bones */
    DObjAnimMat_t mat[1];             /* [48] bone matrices (variable length) */
} DSkel_t;

/*
 * XBoneHierarchy — bone name array + inline parent list.
 */
typedef struct XBoneHierarchy_s {
    unsigned short *names;        /* [0] bone name string indices */
    unsigned char   parentList[1];/* [8] bone parent index list (variable length) */
} XBoneHierarchy_t;

/*
 * XModelParts_s — bone data for an XModel.
 */
typedef struct XModelParts_s {
    short              numBones;           /* [0]  total bone count */
    short              numRootBones;       /* [2]  root bone count */
    unsigned char      _pad[4];            /* [4]  alignment */
    XBoneHierarchy_t  *hierarchy;          /* [8]  bone hierarchy */
    short             *quats;              /* [16] compressed quaternions */
    float             *trans;              /* [24] bone translations */
    unsigned char     *partClassification; /* [32] part classification bytes */
    DSkelPartBits_t    skel_partBits;      /* [40] embedded skel partBits */
    DObjAnimMat_t      skel_mat[1];        /* [88] embedded base pose matrices */
} XModelParts_t;

#define XMODEL_PARTS_SKEL(parts) ((DSkel_t *)&(parts)->skel_partBits)

/*
 * XBoneInfo — per-bone bounding info, 40 bytes.
 */
typedef struct XBoneInfo_s {
    float bounds[2][3];    /* [0]  bone mins/maxs */
    float offset[3];       /* [24] bone center offset */
    float radiusSquared;   /* [36] bone sphere radius squared */
} XBoneInfo_t;

/*
 * XModelCollTri_s — collision triangle, 48 bytes.
 */
typedef struct XModelCollTri_s {
    float plane[4];        /* [0]  triangle plane equation */
    float svec[4];         /* [16] s-axis edge vector */
    float tvec[4];         /* [32] t-axis edge vector */
} XModelCollTri_t;

/*
 * XModelCollSurf_s — collision surface, 48 bytes.
 */
typedef struct XModelCollSurf_s {
    XModelCollTri_t *collTris;    /* [0]  collision triangles */
    int              numCollTris; /* [8]  triangle count */
    float            mins[3];     /* [12] surface mins */
    float            maxs[3];     /* [24] surface maxs */
    int              boneIdx;     /* [36] bone index */
    int              contents;    /* [40] content flags */
    int              surfFlags;   /* [44] surface flags */
} XModelCollSurf_t;

#define XMODEL_COLLSURFS(m)         (*(XModelCollSurf_t **)((char *)(m) + 0xA8))
#define XMODEL_SET_COLLSURFS(m, v)  (*(XModelCollSurf_t **)((char *)(m) + 0xA8) = (v))

/*
 * XSurfaceTempVert — CoD4 v25 vertices expanded into the compiler's 64-byte
 * shape.  This is not
 * the byte-for-byte wire record: weighted v25 vertices have a 63-byte base
 * followed by 4 bytes per secondary weight.
 */
typedef struct XSurfaceTempVert_s {
    float          normal[3];   /* [0]  vertex normal */
    unsigned char  color[4];    /* [12] vertex color RGBA */
    float          tangent[3];  /* [16] tangent vector */
    float          texcoordU;   /* [28] texture U */
    float          binormal[3]; /* [32] binormal vector */
    float          texcoordV;   /* [44] texture V */
    float          pos[3];      /* [48] vertex position */
    unsigned char  numWeights;  /* [60] number of bone weights */
    unsigned char  pad3D;       /* [61] padding */
    unsigned short boneOffset;  /* [62] primary bone offset */
} XSurfaceTempVert_t;

/*
 * XSurfaceBlendEntry — additional blend bone data, 16 bytes.  v25 stores only
 * bone index + weight for a secondary influence; the loader repeats the
 * vertex position in data[] for the compiler's deform path.
 */
typedef struct XSurfaceBlendEntry_s {
    float          data[3];    /* [0]  blend position/delta */
    unsigned short boneOffset; /* [12] bone offset */
    unsigned short weight;     /* [14] blend weight */
} XSurfaceBlendEntry_t;

/*
 * XSurface — per-surface vertex/triangle data, 40 bytes.
 */
typedef struct XSurface_s {
    unsigned char   tileMode;   /* [0]  CoD4 v25 surface tile mode */
    unsigned char   deformed;   /* [1]  deformation flag */
    unsigned short  vertCount;  /* [2]  vertex count */
    unsigned short  triCount;   /* [4]  triangle count */
    short           boneOffset; /* [6]  primary bone offset */
    unsigned short *triIndices; /* [8]  triangle index buffer */
    void           *verts;      /* [16] packed vertex buffer */
    unsigned char   _pad[16];   /* [24] */
} XSurface_t;

/*
 * XModelSurfs — surface data for one LOD level.
 */
typedef struct XModelSurfs_s {
    XSurface_t **surfs;      /* [0] surface pointer array */
    int          partBits[4];/* [8] part bit mask */
} XModelSurfs_t;

/*
 * XModelLodInfo — per-LOD data within XModel, 40 bytes.
 */
typedef struct XModelLodInfo_s {
    const char     *filename;   /* [0]  xmodelsurfs file name */
    short           numsurfs;   /* [8]  surface count */
    unsigned char   _pad0A[6];  /* [10] alignment */
    unsigned short *surfNames;  /* [16] surface name indices */
    XModelSurfs_t  *modelSurfs; /* [24] loaded surface data */
    unsigned char   _pad20[8];  /* [32] */
} XModelLodInfo_t;

/* Parsed CoD4 raw xmodel v25 configuration (0x1430 bytes).  The wire order is
 * flags/bounds/physics string, four (distance, filename) entries, collLod. */
typedef struct XModelConfigEntry_s {
    char  filename[1024];
    float dist;
} XModelConfigEntry_t;

typedef struct XModelConfig_s {
    XModelConfigEntry_t entries[4];
    float               mins[3];
    float               maxs[3];
    int                 collLod;
    unsigned char       flags;
    char                physicsPresetFilename[1024];
} XModelConfig_t;

/*
 * XModel — model data. lodInfo[4] spans [16..175] with stride 40.
 * collSurfs pointer at +0xA8 physically overlays lodInfo[3]._pad20.
 */
typedef struct XModel_s {
    XModelParts_t   *parts;       /* [0]   bone/skeleton data */
    unsigned char    _pad08[8];   /* [8]   */
    union {
        XModelLodInfo_t lodInfo[4];   /* [16]  4 LODs, 40 bytes each */
        struct {
            unsigned char _lodInfoPrefix[152]; /* [16..167] lodInfo[0..2] + lodInfo[3] up to modelSurfs */
            void         *collSurfs;           /* [168] collision surfaces (alias of lodInfo[3]._pad20) */
        };
    };
    int              numCollSurfs;/* [176] collision surface count */
    int              contents;    /* [180] content flags */
    XBoneInfo_t     *boneInfo;    /* [184] per-bone bounds/offsets */
    float            mins[3];     /* [192] model mins */
    float            maxs[3];     /* [204] model maxs */
    short            numLods;     /* [216] LOD count */
    short            collLod;     /* [218] collision LOD index */
    unsigned char    _padDC[12];  /* [220] */
    int              memUsage;    /* [232] memory usage bytes */
    unsigned char    _padEC[4];   /* [236] alignment */
    const char      *name;        /* [240] model asset name */
    unsigned char    flags;       /* [248] model flags */
    unsigned char    bad;         /* [249] error flag */
} XModel_t;

/*
 * DObjModel_s — input to DObjCreate, 24 bytes.
 */
typedef struct DObjModel_s {
    XModel_t   *model;           /* [0]  model pointer */
    const char *boneName;        /* [8]  attach bone name */
    int         ignoreCollision; /* [16] ignore collision flag */
    int         _pad;            /* [20] */
} DObjModel_t;

/*
 * DObj_s — dynamic object (skeletal model instance), 152 bytes.
 */
typedef struct XAnimTree_s XAnimTree_s;

typedef struct DObj_s {
    XAnimTree_s   *tree;                             /* [0]   animation tree */
    DSkel_t       *skel;                             /* [8]   skeleton (partBits + mat array) */
    int            timeStamp;                        /* [16]  skel update timestamp */
    unsigned char  _pad14[4];                        /* [20]  alignment */
    void          *animToModel;                      /* [24]  anim bone -> model bone remap */
    unsigned short duplicateParts;                   /* [32]  bit mask of duplicated parts */
    unsigned char  _pad22[6];                        /* [34] */
    unsigned char  numModels;                        /* [40]  sub-model count */
    unsigned char  numBones;                         /* [41]  bone count */
    unsigned char  ignoreCollision;                  /* [42]  ignore collision flag */
    unsigned char  _pad2B[5];                        /* [43]  alignment */
    XModel_t      *models[DOBJ_MAX_SUBMODELS];       /* [48]  sub-model pointers */
    unsigned char  modelParents[DOBJ_MAX_SUBMODELS]; /* [112] parent index per model */
    unsigned char  matOffset[DOBJ_MAX_SUBMODELS];    /* [120] bone matrix offset per model */
    float          mins[3];                          /* [128] bounds mins */
    float          maxs[3];                          /* [140] bounds maxs */
} DObj_t;



/* -------------------------------------------------------------------------------

Struct Definitions — lighting_types.h

------------------------------------------------------------------------------- */

typedef struct TransferBlock_s TransferBlock_t; /* forward decl for SampleVars_t */

/* Native compile.cpp stores directional direct-light influences as packed
 * 24-byte records at SampleVars +82/+84.  Keeping the record itself native-
 * sized while widening only the owning pointer preserves the producer math on
 * x64 without changing the fixed 32-byte LightmapSample slots. */
typedef struct DirectTransport_s {
    float color[3];
    float direction[3];
} DirectTransport_t;

/*
 * SampleVars — per-sample lighting variable data.  The x64 port widens the
 * native sky-influence pointer into an explicit trailing field.
 * +0x10: incident[12] — 4 SH bands x 3 channels (48 bytes)
 * +0x40: scattered[3] — accumulated scattered light (12 bytes)
 * +0x4C: unscattered[3] — accumulated unscattered light (12 bytes)
 * +0x58: transferHead — pointer to first TransferBlock
 */
typedef struct SampleVars_s {
    float intensity[4];        /* [0]  sub-pixel area weights */
    float incident[12];        /* [16] 4 SH bands x 3 RGB */
    union {
        float scattered[3];    /* [64] scattered light */
        float gathered[3];     /* [64] alias used by lighting.c */
    };
    float          unscattered[3]; /* [76] unscattered light */
    TransferBlock_t *transferHead;   /* [88] first transfer block */
    float          *skyInfluences;   /* [96] one normalized weight per trace */
    /* Native +32..+40: direct and bounced incident RGB accumulated for use
     * as a source by this sample's neighbours.  Kept trailing in the x64
     * representation so the recovered pointer-bearing fields stay aligned. */
    float           gatheredIncident[3];
    /* Native +68..+76: direction-independent light (coincident point-light
     * hits).  Native +82/+84 follows with a growable DirectTransport array. */
    float             coincident[3];
    DirectTransport_t *directTransports;
    unsigned int       directTransportCount;
    unsigned int       directTransportCapacity;
    /* Native SampleVars +0x50: one validity bit for each 2x2 subsample. */
    unsigned char      validMask;
} SampleVars_t;

/*
 * LightingSample — lighting sample reference.
 */
typedef struct LightingSample_s {
    SampleVars_t *vars; /* [0] sample variables */
} LightingSample_t;

/*
 * LightmapSample — 32-byte slot indexed from g_lightingSamples with stride 32.
 * Also aliased as LightingSampleLock during the gather phase (see below).
 */
typedef struct LightmapSample_s {
    SampleVars_t *vars;              /* [0]  sample variables */
    float         weight;            /* [8]  accumulated weight (= totalWeight in lock view) */
    float         channelWeight[4];  /* [12] per-corner accumulator (written by GatherLightingSampleWithLock) */
    unsigned char _pad[4];           /* [28] trailing alignment */
} LightmapSample_t;

/*
 * LightingHit — hit result from FindLightingSamplesAndNormal, 16 bytes.
 */
typedef struct LightingHit_s {
    LightingSample_t *sample; /* [0]  lighting sample hit */
    float             weight; /* [8]  hit weight */
    int               _pad;   /* [12] */
} LightingHit_t;



/* -------------------------------------------------------------------------------

Struct Definitions — geometry.c

------------------------------------------------------------------------------- */

/*
 * Triangle — 88 bytes. Core triangle structure.
 */
typedef struct Triangle_s {
    int            cacheStamp[4];/* [0]  vis cache stamps */
    int            vertIndex[3]; /* [16] vertex indices */
    unsigned short materialIdx;  /* [28] material slot */
    unsigned short lightmapIdx;  /* [30] lightmap slot */
    float          normal[3];    /* [32] plane normal */
    float          dist;         /* [44] plane distance */
    float          baryVec0[4];  /* [48] barycentric S vector + offset */
    float          baryVec1[4];  /* [64] barycentric T vector + offset */
    MaterialDef_t *material;     /* [80] surface material */
    unsigned char  primaryLightIndex; /* CoD4 source tri-soup primary light */
} Triangle_t;

/*
 * DrawVert — 68 bytes. Per-vertex data.
 */
typedef struct DrawVert_s {
    float         pos[3];      /* [0]  position */
    float         normal[3];   /* [12] vertex normal */
    unsigned char color[4];    /* [24] vertex color */
    float         texCoord[2]; /* [28] texture UV */
    float         lmCoord[2];  /* [36] lightmap UV */
    float         tangent[3];  /* [44] tangent vector */
    float         binormal[3]; /* [56] binormal vector */
} DrawVert_t;

/*
 * SurfaceInfo — surface descriptor for AddTriangle / AddTrianglesForSurface.
 */
typedef struct SurfaceInfo_s {
    unsigned short materialRef;  /* [0]  material slot */
    unsigned short lightmapIdx;  /* [2]  lightmap slot */
    int            vertBase;     /* [4]  first vertex index */
    unsigned short _pad08;       /* [8] */
    unsigned short indexCount;   /* [10] triangle index count */
    int            indicesOffset;/* [12] offset into triangle index buffer */
} SurfaceInfo_t;

/*
 * RayHitResult — 24 bytes. Best hit during ray trace.
 */
typedef struct RayHitResult_s {
    Triangle_t *triangle; /* [0]  hit triangle */
    float       baryU;    /* [8]  barycentric U */
    float       baryV;    /* [12] barycentric V */
    float       fraction; /* [16] hit fraction */
} RayHitResult_t;

/*
 * RayTraceContext — 48 bytes. Ray trace input.
 */
typedef struct RayTraceContext_s {
    int             cacheIndex; /* [0]  trace cache index */
    float           start[3];   /* [4]  ray start */
    float           end[3];     /* [16] ray end */
    float           delta[3];   /* [28] end - start */
    RayHitResult_t *hitResult;  /* [40] output hit result */
} RayTraceContext_t;

/*
 * LightingSampleLock — alias view of LightmapSample during the gather phase.
 * Same 32-byte slot in g_lightingSamples; uses .weight and .channelWeight[4].
 */
typedef LightmapSample_t LightingSampleLock_t;

/*
 * LightingSampleResult — result from GatherLightingSample.
 */
typedef struct LightingSampleResult_s {
    LightingSampleLock_t *lock;   /* [0]  sample lock */
    int                   index0; /* [8]  index 0 */
    int                   index1; /* [12] index 1 */
} LightingSampleResult_t;

/*
 * BspFileNode — source BSP disk node, 36 bytes.
 * Layout matches cod2map BspNode_disk_t; validated by LUMP_NODES
 * being swapped via SwapLongsBlock_generic (bspfile.c), i.e. all
 * 9 dwords are ints/floats — mins/maxs at +12/+24 follow planeNum+children.
 */
typedef struct BspFileNode_s {
    int planeNum;    /* [0]  plane index */
    int children[2]; /* [4]  child indices (neg = -(leaf+1)) */
    int mins[3];     /* [12] bounding box mins (float bits as int) */
    int maxs[3];     /* [24] bounding box maxs (float bits as int) */
} BspFileNode_t;

/*
 * LightTransferMapping — userData for ProcessLightingSampleArea callback, 152 bytes.
 */
typedef struct LightTransferMapping_s {
    int   param;        /* [0]   callback param */
    int   lightmapIdx;  /* [4]   target lightmap */
    float posConst[3];  /* [8]   position constant */
    float posUCoeff[3]; /* [20]  position U coefficient */
    float posVCoeff[3]; /* [32]  position V coefficient */
    float tanConst[3];  /* [44]  tangent constant */
    float tanUCoeff[3]; /* [56]  tangent U coefficient */
    float tanVCoeff[3]; /* [68]  tangent V coefficient */
    float binConst[3];  /* [80]  binormal constant */
    float binUCoeff[3]; /* [92]  binormal U coefficient */
    float binVCoeff[3]; /* [104] binormal V coefficient */
    float normConst[3]; /* [116] normal constant */
    float normUCoeff[3];/* [128] normal U coefficient */
    float normVCoeff[3];/* [140] normal V coefficient */
    int   triangleIndex; /* source triangle used by relight suppression keys */
    unsigned char primaryLightIndex; /* source surface primary light */
} LightTransferMapping_t;

/*
 * BSPCollisionNode — 24 bytes. Internal BSP tree node.
 */
typedef struct BSPCollisionNode_s {
    float normal[3]; /* [0]  plane normal */
    float dist;      /* [12] plane distance */
    int   child[2];  /* [16] child node indices */
} BSPCollisionNode_t;

/*
 * BSPCollisionLeaf — 8 bytes. Leaf data in collision BSP.
 */
typedef struct BSPCollisionLeaf_s {
    int triCount;  /* [0] triangle count */
    int triOffset; /* [4] triangle offset */
} BSPCollisionLeaf_t;



/* -------------------------------------------------------------------------------

Struct Definitions — compile.c

------------------------------------------------------------------------------- */

/*
 * TransferEntry — single lighting transfer entry, 16 bytes.
 */
typedef struct Sample_s Sample_t; /* forward decl */
typedef struct TransferEntry_s {
    Sample_t *toSample; /* [0]  destination sample */
    int       lightIdx; /* [8]  light index */
    float     weight;   /* [12] transfer weight */
} TransferEntry_t;

/*
 * TransferBlock — hash block holding up to 15 transfer entries.
 */
typedef struct TransferBlock_s {
    TransferEntry_t         entries[TRANSFERS_PER_BLOCK]; /* [0]   transfer entries */
    struct TransferBlock_s *next;                         /* [240] next block in chain */
} TransferBlock_t;

/*
 * Sample — top-level sample structure.
 */
typedef struct Sample_s {
    SampleVars_t *vars;   /* [0] sample variables */
    float         areaX2; /* [8] 2 * sample area */
} Sample_t;

/*
 * SubSample — lighting subsample with direction basis.
 */
typedef struct SubSample_s {
    Sample_t     *sample;    /* [0]  parent lightmap sample */
    int           s;         /* [8]  sub-pixel s (= pixelS & 1) */
    int           t;         /* [12] sub-pixel t (= pixelT & 1) */
} SubSample_t;

/*
 * LightDirEntry — light direction with jitter radius, 16 bytes.
 */
typedef struct LightDirEntry_s {
    float x;      /* [0]  direction x */
    float y;      /* [4]  direction y */
    float z;      /* [8]  direction z */
    float radius; /* [12] jitter radius */
} LightDirEntry_t;



/* -------------------------------------------------------------------------------

Struct Definitions — r_image_wavelet.c / r_imagedecode.c

------------------------------------------------------------------------------- */

/*
 * WaveletDecodeState — bitstream reader for wavelet decompression.
 */
typedef struct WaveletDecodeState_s {
    unsigned short bits;        /* [0]  bit accumulator */
    unsigned short bitPos;      /* [2]  current bit position */
    unsigned char  _pad[4];     /* [4]  alignment */
    unsigned char *bytePtr;     /* [8]  bitstream byte pointer */
    int            width;       /* [16] image width */
    int            height;      /* [20] image height */
    int            channels;    /* [24] channel count */
    int            bpp;         /* [28] bits per pixel */
    int            mipLevel;    /* [32] current mip level */
    unsigned char  initialized; /* [36] init flag */
} WaveletDecodeState_t;

/*
 * ImageDecodeState — destination state for block decoders.
 */
typedef struct ImageDecodeState_s {
    char           name[68];   /* [0]  image name (inline strcpy in Image_LoadIWI) */
    unsigned char  flags44;    /* [68] flags byte */
    unsigned char  _pad[3];    /* [69] alignment */
    int            stride;     /* [72] pixel row stride */
    int            height;     /* [76] pixel row count */
    unsigned char *pixels;     /* [80] pixel buffer */
} ImageDecodeState_t;

/*
 * ImageInfo — image file header info.
 */
typedef struct ImageInfo_s {
    char          magic[3]; /* [0]  "IWi" */
    unsigned char version;  /* [3]  CoD4 IWI version 6 */
    unsigned char format;   /* [4]  image format */
    unsigned char flags;    /* [5]  image flags */
    short         width;    /* [6]  image width */
    short         height;   /* [8]  image height */
    short         depth;    /* [10] image depth */
} ImageInfo_t;



/* -------------------------------------------------------------------------------

Struct Definitions — linearmapping.c

------------------------------------------------------------------------------- */

/*
 * LinearMappingData — output from SetupLinearMapping, 168 bytes.
 */
typedef struct LinearMappingData_s {
    double origMatrix[9];  /* [0]   original 3x3 matrix */
    double luMatrix[9];    /* [72]  LU decomposed copy */
    int    permutation[3]; /* [144] row permutation */
    int    axis0;          /* [156] axis index 0 */
    int    axis1;          /* [160] axis index 1 */
    int    dominantAxis;   /* [164] dominant axis index */
} LinearMappingData_t;



/* -------------------------------------------------------------------------------

Struct Definitions — lightmap_bleed.c

------------------------------------------------------------------------------- */

/*
 * LmapSample — lightmap sample, 32 bytes.
 */
typedef struct LmapSample_s {
    float *colorData;    /* [0]  color data pointer */
    float  areaX2;       /* [8]  2 * sample area */
    float  subSample[4]; /* [12] sub-sample weights */
    int    _pad;         /* [28] */
} LmapSample_t;



/* -------------------------------------------------------------------------------

Struct Definitions — cmdline.c

------------------------------------------------------------------------------- */

/*
 * CmdlineSwitch — command-line switch table entry, 24 bytes.
 */
typedef int (*CmdlineHandler_f)(int argc, const char **argv);

typedef struct CmdlineSwitch_s {
    const char       *name;        /* [0]  switch name */
    const char       *description; /* [8]  help description */
    CmdlineHandler_f  handler;     /* [16] handler function */
} CmdlineSwitch_t;



/* -------------------------------------------------------------------------------

Struct Definitions — pointlights.c

------------------------------------------------------------------------------- */

/*
 * LightDef — decoded one-dimensional attenuation curve.
 *
 * The retail x86 layout is 16 bytes: name, width, height, RGB-float data.
 * Pointer widening makes the native logical layout 24 bytes in this x64 port.
 */
typedef struct LightDef_s {
    char  *name;
    int    width;
    int    height;
    float *data;
} LightDef_t;

/*
 * PointLight — point/spot light instance, 72 bytes.
 */
typedef struct PointLight_s {
    int           primaryLightIndex; /* -1 for ordinary entity lights */
    float         origin[3];    /* [0]  light origin */
    float         radius;       /* [12] influence radius */
    float         falloffScale; /* [16] falloff scale */
    float         color[3];     /* [20] light color */
    LightDef_t   *def;          /* [32] light definition */
    unsigned char isSpot;       /* [40] spotlight flag */
    unsigned char _pad[3];      /* [41] alignment */
    float         spotDir[3];   /* [44] spotlight direction */
    float         spotCosAngle; /* [56] spot cosine angle */
    float         spotScale;    /* [60] spot scale */
    float         spotOffset;   /* [64] spot offset */
    int           spotExponent; /* [68] spot exponent */
} PointLight_t;



/* -------------------------------------------------------------------------------

Struct Definitions — groundlight.c

------------------------------------------------------------------------------- */

/*
 * GroundLitModel — linked list node for ground-lit static model, 48 bytes.
 */
typedef struct GroundLitModel_s {
    void                   *entity; /* [0]  entity pointer */
    float                   pos[3]; /* [8]  ground position */
    float                   radius; /* [20] influence radius */
    float                   dir[3]; /* [24] surface direction */
    struct GroundLitModel_s *next;  /* [40] next in list */
} GroundLitModel_t;



/* -------------------------------------------------------------------------------

Struct Definitions — lightgrid.c

------------------------------------------------------------------------------- */

/*
 * GridSamplePoint — light grid sample point, 6 bytes.
 */
typedef struct GridSamplePoint_s {
    unsigned short x; /* [0] grid x */
    unsigned short y; /* [2] grid y */
    unsigned short z; /* [4] grid z */
} GridSamplePoint_t;

/*
 * GridSampleEntry — light grid sample for sorting, 8 bytes.
 */
typedef struct GridSampleEntry_s {
    int           key;       /* [0] sort key */
    unsigned char secondary; /* [4] secondary tag */
} GridSampleEntry_t;

/*
 * StaticModelGridSample — linked list node, 24 bytes.
 */
typedef struct StaticModelGridSample_s {
    int                             data[3];  /* [0]  sample data */
    unsigned char                   _pad[4];  /* [12] alignment */
    struct StaticModelGridSample_s *next;     /* [16] next in list */
} StaticModelGridSample_t;



/* -------------------------------------------------------------------------------

Struct Definitions — modelcollision.c

------------------------------------------------------------------------------- */

/*
 * CollisionTri — collision triangle record, 80 bytes.
 */
typedef struct CollisionTri_s {
    float  plane[4];       /* [0]  triangle plane */
    float  baryCoords0[4]; /* [16] first barycentric */
    float  baryCoords1[4]; /* [32] second barycentric */
    void  *surfaceRef;     /* [48] surface pointer */
    float  texcoord0[2];   /* [56] texcoord v0 */
    float  texcoord1[2];   /* [64] texcoord v1 */
    float  texcoord2[2];   /* [72] texcoord v2 */
} CollisionTri_t;

/*
 * CollisionAabbTree_t — BSP collision AABB tree node, 56 bytes.
 */
typedef struct CollisionAabbTree_s {
    float                        mins[3];    /* [0]  node mins */
    float                        maxs[3];    /* [12] node maxs */
    int                          itemCount;  /* [24] leaf item count */
    int                          _pad1C;     /* [28] alignment */
    CollisionTri_t              *data;       /* [32] leaf data pointer */
    int                          childCount; /* [40] child count */
    int                          _pad2C;     /* [44] alignment */
    struct CollisionAabbTree_s  *firstChild; /* [48] first child node */
} CollisionAabbTree_t;

/*
 * CollisionMeshNode — linked list node for model collision data, 24 bytes.
 */
typedef struct CollisionMeshNode_s {
    void                       *model;    /* [0]  source model pointer */
    CollisionAabbTree_t        *collTree; /* [8]  collision tree */
    struct CollisionMeshNode_s *next;     /* [16] next in list */
} CollisionMeshNode_t;

/*
 * CollisionInstance — placed collision model instance, 104 bytes.
 */
typedef struct CollisionInstance_s {
    CollisionMeshNode_t        *mesh;         /* [0]  collision mesh */
    unsigned char               _pad[16];     /* [8] */
    float                       origin[3];    /* [24] instance origin */
    float                       invMatrix[9]; /* [36] inverse world transform */
    float                       absMins[3];   /* [72] world-space mins */
    float                       absMaxs[3];   /* [84] world-space maxs */
    struct CollisionInstance_s *next;         /* [96] next in list */
} CollisionInstance_t;

/*
 * BSPSubdivNode — BSP subdivision tree node, 24 bytes.
 */
typedef struct BSPSubdivNode_s {
    int                  splitAxis;  /* [0]  split axis index */
    float                splitPos;   /* [4]  split plane position */
    int                  childCount; /* [8]  child count */
    int                  _pad;       /* [12] alignment */
    CollisionInstance_t *list;       /* [16] instance list head */
} BSPSubdivNode_t;

/*
 * ModelPlacement — model placement with position and rotation.
 */
typedef struct ModelPlacement_s {
    float origin[3];  /* [0]  position */
    float axis[3][3]; /* [12] rotation matrix */
} ModelPlacement_t;



/* -------------------------------------------------------------------------------

Struct Definitions — mapio.c

------------------------------------------------------------------------------- */

/*
 * KeyValuePair — linked list node for entity key-value pairs, 24 bytes.
 */
typedef struct KeyValuePair_s {
    struct KeyValuePair_s *next;  /* [0]  next pair in list */
    const char            *key;   /* [8]  key string */
    const char            *value; /* [16] value string */
} KeyValuePair_t;

/*
 * Entity — map entity with key-value pairs.
 */
typedef struct Entity_s {
    unsigned char    _pad[48];   /* [0] */
    KeyValuePair_t  *keyValues;  /* [48] first key-value pair */
} Entity_t;



/* -------------------------------------------------------------------------------

Struct Definitions — aabbtree.c

------------------------------------------------------------------------------- */

/*
 * AabbTreeNode_t — 16 bytes. Internal build node.
 */
typedef struct AabbTreeNode_s {
    int firstItem;  /* [0]  first item index */
    int itemCount;  /* [4]  item count */
    int firstChild; /* [8]  first child index */
    int childCount; /* [12] child count */
} AabbTreeNode_t;

/*
 * AabbTreeBuilder_t — builder context.
 */
typedef struct AabbTreeBuilder_s {
    void           *itemData;         /* [0]  item data buffer */
    int             itemCount;        /* [8]  item count */
    int             itemStride;       /* [12] item stride in bytes */
    int             hasBoundsData;    /* [16] bounds-data flag */
    float          *itemMins;         /* [24] per-item mins array */
    float          *itemMaxs;         /* [32] per-item maxs array */
    AabbTreeNode_t *nodes;            /* [40] output node buffer */
    int             maxNodes;         /* [48] max node capacity */
    int             minPartitionSize; /* [52] minimum partition size */
    int             minLeafItems;     /* [56] minimum leaf item count */
} AabbTreeBuilder_t;



/* -------------------------------------------------------------------------------

Struct Definitions — r_xsurface.c

------------------------------------------------------------------------------- */

/*
 * BoneMatrix — 3x4 column-major bone matrix, 64 bytes.
 */
typedef struct BoneMatrix_s {
    float col0[4]; /* [0]  column 0 */
    float col1[4]; /* [16] column 1 */
    float col2[4]; /* [32] column 2 */
    float col3[4]; /* [48] column 3 */
} BoneMatrix_t;



/* -------------------------------------------------------------------------------

Struct Definitions — bspfile.c

------------------------------------------------------------------------------- */

typedef struct bspLumpEntry_s {
    int size;   /* [0] lump size in bytes */
    int offset; /* [4] file offset */
} bspLumpEntry_t;

typedef struct dmaterial_s {
    char material[64];  /* [0]  material name */
    int  surfaceFlags;  /* [64] surface flags */
    int  contentFlags;  /* [68] content flags */
} dmaterial_t;

typedef struct bspPlane_s {
    float normal[3]; /* [0]  plane normal */
    float dist;      /* [12] plane distance */
} bspPlane_t;

typedef struct bspTriSoup_s {
    unsigned short materialIndex; /* [0]  material slot */
    unsigned short lightmapIndex; /* [2]  lightmap slot */
    int            firstVertex;   /* [4]  first vertex */
    unsigned short vertexCount;   /* [8]  vertex count */
    unsigned short indexCount;    /* [10] index count */
    int            firstIndex;    /* [12] first index */
} bspTriSoup_t;

typedef struct bspDrawVert_s {
    float pos[3];      /* [0]  position */
    float normal[3];   /* [12] normal */
    int   color;       /* [24] packed color */
    float uv[2];       /* [28] texture UV */
    float lmUv[2];     /* [36] lightmap UV */
    float tangent[3];  /* [44] tangent */
    float binormal[3]; /* [56] binormal */
} bspDrawVert_t;

typedef struct bspNode_s {
    int planeNum;    /* [0]  plane index */
    int children[2]; /* [4]  child node indices */
    int mins[3];     /* [12] node mins */
    int maxs[3];     /* [24] node maxs */
} bspNode_t;

typedef struct bspLeaf_s {
    int cluster;            /* [0]  cluster index */
    int area;               /* [4]  area index */
    int firstCollisionAABB; /* [8]  first collision AABB */
    int numCollisionAABBs;  /* [12] collision AABB count */
    int firstLeafSurface;   /* [16] first leaf surface */
    int numLeafSurfaces;    /* [20] leaf surface count */
    int firstLeafBrush;     /* [24] first leaf brush */
    int numLeafBrushes;     /* [28] leaf brush count */
    int cellNum;            /* [32] cell index */
} bspLeaf_t;

typedef struct bspModel_s {
    float mins[3];      /* [0]  model mins */
    float maxs[3];      /* [12] model maxs */
    int   firstSurface; /* [24] first surface */
    int   numSurfaces;  /* [28] surface count */
    int   firstBrush;   /* [32] first brush */
    int   numBrushes;   /* [36] brush count */
} bspModel_t;



/* -------------------------------------------------------------------------------

Struct Definitions — poly2d.c

------------------------------------------------------------------------------- */

typedef void (*ForEach2dAreaCallback)(float areaX2, float *centroid,
                                     float *polyVerts, int vertCount,
                                     void *userData, int areaIndex);



/* -------------------------------------------------------------------------------

Struct Definitions — dvar.c

------------------------------------------------------------------------------- */

typedef union DvarValue_u {
    int           enabled;  /* [0] bool variant */
    int           integer;  /* [0] int variant */
    float         value;    /* [0] float variant */
    float        *vector;   /* [0] vec2/vec3/vec4 variant */
    const char   *string;   /* [0] string variant */
    unsigned char color[4]; /* [0] RGBA color variant */
} DvarValue_t;

typedef union DvarLimits_u {
    struct { float min; float max;                         } decimal;     /* [0] float min/max */
    struct { int   min; int   max;                         } integer;     /* [0] int min/max */
    struct { int   stringCount; const char **strings;      } enumeration; /* [0] enum strings */
} DvarLimits_t;

typedef struct dvar_s {
    const char    *name;     /* [0]  dvar name string */
    unsigned short flags;    /* [8]  dvar flags */
    unsigned char  type;     /* [10] dvar type */
    unsigned char  modified; /* [11] modified flag */
    DvarValue_t    current;  /* [16] current value */
    DvarValue_t    latched;  /* [24] latched value */
    DvarValue_t    reset;    /* [32] reset/default value */
    DvarLimits_t   domain;   /* [40] value domain/limits */
    struct dvar_s *next;     /* [56] sorted linked list next */
    struct dvar_s *hashNext; /* [64] hash chain next */
} dvar_t;



/* -------------------------------------------------------------------------------

Struct Definitions — com_memory.c

------------------------------------------------------------------------------- */

/*
 * HunkDataNode — hash table node for Hunk_FindDataForFile.
 */
typedef struct HunkDataNode_s {
    void                  *data;    /* [0]  data pointer */
    struct HunkDataNode_s *next;    /* [8]  next in hash chain */
    unsigned char          type;    /* [16] type tag */
    char                   name[1]; /* [17] variable-length file name */
} HunkDataNode_t;



/* -------------------------------------------------------------------------------

Global Variables — cmdline settings

------------------------------------------------------------------------------- */

extern int g_numThreads;
extern char g_modelShadows;
extern char g_extraVerbose2;
extern char g_disableModelShadows;
extern char g_unusedFlag1;         /* 0x480897: set to 1 in init, never read */
extern char g_unusedFlag2;         /* 0x480898: set to 1 in init, never read */
extern char g_relightLoadEnabled;
extern char g_relightSaveEnabled;
extern int g_superSampleAlpha;
extern int g_superSample;
extern float g_jitter;
extern int g_traces;
extern int g_lightmapHeight;
extern int g_traceFilter;
extern int g_basisDirCount;
extern int g_maxBounces;
/* Compile phase selector: direct radiosity rasterization precedes transport,
 * matching the two native dispatcher passes at 0x40C6E0 and 0x40C700. */
extern int g_lightingTransportPass;
extern int g_lightingSeedSkyPass;
extern int g_unusedInt1;           /* 0x4808AC: set to 32 in init, never read */
extern int g_unusedInt2;           /* 0x4808B0: set to 24 in init, never read */
extern int g_unusedInt3;           /* 0x4808B4: set to 16 in init, never read */
extern int g_unusedInt4;           /* 0x4808B8: set to 24 in init, never read */
extern float g_sunDirX;            /* 0x4808BC: sun direction X */
extern float g_sunDirY;            /* 0x4808C0: sun direction Y */
extern float g_sunDirZ;            /* 0x4808C4: sun direction Z */
extern float g_sunColorR;          /* 0x4808C8: sun color R (default 0.7) */
extern float g_sunColorG;          /* 0x4808CC: sun color G (default 0.7) */
extern float g_sunColorB;          /* 0x4808D0: sun color B (default 0.7) */
extern float g_sunRadiosityR;      /* bounced directional sunlight R */
extern float g_sunRadiosityG;      /* bounced directional sunlight G */
extern float g_sunRadiosityB;      /* bounced directional sunlight B */
extern float g_backfaceLightR;     /* 0x4808D4: backface light R (default 0.3) */
extern float g_backfaceLightG;     /* 0x4808D8: backface light G (default 0.3) */
extern float g_backfaceLightB;     /* 0x4808DC: backface light B (default 0.3) */
extern float g_ambientR;
extern float g_ambientG;
extern float g_ambientB;
extern float g_radiosityScale;
extern float g_gamma;
extern float g_contrastGain;
extern char g_radiosityScaleOverridden;
extern char g_contrastGainOverridden;
extern char g_verbose;
extern char g_extraVerbose;
extern int g_warningLevel;
extern int g_platform;
extern char g_platformByte;
#ifndef MAX_OS_PATH_SHORT
#define MAX_OS_PATH_SHORT            256
#endif
extern char g_mapName[MAX_OS_PATH_SHORT];
extern char g_bspPath[MAX_OS_PATH_SHORT];
extern CmdlineSwitch_t g_cmdlineSwitches[NUM_CMDLINE_SWITCHES];

extern char g_aoEnabled;
extern int g_aoSamples;
extern float g_aoDist;
extern float *g_aoFactors;
extern float ComputeAmbientOcclusion(float *pos, float *basis, int numSamples, float maxDist);

extern char g_adaptiveEnabled;
extern float g_adaptiveBias;
extern float g_adaptiveMin;
extern float g_adaptiveMax;
extern void AdaptiveLightmapRepack(int threadCount);
extern void UVRepack(void);
extern char g_uvRepackEnabled;
extern char g_embreeEnabled;


/* -------------------------------------------------------------------------------

Global Variables — bspfile.c data arrays

------------------------------------------------------------------------------- */

/* MAX_MAP_* sizes — from cod2map.h, define the .bss array dimensions */
#define MAX_MAP_PLANES               524288
#define MAX_MAP_MATERIALS            1024
#define MAX_MAP_LIGHTBYTES           0x5D00000
#define MAX_MAP_LIGHTGRID            0x400000
#define MAX_MAP_LIGHTGRIDCOLORS      0xFFFF
#define MAX_RAD_GRIDSAMPLE_BYTES     0x168A213
#define MAX_RAD_GRIDCOLOR_BYTES      (0x2FFFD * 8)
/* CoD4 v22 stores 56 RGB byte samples (168 bytes) per light-grid color.
 * The native palette index is 16-bit, so keep a full-size modern output
 * buffer separate from the 24-byte legacy color table above. */
#define MAX_COD4_GRIDCOLOR_BYTES     (0xFFFF * 168)
#define MAX_RAD_LIGHTMAP_BYTES       0x5D00000
#define MAX_MAP_BRUSHSIDES           655360
#define MAX_MAP_BRUSHES              0x8000
#define MAX_MAP_TRISOUPS             0x8000
#define MAX_MAP_DRAW_VERTS           0x80000
#define MAX_RAD_TRIANGLES            0x100000  /* cod2rad triangle buffer cap — LST 0x40BE0E */
#define MAX_MAP_DRAW_INDEXES         3145728
#define MAX_MAP_VERTEXES             0x80000  /* LST 0x40BDE6 / 0x40BD24 — matches MAX_MAP_DRAW_VERTS */
#define MAX_MAP_CULLGROUPS           2048
#define MAX_MAP_CULLGROUPINDEXES     4096
#define MAX_MAP_SHADOW_VERTS         0x80000
#define MAX_MAP_SHADOW_INDEXES       3145728
#define MAX_MAP_SHADOW_CLUSTERS      256
#define MAX_MAP_SHADOW_AABBTREES     0x20000
#define MAX_MAP_SHADOW_SOURCES       256
#define MAX_MAP_PORTAL_VERTS         0x4000
#define MAX_MAP_OCCLUDERS            4096
#define MAX_MAP_OCCLUDER_PLANES      0x8000
#define MAX_MAP_OCCLUDER_EDGES       0x10000
#define MAX_MAP_OCCLUDER_INDEXES     49152
#define MAX_MAP_AABBTREES            0x100000
#define MAX_MAP_CELLS                1024
#define MAX_MAP_PORTALS              2048
#define MAX_MAP_NODES                0x8000
#define MAX_MAP_LEAFS                0x8000
#define MAX_MAP_LEAFBRUSHES          0x40000
#define MAX_MAP_LEAFSURFACES         0x20000
#define MAX_MAP_COLLISION_VERTS      0x40000
#define MAX_MAP_COLLISION_EDGES      0x80000
#define MAX_MAP_COLLISION_TRIS       0x40000
#define MAX_MAP_COLLISION_BORDERS    0x20000
#define MAX_MAP_COLLISION_PARTS      0x20000
#define MAX_MAP_COLLISION_AABBS      0x40000
#define MAX_MAP_MODELS               1023
#define MAX_MAP_VISIBILITY           0x200000
#define MAX_MAP_PATHS                0x10000
#define MAX_MAP_ENTSTRING            0x1000000
#define MAX_MAP_ENTITIES             0x8000
#define MAX_RAD_MATERIALS            4096
#define MAX_RAD_LIGHTDEFS            64
#define MAX_RAD_POINTLIGHTS          0x800
#define MAX_QPATH                    64
#define MAX_OS_PATH_SHORT            256
#define MAX_OS_PATH                  1024
#define MAX_FILE_EXT                 16
#define MODEL_BOUNDS_FLOATS          12  /* 48-byte texcoord stride per model */
#define MATERIAL_DATA_STRIDE         72  /* name + metadata bytes per material entry */


/* BSP struct types — from cod2map.h */
typedef struct { char material[64]; int surfaceFlags; int contentFlags; } Dmaterial_t;
typedef struct { float normal[3]; float dist; } BspPlane_disk_t;
typedef struct { int planeNum; int children[2]; int mins[3]; int maxs[3]; } BspNode_disk_t;
typedef struct { int cluster; int area; int firstCollisionAABB; int numCollisionAABBs; int firstLeafBrush; int numLeafBrushes; int cellnum; int reserved28; int reserved32; } BspLeaf_disk_t;
typedef struct { float mins[3]; float maxs[3]; int firstTriSoup; int numTriSoups; int firstAABB; int numAABBs; int firstBrush; int numBrushes; } BspModel_t;
typedef struct { union { int planeNum; float dist; }; int shaderNum; } BspBrushSide_t;
typedef struct { short numSides; short shaderNum; } BspBrush_t;
typedef struct { unsigned short materialIndex; unsigned short lightmapIndex; int firstVertex; unsigned short vertexCount; unsigned short indexCount; int firstIndex; } BspTriSoup_t;
typedef struct {
    unsigned char type;
    unsigned char canUseShadowMap;
    unsigned char unused[2];
    float color[3];
    float dir[3];
    float origin[3];
    float radius;
    float cosHalfFovOuter;
    float cosHalfFovInner;
    int exponent;
    float rotationLimit;
    float translationLimit;
    char defName[64];
} BspPrimaryLight_t;
typedef struct { float pos[3]; float normal[3]; int color; float uv[2]; float lmUv[2]; float tangent[3]; float binormal[3]; } BspDrawVert_t;
typedef struct { float mins[3]; float maxs[3]; int firstTriSoup; int triSoupCount; } BspCullGroup_t;
typedef struct { float mins[3]; float maxs[3]; int aabbTreeIndex; int firstPortal; int portalCount; int firstCullGroup; int cullGroupCount; int firstOccluder; int occluderCount; } BspCell_t;
typedef struct { int planeIdx; int cellIdx; int firstVert; int numVerts; } BspPortal_t;
typedef struct { int startPlanes; unsigned short numNewPlanes; unsigned short numNewEdges; int startEdges; int startVerts; unsigned short numNewVerts; unsigned short _pad18; } BspOccluder_t;
typedef struct { char planeRef0; char planeRef1; char facePlane0; char facePlane1; } BspOccluderEdge_t;
typedef struct { int firstTriSoup; int triSoupCount; int childIndex; } BspAabbTreeEntry_t;
typedef struct { int reserved; float xyz[3]; } BspCollisionVert_t;
typedef struct { int reserved; float vertData[3]; float perpVec[9]; float edgeLen; } BspCollisionEdge_t;
typedef struct { float plane[4]; float baryCoords0[4]; float baryCoords1[4]; int vertIdx[3]; int edgeIdx[3]; } BspCollisionTri_t;
typedef struct { float edgeNormal[2]; float planeDist; float vertZ; float perpDist; float crossProduct; float edgeLen; } BspCollisionBorder_t;
typedef struct { unsigned short reserved0; unsigned char triCount; unsigned char borderCount; int firstTri; int firstBorder; } BspCollisionPart_t;
typedef struct { float mins[3]; float maxs[3]; unsigned short firstChild; unsigned short childCount; int firstPartition; } BspCollisionAabb_disk_t;
typedef struct { int colorData; unsigned char pad[2]; unsigned short dirIndex; } BspLightGridEntry_t;
typedef struct { float rgb[3]; float intensity[3]; } BspLightGridColor_t;
typedef struct { float xyz[3]; } BspShadowVert_t;
typedef struct { int firstAabbIndex; int aabbCount; } BspShadowSource_t;
typedef struct { int firstVert; int count; } BspShadowCluster_disk_t;
typedef struct { int firstChildIndex; unsigned short childCount; char isLeaf; char materialPartition; int triSoupIndex; int indexCount; float mins[3]; float maxs[3]; } DiskShadowAabb_t;

/* BSP data arrays — typed to match cod2map */
extern int numBSPMaterials;            extern Dmaterial_t             bspMaterials[];
extern int numBSPLightBytes;           extern char                    bspLightmapData[];
/* bspLightGridHash / bspLightGridColors / numBSPLightGridHash / numBSPLightGridColors
 * are aliased in bspfile.c to g_gridSampleArray / g_gridColorEntries / g_gridSampleArrayCount / g_gridColorCount. */
extern int numBSPPlanes;               extern BspPlane_disk_t         bspPlanes[];
extern int numBSPBrushSides;           extern BspBrushSide_t          bspBrushSidesData[];
extern int numBSPBrushes;              extern BspBrush_t              bspBrushes[];
extern int numBSPTriSoups;             extern BspTriSoup_t            bspTriangles[];
extern unsigned char bspTriSoupPrimaryLightIndices[];
extern int numBSPPrimaryLights;        extern BspPrimaryLight_t        bspPrimaryLights[];
extern int g_sunPrimaryLightIndex;
extern int numBSPDrawVerts;            extern BspDrawVert_t           bspDrawVerts[];
extern int numBSPDrawIndexes;          extern unsigned short          bspDrawIndexes[];
extern int numBSPCullGroups;           extern BspCullGroup_t          bspCullGroups[];
extern int numBSPCullGroupIndexes;     extern int                     bspCullGroupIndexes[];
extern int numBSPShadowVerts;          extern BspShadowVert_t         bspShadowVerts[];
extern int numBSPShadowIndices;        extern short                  *bspShadowIndexes;
extern int numBSPShadowClusters;       extern BspShadowCluster_disk_t bspShadowClusters[];
extern int numBSPShadowAabbTrees;      extern DiskShadowAabb_t        bspShadowData[];
extern int numBSPShadowSources;        extern BspShadowSource_t       bspShadowSources[];
extern int numBSPPortalVerts;          extern float                   bspPortalVerts[][3];
extern int numBSPOccluders;            extern BspOccluder_t           bspOccluders[];
extern int numBSPOccluderPlanes;       extern int                     bspOccluderPlanes[];
extern int numBSPOccluderEdges;        extern BspOccluderEdge_t       bspOccluderEdges[];
extern int numBSPOccluderIndexes;      extern short                   bspOccluderIndexes[];
extern int numBSPAabbTrees;            extern BspAabbTreeEntry_t      bspAabbTrees[];
extern int numBSPCells;                extern BspCell_t               bspCells[];
extern int numBSPPortals;              extern BspPortal_t             bspPortals[];
extern int numBSPNodes;                extern BspNode_disk_t          bspNodes[];
extern int numBSPLeafs;                extern BspLeaf_disk_t          bspLeafs[];
extern int numBSPLeafBrushes;          extern int                     bspLeafBrushes[];
extern int numBSPLeafSurfaces;         extern int                     bspLeafSurfaces[];
extern int numBSPCollisionVerts;       extern BspCollisionVert_t      bspCollisionVerts[];
extern int numBSPCollisionEdges;       extern BspCollisionEdge_t      bspCollisionEdgeData[];
extern int numBSPCollisionTris;        extern BspCollisionTri_t       bspCollisionTriData[];
extern int numBSPCollisionBorders;     extern BspCollisionBorder_t    bspCollisionBorders[];
extern int numBSPCollisionParts;       extern BspCollisionPart_t      bspCollisionParts[];
extern int numBSPCollisionAABBs;       extern BspCollisionAabb_disk_t bspCollisionAABBs[];
extern int numBSPModels;               extern BspModel_t              bspModels[];
extern int numBSPVisBytes;             extern char                    bspVisBytes[];
extern int numBSPPaths;                extern char                    bspPaths[];
extern int bspEntDataSize;             extern char                    bspEntData[];



/* -------------------------------------------------------------------------------

Global Variables — geometry / BSP data

------------------------------------------------------------------------------- */

extern int g_triCount;
extern Triangle_t g_triangles[MAX_RAD_TRIANGLES];
extern int g_vertCount;
extern float g_vertPositions[MAX_MAP_VERTEXES][3];
#define g_vertData ((DrawVert_t *)bspDrawVerts)
#define g_drawIndices bspDrawIndexes
#define g_numBSPNodes numBSPNodes
extern int g_bspNodeCount;
extern BSPCollisionNode_t *g_bspNodeData;
extern int g_bspNodeAlloc;
extern int g_bspLeafCount;
extern BSPCollisionLeaf_t *g_bspLeafData;
extern Triangle_t **g_triPointerArray;
extern int g_bspRefCount;
extern int g_visCache[4];
extern int g_lightTransferCount;
extern int g_totalLightmapPixels;
extern unsigned char g_lightmapOutput[MAX_RAD_LIGHTMAP_BYTES];
extern const float g_shBasis[24]; /* .rdata @ 0x458790, 24 floats = 8 direction vectors */
extern float g_lightScale;



/* -------------------------------------------------------------------------------

Global Variables — lighting

------------------------------------------------------------------------------- */

extern void *g_lightingSampleCallback;
extern void *g_lightingPixelCallback;
extern int g_lightmapSize;
extern void *g_lightingSamples;
extern int g_usefulSampleCount;
extern int g_lightSourceCount;
extern float g_degamma;
extern void *g_sampleVarsPool;
extern int g_totalSampleCount;
extern float g_energyScale;



/* -------------------------------------------------------------------------------

Global Variables — lightgrid

------------------------------------------------------------------------------- */

extern int g_gridSampleCount;
extern StaticModelGridSample_t *g_gridSampleList;
extern GridSamplePoint_t *g_gridPoints;
extern int g_lightmapHeight;
#define g_numTraceDirections g_lightmapHeight
extern float *g_traceDirections;



/* -------------------------------------------------------------------------------

Global Variables — pointlights

------------------------------------------------------------------------------- */

extern int g_numLightDefs;
extern LightDef_t g_lightDefs[MAX_RAD_LIGHTDEFS];
extern int g_numPointLights;
extern PointLight_t g_pointLights[MAX_RAD_POINTLIGHTS];



/* -------------------------------------------------------------------------------

Global Variables — groundlight

------------------------------------------------------------------------------- */

extern GroundLitModel_t *g_groundLitList;



/* -------------------------------------------------------------------------------

Global Variables — mapio

------------------------------------------------------------------------------- */

extern Entity_t g_entities[MAX_MAP_ENTITIES];



/* -------------------------------------------------------------------------------

Global Variables — wavelet decode tables

------------------------------------------------------------------------------- */

extern const unsigned short g_waveletDecodeTable0[4096][2]; /* .rdata @ 0x462AB0 */
extern const unsigned short g_waveletDecodeTable1[4096][2]; /* .rdata @ 0x466AB0 */



/* -------------------------------------------------------------------------------

Function Prototypes — cod2rad.c

------------------------------------------------------------------------------- */

extern void Com_Error(int errorChannel, const char *fmt, ...);
extern void Com_ErrorMsg(int errorLevel, const char *fmt, ...);
extern int LoadBSP(const char *mapName);
extern void WriteBSP(const char *mapName);
extern char Sys_IsMainThread(void);



/* -------------------------------------------------------------------------------

Function Prototypes — print.c

------------------------------------------------------------------------------- */

extern void WarningMsg(int level, const char *fmt, ...);
extern void ErrorMsgV(const char *fmt, va_list arglist);
extern void ErrorMsg(const char *fmt, ...);
extern void PrintMsecDuration(int msec);
extern void UpdateProgressPrint(void);
extern void BeginProgress(const char *label);
extern void SetProgress(int current, int total);
extern void UpdateProgress(int amount);
extern void EndProgress(void);



/* -------------------------------------------------------------------------------

Function Prototypes — cmdlib.c

------------------------------------------------------------------------------- */

extern void Error(const char *fmt, ...);
extern void SetErrorHandler(void (*handler)(const char *, va_list));
extern int SafeRead(FILE *fp, void *buf, int count);
extern int SafeWrite(FILE *fp, void *buf, int count);
extern int LoadFile(const char *filename, void **bufferptr);
extern void StripExtension(const char *path);
extern void CreatePath(const char *path);
extern char *CopyStringInternal(const char *s);
extern void Error_va(const char *fmt, ...);
extern void InitFileSystem(const char *basepath, const char *game, const char *basegame);



/* -------------------------------------------------------------------------------

Function Prototypes — cmdline.c

------------------------------------------------------------------------------- */

extern void ParseCommandLine_Init(void);
extern int PCL_ParseInt(int argc, const char **argv, int *output, int min, int max);
extern int PCL_ParseFloat(int argc, const char **argv, float *output, float min, float max);
extern int PCL_SetVerbose(void);
extern int PCL_SetExtraVerbose(void);
extern int PCL_SetWarningLevel(int argc, const char **argv);
extern int PCL_SetPlatform(int argc, const char **argv);
extern int PCL_LowQualityPreset(void);
extern int PCL_HighQualityPreset(void);
extern int PCL_EnableModelShadows(void);
extern int PCL_DisableModelShadows(void);
extern int PCL_SetLightmapSize(int argc, const char **argv);
extern int PCL_SetJitter(int argc, const char **argv);
extern int PCL_SetSuperSampleAlpha(int argc, const char **argv);
extern int PCL_SetSuperSample(int argc, const char **argv);
extern int PCL_SetMaxBounces(int argc, const char **argv);
extern int PCL_SetRadiosityScale(int argc, const char **argv);
extern int PCL_SetTraceFilterLinear(void);
extern int PCL_SetTraceFilterPoint(void);
extern int PCL_DisableRelight(void);
extern int PCL_SetBasisDirCount(int argc, const char **argv);
extern int PCL_SetBounceFraction(int argc, const char **argv);
extern int PCL_EnableEmbree(void);
extern int PCL_SetGamma(int argc, const char **argv);
extern int PCL_SetContrastGain(int argc, const char **argv);
extern int PCL_SetThreadCount(int argc, const char **argv);
extern int PCL_DisplaySettings(void);
extern int PCL_ParseArgs(int startIdx, const char **argv, int endIdx);
extern int ParseCommandLine(int argc, const char **argv);



/* -------------------------------------------------------------------------------

Function Prototypes — com_math.c

------------------------------------------------------------------------------- */

extern int CompareFunction(const float *a, const float *b);
extern float Vec2DistanceSq(const float *a, const float *b);
extern void Vec3Cross(const float *v1, const float *v2, float *out);
extern float Vec3Normalize(float *vec);
extern void Vec2Normalize(float *vec);
extern void AngleVectors(float *angles, float *forward, float *right, float *up);
extern void AnglesToAxis(float *angles, float *axis);
extern void QuatMultiply(float *q1, float *q2, float *out);
extern void ClearBounds(float *mins, float *maxs);
extern void ClearBounds2D(float *mins, float *maxs);
extern void AddPointToBounds(float *point, float *mins, float *maxs);
extern void AddPointToBounds2D(float *point, float *mins, float *maxs);
extern void ExpandBounds(float *point1, float *point2, float *mins, float *maxs);
extern void GetRotatedBounds(float *box, float *origin, float *matrix, float *outBounds);
extern void PointOnSphereFromUniformDeviates(float u1, float u2, float *out);
extern int PlaneFromPoints(float *plane, float *point0, float *point1, float *point2);
extern int VectorCompareEpsilon(const float *v1, const float *v2, float epsilon, int count);
extern void MatrixTransformVector(float *in1, float *in2, float *out);
extern float Vec3Distance(const float *a, const float *b);
extern int Vec3MajorAxis(const float *dir);
extern void MatrixInverse(float *in, float *out);
extern void UniformPointsOnHemisphere(unsigned int numPoints, float *points, int stride);
extern void UniformPointsOnSphere(unsigned int numPoints, float *points, int stride);



/* -------------------------------------------------------------------------------

Function Prototypes — com_memory.c

------------------------------------------------------------------------------- */

extern void Z_FreeInternal(void *ptr);
extern void Z_VirtualFree(void *ptr);
extern void *Z_Malloc(int size);
extern void *Z_VirtualAlloc(int size);
extern char *Z_StrDup(const char *string);
extern void Hunk_FreeTempMemory(void *buf);
extern void *memmove_thunk(void *dest, const void *src, int count);
extern void *memset_thunk(void *dest, int val, int count);
/* Hunk file-data type constants */
#define FILEDATA_XMODELSURFS    2
#define FILEDATA_XMODELPARTS    3
#define FILEDATA_XMODEL         4

extern void *Hunk_FindDataForFile(int type, const char *name);
extern void *Hunk_AddDataForFile(int type, const char *name, void *data, void *(*allocator)(int));
extern void *Hunk_AllocateTempMemory(int size);



/* -------------------------------------------------------------------------------

Function Prototypes — com_files.c

------------------------------------------------------------------------------- */

extern int FS_HashFileName(const char *fname, int hashSize);
extern int FS_HandleForFile(int streamThread);
extern FILE *FS_FileForHandle(int f);
extern char *FS_BuildOSPath(const char *base, const char *gamedir, const char *qpath, char *ospath, int lenCheck);
extern void FS_CopyFile(const char *fromOSPath, const char *toOSPath);
extern int FS_filelength(int f);
extern void FS_FCloseFile(int f);
extern void FS_FreeFile(void *buffer);
extern void FS_DisplayPath(void);
extern int FS_FilenameCompare(const char *s1, const char *s2);
extern void FS_RegisterDvars(void);
extern int FS_ReadFile(const char *qpath, void **buffer);
extern int FS_Read(void *buffer, int len, int f);
extern int FS_SanitizeFilename(const char *filename, char *sanitizedName, int sanitizedNameSize);
extern const char *FS_ExtractLanguageFromPath(const char *iwdName);
extern int FS_CompareLocalizedPaths(const char **a, const char **b);
extern void FS_AddIwdFilesForGameDirectory(const char *basepath, const char *gamedir);
extern void FS_AddGameDirectory(const char *basepath, const char *gamedir, int localized, int language);
extern void FS_AddSearchPath(const char *basepath, const char *gamedir);
extern void FS_Startup(const char *gamedir);
extern const char *LoadDefaultConfig(void);
extern int FS_FOpenFileRead(const char *filename, int *handleOut, int uniqueFILE, int streamThread);
extern int FS_IsExt(const char *filename);



/* -------------------------------------------------------------------------------

Function Prototypes — bspfile.c

------------------------------------------------------------------------------- */

extern void SwapLongsBlock_generic(void *data, int size);
extern void SwapShortsBlock(void *data, int len);
extern int ParseEntities(void);
extern void SetKeyValue(void *entity, const char *key, const char *value);
extern void UnparseEntities(void);
extern int CopyLump(void *header, int lumpIdx, void *dest, int elemSize, int maxBytes);
extern void LoadBspFile(const char *filename);
extern void SetBspFileExtensions(const char *root);
extern const char *GetBspFileExtension(void);
extern const char *GetPolyFileExtension(void);
extern void AddLump(FILE *fp, void *header, int lumpIdx, void *data, int length);
extern int WriteBspFile(const char *filename, int swapFlag);
extern void SwapDrawSurfaces(void *data, int len);
extern void SwapMaterials(void *data, int len);
extern void SwapNodes(void *data, int len);
extern void SwapLeafs(void *data, int len);
extern void SwapLeafBrushes(void *data, int len);
extern void SwapDrawVerts(void *data, int len);
extern void SwapBrushes(void *data, int len);
extern void SwapCollisionAabbTree(void *data, int len, int preSwap);
extern void SwapBSPFile(int direction);



/* -------------------------------------------------------------------------------

Function Prototypes — compile.c

------------------------------------------------------------------------------- */

extern void GatherSurfaceIncidentEnergyForLightFromDir(float *lightColor, float *direction, float *output);
extern void AllocLightingTransfer(Sample_t *fromSample, void *toSample, int lightIdx, float weight);
extern void NormalizeLightTransfers(Sample_t *sample);
extern int FindLightingSamplesAndNormal(int sampleIdx, float *position, float *normal, float offset, void *outputLighting, float *outputNormal);
extern int FindLightingSamplesAndNormalNearest(int sampleIdx, float *position, float *normal, float offset, void *outputLighting, float *outputNormal);
extern void GatherSkyLighting(int sampleIdx, float *position, float *basis, float skyWeight, float subAreaFactor, unsigned char primaryLightIndex, SubSample_t *subSample);
extern void BounceGatherCallback(Sample_t *sample);
extern void Compile(int threadCount);
extern void GatherPointLightForSample(int sampleIdx, int lightIdx, float *position, float *basis, float subAreaFactor, unsigned char primaryLightIndex, SubSample_t *subSample);
extern void SetupSampleRadii(void);
extern void BuildLightingTransfersForSample(float *bouncedLight, float weight, Sample_t *sample);
extern void GatherBounceForSample(Sample_t *sourceSample, int threadIdx);
extern void RadiosityBounce(float epsilon, int threadCount);
extern int FindLightingTransfersForDirection(int sampleIdx, Sample_t *sample, float *position, float *basis, float subAreaFactor, int dirIdx);
extern int FindLightingTransfers_inner(int sampleIdx, float *position, float *normal, float subAreaFactor, float skyFactor, unsigned char primaryLightIndex, SubSample_t *subSample);
extern int Relight_IsSuppressed(int triangleIndex, int areaIndex);
extern void Relight_RecordSuppressed(int triangleIndex, int areaIndex);



/* -------------------------------------------------------------------------------

Function Prototypes — geometry.c

------------------------------------------------------------------------------- */

extern float BuildTriangleNormal(Triangle_t *tri);
extern void AddTriangle(SurfaceInfo_t *surface, unsigned short materialIdx, void *texcoordData, void *material, int initialTriCount, int triIndex, unsigned char primaryLightIndex);
extern void AddTrianglesForSurface(SurfaceInfo_t *surface, int modelIndex, void *texcoordData);
extern int InitGeometry_AddVerts(float *position, float *texcoord);
extern void AddInvisibleOpaqueTriangle(MaterialDef_t *si, float *pos1, float *pos2, float *pos3, float *tc1, float *tc2, float *tc3);
extern int TestAlphaMask(MaterialDef_t *si, float *texcoord);
extern void InitGeometry_Reset(void);
extern void RayTriangleIntersect(RayTraceContext_t *ray, Triangle_t *tri);
extern int SweepPointThroughModelTriangle(float *plane, float *baryData, float *start, float *end, float *outBaryU, float *outBaryV);
extern int TraceShadowBatched(RayTraceContext_t *ray, Triangle_t **triArray, int triCount);
extern void TraceBSP_r(int nodeIndex, float *endPos, float *startPos, RayTraceContext_t *ray);
extern int TraceVisibility_r(int nodeIndex, float *endPos, float *startPos, RayTraceContext_t *ray);
extern void TraceSetup_and_Dispatch(int cacheIndex, float *startPos, float *endPos, RayHitResult_t *hitResult);
extern int TraceVisibility(int cacheIndex, float *startPos, float *endPos);
extern int ScoreTrianglesAgainstPlane(Triangle_t **triArray, int triCount, float *plane);
extern void GatherLightingSampleWithLock(float area, float *samplePos,
                                         float *polyVerts, int vertCount,
                                         void *userData, int areaIndex);
extern void SetLightingSampleAreas_Callback(int triIndex, int unused);
extern void BuildLightTransfers_Callback(int triIndex, int unused);
extern void SetLightingSampleAreas(int threadCount);
extern void BuildLightTransfers(int threadCount);
extern void ForEachLightmapPixel_Callback(int triIndex, int unused);
extern void ForEachLightmapPixelInPoly(void *callback, int param, int threadCount);
extern void GramSchmidt(float *v);
extern int FindBestAxisSplit(Triangle_t **triArray, int triCount, float *outPlane);
extern int FindBestSplitPlane(Triangle_t **triArray, int triCount, float *outPlane);
extern int BuildCollisionBSP_Partition(Triangle_t **triRefs, int triCount);
extern int ClassifyTriangleSide(float *plane, int side, Triangle_t **triArray, int triCount);
extern int BuildCollisionBSP_r(int nodeIndex, Triangle_t **triArray, int triCount);
extern void BuildCollisionBSP(void);
extern void SetupBSPNodes(void);
extern void InitGeometry(void);
extern void InitEmbreeScene(void);
extern int TraceVisibility_Embree(float *startPos, float *endPos);
extern int TraceStaticModels_Embree(float *startPos, float *endPos);
extern void TraceSetup_Embree(int cacheIndex, float *startPos, float *endPos, RayHitResult_t *hitResult);
extern void BuildLightTransfers_PerTri(int triIndex, int param, int transferCount, void *mapping);
extern int ComputeLinearMappingForTriangle(Triangle_t *tri, void *outMapping);
extern void ForEachLightingSampleInTriangle(Triangle_t *tri, int count, void *callback, void *userData);
extern void ProcessLightingSampleArea(float areaX2, float *centroid, float *polyVerts, int vertCount, void *userData, int areaIndex);



/* -------------------------------------------------------------------------------

Function Prototypes — lighting.c

------------------------------------------------------------------------------- */

extern void Lighting_AllocLightmapData(void);
extern void Lighting_RegisterLightmap(int lmapIndex);
extern void GetLightingSample(int lmapIndex, float sScaled, float tScaled, void **outSample);
extern void GetLightingSubSample(int lmapIndex, float sScaled, float tScaled, SubSample_t *outSubSample);
extern float DegammaColorChannel(float color);
extern float GammaCorrectColorChannel(float color);
extern void DegammaColor(float *color);
extern void Lighting_InitSamples(void);
extern void Lighting_ApplyRadiosityScale(LightingSample_t *sample);
extern void Lighting_InitAntiBleed(void);
extern void InitBleeding(int threadCount);
extern unsigned char EncodeGammaCorrectedByte(float value);
extern void BuildFinalLightmaps_TripleLoop(void);
extern void Lighting_GetGatheredLight(LightingSample_t *sample, float *outColor);
extern void AdjustLightingContrast(int sampleCount, int baseIndex, float *srcSamples, float *dstColors);
extern void BuildFinalLightmap_PerPixel(int lmapIndex, int col, int row);
extern unsigned char EncodeFloatInByte(float value);
extern void Lighting_AllocSampleVars(LightingSample_t *sample);
extern void Lighting_IncrementUsefulSampleCount(void);
extern void Lighting_SampleCallback_Trampoline1(int sampleIndex);
extern void Lighting_PixelCallback_Trampoline(int pixelIndex, int a2);
extern void ForEachUsefulLightingSample(void *callback, int a2);
extern void ForEachUsefulLightingSampleThreaded(void *callback, int threadCount);
extern void Lighting_ForEachPixel_Helper(void *callback, int a2);



/* -------------------------------------------------------------------------------

Function Prototypes — lightmap_bleed.c

------------------------------------------------------------------------------- */

extern float Bleed_GetSampleAreaX2_Secondary(void *data, int s, int t);
extern void Bleed_CopySampleX2_Primary(void *srcData, int s, int t, void *dstData);
extern void Bleed_ScaleSampleX2_Secondary(float scale, void *data, int s, int t);
extern void Bleed_ScaleSampleX2_Primary(float scale, void *data, int s, int t);
extern void Bleed_AddWeightedSample_Secondary(void *srcData, int srcS, int srcT, float weight, void *dstData, int dstS, int dstT);
extern void Bleed_AddWeightedSubSample_Primary(void *srcData, int srcS, int srcT, float weight, void *dstData, int dstS, int dstT);
extern void Lmap_FindBleeding_Callback(float unused, float *samplePos, void *data1, void *data2, void *userData);
extern void Bleed_CopySampleX2_Secondary(void *srcData, int s, int t, void *dstData);
extern void Lmap_InitBilinearBleeding(int lmapCount, int threadCount);
extern void Lmap_ApplyBleeding(void *lmapData, int lmapIdx);
extern void Lmap_FindBleedingForSample(void *data, int lmapIdx, int width, int height, float u, float v);
extern void Bleed_FillEmptySubSamples(void *data);



/* -------------------------------------------------------------------------------

Function Prototypes — linearmapping.c

------------------------------------------------------------------------------- */

extern int LU_Decompose3x3(double *matrix, int *permutation);
extern void LU_Solve3x3(double *luMatrix, int *permutation, double *b);
extern void SolveLinearMapping(double *origMatrix, double *luMatrix, int *permutation, float input0, float input1, float input2, float *output, int outIdx0, int outIdx1, int outIdx2);
extern int SetupLinearMapping(float *basis, float *uv0, float *uv1, float *uv2, LinearMappingData_t *output);
extern void OrientationDirToWorldDir(LinearMappingData_t *mapping, float dirX, float dirY, float dirZ, float *output);



/* -------------------------------------------------------------------------------

Function Prototypes — lightgrid.c

------------------------------------------------------------------------------- */

extern void CalculateLightGrid_Worker(int gridIndex, int flags);
extern void AllocGridTraceDirections(void);
extern void AddStaticModelLightGridSamples(void);
extern void GatherIncidentEnergyInSpaceForLightFromDir(float *lightColor, float *direction, float *outBuffer);
extern void CalculateLightGrid_GatherLight(int flags, int gridIndex, float *outBuffer);
extern short LightGrid_EncodeSH(float scale, float *buffer);
extern short LightGrid_FindOrInsertColor(unsigned char *colorData);
extern void CalculateLightGrid(int flags);
extern void CalculateLightGrid_Setup(void);
extern void CalculateLightGrid_SortPoints(void);
extern void TraceOctantSkyVisibility(int flags, float *pos, unsigned char *outResult);
extern void ExpandStaticModelOrigins(void);
extern int GridSamplePoint_CompareForSort(const void *a, const void *b);
extern int GridSamplePoint_Compare(const void *a, const void *b);
extern void AddStaticModelLightGridSample(int *data);



/* -------------------------------------------------------------------------------

Function Prototypes — pointlights.c

------------------------------------------------------------------------------- */

extern LightDef_t *LoadLightDef(const char *name);
extern void AddPointLight(int primaryLightIndex, float *origin, float radius, float *color, const char *defName);
extern void AddSpotLight(int primaryLightIndex, float *origin, float radius, float *color, const char *defName, float *spotDir, float outerCos, float innerCos, int exponent);
extern int GetPointLightCount(void);
extern int PointLightEvaluatePoint(int surfacePrimaryLightIndex, int traceIndex, int lightIndex, float *pos, float *normal,
                                   float *outDir, float *outColor, float *outDot);



/* -------------------------------------------------------------------------------

Function Prototypes — groundlight.c

------------------------------------------------------------------------------- */

extern void AddStaticModelToGroundLitList(void *entity, float *pos, float radius, float *dir);
extern int CalculateGroundLighting_Worker(float *pos, float radius, float *dir, float *outColor, float *outIntensity);
extern void CalculateGroundLightingForAllStaticModels(void);



/* -------------------------------------------------------------------------------

Function Prototypes — modelcollision.c

------------------------------------------------------------------------------- */

extern int TraceModelCollision_Node(int nodeIndex, float *start, float *end);
extern void BuildBSPSubdivision(int nodeIndex, float *mins, float *maxs);
extern void BuildModelCollision(float *mins, float *maxs);
extern CollisionMeshNode_t *BuildStaticModelCollisionMesh(void *model);
extern void CM_TraceBox(void *model, float *scale, ModelPlacement_t *placement, float *outCenter, float *outBoxHeight);
extern int TraceModelCollision_r(CollisionAabbTree_t *node, float *traceData);
extern int TraceModelCollision_Walk(CollisionInstance_t *inst, float *traceData);
extern int TraceStaticModels(float *start, float *end);

/* ---------------------------------------------------------------------------

Function Prototypes — cm_tracebox.c

------------------------------------------------------------------------------- */

extern void CM_CalcTraceExtents(float *trace);
extern int CM_TraceBoundsTest(float *traceData, float *absMins, float *absMaxs, float fraction);

/* -------------------------------------------------------------------------------

Function Prototypes — materials.c

------------------------------------------------------------------------------- */

extern void *LoadMaterial(const char *materialName);

/* -------------------------------------------------------------------------------

Function Prototypes — polyfile.c

------------------------------------------------------------------------------- */

extern void Map_ReadPolyFile(const char *filename);

/* -------------------------------------------------------------------------------

Function Prototypes — targetplatform.c

------------------------------------------------------------------------------- */

extern PlatformEntry_t *g_targetPlatform;
extern int SetTargetPlatformByName(const char *cmdlineSwitch);
extern int ValidatePlatformSet(void);

/* -------------------------------------------------------------------------------

Function Prototypes — aabbtree.c

------------------------------------------------------------------------------- */

extern int AabbFindBestSplitPlane(float *itemMins, float *itemMaxs, int *indices, int itemCount, int *outBestAxis, float *outSplitPos);
extern int AabbPartition(int itemCount, AabbTreeBuilder_t *builder, int *indices, int *outFrontCount, int *outMidStart);
extern void AabbCreateNode(AabbTreeNode_t *parent, AabbTreeBuilder_t *builder, int *indices, int offset, int count);
extern int AabbBuildTree_r(AabbTreeNode_t *node, AabbTreeBuilder_t *builder, int *indices);
extern int BuildAabbTree(AabbTreeBuilder_t *builder);



/* -------------------------------------------------------------------------------

Function Prototypes — poly2d.c

------------------------------------------------------------------------------- */

extern float AreaX2AndCentroidFor2dPoly(float *points, int numPoints, float *centroidOut);
extern void Split2dPolyAlongAxis(float *coords, int vertCount, int axis, float splitValue, float *coordsFront, int *frontCountOut, float *coordsBack, int *backCountOut);
extern void ForEach2dArea(void *polyArray, int numPolys, int gridSizeX, int numCellsY, float startX, float startY, float cellSizeX, float cellSizeY, ForEach2dAreaCallback callback, void *userData);



/* -------------------------------------------------------------------------------

Function Prototypes — mapio.c

------------------------------------------------------------------------------- */

extern const char *ValueForKey(Entity_t *entity, const char *key);
extern int ParseVectorString(Entity_t *entity, const char *key, float *outVector);
extern void ProcessBrushModelTriangles(Entity_t *entity, const char *modelValue);
extern void ProcessEntity(Entity_t *entity);
extern void ProcessLightEntity(Entity_t *entity);
extern void ProcessBrushModel(Entity_t *entity);
extern void GetEntityOriginAndAngles(Entity_t *entity, float *outTransform);
extern void ProcessEntities(Entity_t *entity);
extern void Map_Write(const char *filename);
extern char Map_Read(const char *filename);



/* -------------------------------------------------------------------------------

Function Prototypes — dobj.c

------------------------------------------------------------------------------- */

extern int DObjGetBoneIndex(const DObj_t *obj, unsigned int name);
extern void DObjCreateDuplicateParts(DObj_t *obj);
extern void DObjSetBounds(DObj_t *obj);
extern void DObjCreate(DObjModel_t *dobjModels, unsigned int numModels, struct XAnimTree_s *tree, DObj_t *obj, unsigned short entnum);
extern void DObjCreateSkel(DObj_t *obj, DSkel_t *skel, int time);
extern void DObjGetMatrices(DObj_t *obj, int *partBits, void *outMatrices);
extern XSurface_t *DObjGetSurface(const DObj_t *obj, int modelIndex, int surfIndex, int lod);
extern const char *DObjGetSurfaceName(const DObj_t *obj, int modelIndex, int surfIndex, int lod);
extern const char *DObjGetBoneName(const DObj_t *obj, int index);
extern void DObjDumpInfo(const DObj_t *obj);
extern void DObjCalcSkel(const DObj_t *obj, int *partBits);
extern void DObjCalcAnim(const DObj_t *obj, int *partBits);
extern int DObjGetSurfaces(const DObj_t *obj, unsigned short *surfMap, int maxSurfaces, int lod);

/* xanim_public.c — inlined helpers that MSVC emitted as standalone functions */
extern void DObjAnimMatToAxis(const DObjAnimMat_t *mat, float axis[3][3]);
extern void DObjAnimMatToSkelMat(const DObjAnimMat_t *mat, DObjSkelMat_t *skelMat);



/* -------------------------------------------------------------------------------

Function Prototypes — xmodel.c

------------------------------------------------------------------------------- */

extern int XModelBad(const XModel_t *model);
extern void XModelFree(XModel_t *model);
extern XModelSurfs_t *XModelSurfsFindData(const char *name);
extern void XModelSurfsSetData(const char *name, XModelSurfs_t *surfs, void *(*alloc)(int));
extern XModelParts_t *XModelPartsFindData(const char *name);
extern void XModelPartsSetData(const char *name, XModelParts_t *parts, void *(*alloc)(int));
extern XModel_t *XModelPrecache(const char *name, void *(*alloc)(int), void *(*allocColl)(int));
extern int XModelGetBoneIndex(const XModel_t *model, unsigned int name);
extern void XModelGetBounds(const XModel_t *model, float *mins, float *maxs);
extern void *XModel_AllocZeroed(int size);



/* -------------------------------------------------------------------------------

Function Prototypes — xmodel_load_obj.c

------------------------------------------------------------------------------- */

extern void XModelCalcBasePose(XModelParts_t *modelParts);
extern void XModelReadCompressedQuat(const unsigned char **pos, short *quat);
extern void XModelReadCollSurfs(const unsigned char **pos, XModel_t *model, void *(*alloc)(int), const char *name);
extern void R_XModelSurfsReadData(XModel_t *model, const char *surfFilename, XSurface_t **surfsArray, int *partBits, int numsurfs, const unsigned char **pos, void *(*alloc)(int));
extern XModel_t *XModelLoadFile(const char *name, void *(*alloc)(int), void *(*allocColl)(int));
extern int XModelSurfsLoad(XModel_t *model, void *(*alloc)(int));
extern int XModel_ReadHeader(const char *name, const unsigned char **pos, XModelConfig_t *config);



/* -------------------------------------------------------------------------------

Function Prototypes — xmodel_utils.c

------------------------------------------------------------------------------- */

extern XModel_t *XModelLoad(const char *name, void *(*alloc)(int), void *(*allocColl)(int));
extern const char *XModelGetName(const XModel_t *model);
extern int XModelGetSurfaces(const XModel_t *model, XSurface_t **surfaces, int lod, int **partBits);



/* -------------------------------------------------------------------------------

Function Prototypes — r_xsurface.c

------------------------------------------------------------------------------- */

extern int XSurfaceGetNumTris(XSurface_t *surface);
extern void R_XSurfaceCopyIndices(XSurface_t *surface, unsigned short *dstIndices, unsigned short baseIndex);
extern void R_XSurfaceDeformVerts(XSurface_t *surface, BoneMatrix_t *boneMats, float *outPositions, float *outTexcoords, float *outNormals);



/* -------------------------------------------------------------------------------

Function Prototypes — r_xsurface_load_obj.c

------------------------------------------------------------------------------- */

extern XSurface_t *R_XSurfaceLoadObj(XModel_t *model, int *partBits, const unsigned char **pos, void *(*alloc)(int));



/* -------------------------------------------------------------------------------

Function Prototypes — r_image_wavelet.c

------------------------------------------------------------------------------- */

extern void Wavelet_ReadBits(int bitCount, WaveletDecodeState_t *state);
extern void Wavelet_DecodeCoefficients(unsigned char *dst, int bytesPerPixel, WaveletDecodeState_t *state);
extern void Wavelet_Decompress(unsigned char *src, unsigned char *dst, WaveletDecodeState_t *decode);



/* -------------------------------------------------------------------------------

Function Prototypes — r_imagedecode.c

------------------------------------------------------------------------------- */

extern void DecodeDXT1Block(unsigned char *src, ImageDecodeState_t *dst, int blockX, int blockY, int hasAlpha);
extern void DecodeDXT1(unsigned char *src, ImageDecodeState_t *dst, int blockX, int blockY);
extern void DecodeDXT3(unsigned char *src, ImageDecodeState_t *dst, int blockX, int blockY);
extern void DecodeDXT5(unsigned char *src, ImageDecodeState_t *dst, int blockX, int blockY);
extern void Image_ConvertPixels(ImageDecodeState_t *dst, ImageInfo_t *srcInfo, unsigned char *srcData);
extern void Image_DecodeCompressed(ImageDecodeState_t *dst, ImageInfo_t *srcInfo, unsigned char *srcData, int srcLen);
extern void Image_DecodeDXT(ImageDecodeState_t *dst, ImageInfo_t *srcInfo, unsigned char *srcData, int srcLen);
extern void Image_LoadIWI(const char *imageName, ImageDecodeState_t *dst);
/* gfx_d3d/r_light_load_obj.cpp: resolve the single attenuation image
 * referenced by a `lights/<name>` asset. */
extern int LoadLightDefImages(const char *name, ImageDecodeState_t *outImage);
extern void Image_DecodeUncompressed(ImageDecodeState_t *dst, ImageInfo_t *srcInfo, unsigned char *srcData, int srcLen);



/* -------------------------------------------------------------------------------

Function Prototypes — q_shared.c

------------------------------------------------------------------------------- */

extern int ShortSwap(int l);
extern int ShortNoSwap(int l);
extern int LongSwap(int l);
extern int LongNoSwap(int l);
extern unsigned long long Long64Swap(long long ll);
extern long long Long64NoSwap(long long ll);
extern float FloatReadSwap(int f);
extern float FloatReadNoSwap(int f);
extern int FloatWriteSwap(float f);
extern int FloatWriteNoSwap(float f);
extern void Swap_Init(void);
extern char *va(const char *format, ...);
extern int CanKeepStringPointer(const char *ptr);
extern void Com_StripExtension(const char *in, char *out);
extern char *I_strncpyz(char *dest, const char *src, int destsize);
extern int Com_sprintf(char *dest, int size, const char *format, ...);
extern int I_islower(int c);
extern int I_strncmp(const char *s1, const char *s2, int n);
extern int I_strcmp(const char *s0, const char *s1);
extern int I_stricmp(const char *s0, const char *s1);
extern int I_strnicmp(const char *s0, const char *s1, int n);
extern char *I_strlwr(char *s);
extern void Com_AssembleFilepath(const char *folder, const char *name, const char *extension, char *path, int maxCharCount);
extern int I_stristr(const char *wild, const char *s);
extern int Com_Filter(const char *filter, const char *name, int casesensitive);
extern int Com_FilterPath(const char *filter, const char *name, int casesensitive);
extern void MatrixTransformPoint(float *mat, float *pos, float *out);
extern void MatrixTransformDirection(float *mat, float *dir, float *out);
extern void MatrixTransformVector3(float *in1, float *mat, float *out);
extern short BigShort(short value);
extern int BigLong(int value);
extern float BigFloat(float value);
extern void Swap_Init_BigEndian(void);



/* -------------------------------------------------------------------------------

Function Prototypes — q_parse.c

------------------------------------------------------------------------------- */

extern void Com_BeginParseSession(const char *name);
extern char *Com_ParseInternal(char **data_p, int crossline);
extern char *Com_Parse(char **data_p);
extern char *Com_ParseOnLine(char **data_p);
extern char *Com_ParseCSV(char **data_p, int crossline);



/* -------------------------------------------------------------------------------

Function Prototypes — dvar.c

------------------------------------------------------------------------------- */

extern char *Dvar_CopyString(const char *string);
extern void Dvar_FreeCurrentStringValue(dvar_t *dvar);
extern void Dvar_FreeLatchedStringValue(dvar_t *dvar);
extern void Dvar_AssignCurrentStringValue(dvar_t *dvar, const char *string);
extern void Dvar_AssignResetStringValue(dvar_t *dvar, const char *string);
extern const char *Dvar_ValueToString(dvar_t *dvar, DvarValue_t value);
extern int Dvar_StringToBool(const char *string);
extern float Dvar_StringToFloat(const char *string);
extern int Dvar_StringToInt(const char *string);
extern float *Dvar_StringToVec2(const char *string);
extern float *Dvar_StringToVec3(const char *string);
extern float *Dvar_StringToVec4(const char *string);
extern int Dvar_StringToEnum(DvarLimits_t *domain, const char *string);
extern void Dvar_StringToColor(const char *string, unsigned char *color);
extern int Dvar_ValueInDomain(unsigned char type, DvarValue_t value, DvarLimits_t domain);
extern int Dvar_ValuesEqual(unsigned char type, DvarValue_t val0, DvarValue_t val1);
extern void Dvar_ClearModified(dvar_t *dvar);
extern dvar_t *Dvar_FindVar(const char *name);
extern dvar_t *Dvar_RegisterBool(const char *name, int value, unsigned short flags);
extern dvar_t *Dvar_RegisterInt(const char *name, int value, int min, int max, unsigned short flags);
extern dvar_t *Dvar_RegisterString(const char *name, const char *value, unsigned short flags);
extern void Dvar_SetStringValue(dvar_t *dvar, const char *string, int source);
extern void Dvar_SetStringByName(const char *name, const char *value);
extern void Dvar_Init(void);
extern void Dvar_MakeExplicitType(dvar_t *dvar);
extern int Dvar_DomainToString_Vector(int components, float *domain, char *outBuffer, int outBufferLen);
extern char *Dvar_DomainToString(unsigned char type, DvarLimits_t domain, char *outBuffer, int outBufferLen, int *outLineCount);
extern void Dvar_SetResetValue(dvar_t *dvar, DvarValue_t value);
extern void Dvar_SetLatchedVariant(dvar_t *dvar, DvarValue_t value);
extern void Dvar_SetCurrentVariant(dvar_t *dvar, DvarValue_t value);
extern void Dvar_SetVariant(dvar_t *dvar, DvarValue_t value, int source);
extern void Dvar_ReRegister(dvar_t *dvar, const char *name, unsigned char type, unsigned short flags, DvarValue_t resetValue, DvarLimits_t *domain);
extern void Dvar_UpdateReRegister(dvar_t *dvar, const char *name, unsigned char type, unsigned short flags, DvarValue_t resetValue, DvarLimits_t *domain);
extern void Dvar_Reregister(dvar_t *dvar, const char *name, unsigned char type, unsigned short flags, DvarValue_t resetValue, DvarLimits_t *domain);
extern dvar_t *Dvar_RegisterNew(const char *name, unsigned char type, unsigned short flags, DvarValue_t value, DvarLimits_t *domain);
extern dvar_t *Dvar_RegisterVariant(const char *name, unsigned char type, unsigned short flags, DvarValue_t value, DvarLimits_t *domain);
extern dvar_t *Dvar_Register_internal(const char *dvarName, const char *value, unsigned short flags);



/* -------------------------------------------------------------------------------

Function Prototypes — scr_memorytree.c

------------------------------------------------------------------------------- */

extern void MT_Init(void);
extern unsigned short MT_AllocIndex(int numBytes, int context);
extern void MT_FreeIndex(unsigned int nodeNum, int numBytes);
extern void MT_Free(void *ptr, int numBytes);
extern void MT_FreeBitmapUnused(unsigned char *bitmap);



/* -------------------------------------------------------------------------------

Function Prototypes — scr_stringlist.c

------------------------------------------------------------------------------- */

extern void SL_InitOrShutdown(void);
extern void SL_Init(void);
extern void SL_FreeString(unsigned int stringValue, ScriptStringRef *refStr, unsigned int len);
extern unsigned int SL_GetString(const char *str, unsigned int user, int context);
extern unsigned int SL_GetStringOfLen(const char *str, unsigned int user, unsigned int len, int context);
extern unsigned int SL_FindString(const char *str);
extern unsigned int SL_FindStringOfLen(const char *str, unsigned int len);
extern const char *SL_ConvertToString(unsigned int stringValue);
extern const char *SL_DebugConvertToString(unsigned int stringValue);
extern void SL_Shutdown(void);
extern void SL_RemoveRefToString(unsigned int stringValue);
extern char *SL_InternString(const char *str);



/* -------------------------------------------------------------------------------

Function Prototypes — threads.c

------------------------------------------------------------------------------- */

extern void ForEachQuantum(unsigned int count, void (*workFunc)(unsigned int, unsigned int), unsigned int threadCount);
extern void AcquireThreadLock(unsigned int lockIndex);
extern void ReleaseThreadLock(unsigned int lockIndex);



/* -------------------------------------------------------------------------------

Function Prototypes — win_common.c

------------------------------------------------------------------------------- */

extern int Sys_Mkdir(const char *path);
extern const char *Sys_DefaultCDPath(void);
extern const char *Sys_DefaultHomePath(void);
extern char *Sys_Cwd(void);
extern char **Sys_ListFiles(const char *directory, const char *extension, char *filter, int *numfiles, int wantsubs);
extern void Sys_FreeFileList(char **list);
extern int Sys_DirectoryHasContents(const char *directory);



/* -------------------------------------------------------------------------------

Function Prototypes — misc externs (CRT thunks, assert)

------------------------------------------------------------------------------- */

extern int AssertFailed(const char *expr, const char *file, int line, int skip, int type);
/* 2-arg Assert used throughout source files: Assert(condition, disableFlag) */
#define Assert(cond, disableFlag) \
    ((cond) || (disableFlag) ? (void)0 : (void)AssertFailed(#cond, __FILE__, __LINE__, 0, 1))
#define AssertVar(cond, disableFlag) \
    ((cond) || (disableFlag) ? (void)0 : \
     (AssertFailed(#cond, __FILE__, __LINE__, 0, 1) == 2 ? (void)((disableFlag) = 1) : (void)0))
extern void Com_Printf(const char *fmt, ...);
extern void *Malloc(int size);
extern void LockMutex(void *obj);
extern void UnlockMutex(void *obj);
extern void SetPrintCallback(void *callback);
extern void DefaultPrintCallback(const char *msg);
extern void ForEachLightmapPixel(int count, void *callback, int a3);
extern void SphericalToCartesian(float theta, float phi, float *outDir);



#endif /* COD2RAD64_H */
