#pragma once
// KIWI (GLASS): BO1 -> KIWI compatibility layer for the BO1 glass files in this folder.
// BO1 names KIWI lacks are absorbed here; other edits are marked "// KIWI (GLASS):".

#include <universal/q_shared.h>
#include <universal/assertive.h>
#include <universal/com_math.h>
#include <universal/profile.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/msg_mp.h>
#include <win32/win_local.h>

// windows.h redefines LOBYTE / HIBYTE as rvalues; BO1 assigns through them (q_shared.h forms)
#undef LOBYTE
#undef HIBYTE
#define LOBYTE(x)  BYTEn(x,LOW_IND(x,_BYTE))
#define HIBYTE(x)  BYTEn(x,HIGH_IND(x,_BYTE))

#include <cstdarg>
#include <cstdio>
#include <cfloat>
#include <intrin.h>

#include "glass_kiwi_phys.h"

struct Glass;
struct Glasses;
struct GlassDef;
struct GfxBrushModel;
struct Material;
struct GfxPackedVertex;
struct GfxLightingInfo;
struct r_double_index_t;

// ---------------------------------------------------------------------------
// asserts / profiling / misc BO1 helpers
// ---------------------------------------------------------------------------

// KIWI's handler breaks itself; return "handled" so BO1's caller skips its __debugbreak
inline bool Assert_MyHandler(const char *filename, int line, int type, const char *fmt, ...)
{
    char message[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    MyAssertHandler(filename, line, type, "%s", message);
    return true;
}

#ifndef PROF_SCOPED_RUNTIME_NAME
#define PROF_SCOPED_RUNTIME_NAME(name) ((void)(name))
#endif

inline void __cdecl BLOPS_NULLSUB() { }

constexpr CriticalSection CRITSECT_PHYSICS_UPDATE = CRITSECT_PHYSICS; // KIWI steps ODE on the main thread

inline LARGE_INTEGER tlPcGetTick()
{
    LARGE_INTEGER tick;
    QueryPerformanceCounter(&tick);
    return tick;
}

inline double GlassKiwi_TicksPerMS()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (double)frequency.QuadPart / 1000.0;
}
inline const double tlPcTicksPerMS = GlassKiwi_TicksPerMS();

// BO1 melee trace pattern (DynEntity_client.h traceOffsets, same values)
static const float traceOffsets_0[5][2] = { { 0.0f, 0.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f } };

// ---------------------------------------------------------------------------
// dvars: BO1 _Dvar_Register* take min/max directly
// ---------------------------------------------------------------------------

inline const dvar_s *_Dvar_RegisterBool(const char *name, bool value, unsigned int flags, const char *description)
{
    return Dvar_RegisterBool(name, value, (uint16_t)flags, description);
}

inline const dvar_s *_Dvar_RegisterInt(const char *name, int value, int min, int max, unsigned int flags, const char *description)
{
    DvarLimits limits;
    limits.integer.min = min;
    limits.integer.max = max;
    return Dvar_RegisterInt(name, value, limits, (uint16_t)flags, description);
}

inline const dvar_s *_Dvar_RegisterFloat(const char *name, float value, float min, float max, unsigned int flags, const char *description)
{
    return Dvar_RegisterFloat(name, value, min, max, (uint16_t)flags, description);
}

inline const dvar_s *_Dvar_RegisterVec2(const char *name, float x, float y, float min, float max, unsigned int flags, const char *description)
{
    DvarLimits limits;
    limits.value.min = min;
    limits.value.max = max;
    return Dvar_RegisterVec2(name, x, y, limits, (uint16_t)flags, description);
}

// ---------------------------------------------------------------------------
// math (BO1 com_math names KIWI lacks or spells differently)
// ---------------------------------------------------------------------------

inline void Vec3Min(const float *a, const float *b, float *out)
{
    out[0] = a[0] < b[0] ? a[0] : b[0];
    out[1] = a[1] < b[1] ? a[1] : b[1];
    out[2] = a[2] < b[2] ? a[2] : b[2];
}

inline void Vec3Max(const float *a, const float *b, float *out)
{
    out[0] = a[0] > b[0] ? a[0] : b[0];
    out[1] = a[1] > b[1] ? a[1] : b[1];
    out[2] = a[2] > b[2] ? a[2] : b[2];
}

inline void Vec3Abs(const float *v, float *out)
{
    out[0] = fabsf(v[0]);
    out[1] = fabsf(v[1]);
    out[2] = fabsf(v[2]);
}

// BO1 passes a 3x3 axis immediately followed by an origin (Glass::outlineAxis +
// outlineOrigin, GlassShard::axis + origin); KIWI's takes mat4x3 references.
inline void MatrixInverseOrthogonal43(const float (*in)[3], float (*out)[3])
{
    // a mat4x3 lvalue also decays to float (*)[3], so name KIWI's reference overload explicitly
    void(__cdecl *kiwiInverse)(const mat4x3 &, mat4x3 &) = MatrixInverseOrthogonal43;
    kiwiInverse(*(const mat4x3 *)in, *(mat4x3 *)out);
}

// ---------------------------------------------------------------------------
// traces
// ---------------------------------------------------------------------------

// BO1 intersect_extents_aabb = "the trace can reach the box"; KIWI's CM_TraceBox
// returns nonzero when the box is culled.
inline bool intersect_extents_aabb(const TraceExtents *extents, const float *mins, const float *maxs, float fraction)
{
    return !CM_TraceBox(extents, (float *)mins, (float *)maxs, fraction);
}

// ---------------------------------------------------------------------------
// client / cgame
// ---------------------------------------------------------------------------

// KIWI has one local client
inline bool CL_LocalClient_IsFirstActive(int localClientNum)
{
    return localClientNum == 0;
}

inline bool CL_LocalClient_IsActive(int localClientNum)
{
    return localClientNum == 0;
}

// BO1 name of KIWI's CG_GetLocalClientConnectionState (glass_kiwi.cpp)
int CL_GetLocalClientConnectionState(int localClientNum);

// BO1: CG_PlaySound(localClientNum, origin, ?, ?, volume, aliasName)
void CG_PlaySound(int localClientNum, float *origin, int unused0, int unused1, float unused2, char *aliasName);

// BO1 CSC (client script) callback; CoD4 has no client scripts
inline void CScr_GlassSmash(float *origin, float *dir) { }

// BO1 bayonet; CoD4 has none
inline bool BG_IsBayonetWeapon(int weapon) { return false; }
#define player_bayonetRange player_meleeRange

// BO1 debug-draw helpers with BO1's argument lists
void CL_AddDebugLine(const float *start, const float *end, const float *color, int depthTest, int duration);
void CL_AddDebugString(const float *xyz, const float *color, float scale, const char *text, int duration);
void CL_AddDebugBox(const float *origin, const float *mins, const float *maxs, float yaw, const float *color, int depthTest, int duration);
void CL_AddDebugBox(const float (*axis)[3], const float *origin, const float *mins, const float *maxs, const float *color, int depthTest, int duration);

// ---------------------------------------------------------------------------
// renderer entry points the glass code calls (KIWI side lives in gfx_d3d)
// ---------------------------------------------------------------------------

void R_AddGlassBrushToScene(const GfxBrushModel *bmodel, const float *origin, const float *angles, Material *material);
int R_CullBoxCurDpvs_SceneSelect(const float *minmax, unsigned int localClientNum, int sceneSelect);
void R_InitGlassRenderBuffers(unsigned int numIndices, unsigned int numVerts, unsigned int unused);
void R_FreeGlassRenderBuffers();
void R_AddGlassDrawSurf(
    Material *material,
    unsigned __int16 *indices,
    int numIndices,
    unsigned int lightHandle,
    unsigned int primaryLightIndex,
    unsigned int reflectionProbeIndex,
    unsigned int surfType);
inline void R_StreamTouchMaterial(Material *material) { } // KIWI has no image streaming

unsigned int __cdecl R_PassbackPrimaryLightCallback(const void *userData);

// BO1 (LWSS-patched) R_AllocModelLighting with a reuse tolerance; KIWI's cache
// matches the exact origin only, so the glass uses its group's grid-cell centre.
unsigned int R_AllocModelLighting(
    float *lightingOrigin,
    float tolerance,
    unsigned int dynEntId,
    uint16_t *cachedLightingHandle,
    unsigned int(__cdecl *GetPrimaryLightCallback)(const void *),
    const void *userData,
    GfxLightingInfo *lightingInfoOut);

// ---------------------------------------------------------------------------
// glass marks, scripts
// ---------------------------------------------------------------------------

void FX_MarkGlassDetachAll(int localClientNum, unsigned int glassIndex);
void Scr_GlassSmash(float *pos, float *dir); // BO1 CodeCallback_GlassSmash (optional)

// ---------------------------------------------------------------------------
// KIWI glue (glass_kiwi_load.cpp, glass_kiwi.cpp)
// ---------------------------------------------------------------------------

Glasses *GlassKiwi_GetGlasses();
void GlassKiwi_FreeGlasses();
unsigned int GlassKiwi_StartState(unsigned int index); // "startbroken" key: PRISTINE/CRACKED/SHATTERED
void GlassKiwi_ResolveClientAssets();
void GlassKiwi_DrawNames(); // glassShowNames: each pane's glass def above it

// BO1 drops shards silently on a full pool; KIWI warns, once a second per pool
enum GlassKiwiPool
{
    GLASS_POOL_SHARDS,
    GLASS_POOL_GROUPS,
    GLASS_POOL_PHYSICS,
    GLASS_POOL_SHARD_MEMORY,
    GLASS_POOL_COUNT,
};
void GlassKiwi_WarnPoolFull(GlassKiwiPool pool, unsigned int inUse);

// one sound per bullet / melee action that hits a resting shard (BO1 had none)
void GlassKiwi_NoteShardHit(const float *pos);
void GlassKiwi_BeginShardHits();
void GlassKiwi_EndShardHits(bool melee);

#include "glass_kiwi_hooks.h" // engine-facing entry points
