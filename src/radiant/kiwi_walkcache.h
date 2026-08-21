#pragma once
//  kiwi_walkcache.h - ONE recording of the brush tree's SHAPE (a flat pre-order node
//  list with subtree jumps), replayed by every pass that walks it.  Every cull, filter
//  and draw decision still happens per node, per frame, at the client's own replay.

// INCLUDE AFTER qe3.h: GfxColor is a UNION (r_gfx.h:23) and cannot be forward-declared
// here; qe3.h is what brings it, selbrush_t and the complete orientation_t.
struct selbrush_t;
struct orientation_t;
struct xywndState_t;

// Pre-order.  A PREFAB node is followed by its subtree and carries `subtreeEnd`; a LEAF
// has subtreeEnd == 0.  `orient` is the pool index of the space this brush's def->mins
// live in; `childOrient` is the composed orientation its own contents live in.
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

// The STRUCTURE-ONLY signal: bumped when the brush tree gains, loses or reclassifies a
// node (instance alloc/free, display-list link/unlink, entity relink, classname change) —
// NOT by a transform or a texture edit, which the epoch above does cover.  Pollers that
// only care about "is the set of objects the same" gate on this so a drag does not make
// them re-run.  Superset-safe: an extra bump costs one recount, a missed one is a stale
// readout, so every funnel that could change the set bumps it.
void     KiwiWalkCache_MarkStructural();
unsigned KiwiWalkCache_StructEpoch();

// The recording for `listHead`, recorded on demand.  NULL = unavailable (cap hit or
// allocation failure): the caller walks as before.
const KiwiWalk *KiwiWalkCache_Get( selbrush_t *listHead, const orientation_t *rootOrient );

// A client keeping its own top-level list loop brackets it with Begin/End and calls
// KiwiWalk_TopLevel per brush to put the cursor on the matching recorded node; ANY
// mismatch abandons the session.  `backward` = the list is walked through ->prev.
bool KiwiWalk_BeginReplay( selbrush_t *listHead, const orientation_t *rootOrient, bool backward );
void KiwiWalk_EndReplay();
bool KiwiWalk_TopLevel( const selbrush_t *brush );

// "spawnflags" for the node the cursor is on, without the epair walk.
// -1 = unknown (no session / cursor mismatch): the caller reads the epair itself.
int  KiwiWalk_SpawnflagsBit( const selbrush_t *brush );

// DrawBrush_PrefabContents' half.  Enter answers with the RECORDED composed orientation
// and pushes the node; DrawChildren replays the direct children through the same gate
// the immediate loop applies; Leave pops.  Enter false = the caller runs its own loop.
bool KiwiWalk_PrefabEnter( const selbrush_t *bboxBrush, const orientation_t **orientOut );
void KiwiWalk_DrawChildren( const orientation_t *prefabOrient, int viewType, int technique,
                            GfxColor *col, char width, int drawFlags, const char *childPrefix,
                            xywndState_t *drawXY );
void KiwiWalk_PrefabLeave();

// ── KIWI: "HAS THIS OBJECT CHANGED?" ─────────────────
// A fold over every input DrawBrush would read from the recorded subtree the replay
// cursor is sitting on (KiwiWalk_TopLevel put it there): brush and def identity, the
// def's geometry version, the filter/hidden bits, the owning entity's placement and
// model version, the recorded spawnflags, and — for a prefab node — its composed
// orientation.  Equal signature = the object draws identically, so its cached surf
// records stand; different = redraw it.  0 means UNKNOWN (no recording, or the cursor
// is not on this brush) and the caller must treat that as "changed".
// Cost is one pass over the recorded subtree with no allocation and no string work.
unsigned long long KiwiWalk_SubtreeSignature( const selbrush_t *brush );

int  KiwiWalkCache_ResidentKB();
