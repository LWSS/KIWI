#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// FAST_HOT keeps this replay loop at /O2 in Debug while dispatched brush.cpp code stays /Od.

#include "stdafx.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "qe3.h"                  // selbrush_t (qe3.h:429), entity_s, orientation_t, g_qeglobals
#include "prefs.h"                // g_PrefsDlg (prefs.h:119) — fast_2d_view_dragging
#include "xywnd.h"                // xywndState_t (xywnd.h:49) — the XY client's cull state
#include "kiwi_walkcache.h"

// The recording COPIES orientation_t by value, so its size is load-bearing here.
static_assert( sizeof( orientation_t ) == 48, "orientation_t (fxprimitives.h != IDB)" );

// win_qe3.cpp:123
extern int  Sys_Printf( const char *fmt, ... );
// entity.cpp:629
extern void Entity_GetOrientation( entity_s_def *ent, orientation_t *orParent, orientation_t *orOut );
// entity.cpp:89
extern char *ValueForKey2( const entity_s *e, const char *key );
// filters.cpp:688
extern char FilterBrush( selbrush_t *a1, int a2 );
// brush.cpp:7389
extern void DrawBrush( selbrush_t *b, const orientation_t *orient, int viewType,
                       int technique, GfxColor *col, char width, int drawFlags,
                       const char *layerPrefix );
// brush.cpp:7068 (0x478a50)
extern bool PrefabContent_IsClipBrush( selbrush_t *b );
// brush.cpp:7084-7087
extern int  g_edPrefabPrefabsWalked;
extern int  g_edPrefabBrushesWalked;
extern int  g_edPrefabBrushesDrawn;

// prefab_s - local mirror of entity.cpp:316's prefab_s (0x54); only the list sentinels are read.
// Shared native prefab_s is declared in qe3.h.

// Exceeding any cap abandons recording so the caller runs the original walk.
#define KIWI_WALK_MAX_NODES     1000000
#define KIWI_WALK_MAX_ORIENTS     65536      // one per prefab instance reached by the walk
#define KIWI_WALK_MAX_DEPTH          32      // nested-prefab depth; a cycle would be caught here

// Shared invalidation epoch; kiwi_shadowcache.cpp reads it through this owner.
static unsigned s_epoch = 1;

void KiwiWalkCache_Invalidate()
{
    ++s_epoch;
}

unsigned KiwiWalkCache_Epoch()
{
    return s_epoch;
}

// Structure-only epoch avoids invalidating set pollers on each drag-frame s_epoch bump.
static unsigned s_structEpoch = 1;

void KiwiWalkCache_MarkStructural()
{
    ++s_structEpoch;
    ++s_epoch;              // a structural change also invalidates the recording itself
}

unsigned KiwiWalkCache_StructEpoch()
{
    return s_structEpoch;
}

// Two slots cover the active and selected display-list heads used by current callers.
struct WalkRec
{
    const selbrush_t *head;
    unsigned          epoch;        // 0 = empty/invalid
    KiwiWalkNode     *nodes;
    int               nodeCount;
    int               nodeCap;
    orientation_t    *orients;
    int               orientCount;
    int               orientCap;
    int              *topLevel;
    int               topCount;
    int               topCap;
    KiwiWalk          view;         // what KiwiWalkCache_Get hands out
};

static WalkRec s_walks[2];


static void WalkRec_Free( WalkRec *w )
{
    free( w->nodes );
    free( w->orients );
    free( w->topLevel );
    w->nodes       = nullptr;
    w->orients     = nullptr;
    w->topLevel    = nullptr;
    w->nodeCount   = w->nodeCap   = 0;
    w->orientCount = w->orientCap = 0;
    w->topCount    = w->topCap    = 0;
    w->epoch       = 0;
    w->head        = nullptr;
}

static WalkRec *WalkRec_For( const selbrush_t *head )
{
    for ( int i = 0; i < 2; ++i )
        if ( s_walks[i].head == head )
            return &s_walks[i];
    for ( int i = 0; i < 2; ++i )
        if ( !s_walks[i].head )
            return &s_walks[i];
    return nullptr;
}

// A failed realloc ABANDONS the recording; a partial replay would silently drop geometry.
static bool Walk_GrowNodes( WalkRec *w )
{
    if ( w->nodeCount < w->nodeCap )
        return true;
    int cap = w->nodeCap ? w->nodeCap * 2 : 4096;
    if ( cap > KIWI_WALK_MAX_NODES )
        cap = KIWI_WALK_MAX_NODES;
    if ( w->nodeCount >= cap )
        return false;
    KiwiWalkNode *p = (KiwiWalkNode *)realloc( w->nodes, (size_t)cap * sizeof( KiwiWalkNode ) );
    if ( !p )
        return false;
    w->nodes   = p;
    w->nodeCap = cap;
    return true;
}

static bool Walk_GrowOrients( WalkRec *w )
{
    if ( w->orientCount < w->orientCap )
        return true;
    int cap = w->orientCap ? w->orientCap * 2 : 256;
    if ( cap > KIWI_WALK_MAX_ORIENTS )
        cap = KIWI_WALK_MAX_ORIENTS;
    if ( w->orientCount >= cap )
        return false;
    orientation_t *p = (orientation_t *)realloc( w->orients, (size_t)cap * sizeof( orientation_t ) );
    if ( !p )
        return false;
    w->orients   = p;
    w->orientCap = cap;
    return true;
}

static bool Walk_GrowTop( WalkRec *w )
{
    if ( w->topCount < w->topCap )
        return true;
    int cap = w->topCap ? w->topCap * 2 : 256;
    int *p = (int *)realloc( w->topLevel, (size_t)cap * sizeof( int ) );
    if ( !p )
        return false;
    w->topLevel = p;
    w->topCap   = cap;
    return true;
}

// Record independently: a client's filter/cull gates would make the result frame-specific.
static bool Walk_RecordList( WalkRec *w, selbrush_t *listHead, int orientIdx, int depth );

// False only on a cap hit or allocation failure (the caller then abandons the recording).
static bool Walk_RecordPrefab( WalkRec *w, selbrush_t *b, entity_s *ent, int orientIdx, int depth )
{
    prefab_s *pf = (prefab_s *)ent->prefab;

    // The parent is COPIED out of the pool first: pushing the child can realloc the pool.
    orientation_t parent = w->orients[orientIdx];
    orientation_t childOrient;
    Entity_GetOrientation( (entity_s_def *)ent->def, &parent, &childOrient );

    if ( !Walk_GrowOrients( w ) )
        return false;
    const int childIdx = w->orientCount++;
    w->orients[childIdx] = childOrient;

    if ( !Walk_GrowNodes( w ) )
        return false;
    const int nodeIdx = w->nodeCount++;
    {
        KiwiWalkNode *n = &w->nodes[nodeIdx];
        n->brush       = b;
        n->orient      = orientIdx;
        n->childOrient = childIdx;
        n->subtreeEnd  = nodeIdx + 1;      // closed below
        // An entity key DrawBrush reads per prefab per frame, so it belongs to the recording.
        n->flags       = atol( ValueForKey2( ent->def, "spawnflags" ) )
                       ? KIWI_WALK_SPAWNFLAGS : 0;
    }

    if ( !Walk_RecordList( w, (selbrush_t *)&pf->active_brushlist, childIdx, depth + 1 ) )
        return false;

    w->nodes[nodeIdx].subtreeEnd = w->nodeCount;
    return true;
}

static bool Walk_RecordList( WalkRec *w, selbrush_t *listHead, int orientIdx, int depth )
{
    if ( depth >= KIWI_WALK_MAX_DEPTH )
        return false;                       // pathological nesting (or a cycle): no recording
    selbrush_t *sentinel = listHead;
    for ( selbrush_t *b = listHead->next; b && b != sentinel; b = b->next )
    {
        if ( depth == 0 )
        {
            if ( !Walk_GrowTop( w ) )
                return false;
            w->topLevel[w->topCount++] = w->nodeCount;
        }
        entity_s *ent = b->owner;
        if ( ent && ent->prefab )
        {
            if ( !Walk_RecordPrefab( w, b, ent, orientIdx, depth ) )
                return false;
            continue;
        }
        if ( !Walk_GrowNodes( w ) )
            return false;
        KiwiWalkNode *n = &w->nodes[w->nodeCount++];
        n->brush       = b;
        n->orient      = orientIdx;
        n->childOrient = -1;
        n->subtreeEnd  = 0;                 // leaf
        n->flags       = 0;
    }
    return true;
}

// Report a real refusal once per session; empty lists are not failures.
static void Walk_ReportCap()
{
    static bool s_reported = false;
    if ( s_reported )
        return;
    s_reported = true;
    Sys_Printf( "KIWI walk cache: this map exceeds the cache limits (cap %d nodes / "
                "%d orientations / %d nesting levels) - the entity, 2D and sun walks "
                "run uncached.\n",
                KIWI_WALK_MAX_NODES, KIWI_WALK_MAX_ORIENTS, KIWI_WALK_MAX_DEPTH );
}

const KiwiWalk *KiwiWalkCache_Get( selbrush_t *listHead, const orientation_t *rootOrient )
{
    if ( !listHead || !rootOrient )
        return nullptr;
    WalkRec *w = WalkRec_For( listHead );
    if ( !w )
        return nullptr;
    if ( w->head == listHead && w->epoch == s_epoch && w->nodeCount > 0 )
        return &w->view;

    // The epoch is re-checked after the walk: an edit funnel really can fire from inside one.
    const unsigned openedAt = s_epoch;
    WalkRec_Free( w );
    w->head = listHead;

    if ( !Walk_GrowOrients( w ) )
    {
        WalkRec_Free( w );
        Walk_ReportCap();
        return nullptr;
    }
    const int rootIdx = w->orientCount++;
    w->orients[rootIdx] = *rootOrient;

    if ( !Walk_RecordList( w, listHead, rootIdx, 0 ) )
    {
        WalkRec_Free( w );
        Walk_ReportCap();
        return nullptr;
    }
    if ( w->nodeCount <= 0 )
    {
        WalkRec_Free( w );      // an empty brush list: nothing to cache, nothing wrong
        return nullptr;
    }
    if ( openedAt != s_epoch )
    {
        WalkRec_Free( w );      // edited mid-walk: costs one uncached frame
        return nullptr;
    }

    w->epoch              = s_epoch;
    w->view.nodes         = w->nodes;
    w->view.nodeCount     = w->nodeCount;
    w->view.orients       = w->orients;
    w->view.orientCount   = w->orientCount;
    w->view.topLevel      = w->topLevel;
    w->view.topLevelCount = w->topCount;
    return &w->view;
}

// One replay session at a time; each pass closes before the next begins.
static const KiwiWalk *s_replay   = nullptr;
static bool            s_backward = false;
static int             s_topCursor = 0;    // next topLevel[] entry to match
static int             s_curNode   = -1;   // the node the client is dispatching now

// A mismatch = the tree changed without an epoch bump: end the session AND drop the recording.
static void Walk_Abandon()
{
    s_replay  = nullptr;
    s_curNode = -1;
    KiwiWalkCache_Invalidate();
}

bool KiwiWalk_BeginReplay( selbrush_t *listHead, const orientation_t *rootOrient, bool backward )
{
    s_replay   = nullptr;
    s_curNode  = -1;
    s_backward = backward;
    const KiwiWalk *w = KiwiWalkCache_Get( listHead, rootOrient );
    if ( !w || w->topLevelCount <= 0 )
        return false;
    s_replay    = w;
    s_topCursor = backward ? ( w->topLevelCount - 1 ) : 0;
    return true;
}

void KiwiWalk_EndReplay()
{
    s_replay  = nullptr;
    s_curNode = -1;
}

// Search from the cursor so filtered nodes may be skipped; a miss means an un-signaled change.
bool KiwiWalk_TopLevel( const selbrush_t *brush )
{
    if ( !s_replay )
        return false;
    if ( s_backward )
    {
        for ( int i = s_topCursor; i >= 0; --i )
        {
            if ( s_replay->nodes[s_replay->topLevel[i]].brush == brush )
            {
                s_curNode   = s_replay->topLevel[i];
                s_topCursor = i - 1;
                return true;
            }
        }
    }
    else
    {
        for ( int i = s_topCursor; i < s_replay->topLevelCount; ++i )
        {
            if ( s_replay->nodes[s_replay->topLevel[i]].brush == brush )
            {
                s_curNode   = s_replay->topLevel[i];
                s_topCursor = i + 1;
                return true;
            }
        }
    }
    Walk_Abandon();
    return false;
}

int KiwiWalk_SpawnflagsBit( const selbrush_t *brush )
{
    if ( !s_replay || s_curNode < 0 )
        return -1;
    const KiwiWalkNode &n = s_replay->nodes[s_curNode];
    if ( n.brush != brush || !n.subtreeEnd )
        return -1;
    return ( n.flags & KIWI_WALK_SPAWNFLAGS ) ? 1 : 0;
}

bool KiwiWalk_PrefabEnter( const selbrush_t *bboxBrush, const orientation_t **orientOut )
{
    if ( !s_replay || s_curNode < 0 )
        return false;
    const KiwiWalkNode &n = s_replay->nodes[s_curNode];
    if ( n.brush != bboxBrush || !n.subtreeEnd || n.childOrient < 0
         || n.childOrient >= s_replay->orientCount )
    {
        // Not where the cursor thinks it is: the recording no longer describes this tree.
        Walk_Abandon();
        return false;
    }
    *orientOut = &s_replay->orients[n.childOrient];
    return true;
}

void KiwiWalk_PrefabLeave()
{
    // The child loop restores s_curNode itself; this exists so the call sites read as a bracket.
}

// Mirrors DrawBrush_PrefabContents' direct-child loop (brush.cpp:7335-7384); keep tests/order in sync.
void KiwiWalk_DrawChildren( const orientation_t *prefabOrient, int viewType, int technique,
                            GfxColor *col, char width, int drawFlags, const char *childPrefix,
                            xywndState_t *drawXY )
{
    if ( !s_replay || s_curNode < 0 )
        return;
    const int             myNode = s_curNode;
    const KiwiWalkNode   *nodes  = s_replay->nodes;
    const int             end    = nodes[myNode].subtreeEnd;

    // 0x478d81: fast-drag filtering requires an active drag, the preference, and the XY path.
    const char fastDrag = ( g_qeglobals.toggle_unk02
                            && g_PrefsDlg->fast_2d_view_dragging
                            && drawXY != nullptr ) ? 1 : 0;

    for ( int i = myNode + 1; i < end; )
    {
        const KiwiWalkNode &n = nodes[i];
        const int next = n.subtreeEnd ? n.subtreeEnd : i + 1;
        ++g_edPrefabBrushesWalked;
        if ( FilterBrush( n.brush, fastDrag ) )     // 0x478d9a
        {
            i = next;
            continue;
        }
        if ( ( drawFlags & 2 ) != 0 && PrefabContent_IsClipBrush( n.brush ) )
        {
            i = next;
            continue;
        }
        ++g_edPrefabBrushesDrawn;
        s_curNode = i;                              // so a nested prefab finds itself
        DrawBrush( n.brush, prefabOrient, viewType, technique, col, width, drawFlags,
                   childPrefix );
        // A nested mismatch frees nothing, so the remaining siblings are still drawn.
        s_curNode = s_replay ? myNode : -1;
        i = next;
    }
    if ( s_replay )
        s_curNode = myNode;
}

// Per-object change signature (kiwi_walkcache.h).
namespace
{
    inline void Sig_Mix( unsigned long long &h, unsigned long long v )
    {
        h ^= v;
        h *= 1099511628211ull;                 // FNV-1a's prime; the fold, not a hash table
    }

    void Sig_Node( unsigned long long &h, const KiwiWalkNode &n, const KiwiWalk *w )
    {
        const selbrush_t *b = n.brush;
        Sig_Mix( h, (unsigned long long)(uintptr_t)b );
        if ( !b )
            return;
        Sig_Mix( h, (unsigned long long)(uintptr_t)b->def );
        // brush_t::version changes with geometry/texdef rebuilds and gates DrawBrush faceVis sync.
        if ( b->def )
        {
            Sig_Mix( h, (unsigned long long)(unsigned short)b->def->version );
            // KIWI (2026-09-10, user: "large amounts of terrain sculpting ... appears to
            // UNDO my work"): a patch edit that rebuilds only the control grid
            // (Patch_Rebuild without bounds: texture paint, blend, heatmap re-tints,
            // RebuildAllPatchVisuals) bumps patchMesh_t::version but NOT the symbiot
            // brush's version, so this signature stayed equal and the camera's surf-cache
            // PATCH pass kept replaying the segments it recorded for the OLD instance
            // visuals - the vertex buffer freed and re-uploaded by the live selected-pass
            // draw - and the sculpt looked reverted until some unrelated structural
            // change re-recorded the pass.  The patch def version is part of the object
            // now, so every grid edit dirties its segment.
            if ( b->def->patch )
                Sig_Mix( h, (unsigned long long)(unsigned short)b->def->patch->version );
        }
        // The bits FilterBrush reads: hidden, filtered, layer.
        Sig_Mix( h, (unsigned long long)(unsigned)( b->brushFlags & 0x1F ) );
        const entity_s *owner = b->owner;
        Sig_Mix( h, (unsigned long long)(uintptr_t)owner );
        if ( owner && owner->def )
        {
            const entity_s *eDef = owner->def;
            Sig_Mix( h, (unsigned long long)(uintptr_t)eDef->eclass );
            Sig_Mix( h, (unsigned long long)(uintptr_t)eDef->modelClass );
            Sig_Mix( h, (unsigned long long)(unsigned)eDef->modelInst );
            Sig_Mix( h, (unsigned long long)(unsigned)eDef->version );
            // Checkkey_Model bumps this for angles/modelscale/model (brush.cpp:794, :875);
            // bbox def->version does not cover placement.
            Sig_Mix( h, (unsigned long long)(unsigned)eDef->version_prob_wrong );
            for ( int k = 0; k < 3; ++k )
                Sig_Mix( h, (unsigned long long)*(const unsigned *)&eDef->origin[k] );
        }
        Sig_Mix( h, (unsigned long long)(unsigned)n.flags );
        if ( w && n.childOrient >= 0 && n.childOrient < w->orientCount )
        {
            // Epair changes rebuild the recording, so this composed orientation is current.
            const unsigned *f = (const unsigned *)&w->orients[n.childOrient];
            for ( int k = 0; k < (int)( sizeof( orientation_t ) / sizeof( unsigned ) ); ++k )
                Sig_Mix( h, (unsigned long long)f[k] );
        }
    }
}

unsigned long long KiwiWalk_SubtreeSignature( const selbrush_t *brush )
{
    if ( !s_replay || s_curNode < 0 || s_curNode >= s_replay->nodeCount )
        return 0;
    const KiwiWalkNode *nodes = s_replay->nodes;
    if ( nodes[s_curNode].brush != brush )
        return 0;
    const int end = nodes[s_curNode].subtreeEnd
                  ? nodes[s_curNode].subtreeEnd
                  : ( s_curNode + 1 );
    if ( end <= s_curNode || end > s_replay->nodeCount )
        return 0;
    unsigned long long h = 1469598103934665603ull;    // FNV-1a offset basis
    for ( int i = s_curNode; i < end; ++i )
        Sig_Node( h, nodes[i], s_replay );
    // Never hand back 0: that value is reserved for "unknown".
    return h ? h : 1ull;
}

void KiwiWalkCache_Shutdown()
{
    s_replay  = nullptr;
    s_curNode = -1;
    for ( int i = 0; i < 2; ++i )
        WalkRec_Free( &s_walks[i] );
    KiwiWalkCache_MarkStructural();   // every recorded node pointer is gone
}

int KiwiWalkCache_ResidentKB()
{
    int bytes = 0;
    for ( int i = 0; i < 2; ++i )
    {
        bytes += s_walks[i].nodeCap   * (int)sizeof( KiwiWalkNode );
        bytes += s_walks[i].orientCap * (int)sizeof( orientation_t );
        bytes += s_walks[i].topCap    * (int)sizeof( int );
    }
    return bytes / 1024;
}

