#pragma once
// Records brush-tree shape as a flat pre-order list with subtree jumps.
// Replays preserve each client's per-frame filter, cull, and draw decisions.

// INCLUDE AFTER qe3.h: GfxColor is a UNION (r_gfx.h:23) and cannot be forward-declared
// here; qe3.h is what brings it, selbrush_t and the complete orientation_t.
struct selbrush_t;
struct orientation_t;
struct xywndState_t;

// Prefabs own [next node, subtreeEnd); leaves use subtreeEnd == 0.
// orient indexes the brush's space; childOrient indexes its contents' composed space.
struct KiwiWalkNode
{
    selbrush_t *brush;
    int         orient;       // pool index of this node's own space
    int         childOrient;  // PREFAB: pool index of its composed orientation; -1 = leaf
    int         subtreeEnd;   // PREFAB: one past its subtree.  0 = leaf.
    int         flags;        // bit 0 = the prefab's "spawnflags" epair is non-zero
};

#define KIWI_WALK_SPAWNFLAGS 1

struct KiwiWalk
{
    const KiwiWalkNode  *nodes;
    int                  nodeCount;
    const orientation_t *orients;
    int                  orientCount;
    const int           *topLevel;      // node indices at depth 0, in list order
    int                  topLevelCount;
};

// Drop every recording.  Cheap; safe from any edit path, including inside a walk.
void     KiwiWalkCache_Invalidate();
unsigned KiwiWalkCache_Epoch();
void     KiwiWalkCache_Shutdown();

// Structure-only epoch for instance-set changes (alloc/free, list link/unlink, relink,
// classname). Transforms/textures bump only the general epoch. Extra structural bumps are
// safe, but a missed bump yields stale set-based consumers.
void     KiwiWalkCache_MarkStructural();
unsigned KiwiWalkCache_StructEpoch();

// The recording for `listHead`, recorded on demand.  NULL = unavailable (cap hit or
// allocation failure): the caller walks as before.
const KiwiWalk *KiwiWalkCache_Get( selbrush_t *listHead, const orientation_t *rootOrient );

// Clients bracket their top-level loop with Begin/End and call TopLevel per dispatched brush.
// Any mismatch abandons replay; backward selects ->prev order.
bool KiwiWalk_BeginReplay( selbrush_t *listHead, const orientation_t *rootOrient, bool backward );
void KiwiWalk_EndReplay();
bool KiwiWalk_TopLevel( const selbrush_t *brush );

// "spawnflags" for the node the cursor is on, without the epair walk.
// -1 = unknown (no session / cursor mismatch): the caller reads the epair itself.
int  KiwiWalk_SpawnflagsBit( const selbrush_t *brush );

// Enter returns the recorded prefab orientation; DrawChildren replays direct children through
// DrawBrush_PrefabContents' gates. Leave is a call-site bracket only; false means walk locally.
bool KiwiWalk_PrefabEnter( const selbrush_t *bboxBrush, const orientation_t **orientOut );
void KiwiWalk_DrawChildren( const orientation_t *prefabOrient, int viewType, int technique,
                            GfxColor *col, char width, int drawFlags, const char *childPrefix,
                            xywndState_t *drawXY );
void KiwiWalk_PrefabLeave();

// Folds identities, geometry/model versions, filter bits, entity placement, spawnflags, and
// prefab orientations across the current recorded subtree. Equal keeps cached surfaces valid;
// 0 means UNKNOWN and must be treated as changed. No allocation or string work.
unsigned long long KiwiWalk_SubtreeSignature( const selbrush_t *brush );

int  KiwiWalkCache_ResidentKB();
