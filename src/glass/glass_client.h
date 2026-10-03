#pragma once

#include "glass.h"

struct GlassShard;
struct GlassRenderer;
struct msg_t;
struct trace_t;
struct pointtrace_t;
struct moveclip_t;

struct GlassClient // sizeof=0xC
{
    struct Outlines // sizeof=0x8
    {
        struct Outline // sizeof=0xC
        {
            unsigned int numVerts;
            unsigned int edgeFlags;
            float (*verts)[2];
        };

        unsigned int numOutlines;
        GlassClient::Outlines::Outline *outlines;

        Outlines(const GlassShard *baseShard, const GlassShard **shards, int numShards);

        int InitShards(const GlassShard *baseShard, const GlassShard **shards, int maxNewShards);

        static int __cdecl CalcMemorySize(const GlassShard **shards, int numShards);
    };

    const Glass *glass;
    GlassState state;
    GlassClient::Outlines *outlines;


    void __thiscall Init(const Glass *gls);
    void __thiscall Reset();
    void __thiscall SetState(
        int localClientNum,
        GlassState::State newState,
        const float *hitPos,
        float *hitDir,
        bool gameState);
    void __thiscall PlayShatterFX(
        int localClientNum,
        const float *hitPos,
        const float *hitDir);
    void __thiscall Update(int localClientNum);
    void __thiscall SetBrushMaterial(GlassState::State state);
    void __thiscall Shatter(const float *pos, const float *dir);
    char __thiscall PreShatter();

};

struct StackAllocator // sizeof=0xC
{                                       // XREF: .data:StackAllocator GlassesClient::allocator/r
    unsigned __int8 *memory;            // XREF: GlassesClient::InitAllocator(Glasses *)+1B/w
    unsigned int size;                  // XREF: GlassesClient::InitAllocator(Glasses *)+24/w
    // GlassesClient::GetFreeMem(void)+3/r
    unsigned int pos;                   // XREF: GlassesClient::InitAllocator(Glasses *)+2A/w
};

struct GlassesClient // sizeof=0x10
{
    GlassClient *glasses;
    unsigned int numGlasses;
    GlassRenderer *renderer;
    unsigned int lastPreShatter;

    void *operator new(size_t size);
    void operator delete(void *ptr);

    GlassesClient(const Glasses *glss);
    ~GlassesClient();

    static StackAllocator allocator;

    static void __cdecl InitAllocator(Glasses *glasses);
    static unsigned int *__cdecl Allocate(int size, const char *file, int line);
    static void __cdecl Free(char *ptr);
    static unsigned int __cdecl GetFreeMem();

    const GlassClient *__thiscall GetGlass(int idx);
    void __thiscall Reset();
    void __thiscall Update(int localClientNum);
    void __thiscall ParseSnapshot(int localClientNum, msg_t *msg, bool gameState);
    void __thiscall WriteDemoSnapshot(msg_t *msg);
    void __thiscall PreShatterNext();
    void __thiscall TracePoint(const pointtrace_t *clip, trace_t *results);
    unsigned int __thiscall AreaGlasses(
        const float *mins,
        const float *maxs,
        const Glass **glss,
        unsigned int maxGlasses);
    void __thiscall ClipMoveTrace(const moveclip_t *clip, trace_t *results);
};

int __cdecl compareOutlineEdges(float *s1, float *s2);

void __cdecl GlassCl_AllocateMemory();
void __cdecl GlassCl_FreeMemory();
void __cdecl GlassCl_Reset(int localClientNum);
void __cdecl GlassCl_Update(int localClientNum);
void __cdecl GlassCl_ReadGameState(int localClientNum, msg_t *msg);
void __cdecl GlassCl_ParseSnapshot(int localClientNum, msg_t *msg);
void __cdecl GlassCl_WriteDemoSnapshot(msg_t *msg);
void __cdecl GlassCl_WaitUpdate();
void __cdecl GlassCl_BeginGenerateVerts(unsigned int localClientNum, unsigned int viewIndex);
void __cdecl GlassCl_WaitGenerateVerts();
void __cdecl GlassCl_GenerateVerts(int localClientNum, unsigned int viewIndex);
void __cdecl GlassCl_ExplosionEvent(
                int localClientNum,
                const float *origin,
                float damageInner,
                float damageOuter,
                float radius,
                int mod);
void __cdecl GlassCl_TracePoint(const pointtrace_t *clip, trace_t *results);
void __cdecl GlassCl_MeleeEvent(int localClientNum, int attackerEntNum);
void __cdecl GlassCl_DrawDebug();
unsigned int __cdecl GlassCl_AreaGlasses(
                const float *mins,
                const float *maxs,
                const Glass **glasses,
                unsigned int maxGlasses);

void __cdecl GlassCl_ClipMoveTrace(const moveclip_t *clip, trace_t *results);

extern GlassesClient *clGlasses;
