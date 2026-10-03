#pragma once
//#include <universal/dvar.h> // KIWI (GLASS): BO1-only header; dvar_s comes from qcommon.h via glass_kiwi.h
#include "glass_kiwi.h"
#include <gfx_d3d/r_material.h>

#include "glass_shard.h"
#include "glass_allocator.h"

struct GlassClient;
struct Glasses;

struct GlassLock // sizeof=0x4
{                                       // XREF: GlassRenderer/r
    volatile unsigned int lock;
};

struct GlassRenderer // sizeof=0x4DF8
{
    enum ActionType_t : __int32 // not a real enum name
    {                                       // XREF: GlassRenderer::Action/r
        SHATTER     = 0x0,
        TRACE_POINT = 0x1,
        EXPLOSION   = 0x2,
        MELEE       = 0x3,
        MAX_ACTIONS = 0x64,
    };

    struct Action // sizeof=0x38
    {                                       // XREF: GlassRenderer/r
        ActionType_t type;
        //$F8734D8CD8D5548044E567214B041690 ___u1;
        union //$F8734D8CD8D5548044E567214B041690 // sizeof=0x34
        {                                       // XREF: GlassRenderer::Action/r
            struct //GlassRenderer::Action::<unnamed_tag>::<unnamed_type_shatter> // sizeof=0x20
            {                                       // XREF: $F8734D8CD8D5548044E567214B041690/r
                GlassClient *glass;
                bool gameState;
                // padding byte
                // padding byte
                // padding byte
                float hitPos[3];
                float hitDir[3];
            } shatter;
            struct //GlassRenderer::Action::<unnamed_tag>::<unnamed_type_trace> // sizeof=0x18
            {                                       // XREF: $F8734D8CD8D5548044E567214B041690/r
                float p0[3];
                float p1[3];
            } trace;
            struct //GlassRenderer::Action::<unnamed_tag>::<unnamed_type_explosion> // sizeof=0x1C
            {                                       // XREF: $F8734D8CD8D5548044E567214B041690/r
                float origin[3];
                float damageInner;
                float damageOuter;
                float radius;
                int mod;
            } explosion;

            struct //GlassRenderer::Action::<unnamed_tag>::<unnamed_type_melee> // sizeof=0x34
            {                                       // XREF: $F8734D8CD8D5548044E567214B041690/r
                float eyePos[3];
                float forward[3];
                float right[3];
                float up[3];
                float range;
            } melee;
        };
    };

    struct SortedShardsList : std::list<GlassShard *,SmallAllocatorTemplate<GlassShard *> > // sizeof=0x1C
    {
        // aislop

        SortedShardsList() = default;

        // pass allocator down to SmallAllocatorTemplate
        explicit SortedShardsList(SmallAllocator *alloc) 
            : std::list<GlassShard *, SmallAllocatorTemplate<GlassShard *>>(SmallAllocatorTemplate<GlassShard *>(alloc))
        {
        }

        // new/delete operators here are inlined in the bin
        //static void *operator new(size_t size, SmallAllocator *alloc);
        //static void *operator new(size_t size);
        //static void operator delete(void *ptr);

        void *operator new(size_t size, SmallAllocator *alloc)
        {
            return GlassesClient::Allocate(size, "C:\\projects_pc\\cod\\codsrc\\src\\glass\\glass_renderer.cpp", 72);
        }

        void *operator new(size_t size)
        {
            return GlassesClient::Allocate(
                size,
                "C:\\projects_pc\\cod\\codsrc\\src\\glass\\glass_renderer.cpp",
                72);
        }

        void operator delete(void *ptr)
        {
            GlassesClient::Free((char *)ptr);
        }

        void InsertReverse(GlassShard *shard);
        void Insert(GlassShard *shard);
    };

    void *operator new(size_t size);
    void operator delete(void *ptr);

    const dvar_s *cullShards;
    const dvar_s *drawDebug;
    const dvar_s *drawBBox;
    const dvar_s *drawShardOutline;
    const dvar_s *pickShards;
    const dvar_s *debugSplit;
    const dvar_s *freezeShards;
    const dvar_s *broom;
    struct //GlassRenderer::<unnamed_type_stat> // sizeof=0x10
    {                                       // XREF: GlassRenderer/r
        int numMovingShards;
        int numVisGroups;
        int numVisShards;
        int numOOMGroups;
    } stat;
    struct //GlassRenderer::<unnamed_type_stat> // sizeof=0x10
    {                                       // XREF: GlassRenderer/r
        int numMovingShards;
        int numVisGroups;
        int numVisShards;
        int numOOMGroups;
    } prevStat;
    int minFreeVertsMemory;
    int minFreeIndicesMemory;
    int minFreeShardsMemory;
    int maxCrashShards;
    unsigned int numShatters;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    unsigned __int64 shatterTimer;
    unsigned int numSplits;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    unsigned __int64 splitTimer;
    unsigned __int64 triangulateTimer;
    unsigned int genVertsCount;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    unsigned __int64 genVertsTimer;
    const dvar_s *maxShardSplit;
    const dvar_s *shardShatterSizeLimitScale;
    const dvar_s *defragMemory;
    const dvar_s *defragIndices;
    const dvar_s *freeBuffersDelay;
    const dvar_s *smpGlass;
    const dvar_s *shardSplitDir;
    const dvar_s *shardSplitDir2;
    const dvar_s *shardEdgeSize;
    const dvar_s *forceMultiplier;
    const dvar_s *forceOriginMult;
    const dvar_s *forceAttenuation;
    const dvar_s *extraVelocity;
    const dvar_s *doMaintenance;
    const dvar_s *lowLodDist;
    const dvar_s *maxShardLife;
    const dvar_s *timeUntilDropRange;
    const dvar_s *narrowShardRatio;
    const dvar_s *shatterFxMaxDist;
    const dvar_s *shatterFxMinEdgeLength;
    unsigned int frame;
    int timeLastUpdate;
    float deltaTime;
    GlassLock rendererLock;
    SmallAllocator smallAllocator;
    Allocator shardMemoryAllocator;
    FixedSizeAllocator<ShardGroup*> *groupsAllocator;
    FixedSizeAllocator<GlassShard*> *shardsAllocator;
    FixedSizeAllocator<GlassPhysics*> *physicsAllocator;
    GlassRenderer::SortedShardsList *colidingShards;
    GlassRenderer::SortedShardsList *tempShardsList;
    Material *usedMaterials[32];
    unsigned int numUsedMaterials;
    float allBBoxMin[3];
    float allBBoxMax[3];
    float coordScale[3];
    GlassShard *groupChanges[2048];
    int numGroupChanges;
    int maxNumGroupChanges;
    GlassShardMeshVertex *vertexList[22];
    GlassRenderer::Action actions[200];
    volatile unsigned int actionInputIndex;
    volatile unsigned int actionOutputIndex;

    GlassRenderer(const Glasses *glasses);
    ~GlassRenderer();

    void __thiscall Reset();
    void __thiscall InitShardMeshVertexLists();
    void __thiscall RemoveGlassShards(unsigned int glassIndex);
    GlassShard *__thiscall AllocShard();
    void __thiscall FreeShard(GlassShard *shard);
    GlassPhysics *__thiscall AllocPhysics();
    void __thiscall FreePhysics(GlassPhysics *phys);
    void __thiscall RemovePhysicsShards();
    void __thiscall AddGroupChange(GlassShard *shard);
    void __thiscall DoGroupChanges();
    void __thiscall Update(int threadId);
    void __thiscall GenerateVerts(
        int localClientNum,
        unsigned int viewIndex,
        unsigned int threadId);
    void __thiscall ExplosionEvent(
        const float *origin,
        float damageInner,
        float damageOuter,
        float radius,
        int mod);
    int __thiscall TracePoint(float *p0, const float *p1);
    void __thiscall Melee(
        float *eyePos,
        const float *forward,
        const float *right,
        const float *up,
        float range);
    unsigned int __thiscall CalcPackedPos(const float *pos);
    GlassPhysics *__thiscall GetShardGroup(const float *pos, const GlassDef *glassDef);
    GlassPhysics *__thiscall GetShardGroup(
        unsigned int packedPos,
        const GlassDef *glassDef);
    void __thiscall FreeShardGroup(ShardGroup *grp);
    Allocator::Memory **__thiscall AllocateShardMemory(
        unsigned int size,
        GlassShard *shard);
    void __thiscall FreeShardMemory(unsigned int *ptr);
    unsigned int __thiscall ShardMemorySize(unsigned int *ptr);
    bool __thiscall AddColidingShard(GlassShard *shard);
    void __thiscall RemoveColidingShard(GlassShard *shard);
    GlassShard *__thiscall GetSmallestColidingShard();
    bool __thiscall IsVisible(const float *minmax, unsigned int localClientNum);
    void __thiscall AddShatterAction(
        GlassClient *glass,
        const float *hitPos,
        const float *hitDir,
        bool gameState);
    void __thiscall AddExplosionAction(
        const float *origin,
        float damageInner,
        float damageOuter,
        float radius,
        int mod);
    void __thiscall AddTraceAction(const float *p0, const float *p1);
    void __thiscall AddMeleeAction(
        const float *eyePos,
        const float *forward,
        const float *right,
        const float *up,
        float range);
    void __thiscall ExecuteActions();
    void __thiscall StartMaintenance();
    void __thiscall GetSmallestShards(
        unsigned int maxShards,
        bool checkVisible,
        bool isVisible);
    void __thiscall GetLargestShards(
        unsigned int maxShards,
        bool checkVisible,
        bool isVisible);
    void __thiscall AddUsedShardMaterial(Material *material);
    void __thiscall DoMaintenance();
    static void __cdecl CrashGlassCmd();
    void __thiscall StartCrashGlass();
    void __thiscall CrashGlass();
    static void __cdecl PrintHwmCmd();
    void __thiscall PrintHwm();
    void __thiscall Broom();
    void __thiscall DrawDebug();
    void __thiscall BeginUpdate();
};




//void __cdecl Sys_WaitInterlockedCompareExchange(volatile int *destination, int value, int comperand);
