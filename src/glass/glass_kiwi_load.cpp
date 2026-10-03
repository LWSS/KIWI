// KIWI (GLASS): builds the Glasses table at map load (BO1's came from a linker asset) from
// the "glass" entities' submodels, and loads GlassDefs. Server and client must build the same
// list (snapshots index panes by position), so it depends on BSP data only.

#include "glass_kiwi.h"
#include "glass_kiwi_defaults.h"
#include "glass_kiwi_slab.h"
#include "glass_load_obj.h"
#include "glass_server.h"
#include "glass_client.h"

#include <client/client.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <physics/phys_local.h>
#include <universal/com_files.h>
#include <universal/com_memory.h>
#include <universal/q_parse.h>
#include <xanim/xanim.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_init.h>
#include <EffectsCore/fx_system.h>

extern GlassesServer svGlasses;
char *__cdecl CM_EntityString(); // game/g_load_utils.cpp

#define GLASS_KIWI_MAX_POLY_VERTS   1024

struct GlassKiwiDef
{
    GlassDef def; // must stay first: Glass::glassDef points at it
    char name[64];
    char pristineMaterial[64]; // blank = the pane's own compiled material
    char crackedMaterial[64];
    char shardMaterial[64];
    char crackSound[64];
    char shatterSound[64];
    char autoShatterSound[64];
    char crackEffect[64];
    char shatterEffect[64];
    float uvScale;
    int frameEdges;
    bool fileFound;
};

// one GlassDef per (def file, pristine material): a blank pristine resolves per pane
struct GlassKiwiClientDef
{
    GlassDef def;
    const GlassKiwiDef *kiwiDef;
};

static Glasses s_glassKiwiList;
static Glass *s_glassKiwiPanes;
static GlassKiwiDef *s_glassKiwiDefs;
static unsigned int s_glassKiwiNumDefs;
static unsigned int s_glassKiwiMaxDefs;
static GlassKiwiClientDef *s_glassKiwiClientDefs;
static unsigned int s_glassKiwiNumClientDefs;
static float (*s_glassKiwiOutlines)[2];
static char s_glassKiwiMapName[64];
static unsigned int s_glassKiwiChecksum;
static bool s_glassKiwiBuilt;
static bool s_glassKiwiCmdsAdded;
static const dvar_s *glassShowNames;
static cmd_function_s GlassKiwi_List_VAR;

static void *GlassKiwi_Alloc(int size)
{
    void *mem = Z_Malloc(size, "GlassKiwi", 9);
    memset(mem, 0, size);
    return mem;
}

static void GlassKiwi_Free(void *mem)
{
    if (mem)
        Z_Free(mem, 9);
}

/*
==============
GlassKiwi_SetDefDefaults
==============
*/
static void GlassKiwi_SetDefDefaults(GlassKiwiDef *kd, const char *name)
{
    memset(kd, 0, sizeof(*kd));
    I_strncpyz(kd->name, name, sizeof(kd->name));
    I_strncpyz(kd->crackedMaterial, GLASS_KIWI_DEFAULT_CRACKED_MATERIAL, sizeof(kd->crackedMaterial));
    I_strncpyz(kd->shardMaterial, GLASS_KIWI_DEFAULT_SHARD_MATERIAL, sizeof(kd->shardMaterial));
    I_strncpyz(kd->crackSound, GLASS_KIWI_DEFAULT_CRACK_SOUND, sizeof(kd->crackSound));
    I_strncpyz(kd->shatterSound, GLASS_KIWI_DEFAULT_SHATTER_SOUND, sizeof(kd->shatterSound));
    I_strncpyz(kd->autoShatterSound, GLASS_KIWI_DEFAULT_SHATTER_SOUND, sizeof(kd->autoShatterSound));
    I_strncpyz(kd->shatterEffect, GLASS_KIWI_DEFAULT_SHATTER_EFFECT, sizeof(kd->shatterEffect));
    kd->uvScale = 1.0f / 64.0f;     // BO1's glass numbers (every BO1 glass def uses them)
    kd->frameEdges = 1;

    kd->def.maxHealth = 120;
    kd->def.minShardSize = 40.0f;
    kd->def.maxShardSize = 200.0f;
    kd->def.shardLifeProbablility = 0.5f;
    kd->def.maxShards = 0;
}

static void GlassKiwi_InfoString(const char *info, const char *key, char *out, int outSize)
{
    const char *value = Info_ValueForKey(info, key);
    if (value && *value)
        I_strncpyz(out, value, outSize);
}

static float GlassKiwi_InfoFloat(const char *info, const char *key, float defaultValue)
{
    const char *value = Info_ValueForKey(info, key);
    return (value && *value) ? (float)atof(value) : defaultValue;
}

static int GlassKiwi_InfoInt(const char *info, const char *key, int defaultValue)
{
    const char *value = Info_ValueForKey(info, key);
    return (value && *value) ? atoi(value) : defaultValue;
}

/*
==============
GlassKiwi_LoadDefFile

raw/glass/<name> (a rawfile in fastfiles): "GLASSDEF\key\value\..." (raw/physic layout).
A missing or bad file keeps the built-in defaults; the pane list doesn't change.
==============
*/
static void GlassKiwi_LoadDefFile(GlassKiwiDef *kd)
{
    char path[MAX_QPATH];
    char *buffer;
    const char *info;

    if (Com_sprintf(path, sizeof(path), "glass/%s", kd->name) < 0)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass def name '%s' too long\n", kd->name);
        return;
    }

    buffer = Com_LoadRawTextFile(path);
    if (!buffer)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: glass def '%s' not found, using defaults\n", kd->name);
        return;
    }

    info = buffer;
    if (strncmp(info, "GLASSDEF", 8) || !Info_Validate(info + 8))
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: file [%s] is not a glass def file\n", path);
        Com_UnloadRawTextFile(buffer);
        return;
    }
    info += 8;

    kd->def.maxHealth = GlassKiwi_InfoInt(info, "maxHealth", kd->def.maxHealth);
    kd->def.minShardSize = GlassKiwi_InfoFloat(info, "minShardSize", kd->def.minShardSize);
    kd->def.maxShardSize = GlassKiwi_InfoFloat(info, "maxShardSize", kd->def.maxShardSize);
    kd->def.shardLifeProbablility = GlassKiwi_InfoFloat(info, "shardLifeProbability", kd->def.shardLifeProbablility);
    kd->def.maxShards = GlassKiwi_InfoInt(info, "maxShards", kd->def.maxShards);
    kd->uvScale = GlassKiwi_InfoFloat(info, "uvScale", kd->uvScale);
    kd->frameEdges = GlassKiwi_InfoInt(info, "frameEdges", kd->frameEdges);

    GlassKiwi_InfoString(info, "pristineMaterial", kd->pristineMaterial, sizeof(kd->pristineMaterial));
    GlassKiwi_InfoString(info, "crackedMaterial", kd->crackedMaterial, sizeof(kd->crackedMaterial));
    GlassKiwi_InfoString(info, "shardMaterial", kd->shardMaterial, sizeof(kd->shardMaterial));
    GlassKiwi_InfoString(info, "crackSound", kd->crackSound, sizeof(kd->crackSound));
    GlassKiwi_InfoString(info, "shatterSound", kd->shatterSound, sizeof(kd->shatterSound));
    GlassKiwi_InfoString(info, "autoShatterSound", kd->autoShatterSound, sizeof(kd->autoShatterSound));
    GlassKiwi_InfoString(info, "crackEffect", kd->crackEffect, sizeof(kd->crackEffect));
    GlassKiwi_InfoString(info, "shatterEffect", kd->shatterEffect, sizeof(kd->shatterEffect));

    if (kd->def.maxHealth < 1)
        kd->def.maxHealth = 1;
    if (kd->uvScale <= 0.0f)
        kd->uvScale = 1.0f / 64.0f;

    kd->fileFound = true;
    Com_UnloadRawTextFile(buffer);
}

static void GlassKiwi_FinishDef(GlassKiwiDef *kd)
{
    kd->def.name = kd->name;
    kd->def.crackSound = kd->crackSound;
    kd->def.shatterShound = kd->shatterSound;
    kd->def.autoShatterShound = kd->autoShatterSound;
}

static GlassKiwiDef *GlassKiwi_FindOrLoadDef(const char *name)
{
    GlassKiwiDef *kd;
    unsigned int i;

    if (!name || !*name)
        name = GLASS_KIWI_DEFAULT_DEF;

    for (i = 0; i < s_glassKiwiNumDefs; ++i)
    {
        if (!I_stricmp(s_glassKiwiDefs[i].name, name))
            return &s_glassKiwiDefs[i];
    }

    iassert(s_glassKiwiNumDefs < s_glassKiwiMaxDefs);
    kd = &s_glassKiwiDefs[s_glassKiwiNumDefs++];
    GlassKiwi_SetDefDefaults(kd, name);
    GlassKiwi_LoadDefFile(kd);
    GlassKiwi_FinishDef(kd);
    return kd;
}

struct GlassKiwiBrushCollect
{
    const cbrush_t *brushes[4];
    int numBrushes;
};

static void __cdecl GlassKiwi_CollectBrush(const cbrush_t *brush, void *userData)
{
    GlassKiwiBrushCollect *collect = (GlassKiwiBrushCollect *)userData;
    int i;

    for (i = 0; i < collect->numBrushes && i < ARRAY_COUNT(collect->brushes); ++i)
    {
        if (collect->brushes[i] == brush)
            return;
    }
    if (collect->numBrushes < ARRAY_COUNT(collect->brushes))
        collect->brushes[collect->numBrushes] = brush;
    ++collect->numBrushes;
}

static void GlassKiwi_GetSidePlane(const cbrush_t *brush, const float (*axialPlanes)[4], unsigned int sideIndex, float *plane)
{
    if (sideIndex < 6)
    {
        plane[0] = axialPlanes[sideIndex][0];
        plane[1] = axialPlanes[sideIndex][1];
        plane[2] = axialPlanes[sideIndex][2];
        plane[3] = axialPlanes[sideIndex][3];
    }
    else
    {
        const cplane_s *p = brush->sides[sideIndex - 6].plane;
        plane[0] = p->normal[0];
        plane[1] = p->normal[1];
        plane[2] = p->normal[2];
        plane[3] = p->dist;
    }
}

/*
==============
GlassKiwi_BuildPane

Fills one Glass from the pane's single slab brush. Returns false (and says why) on
a geometric failure; those are the only panes the builder skips.
==============
*/
static bool GlassKiwi_BuildPane(Glass *glass, unsigned int brushModel, const float *origin, int entityIndex)
{
    cmodel_t *cmodel;
    GlassKiwiBrushCollect collect;
    float bmins[3], bmaxs[3];
    float axialPlanes[6][4];
    Poly polys[GLASS_KIWI_MAX_POLYS];
    float polyVerts[GLASS_KIWI_MAX_POLY_VERTS][3];
    GlassKiwiSlabFace faces[GLASS_KIWI_MAX_POLYS];
    GlassKiwiSlab slab;
    const cbrush_t *brush;
    unsigned int sideCount, side, i;
    int k;

    if (!CM_ClipHandleIsValid(brushModel) || brushModel == 0 || brushModel >= cm.numSubModels)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d: bad brush model *%u\n", entityIndex, brushModel);
        return false;
    }

    cmodel = CM_ClipHandleToModel(brushModel);
    if (!cmodel->leaf.brushContents || !cmodel->leaf.leafBrushNode)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): no brushes\n", entityIndex, brushModel);
        return false;
    }

    // the node subtree belongs to this submodel only, so walk all of it
    collect.numBrushes = 0;
    for (k = 0; k < 3; ++k)
    {
        bmins[k] = -FLT_MAX;
        bmaxs[k] = FLT_MAX;
    }
    CM_ForEachBrushInLeafBrushNode_r(&cm.leafbrushNodes[cmodel->leaf.leafBrushNode], bmins, bmaxs, false, 0, GlassKiwi_CollectBrush, &collect);
    if (collect.numBrushes != 1)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): has %d brushes, a pane must be exactly one slab brush\n",
            entityIndex, brushModel, collect.numBrushes);
        return false;
    }
    brush = collect.brushes[0];

    sideCount = brush->numsides + 6;
    if (sideCount > GLASS_KIWI_MAX_POLYS)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): brush has too many sides (%u)\n", entityIndex, brushModel, brush->numsides);
        return false;
    }

    CM_BuildAxialPlanes(brush, &axialPlanes);
    Phys_BuildWindingsForBrush2(brush, polys, GLASS_KIWI_MAX_POLYS, polyVerts, GLASS_KIWI_MAX_POLY_VERTS);

    for (side = 0; side < sideCount; ++side)
    {
        faces[side].pts = (const float *)polys[side].pts;
        faces[side].ptCount = polys[side].ptCount;
        faces[side].ptStride = 3;
        GlassKiwi_GetSidePlane(brush, axialPlanes, side, faces[side].plane);
    }

    switch (GlassKiwi_BuildSlab(faces, sideCount, origin, &slab))
    {
    case GLASS_SLAB_OK:
        break;
    case GLASS_SLAB_NO_FACES:
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): brush has no faces\n", entityIndex, brushModel);
        return false;
    case GLASS_SLAB_NOT_SLAB:
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): back face is %.2f deg off parallel to the largest face (max %.0f), a pane must be a flat slab\n",
            entityIndex, brushModel, slab.wedgeDeg, GLASS_KIWI_MAX_WEDGE_DEG);
        return false;
    case GLASS_SLAB_THICKNESS:
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): pane is %.2f units thick, must be %.1f..%.0f\n",
            entityIndex, brushModel, slab.thickness, GLASS_KIWI_MIN_THICKNESS, GLASS_KIWI_MAX_THICKNESS);
        return false;
    case GLASS_SLAB_POINT_COUNT:
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): front face has %u points, must be 3..%d\n",
            entityIndex, brushModel, slab.numPts, GLASS_KIWI_MAX_OUTLINE);
        return false;
    case GLASS_SLAB_NO_AREA:
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d (*%u): front face has no area\n", entityIndex, brushModel);
        return false;
    }

    glass->brushModel = brushModel;
    glass->origin[0] = origin[0];
    glass->origin[1] = origin[1];
    glass->origin[2] = origin[2];
    glass->angles[0] = 0.0f;
    glass->angles[1] = 0.0f;
    glass->angles[2] = 0.0f;

    // world bounds of every face, grown a little
    glass->absmin[0] = glass->absmin[1] = glass->absmin[2] = FLT_MAX;
    glass->absmax[0] = glass->absmax[1] = glass->absmax[2] = -FLT_MAX;
    for (side = 0; side < sideCount; ++side)
    {
        for (i = 0; i < polys[side].ptCount; ++i)
        {
            float p[3];

            Vec3Add(polys[side].pts[i], origin, p);
            Vec3Min(glass->absmin, p, glass->absmin);
            Vec3Max(glass->absmax, p, glass->absmax);
        }
    }
    for (k = 0; k < 3; ++k)
    {
        glass->absmin[k] -= 0.5f;
        glass->absmax[k] += 0.5f;
    }

    glass->numOutlineVerts = (unsigned __int8)slab.numPts;
    for (i = 0; i < slab.numPts; ++i)
    {
        glass->outline[i][0] = slab.outline[i][0];
        glass->outline[i][1] = slab.outline[i][1];
    }
    memcpy(glass->outlineAxis, slab.axis, sizeof(slab.axis));
    glass->outlineOrigin[0] = slab.centroid[0];
    glass->outlineOrigin[1] = slab.centroid[1];
    glass->outlineOrigin[2] = slab.centroid[2];
    // negative: back face inside the slab, keeps BO1's shard caps and winding facing out
    glass->thickness = -slab.thickness;
    return true;
}

static bool GlassKiwi_ParseVec3(const char *value, float *out)
{
    return sscanf(value, "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

static unsigned int GlassKiwi_CountGlassEntities(const char *entityString)
{
    const char *ptr = entityString;
    const char *token;
    unsigned int count = 0;

    while (1)
    {
        token = (const char *)Com_Parse(&ptr);
        if (!ptr)
            break;
        if (!I_stricmp(token, "classname"))
        {
            token = (const char *)Com_Parse(&ptr);
            if (!ptr)
                break;
            if (!I_stricmp(token, "glass"))
                ++count;
        }
    }
    return count;
}

/*
==============
GlassKiwi_FreeGlasses
==============
*/
void GlassKiwi_FreeGlasses()
{
    GlassKiwi_PhysReset();
    GlassKiwi_Free(s_glassKiwiPanes);
    GlassKiwi_Free(s_glassKiwiOutlines);
    GlassKiwi_Free(s_glassKiwiDefs);
    GlassKiwi_Free(s_glassKiwiClientDefs);
    s_glassKiwiPanes = 0;
    s_glassKiwiOutlines = 0;
    s_glassKiwiDefs = 0;
    s_glassKiwiClientDefs = 0;
    s_glassKiwiNumDefs = 0;
    s_glassKiwiMaxDefs = 0;
    s_glassKiwiNumClientDefs = 0;
    memset(&s_glassKiwiList, 0, sizeof(s_glassKiwiList));
    s_glassKiwiMapName[0] = 0;
    s_glassKiwiChecksum = 0;
    s_glassKiwiBuilt = false;
}

// BO1 per-map sizes: 40 shards + 11000 bytes per pane, 1000 physics, 65536 / 131072 verts / indices.
// One group per shard (resting shards sit one per cell; a short group pool made shatters empty).
static void GlassKiwi_SetCapacities(Glasses *list)
{
    unsigned int shardMemorySize;

    list->maxShards = 40 * list->numGlasses > 2048 ? 40 * list->numGlasses : 2048;
    list->maxGroups = list->maxShards;
    list->maxPhysics = 1000;
    list->smallAllocatorBlocks = list->maxGroups + 2 * list->maxShards + list->maxPhysics + 256;
    shardMemorySize = list->numGlasses * 8192 + list->maxShards * 512;
    if (shardMemorySize < 11000 * list->numGlasses)
        shardMemorySize = 11000 * list->numGlasses;
    list->shardMemorySize = (shardMemorySize + 0xFFFF) & ~0xFFFFu;
    list->workMemory = 0;
    list->workMemorySize = 0;
    list->maxFreeCmd = 0;
    list->numSlots = 0;
    list->numVerts = 65536;
    list->numIndices = 131072;
}

/*
==============
GlassKiwi_Build
==============
*/
static void GlassKiwi_Build()
{
    const char *entityString;
    const char *ptr;
    const char *token;
    unsigned int maxPanes;
    unsigned int numPanes;
    int entityIndex;
    char key[64];
    char value[1024];

    GlassKiwi_FreeGlasses();
    I_strncpyz(s_glassKiwiMapName, cm.name ? cm.name : "", sizeof(s_glassKiwiMapName));
    s_glassKiwiChecksum = cm.checksum;
    s_glassKiwiBuilt = true;
    s_glassKiwiList.name = s_glassKiwiMapName;

    entityString = CM_EntityString();
    maxPanes = GlassKiwi_CountGlassEntities(entityString);
    if (!maxPanes)
        return;
    if (maxPanes > GLASS_KIWI_MAX_PANES)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: map has %u glass panes, max is %d; extra panes are ignored\n", maxPanes, GLASS_KIWI_MAX_PANES - 1);
        maxPanes = GLASS_KIWI_MAX_PANES - 1;
    }

    s_glassKiwiPanes = (Glass *)GlassKiwi_Alloc(sizeof(Glass) * maxPanes);
    s_glassKiwiOutlines = (float (*)[2])GlassKiwi_Alloc(sizeof(float[2]) * GLASS_KIWI_MAX_OUTLINE * maxPanes);
    s_glassKiwiDefs = (GlassKiwiDef *)GlassKiwi_Alloc(sizeof(GlassKiwiDef) * maxPanes);
    s_glassKiwiMaxDefs = maxPanes;

    numPanes = 0;
    entityIndex = 0;
    ptr = entityString;
    while (numPanes < maxPanes)
    {
        bool isGlass = false;
        bool hasOrigin = false;
        bool hasAngles = false;
        int brushModel = -1;
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float uvScale = -1.0f;
        int frameEdges = -1;
        char defName[64] = { 0 };
        Glass *glass;
        GlassKiwiDef *kd;

        token = (const char *)Com_Parse(&ptr);
        if (!ptr || *token != '{')
            break;
        while (1)
        {
            token = (const char *)Com_Parse(&ptr);
            if (!ptr || *token == '}')
                break;
            I_strncpyz(key, token, sizeof(key));
            token = (const char *)Com_Parse(&ptr);
            if (!ptr)
                break;
            I_strncpyz(value, token, sizeof(value));

            if (!I_stricmp(key, "classname"))
                isGlass = !I_stricmp(value, "glass");
            else if (!I_stricmp(key, "model") && value[0] == '*')
                brushModel = atoi(value + 1);
            else if (!I_stricmp(key, "origin"))
                hasOrigin = GlassKiwi_ParseVec3(value, origin);
            else if (!I_stricmp(key, "glassdef"))
                I_strncpyz(defName, value, sizeof(defName));
            else if (!I_stricmp(key, "uvscale"))
                uvScale = (float)atof(value);
            else if (!I_stricmp(key, "frameedges"))
                frameEdges = atoi(value);
            else if (!I_stricmp(key, "angles") && I_stricmp(value, "0 0 0"))
                hasAngles = true;
        }

        if (isGlass)
        {
            glass = &s_glassKiwiPanes[numPanes];
            glass->outline = &s_glassKiwiOutlines[GLASS_KIWI_MAX_OUTLINE * numPanes];
            if (hasAngles)
                Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: glass entity %d has \"angles\"; panes are compiled in world orientation, ignored\n", entityIndex);
            if (!hasOrigin)
                Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: glass entity %d has no \"origin\"\n", entityIndex);

            if (brushModel > 0 && GlassKiwi_BuildPane(glass, (unsigned int)brushModel, origin, entityIndex))
            {
                kd = GlassKiwi_FindOrLoadDef(defName);
                glass->glassDef = &kd->def;
                glass->index = numPanes;
                glass->uvScale = (uvScale > 0.0f) ? uvScale : kd->uvScale;
                glass->isPlanar = (frameEdges >= 0) ? (frameEdges != 0) : (kd->frameEdges != 0);
                ++numPanes;
            }
            else if (brushModel <= 0)
            {
                Com_PrintError(CON_CHANNEL_SYSTEM, "ERROR: glass entity %d has no brush model\n", entityIndex);
            }
        }
        ++entityIndex;
    }

    s_glassKiwiList.numGlasses = numPanes;
    s_glassKiwiList.glasses = s_glassKiwiPanes;
    GlassKiwi_SetCapacities(&s_glassKiwiList);
    Com_Printf(CON_CHANNEL_SYSTEM, "Glass: %u panes, %u glass defs\n", numPanes, s_glassKiwiNumDefs);
}

/*
==============
GlassKiwi_DrawNames

glassShowNames 1: every pane's glass def (its texture set) floats above it, for
comparing sets side by side on a test map.
==============
*/
void GlassKiwi_DrawNames()
{
    unsigned int i;
    float xyz[3];

    if (!glassShowNames || !glassShowNames->current.enabled || !s_glassKiwiBuilt)
        return;

    for (i = 0; i < s_glassKiwiList.numGlasses; ++i)
    {
        const Glass *glass = &s_glassKiwiList.glasses[i];

        xyz[0] = (glass->absmin[0] + glass->absmax[0]) * 0.5f;
        xyz[1] = (glass->absmin[1] + glass->absmax[1]) * 0.5f;
        xyz[2] = glass->absmax[2] + 4.0f;
        // client bucket, one frame: this runs every client frame (the 5-arg BO1 form is server/duration 0)
        CL_AddDebugString(xyz, colorWhite, 0.35f, glass->glassDef->name, 0, 1);
    }
}

static void __cdecl GlassKiwi_List_f()
{
    static const char *stateNames[5] = { "pristine", "cracked", "shattered", "invalid", "-" };
    unsigned int i;
    unsigned int svState;
    unsigned int clState;

    if (!s_glassKiwiBuilt)
    {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "No glass list built\n");
        return;
    }
    Com_Printf(CON_CHANNEL_DONT_FILTER, "%u glass panes on %s (sv = server state, cl = client state)\n", s_glassKiwiList.numGlasses, s_glassKiwiMapName);
    for (i = 0; i < s_glassKiwiList.numGlasses; ++i)
    {
        const Glass *glass = &s_glassKiwiList.glasses[i];

        svState = (i < svGlasses.numGlasses) ? (svGlasses.glasses[i].state.val.i & 0xF) : 4;
        clState = (clGlasses && i < clGlasses->numGlasses) ? (clGlasses->glasses[i].state.val.i & 0xF) : 4;
        Com_Printf(
            CON_CHANNEL_DONT_FILTER,
            "%4u *%-4u %-20s thick %5.2f  pts %2u  frame %d  uv %.5f  sv %-9s cl %-9s (%.0f %.0f %.0f)\n",
            i,
            glass->brushModel,
            glass->glassDef->name,
            -glass->thickness,
            glass->numOutlineVerts,
            glass->isPlanar,
            glass->uvScale,
            stateNames[svState < 4 ? svState : 4],
            stateNames[clState < 4 ? clState : 4],
            glass->origin[0],
            glass->origin[1],
            glass->origin[2]);
    }
}

/*
==============
GlassKiwi_GetGlasses

Built lazily the first time the server or client asks after a map's CM loads, and
cached by map name and checksum so a listen server builds once and shares it.
==============
*/
Glasses *GlassKiwi_GetGlasses()
{
    if (!s_glassKiwiCmdsAdded)
    {
        Cmd_AddCommandInternal("glassList", GlassKiwi_List_f, &GlassKiwi_List_VAR);
        glassShowNames = Dvar_RegisterBool("glassShowNames", false, DVAR_CHEAT, "Show each glass pane's glass def (texture set) above it");
        GlassKiwi_PhysInit();
        s_glassKiwiCmdsAdded = true;
    }

    if (!cm.isInUse || !cm.mapEnts)
        return 0;

    if (!s_glassKiwiBuilt || s_glassKiwiChecksum != cm.checksum || I_stricmp(s_glassKiwiMapName, cm.name ? cm.name : ""))
        GlassKiwi_Build();

    return &s_glassKiwiList;
}

static const GlassKiwiDef *GlassKiwi_GetKiwiDef(const GlassDef *def)
{
    unsigned int i;

    for (i = 0; i < s_glassKiwiNumDefs; ++i)
    {
        if (&s_glassKiwiDefs[i].def == def)
            return &s_glassKiwiDefs[i];
    }
    for (i = 0; i < s_glassKiwiNumClientDefs; ++i)
    {
        if (&s_glassKiwiClientDefs[i].def == def)
            return s_glassKiwiClientDefs[i].kiwiDef;
    }
    iassert(0);
    return &s_glassKiwiDefs[0];
}

// "wc/" (pane) / "mc/" (shard) picks the lit techset variant; fastfiles store the prefixed name
static Material *GlassKiwi_RegisterMaterial(const char *name, Material *fallback, bool model)
{
    char qualified[256];

    if (!name || !*name)
        return fallback;
    Com_sprintf(qualified, sizeof(qualified), "%s%s", model ? "mc/" : "wc/", name);
    return Material_RegisterHandle(qualified, model ? IMAGE_TRACK_MODEL : IMAGE_TRACK_WORLD);
}

/*
==============
GlassKiwi_ResolveClientAssets

Client side only: materials, effects. Each pane gets the def instance for its own
pristine material (see GlassKiwiClientDef).
==============
*/
void GlassKiwi_ResolveClientAssets()
{
    unsigned int i, d;

    if (!s_glassKiwiBuilt || !s_glassKiwiList.numGlasses)
        return;

    // back to the base defs before the previous client instances go away (cgame restart)
    for (i = 0; i < s_glassKiwiList.numGlasses; ++i)
        s_glassKiwiList.glasses[i].glassDef = (GlassDef *)&GlassKiwi_GetKiwiDef(s_glassKiwiList.glasses[i].glassDef)->def;
    GlassKiwi_Free(s_glassKiwiClientDefs);
    s_glassKiwiClientDefs = (GlassKiwiClientDef *)GlassKiwi_Alloc(sizeof(GlassKiwiClientDef) * s_glassKiwiList.numGlasses);
    s_glassKiwiNumClientDefs = 0;

    for (i = 0; i < s_glassKiwiList.numGlasses; ++i)
    {
        Glass *glass = &s_glassKiwiList.glasses[i];
        const GlassKiwiDef *kd = GlassKiwi_GetKiwiDef(glass->glassDef);
        GfxBrushModel *bmodel = R_GetBrushModel(glass->brushModel);
        Material *paneMaterial = 0;
        Material *pristine;
        GlassKiwiClientDef *cd = 0;

        if (bmodel && bmodel->surfaceCount)
            paneMaterial = rgp.world->dpvs.surfaces[bmodel->startSurfIndex].material;
        pristine = GlassKiwi_RegisterMaterial(kd->pristineMaterial, paneMaterial, false);

        for (d = 0; d < s_glassKiwiNumClientDefs; ++d)
        {
            if (s_glassKiwiClientDefs[d].kiwiDef == kd && s_glassKiwiClientDefs[d].def.pristineMaterial == pristine)
            {
                cd = &s_glassKiwiClientDefs[d];
                break;
            }
        }
        if (!cd)
        {
            cd = &s_glassKiwiClientDefs[s_glassKiwiNumClientDefs++];
            cd->kiwiDef = kd;
            cd->def = kd->def;
            cd->def.pristineMaterial = pristine;
            cd->def.crackedMaterial = GlassKiwi_RegisterMaterial(kd->crackedMaterial, pristine, false);
            cd->def.shardMaterial = GlassKiwi_RegisterMaterial(kd->shardMaterial, 0, true);
            cd->def.crackEffect = kd->crackEffect[0] ? FX_Register(kd->crackEffect) : 0;
            cd->def.shatterEffect = kd->shatterEffect[0] ? FX_Register(kd->shatterEffect) : 0;
        }
        glass->glassDef = &cd->def;

        // apply the def's pristine now (SetBrushMaterial only runs on a state change)
        if (bmodel)
        {
            for (d = 0; d < bmodel->surfaceCount; ++d)
                rgp.world->dpvs.surfaces[bmodel->startSurfIndex + d].material = pristine;
        }
    }
}
