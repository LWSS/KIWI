#pragma once
// KIWI (GLASS): the glass entry points engine files call. Declarations only, so a
// hooked engine file doesn't pull in the BO1 glass headers or the compat layer.

struct msg_t;
struct trace_t;
struct pointtrace_t;
struct moveclip_t;
struct gentity_s;
struct MarkInfo;
struct objInfo;
struct Results;
struct dxBody;

// server (BO1 glass_server.h)
void __cdecl GlassSv_Shutdown();
void __cdecl GlassSv_Update();
void __cdecl GlassSv_ClipMoveTrace(const moveclip_t *clip, trace_t *results);
void __cdecl GlassSv_PointTrace(const pointtrace_t *clip, trace_t *results);
void __cdecl GlassSv_Damage(unsigned int glassId, int damage, int mod, float *pos, float *dir);
void __cdecl GlassSv_RadiusDamage(
    float *origin,
    float radius,
    float coneAngleCos,
    float *coneDirection,
    float innerDamage,
    float outerDamage,
    int mod);
void __cdecl GlassSv_PredictTouch(gentity_s *other);

// client (BO1 glass_client.h)
void __cdecl GlassCl_TracePoint(const pointtrace_t *clip, trace_t *results);
void __cdecl GlassCl_ClipMoveTrace(const moveclip_t *clip, trace_t *results);
void __cdecl GlassCl_ExplosionEvent(
    int localClientNum,
    const float *origin,
    float damageInner,
    float damageOuter,
    float radius,
    int mod);
void __cdecl GlassCl_MeleeEvent(int localClientNum, int attackerEntNum);
void __cdecl GlassCl_Reset(int localClientNum);
void __cdecl GlassCl_DrawDebug();
// script (BO1 g_scr_main)
extern int g_glassSmashCallback; // _callbacksetup::CodeCallback_GlassSmash(pos, dir), 0 = not defined

// KIWI glue (glass_kiwi.cpp)
void GlassKiwi_ServerInit();
void GlassKiwi_WriteSnapshot(msg_t *msg, int sinceTime);
void GlassKiwi_ClientGamestate();
void GlassKiwi_ParseSnapshot(int localClientNum, msg_t *msg);
void GlassKiwi_ClientInit(int localClientNum);
void GlassKiwi_ClientShutdown(int localClientNum);
void GlassKiwi_ClientMapRestart(int localClientNum);
void GlassKiwi_ClientDisconnect();
void GlassKiwi_ClientUpdate(int localClientNum);
void GlassKiwi_RenderFrame(int localClientNum); // renderer: shard simulation + vertices (main thread)
char __cdecl R_MarkFragments_Glass(MarkInfo *markInfo); // r_marks.cpp: decals on intact panes
void GlassKiwi_PhysTestPanes(const objInfo *input, Results *results, dxBody *body); // dCollideWorldGeom: panes block ODE bodies
