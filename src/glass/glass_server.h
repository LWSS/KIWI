#pragma once
#include "glass.h"

struct gentity_s;
struct moveclip_t;
struct movectrace_tlip_t;
struct pointtrace_t;
struct msg_t;
struct trace_t;

struct GlassServer // sizeof=0x2C
{                                       // XREF: GlassesServer/r
    GlassState state;
    Glass *glass;
    int stateChangeTime;
    float health;
    float crackedDamageRate;
    float hitPos[3];
    float hitDir[3];

    void SetState(GlassState::State st, float *pos, float *dir);
};

struct GlassesServer // sizeof=0xABF8
{                                       // XREF: .data:GlassesServer svGlasses/r
    int nextShatterTime;
    int shatterIntervalsMS;
    bool inited;                        // XREF: _dynamic_initializer_for__svGlasses__+6/w
    // padding byte
    // padding byte
    // padding byte
    unsigned int numGlasses;            // XREF: GlassSv_AreaGlasses(float const * const,float const * const,Glass const * * const,uint)+D/r
                                        // GlassSv_AreaGlasses(float const * const,float const * const,Glass const * * const,uint):loc_9A00C1/r ...
    GlassServer glasses[1000];          // XREF: GlassSv_AreaGlasses(float const * const,float const * const,Glass const * * const,uint)+58/o
                                        // GlassSv_PointTrace(pointtrace_t const *,trace_t *)+61/o ...
    const dvar_s *damageMultiplier;     // XREF: GlassSv_Damage(uint,int,int,float const * const,float const * const)+70/r
                                        // GlassSv_Damage(uint,int,int,float const * const,float const * const)+146/r
    const dvar_s *crackedDamageRateRange;
                                        // XREF: GlassServer::SetState(GlassState::State,float const * const,float const * const)+2B/r
                                        // GlassServer::SetState(GlassState::State,float const * const,float const * const)+38/r
    void __thiscall Init();
    void __thiscall Shutdown();
    void __thiscall Update();
    static void __cdecl ShatterAllCmd();
    void __thiscall ShatterAll();
    static void __cdecl ResetAllCmd();
    void __thiscall ResetAll();
    void __thiscall WriteSnapshotToClient(msg_t *msg, int sinceTime);
};

void __cdecl GlassSv_Init();
void __cdecl GlassSv_Shutdown();
void __cdecl GlassSv_Update();
unsigned int __cdecl GlassSv_AreaGlasses(
                const float *mins,
                const float *maxs,
                const Glass **glasses,
                unsigned int maxGlasses);
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
void __cdecl GlassSv_Touch(unsigned int glassId, gentity_s *other);
void __cdecl GlassSv_PredictTouch(gentity_s *other);
void __cdecl GlassSv_WriteSnapshotToClient(msg_t *msg, int sinceTime);
void __cdecl GlassSv_WriteGameState(msg_t *msg);

constexpr int GLASS_MIN_DAMAGE = 5;

