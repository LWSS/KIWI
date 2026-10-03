// KIWI (GLASS): BO1 functions with a different KIWI signature, snapshot glue, renderer entry points

#include "glass_kiwi.h"
#include "glass_kiwi_defaults.h"
#include "glass.h"
#include "glass_client.h"
#include "glass_server.h"
#include "glass_shard.h"
#include "glass_renderer.h"
#include "glass_load_obj.h"

#include <client/client.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_drawsurf.h>
#include <cgame/cg_local.h>
#include <cgame_mp/cg_local_mp.h>
#include <gfx_d3d/r_model_lighting.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_marks.h>
#include <EffectsCore/fx_system.h>
#include <server_mp/server_mp.h>
#include <script/scr_vm.h>

#include <cstddef>

extern GlassesServer svGlasses;

// ---------------------------------------------------------------------------
// Network: BO1's glass block ends every snapshot behind a presence bit. Snapshots are
// parsed before CG_Init, so pane states go to a mirror that outlives cgame and is
// replayed into the BO1 client at CG_Init (BO1 sent the join state in the gamestate).
// ---------------------------------------------------------------------------

#define GLASS_KIWI_NET_MAX_PANES 1024 // 10-bit pane index on the wire
#define GLASS_KIWI_NET_END_MARKER 30154

struct GlassKiwiNetPane
{
    unsigned int state;
    float hitPos[3];
    float hitDir[3];
};

static int s_glassServerInitTime;
static GlassKiwiNetPane s_glassNetPanes[GLASS_KIWI_NET_MAX_PANES];
static unsigned int s_glassNetNumPanes; // from the last full block, 0 = none since the gamestate
static bool s_glassClientSynced; // clGlasses has a state for every pane
static bool s_glassNetCountWarned;

void GlassKiwi_ServerInit()
{
    GlassSv_Init();
    s_glassServerInitTime = svs.time;
}

void GlassKiwi_WriteSnapshot(msg_t *msg, int sinceTime)
{
    if (!svGlasses.numGlasses)
    {
        MSG_WriteBit0(msg);
        return;
    }
    MSG_WriteBit1(msg);
    // GlassesServer::Init zeroes every pane's change time, so after a fast_restart a client
    // whose delta base predates the reset needs the full list
    if (sinceTime > 0 && sinceTime <= s_glassServerInitTime)
        sinceTime = 0;
    GlassSv_WriteSnapshotToClient(msg, sinceTime);
}

static void GlassKiwi_ReadPaneState(msg_t *msg, unsigned int index, unsigned int state)
{
    GlassKiwiNetPane *pane = &s_glassNetPanes[index];
    int i;

    pane->state = state;
    if (state == GlassState::SHATTERED)
    {
        for (i = 0; i < 3; ++i)
            pane->hitPos[i] = MSG_ReadFloat(msg);
        for (i = 0; i < 3; ++i)
            pane->hitDir[i] = MSG_ReadFloat(msg);
    }
    else
    {
        for (i = 0; i < 3; ++i)
        {
            pane->hitPos[i] = 0.0f;
            pane->hitDir[i] = 0.0f;
        }
    }
}

static bool GlassKiwi_ClientCanApply()
{
    if (!clGlasses || !s_glassNetNumPanes)
        return false;
    if (s_glassNetNumPanes != clGlasses->numGlasses)
    {
        if (!s_glassNetCountWarned)
            Com_PrintError(CON_CHANNEL_ERROR, "Glass: server sent %u panes, this client built %u; glass states ignored\n", s_glassNetNumPanes, clGlasses->numGlasses);
        s_glassNetCountWarned = true;
        return false;
    }
    return true;
}

static void GlassKiwi_ApplyPane(int localClientNum, unsigned int index, bool quiet)
{
    GlassKiwiNetPane *pane = &s_glassNetPanes[index];

    clGlasses->glasses[index].SetState(localClientNum, (GlassState::State)pane->state, pane->hitPos, pane->hitDir, quiet);
}

// join, late join and cgame restart: no sounds or effects, and moving shards are removed
// right after the shatter (BO1 gameState = true)
static void GlassKiwi_ReplayMirror(int localClientNum)
{
    unsigned int i;

    for (i = 0; i < s_glassNetNumPanes; ++i)
        GlassKiwi_ApplyPane(localClientNum, i, true);
    s_glassClientSynced = true;
}

void GlassKiwi_ParseSnapshot(int localClientNum, msg_t *msg)
{
    unsigned __int16 changed[GLASS_KIWI_NET_MAX_PANES];
    unsigned int numChanged;
    unsigned int num;
    unsigned int numChanges;
    unsigned int index;
    unsigned int i;
    int check;

    if (msg->overflowed || !MSG_ReadBit(msg))
        return; // no panes on this map

    numChanged = 0;
    if (MSG_ReadBit(msg) == 1)
    {
        if (MSG_ReadBit(msg) == 1)
        {
            num = (unsigned int)MSG_ReadShort(msg);
            if (num > GLASS_KIWI_NET_MAX_PANES)
            {
                Com_PrintError(CON_CHANNEL_ERROR, "Glass: bad pane count %u in snapshot\n", num);
                MSG_Discard(msg);
                return;
            }
            s_glassNetNumPanes = num;
            for (i = 0; i < num; ++i)
            {
                GlassKiwi_ReadPaneState(msg, i, MSG_ReadBits(msg, 2u));
                changed[numChanged++] = (unsigned __int16)i;
            }
        }
        else
        {
            numChanges = (unsigned int)MSG_ReadShort(msg);
            for (i = 0; i < numChanges && !msg->overflowed; ++i)
            {
                index = MSG_ReadBits(msg, 0xAu);
                GlassKiwi_ReadPaneState(msg, index, MSG_ReadBits(msg, 2u));
                if (numChanged < GLASS_KIWI_NET_MAX_PANES)
                    changed[numChanged++] = (unsigned __int16)index;
            }
        }
    }
    check = MSG_ReadShort(msg);
    if (check != GLASS_KIWI_NET_END_MARKER)
    {
        Com_PrintError(CON_CHANNEL_ERROR, "Glass: unexpected end of the snapshot glass block: %x\n", check);
        MSG_Discard(msg);
        return;
    }
    if (msg->overflowed || !GlassKiwi_ClientCanApply())
        return;

    if (!s_glassClientSynced)
    {
        GlassKiwi_ReplayMirror(localClientNum);
        return;
    }
    for (i = 0; i < numChanged; ++i)
    {
        if (changed[i] < clGlasses->numGlasses)
            GlassKiwi_ApplyPane(localClientNum, changed[i], false);
    }
}

void GlassKiwi_ClientGamestate()
{
    memset(s_glassNetPanes, 0, sizeof(s_glassNetPanes));
    s_glassNetNumPanes = 0;
    s_glassClientSynced = false;
    s_glassNetCountWarned = false;
}

void GlassKiwi_ClientDisconnect()
{
    GlassKiwi_ClientGamestate();
}

// CG_Init, after the world, CM and effects are loaded
void GlassKiwi_ClientInit(int localClientNum)
{
    s_glassClientSynced = false;
    if (!GetGlasses())
        return;
    GlassKiwi_ResolveClientAssets();
    GlassCl_AllocateMemory();
    if (GlassKiwi_ClientCanApply())
        GlassKiwi_ReplayMirror(localClientNum);
}

void GlassKiwi_ClientShutdown(int localClientNum)
{
    GlassCl_Reset(localClientNum);
    GlassCl_FreeMemory();
    s_glassClientSynced = false;
}

void GlassKiwi_ClientMapRestart(int localClientNum)
{
    GlassCl_Reset(localClientNum);
}

// scene population: intact and cracked panes go to the renderer's glass brush list
void GlassKiwi_ClientUpdate(int localClientNum)
{
    if (clGlasses && s_glassClientSynced) // BO1 asserts on panes with no state yet
        GlassCl_Update(localClientNum);
    GlassKiwi_DrawNames();
}

/*
==============
CG_PlaySound

BO1 played the crack / shatter alias at the pane origin; KIWI plays aliases by name.
==============
*/
void CG_PlaySound(int localClientNum, float *origin, int unused0, int unused1, float unused2, char *aliasName)
{
    if (!aliasName || !*aliasName)
        return;
    CG_PlaySoundAliasByName(localClientNum, ENTITYNUM_WORLD, origin, aliasName);
}

// BO1 debug-draw argument lists; client bucket (server-bucket duration-0 draws never render)
void CL_AddDebugLine(const float *start, const float *end, const float *color, int depthTest, int duration)
{
    CL_AddDebugLine(start, end, color, depthTest, duration, 0);
}

void CL_AddDebugString(const float *xyz, const float *color, float scale, const char *text, int duration)
{
    CL_AddDebugString(xyz, color, scale, text, 0, duration);
}

void GlassKiwi_WarnPoolFull(GlassKiwiPool pool, unsigned int inUse)
{
    static const char *names[GLASS_POOL_COUNT] = { "shard", "shard group", "shard physics", "shard memory" };
    static int lastWarn[GLASS_POOL_COUNT];
    const int now = Sys_Milliseconds();

    iassert(pool >= 0 && pool < GLASS_POOL_COUNT);
    if (lastWarn[pool] && now - lastWarn[pool] < 1000)
        return;
    lastWarn[pool] = now;
    Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: glass %s pool is full (%u in use), shards dropped (glassHWM)\n", names[pool], inUse);
}

static bool s_shardHit;
static float s_shardHitPos[3];

void GlassKiwi_NoteShardHit(const float *pos)
{
    s_shardHit = true;
    s_shardHitPos[0] = pos[0];
    s_shardHitPos[1] = pos[1];
    s_shardHitPos[2] = pos[2];
}

void GlassKiwi_BeginShardHits()
{
    s_shardHit = false;
}

void GlassKiwi_EndShardHits(bool melee)
{
    if (s_shardHit)
        CG_PlaySound(0, s_shardHitPos, 0, 0, 1.0f, (char *)(melee ? GLASS_KIWI_MELEE_HIT_SOUND : GLASS_KIWI_BULLET_HIT_SOUND));
    s_shardHit = false;
}

static void GlassKiwi_DebugBoxLines(const float (*corners)[3], const float *color, int depthTest, int duration)
{
    static const int edges[12][2] = {
        { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
        { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    int i;

    for (i = 0; i < 12; ++i)
        CL_AddDebugLine(corners[edges[i][0]], corners[edges[i][1]], color, depthTest, duration, 0);
}

void CL_AddDebugBox(const float *origin, const float *mins, const float *maxs, float yaw, const float *color, int depthTest, int duration)
{
    float corners[8][3];
    int i;

    for (i = 0; i < 8; ++i)
    {
        corners[i][0] = origin[0] + ((i & 1) ? maxs[0] : mins[0]);
        corners[i][1] = origin[1] + ((i & 2) ? maxs[1] : mins[1]);
        corners[i][2] = origin[2] + ((i & 4) ? maxs[2] : mins[2]);
    }
    GlassKiwi_DebugBoxLines(corners, color, depthTest, duration);
}

void CL_AddDebugBox(const float (*axis)[3], const float *origin, const float *mins, const float *maxs, const float *color, int depthTest, int duration)
{
    float corners[8][3];
    float local[3];
    int i;

    for (i = 0; i < 8; ++i)
    {
        local[0] = (i & 1) ? maxs[0] : mins[0];
        local[1] = (i & 2) ? maxs[1] : mins[1];
        local[2] = (i & 4) ? maxs[2] : mins[2];
        corners[i][0] = origin[0] + local[0] * axis[0][0] + local[1] * axis[1][0] + local[2] * axis[2][0];
        corners[i][1] = origin[1] + local[0] * axis[0][1] + local[1] * axis[1][1] + local[2] * axis[2][1];
        corners[i][2] = origin[2] + local[0] * axis[0][2] + local[1] * axis[1][2] + local[2] * axis[2][2];
    }
    GlassKiwi_DebugBoxLines(corners, color, depthTest, duration);
}

unsigned int __cdecl R_PassbackPrimaryLightCallback(const void *userData)
{
    return *(const unsigned int *)userData;
}

/*
==============
R_AllocModelLighting (BO1 7-argument form)

KIWI's cache reuses a handle only for the exact origin; snap to a 32-unit grid so moving
shard groups don't fill it. tolerance / dynEntId unused.
==============
*/
unsigned int R_AllocModelLighting(
    float *lightingOrigin,
    float tolerance,
    unsigned int dynEntId,
    uint16_t *cachedLightingHandle,
    unsigned int(__cdecl *GetPrimaryLightCallback)(const void *),
    const void *userData,
    GfxLightingInfo *lightingInfoOut)
{
    float snapped[3];
    int i;

    for (i = 0; i < 3; ++i)
        snapped[i] = floorf(lightingOrigin[i] * (1.0f / 32.0f)) * 32.0f + 16.0f;
    return R_AllocModelLighting(snapped, cachedLightingHandle, GetPrimaryLightCallback, userData, lightingInfoOut);
}

int CL_GetLocalClientConnectionState(int localClientNum)
{
    return CG_GetLocalClientConnectionState(localClientNum);
}

int g_glassSmashCallback; // maps/mp/gametypes/_callbacksetup::CodeCallback_GlassSmash, 0 = not defined

/*
==============
Scr_GlassSmash

BO1 CodeCallback_GlassSmash(pos, dir) for gametype scripts, run when a pane shatters.
==============
*/
void Scr_GlassSmash(float *pos, float *dir)
{
    unsigned __int16 t;

    // BO1 0x662830; the label is optional (GScr_LoadGameTypeScript)
    if (g_glassSmashCallback)
    {
        Scr_AddVector(dir);
        Scr_AddVector(pos);
        t = Scr_ExecThread(g_glassSmashCallback, 2);
        Scr_FreeThread(t);
    }
}

/*
==============
R_MarkFragments_Glass

BO1 r_marks.cpp. Marks on intact panes become world marks tagged modelIndex = glassIndex + 1,
so FX_MarkGlassDetachAll can drop them at shatter. KIWI's callback takes 7 args (BO1 9).
==============
*/
char __cdecl R_MarkFragments_Glass(MarkInfo *markInfo)
{
    FxMarkPoint *v2; // [esp+8h] [ebp-190h]
    float v3; // [esp+10h] [ebp-188h]
    float v4; // [esp+18h] [ebp-180h]
    float v5; // [esp+1Ch] [ebp-17Ch]
    const float *origin; // [esp+48h] [ebp-150h]
    float pos_4; // [esp+54h] [ebp-144h]
    float pos_8; // [esp+58h] [ebp-140h]
    int p; // [esp+5Ch] [ebp-13Ch]
    GfxSurface *surface; // [esp+60h] [ebp-138h]
    unsigned int s; // [esp+64h] [ebp-134h]
    const GfxBrushModel *br; // [esp+68h] [ebp-130h]
    unsigned int i; // [esp+6Ch] [ebp-12Ch]
    float clipPlanes[6][4]; // [esp+70h] [ebp-128h] BYREF
    float glassMatrix[4][3]; // [esp+12Ch] [ebp-6Ch] BYREF
    unsigned int num; // [esp+15Ch] [ebp-3Ch]
    GfxMarkContext markContext; // [esp+160h] [ebp-38h] BYREF
    const Glass *glasses[8]; // [esp+168h] [ebp-30h] BYREF
    bool anyMarks; // [esp+18Bh] [ebp-Dh] BYREF
    float markDir[3]; // [esp+18Ch] [ebp-Ch] BYREF

    PROF_SCOPED("markfragments glass");

    num = GlassCl_AreaGlasses(markInfo->mins, markInfo->maxs, glasses, 8u);
    for ( i = 0; i < num; ++i )
    {
        markContext.modelTypeAndSurf = 0;
        markContext.modelIndex = glasses[i]->index + 1; // KIWI (GLASS): BO1 stored the bare index (see above)
        AnglesToAxis(glasses[i]->angles, glassMatrix);
        origin = glasses[i]->origin;
        glassMatrix[3][0] = *origin;
        glassMatrix[3][1] = origin[1];
        glassMatrix[3][2] = origin[2];
        R_Mark_TransformClipPlanes(markInfo->planes, glassMatrix, clipPlanes);
        MatrixTransposeTransformVector(markInfo->axis[0], *(const mat3x3 *)glassMatrix, markDir);
        anyMarks = 0;
        br = R_GetBrushModel(glasses[i]->brushModel);
        for ( s = 0; s < br->surfaceCount; ++s )
        {
            surface = &rgp.world->dpvs.surfaces[s + br->startSurfIndex];
            if ( R_Mark_MaterialAllowsMarks(surface->material, markInfo->material)
                && !R_MarkFragments_BrushSurface(markInfo, &markContext, clipPlanes, markDir, surface, &anyMarks) )
            {
                return 0;
            }
        }
        if ( anyMarks )
        {
            for ( p = 0; p < markInfo->usedPointCount; ++p )
            {
                v5 = markInfo->points[p].xyz[0];
                v4 = markInfo->points[p].xyz[1];
                pos_4 = (float)(v4 * glassMatrix[1][1]) + (float)((float)(v5 * glassMatrix[0][1]) + glasses[i]->origin[1]);
                pos_8 = (float)(v4 * glassMatrix[1][2]) + (float)((float)(v5 * glassMatrix[0][2]) + glasses[i]->origin[2]);
                v2 = &markInfo->points[p];
                v3 = v2->xyz[2];
                v2->xyz[0] = (float)(v3 * glassMatrix[2][0])
                                     + (float)((float)(v4 * glassMatrix[1][0])
                                                     + (float)((float)(v5 * glassMatrix[0][0]) + glasses[i]->origin[0]));
                v2->xyz[1] = (float)(v3 * glassMatrix[2][1]) + pos_4;
                v2->xyz[2] = (float)(v3 * glassMatrix[2][2]) + pos_8;
            }
            markInfo->callback(
                markInfo->callbackContext,
                markInfo->usedTriCount,
                markInfo->tris,
                markInfo->usedPointCount,
                markInfo->points,
                markInfo->origin,
                markInfo->axis[1]);
            markInfo->usedTriCount = 0;
            markInfo->usedPointCount = 0;
        }
    }
    return 1;
}

/*
==============
GlassKiwi_RenderFrame

BO1 GlassCl_GenerateVerts: simulate, then build shard verts into the glass mesh. Main thread,
in R_GenerateSortedDrawSurfs before the lit FX list is sorted.
==============
*/
void GlassKiwi_RenderFrame(int localClientNum)
{
    if (!clGlasses)
        return;
    if (CL_LocalClient_IsFirstActive(localClientNum))
        clGlasses->renderer->Update(0);
    if (dx.deviceLost || !frontEndDataOut->glassMesh.vb.buffer)
    {
        // can't lock; shards just aren't drawn this frame. GenerateVerts' tail (maintenance, pre-shatter,
        // cleanup) still runs, as in BO1 where the verts went to CPU memory
        clGlasses->renderer->StartMaintenance();
        return;
    }
    R_BeginGlassMeshVerts();
    clGlasses->renderer->GenerateVerts(localClientNum, 0, 0);
    R_EndGlassMeshVerts();
}
