#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Radiant brush/entity undo and redo. Undo brush lists contain brush_t
// definitions; entity instance lists use entity_s nodes.

#include "stdafx.h"
#include "qe3.h"
#include "kiwi_ported.h"   // KIWI: shared engine/editor externs
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── forward declarations ──────────────────────────────────────────────────────

// prefab_s is defined in entity.cpp; forward-declare here to use in Prefab_Init signature.
struct prefab_s;

// brush.cpp
extern size_t      Brush_MemorySize( brush_t *def );
extern brush_t    *Brush_FullClone( brush_t *src );
extern char       *AllocMaterialString( const char *src );

// entity.cpp
extern unsigned int Brush_ApplyEntityColor( brush_t *b );
extern void        Entity_Free_R( entity_s *e );
extern void        Entity_LinkBrushAt( brush_t *b, entity_s *ent, int ordinal );   // KIWI
extern int         Entity_BrushOrdinal( const brush_t *b );                         // KIWI
// KIWI-UX: the typed selection core's legacy-change notification (kiwi_selection.cpp).
extern void        Sel_InvalidateFromLegacy();
// KIWI-UX (shakeout I): the unified undo journal's four hooks (kiwi_undo.h).  Each
// is ONE line at the tail of an existing function and changes no ported behaviour;
// see kiwi_undo.h for the ticket model and the eviction-sync argument.
extern void        KiwiUndo_NoteLegacyRecord( const char *operation );
extern void        KiwiUndo_NoteLegacyEvicted();
extern void        KiwiUndo_NoteLegacyCleared();
extern void        KiwiUndo_NoteLegacyRedoCleared();

extern void        IncRef( entity_s *e, entity_s *list );   // entity.cpp 0x483bf0
extern void        DecRef( entity_s *e );                   // entity.cpp 0x483c30
extern entity_s   *Prefab_Init( prefab_s *instList, entity_s_def *def, selbrush_t *activeList );

// Entity_Clone (0x485090) -- allocs a new entity_s_def, copies epairs.
extern entity_s   *Entity_Clone( const entity_s *src );
// Entity_MemorySize (0x483de0) -- returns _msize(e) + sum of epair strings.
extern size_t      Entity_MemorySize( entity_s *e );

// engine_stubs.cpp / qe3.cpp / other
// Sys_PrintActiveBrushes (0x42c640) populates the brush/entity list diagnostics. Used by
// Undo_GeneralStart's head and Undo_End's tail; both call MainFrm_BrushList here.
extern void        MainFrm_BrushList( const char *label, selbrush_t *list );
// va is in q_shared.h (included via stdafx.h)
// entity.cpp global -- screen-update guard
extern bool        g_bScreenUpdates;

// layers.cpp
extern void        Layers_SetMapLayers();
extern void        Layers_RefreshDialog();

// ── globals ───────────────────────────────────────────────────────────────────
// 0x23F1628
static undo_s  *g_redolist         = nullptr;
// 0x23F15CC
static undo_s  *g_lastredo         = nullptr;
// 0x23F1630
static undo_s  *g_undolist         = nullptr;
// 0x23F162C -- also used by select.cpp via extern
undo_s         *g_lastundo         = nullptr;

// 0x25D5B18
static int      g_undoSize         = 0;
// 0x25D5B1C
static int      g_undoMemorySize   = 0;
// 0x739F6C
static int      g_undoMaxSize      = 64;
// 0x739F70
static int      g_undoMaxMemorySize = 2 * 1024 * 1024;
// 0x739F74
static int      g_undoId           = 1;
// 0x739F78
static int      g_redoId           = 1;

// Accessor for the ON_UPDATE_COMMAND_UI Redo-greying handler (g_lastredo is static).
// True when a redo record is available (matches OnUpdateEditRedo 0x428790).
bool Undo_RedoAvailable() { return g_lastredo != nullptr; }

// KIWI: where each brush def in an undo / redo record sat in its entity's brush list, so
// Undo / Redo put it back there - Entity_LinkBrush appends, and every undo reordered the
// saved map (and the brush numbers cod4map reports).  Keyed by the record's def; a stale
// key is harmless, every lookup is of a def recorded in the same cycle.
static std::unordered_map<const brush_t *, int> s_undoOrdinal;
// Undo_AddBrushList's one walk per entity (a selection of n brushes stays O(n)).
static const std::unordered_map<const brush_t *, int> *s_batchOrdinal = nullptr;

static int Undo_OrdinalOf( const brush_t *def )
{
    if ( s_batchOrdinal )
    {
        auto it = s_batchOrdinal->find( def );
        if ( it != s_batchOrdinal->end() )
            return it->second;
    }
    return Entity_BrushOrdinal( def );
}

// Every brush of the entities that own the defs in `list`, by position.
static void Undo_OrdinalsOf( selbrush_t *list, std::unordered_map<const brush_t *, int> &out )
{
    std::unordered_set<entity_s *> seen;
    for ( selbrush_t *i = list->next; i && i != list; i = i->next )
    {
        brush_t *def = i->def;
        if ( !def || !def->owner || !seen.insert( def->owner ).second )
            continue;
        entity_s *ent = def->owner;
        brush_t *const sentinel = (brush_t *)&ent->def;
        int n = 0;
        for ( brush_t *d = (brush_t *)ent->brushes.prev; d && d != sentinel; d = d->onext, ++n )
            out[d] = n;
    }
}

// Re-link order: the defs of a record, by the position each had (unknown last).
static std::vector<brush_t *> Undo_DefsInOrder( brush_t *list )
{
    std::vector<brush_t *> defs;
    for ( brush_t *d = (brush_t *)list->onext; d && d != list; d = (brush_t *)d->onext )
        defs.push_back( d );
    std::stable_sort( defs.begin(), defs.end(), []( const brush_t *a, const brush_t *b ) {
        auto ia = s_undoOrdinal.find( a ), ib = s_undoOrdinal.find( b );
        return ( ia == s_undoOrdinal.end() ? INT_MAX : ia->second )
             < ( ib == s_undoOrdinal.end() ? INT_MAX : ib->second );
    } );
    return defs;
}

static int Undo_TakeOrdinal( const brush_t *def )
{
    auto it = s_undoOrdinal.find( def );
    if ( it == s_undoOrdinal.end() )
        return -1;
    const int ord = it->second;
    s_undoOrdinal.erase( it );
    return ord;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45de20  Undo_01_NoMoreUndo
// Inserts a brush DEF at the HEAD of a brushlist sentinel (via onext/oprev).
// ─────────────────────────────────────────────────────────────────────────────
static brush_t *Undo_01_NoMoreUndo( brush_t *list, brush_t *brush )
{
    vassert( (brush->refCount >= 0), "(brush->refCount) = %i", brush->refCount );   // 85
    iassert( brush->owner == NULL );                                                // 86

    ++brush->refCount;
    brush_t *old_next = (brush_t *)list->onext;
    brush->oprev      = list;
    brush->onext      = (brush_t *)old_next;
    old_next->oprev   = brush;
    list->onext       = brush;
    return old_next;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45de80  Undo_Refcount
// Decrements refCount and UNLINKS a brush DEF from its current list.
// ─────────────────────────────────────────────────────────────────────────────
static brush_t *Undo_Refcount( brush_t *brush )
{
    vassert( (brush->refCount >= 1), "(brush->refCount) = %i", brush->refCount );   // 98
    --brush->refCount;
    iassert( brush->onext );   // 101
    iassert( brush->oprev );   // 102

    brush_t *next       = (brush_t *)brush->onext;
    ((brush_t *)brush->oprev)->onext = brush->onext;
    next->oprev         = brush->oprev;
    brush->onext        = nullptr;
    brush->oprev        = nullptr;
    return next;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45df20  Undo_ClearRedo
// Frees all redo records (brush defs + entity defs + undo_s nodes).
// ─────────────────────────────────────────────────────────────────────────────
void Undo_ClearRedo()
{
    undo_s *redo = g_redolist;
    while ( redo )
    {
        undo_s *nextredo = redo->next;

        // Free all brush defs in this redo's brushlist (brush_t chain via onext)
        brush_t *brush = (brush_t *)redo->brushlist.onext;
        while ( brush != &redo->brushlist )
        {
            iassert( brush );   // 128
            brush_t *pNext = (brush_t *)brush->onext;
            Undo_Refcount( brush );
            vassert( (brush->refCount == 0), "(brush->refCount) = %i", brush->refCount );   // Undo.cpp:131
            s_undoOrdinal.erase( brush );   // KIWI
            Brush_Free_R( brush );
            brush = pNext;
        }

        // Free all entity defs in this redo's entitylist
        entity_s *entity = redo->entitylist.next;
        while ( entity != &redo->entitylist )
        {
            iassert( entity );   // 136
            entity_s *pNext = entity->next;
            iassert( entity->refCount == 1 );   // Undo.cpp:138
            --entity->refCount;
            Entity_Free_R( entity );
            entity = pNext;
        }

        ::operator delete( redo );
        redo = nextredo;
    }
    g_redolist  = nullptr;
    g_lastredo  = nullptr;
    g_redoId    = 1;

    KiwiUndo_NoteLegacyRedoCleared();   // KIWI-UX (shakeout I)
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e070  Undo_Clear
// Clears both the undo and redo lists.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_Clear()
{
    Undo_ClearRedo();

    undo_s *undo = g_undolist;
    while ( undo )
    {
        undo_s *nextundo = undo->next;

        // Free all brush defs in this undo's brushlist (brush_t chain via onext)
        brush_t *brush = (brush_t *)undo->brushlist.onext;
        while ( brush != &undo->brushlist )
        {
            iassert( brush );   // 162
            brush_t *bNext = (brush_t *)brush->onext;
            g_undoMemorySize -= (int)Brush_MemorySize( brush );
            Undo_Refcount( brush );
            vassert( (brush->refCount == 0), "(brush->refCount) = %i", brush->refCount );   // Undo.cpp:166
            s_undoOrdinal.erase( brush );   // KIWI
            Brush_Free_R( brush );
            brush = bNext;
        }

        // Free all entity defs in this undo's entitylist
        entity_s *entity = undo->entitylist.next;
        while ( entity != &undo->entitylist )
        {
            iassert( entity );   // 171
            entity_s *eNext = entity->next;
            g_undoMemorySize -= (int)Entity_MemorySize( entity );
            iassert( entity->refCount == 1 );   // Undo.cpp:174
            --entity->refCount;
            Entity_Free_R( entity );
            entity = eNext;
        }

        g_undoMemorySize -= sizeof(undo_s);
        ::operator delete( undo );
        undo = nextundo;
    }

    g_undolist       = nullptr;
    g_lastundo       = nullptr;
    g_undoSize       = 0;
    g_undoMemorySize = 0;
    g_undoId         = 1;

    // KIWI-UX (shakeout I): the whole legacy stack just died in this function's own
    // loop (it does NOT go through Undo_FreeFirstUndo), so the journal's LEGACY
    // tickets have to die with it.  Map New / Map Load / Undo_SetMax*Size.
    KiwiUndo_NoteLegacyCleared();
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e230  Undo_SetMaxSize
// ─────────────────────────────────────────────────────────────────────────────
void Undo_SetMaxSize( int size )
{
    Undo_Clear();
    g_undoMaxSize = 1;
    if ( size >= 1 )
        g_undoMaxSize = size;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e250  Undo_GetMaxSize
// ─────────────────────────────────────────────────────────────────────────────
int Undo_GetMaxSize()
{
    return g_undoMaxSize;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e260  Undo_SetMaxMemorySize
// ─────────────────────────────────────────────────────────────────────────────
void Undo_SetMaxMemorySize( int size )
{
    Undo_Clear();
    if ( size < 1024 )
        g_undoMaxMemorySize = 1024;
    else
        g_undoMaxMemorySize = size;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e280  Undo_GetMaxMemorySize
// ─────────────────────────────────────────────────────────────────────────────
int Undo_GetMaxMemorySize()
{
    return g_undoMaxMemorySize;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e290  Undo_FreeFirstUndo
// Removes and frees the oldest undo from the undo list.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_FreeFirstUndo()
{
    undo_s *undo         = g_undolist;
    undo_s *nextUndo     = undo->next;
    nextUndo->prev       = nullptr;
    g_undolist           = nextUndo;

    // Free all brush defs (brush_t chain via onext; re-read head each iteration
    // because Undo_Refcount removes b from the list before Brush_Free_R)
    brush_t *brush = (brush_t *)undo->brushlist.onext;
    while ( brush != &undo->brushlist )
    {
        iassert( brush );   // 235
        g_undoMemorySize -= (int)Brush_MemorySize( brush );
        iassert( brush->refCount == 1 );   // Undo.cpp:237
        Undo_Refcount( brush );
        vassert( (brush->refCount == 0), "(brush->refCount) = %i", brush->refCount );   // Undo.cpp:239
        s_undoOrdinal.erase( brush );   // KIWI
        Brush_Free_R( brush );
        // Re-read head (Undo_Refcount removes brush, Brush_Free_R frees it)
        brush = (brush_t *)undo->brushlist.onext;
    }

    // Free all entity defs
    entity_s *entity = undo->entitylist.next;
    while ( entity != &undo->entitylist )
    {
        iassert( entity );   // 244
        entity_s *eNext  = entity->next;
        g_undoMemorySize -= (int)Entity_MemorySize( entity );
        iassert( entity->refCount == 1 );   // Undo.cpp:247
        --entity->refCount;
        Entity_Free_R( entity );
        entity = eNext;
    }

    g_undoMemorySize -= sizeof(undo_s);
    ::operator delete( undo );
    --g_undoSize;

    // KIWI-UX (shakeout I): THE eviction point (kiwi_undo.h EVICTION SYNC).  Called
    // from Undo_GeneralStart's record-count cap and from Undo_End's memory-cap trim
    // loop, and from nowhere else — so one hook here keeps the journal's LEGACY
    // ticket count equal to the legacy record count, always.
    KiwiUndo_NoteLegacyEvicted();
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e3f0  Undo_GeneralStart
// Allocates a new undo record and appends it to the undo list.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_GeneralStart( const char *operation )
{
    char *s1 = (char *)va( "%s - active_brushes", operation );
    MainFrm_BrushList( s1, &active_brushes );
    char *s2 = (char *)va( "%s - active_brushes", operation );
    MainFrm_BrushList( s2, &selected_brushes );

    if ( g_lastundo && !g_lastundo->done )
        Sys_Printf( "Undo_Start: WARNING last undo not finished.\n" );

    undo_s *_undo = (undo_s *)::operator new( sizeof(undo_s) );
    if ( !_undo )
        return;

    memset( _undo, 0, sizeof(undo_s) );

    // Init brushlist sentinel (brush_t chain, self-pointing)
    _undo->brushlist.onext = (brush_t_def *)&_undo->brushlist;
    _undo->brushlist.oprev = (brush_t_def *)&_undo->brushlist;

    // Init entitylist sentinel
    _undo->entitylist.next = &_undo->entitylist;
    _undo->entitylist.prev = &_undo->entitylist;

    // Link at tail of undo list
    undo_s *prevLast = g_lastundo;
    if ( prevLast )
        prevLast->next = _undo;
    else
        g_undolist = _undo;
    _undo->prev = prevLast;
    _undo->next = nullptr;
    g_lastundo  = _undo;

    _undo->time = (double)clock() / 1000.0;

    int v9 = g_undoId;
    if ( v9 > 2 * g_undoMaxSize || v9 <= 0 )
        v9 = 1;
    _undo->id    = v9;
    g_undoId     = v9 + 1;
    _undo->done  = 0;
    _undo->operation = operation;

    // Reset ownerPrev (undoId) on all active brush defs that already carry this ID
    for ( selbrush_t *pInst = active_brushes.next;
          pInst != &active_brushes;
          pInst = pInst->next )
    {
        brush_t *def = pInst->def;
        if ( def->ownerPrev == (entity_s *)(intptr_t)_undo->id )
            def->ownerPrev = nullptr;
    }
    for ( selbrush_t *pInst = selected_brushes.next;
          pInst != &selected_brushes;
          pInst = pInst->next )
    {
        brush_t *def = pInst->def;
        if ( def->ownerPrev == (entity_s *)(intptr_t)_undo->id )
            def->ownerPrev = nullptr;
    }

    // Reset epairEdits on all entities that already carry this ID
    entity_s *pEntity = entities.next;
    while ( pEntity )
    {
        if ( pEntity == &entities )
            break;
        if ( pEntity->epairEdits == _undo->id )
            pEntity->epairEdits = 0;
        pEntity = pEntity->next;
    }

    g_undoMemorySize += sizeof(undo_s);
    if ( ++g_undoSize > g_undoMaxSize )
        Undo_FreeFirstUndo();
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e590  Undo_BrushInUndo
// Returns true if a brush DEF is already registered in the current undo
// (brush_t.ownerPrev == g_lastundo->id).
// ─────────────────────────────────────────────────────────────────────────────
static bool Undo_BrushInUndo( brush_t *brush )
{
    undo_s *undo = g_lastundo;
    iassert( undo );    // Undo.cpp:335
    iassert( brush );   // Undo.cpp:336
    return brush->ownerPrev == (entity_s *)(intptr_t)undo->id;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e5f0  Undo_EntityInUndo
// Returns true if an entity DEF is already registered in the current undo.
// ─────────────────────────────────────────────────────────────────────────────
static bool Undo_EntityInUndo( entity_s_def *ent )
{
    undo_s *undo = g_lastundo;
    iassert( undo );   // 351
    iassert( ent );    // 352
    return ent->epairEdits == undo->id
        || ent == (entity_s_def *)world_entity->def;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e660  Undo_Start
// Clears redo list, then starts a new undo record.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_Start( const char *operation )
{
    Undo_ClearRedo();
    Undo_GeneralStart( operation );
}

// Forward declaration so Undo_AddBrushList can call Undo_AddEntity below.
void Undo_AddEntity( entity_s *entity );

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e680  Undo_AddBrush
// Adds a brush DEF to the current undo record.  The parameter is typed entity_brush_s*
// but is the brush_t DEF node.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_AddBrush( entity_brush_s *pBrushInst )
{
    brush_t *brush = (brush_t *)(intptr_t)pBrushInst;

    iassert( brush );        // 380
    iassert( g_lastundo );   // 381

    // Skip if already in undo (ownerPrev == current undo id)
    if ( Undo_BrushInUndo( brush ) )
        return;
    // KIWI: an ownerless def cannot be restored (undo re-links by the owner's numberId) and
    // the clone below read brush->owner unguarded
    iassert( brush->owner );
    if ( !brush->owner )
        return;

    // Clone the brush def (0x475E80).
    brush_t *clone = Brush_FullClone( brush );

    // The source brush's layer string is copied onto the clone (the binary inlines
    // Brush_SetLayerString here; its brush.cpp:2354 head-check lives in the helper).
    char *srcLayerStr = brush->parent_layer_string;

    extern void Brush_SetLayerString( brush_t *b, const char *str );   // brush.cpp 0x4758A0
    Brush_SetLayerString( clone, srcLayerStr );

    // The clone remembers the owning entity's numberId.
    clone->unk1 = brush->owner->numberId;

    // The clone keeps the source's ownerPrev; clone->owner = 0 keeps Undo_Undo's
    // owner-walk from following a stale pointer.  Set BEFORE linking the clone into the
    // brushlist.
    clone->ownerPrev = brush->ownerPrev;
    clone->owner     = nullptr;
    s_undoOrdinal[clone] = Undo_OrdinalOf( brush );   // KIWI: undo re-links it there

    // Link clone into undo brushlist at head
    Undo_01_NoMoreUndo( &g_lastundo->brushlist, clone );

    // Mark source brush's ownerPrev with current undo ID
    brush->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;

    // Track memory
    g_undoMemorySize += (int)Brush_MemorySize( clone );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e7c0  Undo_AddBrushList
// Adds all brush DEFs from a selbrush_t display-list into the current undo.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_AddBrushList( selbrush_t *sb )
{
    if ( !g_lastundo )
    {
        Sys_Printf( "Undo_AddBrushList: no last undo.\n" );
        return;
    }
    if ( g_lastundo->entitylist.next != &g_lastundo->entitylist )
        Sys_Printf( "Undo_AddBrushList: WARNING adding brushes after entity.\n" );

    std::unordered_map<const brush_t *, int> ordinals;   // KIWI: one walk per entity
    Undo_OrdinalsOf( sb, ordinals );
    s_batchOrdinal = &ordinals;

    // Walks backwards through the display list (sb->prev).
    for ( selbrush_t *i = sb->prev; i != sb; i = i->prev )
    {
        entity_s_def *owner = (entity_s_def *)i->def->owner;
        if ( *(int *)&owner->eclass->fixedsize )
            Undo_AddEntity( (entity_s *)owner );
        Undo_AddBrush( (entity_brush_s *)i->def );
    }
    s_batchOrdinal = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e870  Undo_EndBrushList
// Stamps the current undo ID into all brush defs in a display list.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_EndBrushList( selbrush_t *brushlist )
{
    if ( !g_lastundo )
        return;
    if ( g_lastundo->done )
        return;

    for ( selbrush_t *i = brushlist->next; i; i = i->next )
    {
        if ( i == brushlist )
            break;
        brush_t *def = i->def;
        def->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;
        entity_s *owner = (entity_s *)def->owner;
        if ( *(int *)&owner->eclass->fixedsize )
            owner->epairEdits = g_lastundo->id;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e8b0  Undo_AddEntity
// Adds an entity DEF to the current undo record.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_AddEntity( entity_s *entity )
{
    iassert( entity );   // 500

    if ( Undo_EntityInUndo( (entity_s_def *)entity ) )
    {
        return;
    }

    entity_s *clone = Entity_Clone( entity );
    vassert( (clone->refCount == 0), "(clone->refCount) = %i", clone->refCount );   // Undo.cpp:506
    ++clone->refCount;

    // Save old undo ID from source into clone
    clone->epairEdits = entity->epairEdits;
    clone->numberId   = entity->numberId;

    // Stamp source entity with current undo ID
    entity->epairEdits = g_lastundo->id;

    // Link clone into the undo entitylist
    IncRef( clone, &g_lastundo->entitylist );

    g_undoMemorySize += (int)Entity_MemorySize( clone );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e990  Undo_AddEntity_W
// Adds an entity AND all of its brush defs to the current undo.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_AddEntity_W( entity_s *a1 )
{
    if ( g_lastundo )
    {
        Undo_AddEntity( (entity_s *)a1 );
        if ( a1 != (entity_s *)world_entity->def )
        {
            // Walk the entity's brush DEF list and Undo_AddBrush each: start =
            // brushes.prev (the first brush def), sentinel = &a1->def, step via the brush
            // def's onext, and pass the def DIRECTLY.
            brush_t *b = (brush_t *)a1->brushes.prev;
            for ( ; (void *)b != (void *)&a1->def; b = b->onext )
                Undo_AddBrush( (entity_brush_s *)b );
        }
    }
    else
    {
        Sys_Printf( "Undo_AddEntity: no last undo.\n" );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45e9e0  Undo_SetIdForEntity
// Stamps the current undo ID into an entity and all its brush defs.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_SetIdForEntity( entity_s_def *ent )
{
    if ( !g_lastundo )
        return;
    if ( g_lastundo->done )
        return;
    if ( ent == (entity_s_def *)world_entity->def )
        return;

    ent->epairEdits = g_lastundo->id;
    // Stamp the undo id into each brush DEF's ownerPrev: same def-list walk as
    // Undo_AddEntity_W (brushes.prev .. &ent->def, step via the def's onext).
    for ( brush_t *b = (brush_t *)ent->brushes.prev;
          (void *)b != (void *)&ent->def; b = b->onext )
        b->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45ea20  Undo_End
// Closes the current undo record.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_End()
{
    if ( !g_lastundo )
        return;
    if ( g_lastundo->done )
        return;

    undo_s *v0 = g_lastundo;
    g_lastundo->done = 1;

    // Trim oldest undos while memory exceeds limit (always keep at least one)
    while ( g_undoMemorySize > g_undoMaxMemorySize )
    {
        if ( g_undolist == v0 )
            break;
        Undo_FreeFirstUndo();
        if ( g_undoMemorySize <= g_undoMaxMemorySize )
            break;
        v0 = g_lastundo;
    }

    MarkMapModified();
    // Refresh the brush/entity list diagnostics.
    {
        char *s1 = (char *)va( "%s - active_brushes", g_lastundo->operation );
        MainFrm_BrushList( s1, &active_brushes );
        char *s2 = (char *)va( "%s - active_brushes", g_lastundo->operation );
        MainFrm_BrushList( s2, &selected_brushes );
    }

    // KIWI-UX (shakeout I): THE single close point, and therefore the one place a
    // LEGACY ticket can honestly be minted (kiwi_undo.h WHERE THE TICKETS COME
    // FROM).  This function is the ONLY writer of `done`, both early-outs above
    // mean "there is no record to close", and both Undo_Undo and Undo_Redo consume
    // records only in the `done` state — so a ticket here names a record that can
    // actually be undone.  `operation` is a literal/static by the same contract the
    // record itself relies on (line 390 stores the POINTER), so the journal keeps
    // the pointer rather than copying.
    // The hook is deaf while KiwiCmd_UndoCancel is unwinding its own bracket
    // (KiwiUndo_SuppressBegin/End, kiwi_command.cpp) — that close is followed
    // immediately by an Undo_Undo that destroys the record again.
    KiwiUndo_NoteLegacyRecord( g_lastundo->operation );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45ea90  Undo_Undo
// Undoes the last completed operation.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_Undo()
{
    if ( !g_lastundo )
    {
        Sys_Printf( "Nothing left to undo.\n" );
        return;
    }
    if ( !g_lastundo->done )
        Sys_Printf( "Undo_Undo: WARNING: last undo not yet finished!\n" );

    // Refresh the brush/entity list diagnostics.
    {
        char *s1 = (char *)va( "%s - active_brushes", g_lastundo->operation );
        MainFrm_BrushList( s1, &active_brushes );
        char *s2 = (char *)va( "%s - active_brushes", g_lastundo->operation );
        MainFrm_BrushList( s2, &selected_brushes );
    }

    undo_s *undo = g_lastundo;

    // Detach undo from the list tail
    if ( undo->prev )
        undo->prev->next = nullptr;
    else
        g_undolist = nullptr;
    g_lastundo = undo->prev;

    // Allocate a new redo record
    undo_s *redo = (undo_s *)::operator new( sizeof(undo_s) );
    if ( !redo )
        return;
    memset( redo, 0, sizeof(undo_s) );

    redo->brushlist.onext  = (brush_t_def *)&redo->brushlist;
    redo->brushlist.oprev  = (brush_t_def *)&redo->brushlist;
    redo->entitylist.next  = &redo->entitylist;
    redo->entitylist.prev  = &redo->entitylist;

    // Append redo to the redo list tail
    undo_s *prevRedo = g_lastredo;
    if ( prevRedo )
        prevRedo->next = redo;
    else
        g_redolist = redo;
    redo->prev = prevRedo;
    redo->next = nullptr;
    g_lastredo = redo;

    redo->time      = (double)clock() / 1000.0;
    redo->id        = g_redoId;
    redo->done      = 1;
    g_redoId        = g_redoId + 1;
    redo->operation = undo->operation;

    // Reset redoId on all active brush defs and entities that already carry it
    // (brush defs keep it in def->def).
    for ( selbrush_t *i = active_brushes.next; i != &active_brushes; i = i->next )
    {
        brush_t *def = i->def;
        if ( def->def == (brush_t_def *)(intptr_t)redo->id )
            def->def = nullptr;
    }
    // Clears the entity redoId stamp, NOT epairEdits; it is the same field Phase 3
    // stamps — stale stamps must not collide.
    for ( entity_s *v12 = entities.next; v12 != &entities; v12 = v12->next )
    {
        if ( v12->redoId == redo->id )
            v12->redoId = 0;
    }

    // Deselect all
    Select_Deselect( 1 );

    // KIWI: where the defs going to the redo record sit, before any unlink (Undo_Redo puts
    // them back there).
    {
        std::unordered_map<const brush_t *, int> ordinals;
        Undo_OrdinalsOf( &active_brushes, ordinals );
        for ( selbrush_t *inst = active_brushes.next; inst != &active_brushes; inst = inst->next )
            if ( inst->def->ownerPrev == (entity_s *)(intptr_t)undo->id )
            {
                auto it = ordinals.find( inst->def );
                s_undoOrdinal[inst->def] = it != ordinals.end() ? it->second : -1;
            }
    }

    // ── Phase 1: Move "created" brush instances (ownerPrev==undo->id) into redo's brushlist.
    {
        brush_t   *p_redoBrushlist = &redo->brushlist;
        selbrush_t *inst = active_brushes.next;
        while ( inst != &active_brushes )
        {
            selbrush_t *pNextInst = (selbrush_t *)&inst->next->prev;
            brush_t *def = inst->def;

            if ( def->ownerPrev == (entity_s *)(intptr_t)undo->id )
            {
                // Save entity instance ID for later redo restore
                def->unk1 = def->owner->numberId;

                {
                    selbrush_t *brushInst = inst;
                    vassert( (brushInst->def->refCount >= 2), "(brushInst->def->refCount) = %i", brushInst->def->refCount );   // Undo.cpp:702
                }

                Entity_UnlinkBrush( def );   // entity.cpp:1208

                // Move def into redo's brushlist
                Undo_01_NoMoreUndo( p_redoBrushlist, def );
                Brush_Free( inst );

                {
                    brush_t *brushDef = def;
                    iassert( brushDef->refCount == 1 );   // Undo.cpp:707
                }
            }
            inst = (selbrush_t *)pNextInst;
        }
    }

    // ── Phase 2: Move "created" entity instances (epairEdits==undo->id) into redo's entitylist.
    {
        entity_s *pEInst = entityInsts.next;
        entity_s *p_entitylist_redo = &redo->entitylist;
        while ( pEInst != &entityInsts )
        {
            entity_s     *pNextEInst = (entity_s *)&pEInst->next->prev;
            entity_s_def *def        = (entity_s_def *)pEInst->def;

            if ( def->epairEdits == undo->id )
            {
                // IncRef's 690 assert is provably true after DecRef nulls both links.
                DecRef( (entity_s *)def );
                IncRef( (entity_s *)def, p_entitylist_redo );

                {
                    entity_s_def *entity = def;
                    iassert( entity->refCount == 1 );   // Undo.cpp:720
                    ++entity->refCount;
                    Entity_Free( pEInst );
                    iassert( entity->refCount == 1 );   // Undo.cpp:723
                }
            }
            pEInst = pNextEInst;
        }
    }

    // ── Phase 3: Restore entity defs from the undo record back into the live list.
    for ( entity_s *j = undo->entitylist.next;
          j && j != &undo->entitylist;
          j = undo->entitylist.next )
    {
        {
            entity_s *entity = j;
            iassert( entity->refCount == 1 );   // Undo.cpp:728
        }

        g_undoMemorySize -= (int)Entity_MemorySize( j );

        entity_s_def *worldDef = (entity_s_def *)world_entity->def;
        int jEntityId = j->numberId;

        if ( jEntityId == worldDef->numberId )
        {
            // World entity: restore the saved worldspawn epairs onto the live world.
            // Same swap idiom as Undo_Redo (see the note there): j takes the OLD world
            // epairs so Entity_Free_R frees those, world keeps the restored chain.
            epair_t *tmpEpairs       = worldDef->epairs;
            worldDef->epairs         = j->epairs;
            j->epairs                = tmpEpairs;
            Entity_Free_R( j );
        }
        else
        {
            // Non-world entity: unlink from undo list, insert into entities.
            DecRef( j );
            IncRef( j, &entities );

            j->redoId = redo->id;

            {
                entity_s *entity = j;
                iassert( entity->refCount == 1 );   // Undo.cpp:746
                Prefab_Init( (prefab_s *)&entityInsts, (entity_s_def *)j, &active_brushes );
                --entity->refCount;
                iassert( entity->refCount == 1 );   // Undo.cpp:749
            }

            // Verify the def's brush-def-list sentinel is empty: the sentinel's onext side
            // is j->brushes.prev (NOT brushes.ownerPrev), and the list head is &j->def.
            iassert( (void *)j->brushes.prev == (void *)&j->def );   // Undo.cpp:750 ([j+0x0C])
            iassert( (void **)j->def == (void **)&j->def );           // Undo.cpp:751
        }
    }

    // ── Phase 4: Restore brush defs from the undo record back into the live list.
    // KIWI: in the order of the positions they had, each linked back at its own (the
    // binary walked the record head-first and appended every one).
    undo_s *v35 = undo;
    {
        const std::vector<brush_t *> defs = Undo_DefsInOrder( &undo->brushlist );
        for ( brush_t *bDef : defs )
        {
            const int ordinal = Undo_TakeOrdinal( bDef );
            g_undoMemorySize -= (int)Brush_MemorySize( bDef );
            Undo_Refcount( bDef );

            // Find the entity instance whose DEF carries the saved owner numberId.
            entity_s *ownerInst = nullptr;
            for ( entity_s *e = entityInsts.next; e != &entityInsts; e = e->next )
            {
                entity_s_def *eDef = (entity_s_def *)e->def;
                if ( eDef->numberId == bDef->unk1 )
                {
                    ownerInst = e;
                    break;
                }
            }

            // Link brush def into entity
            if ( ownerInst && ownerInst != &entityInsts )
            {
                entity_s_def *ownerDef = (entity_s_def *)ownerInst->def;
                Entity_LinkBrushAt( bDef, (entity_s *)ownerDef, ordinal );   // KIWI: was appended
            }
            else
            {
                // Fall back to world entity
                entity_s_def *worldDef = (entity_s_def *)world_entity->def;
                Entity_LinkBrush( bDef, (entity_s *)worldDef );
                ownerInst = world_entity;
            }

            // Add instance to active list
            selbrush_t *newInst = Brush_AddToList( bDef, ownerInst );
            if ( newInst->next || newInst->prev )
                Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
            Brush_AddToList2( newInst );

            // Stamp with redo ID
            bDef->def = (brush_t_def *)(intptr_t)redo->id;
        }
    }

    Sys_Printf( "%s undone.\n", v35->operation );

    g_undoMemorySize -= sizeof(undo_s);
    ::operator delete( v35 );
    --g_undoSize;

    if ( --g_undoId <= 0 )
        g_undoId = 2 * g_undoMaxSize;

    // Rebuild brush visuals for all selected + active brushes
    g_bScreenUpdates = 1;
    for ( selbrush_t *v44 = selected_brushes.next; v44 != &selected_brushes; v44 = v44->next )
        Brush_UpdateSpecialMaterialFlag( v44->def );
    for ( selbrush_t *m = active_brushes.next; m != &active_brushes; m = m->next )
        Brush_UpdateSpecialMaterialFlag( m->def );

    char *v46 = (char *)va( "%s - active_brushes", "after undo" );
    MainFrm_BrushList( v46, &active_brushes );
    char *v47 = (char *)va( "%s - active_brushes", "after undo" );
    MainFrm_BrushList( v47, &selected_brushes );

    // KIWI: undo after saving changes the document and must restore the save prompt.
    MarkMapModified();

    Sys_Printf( "Updating layers...\n" );
    Layers_SetMapLayers();
    Layers_RefreshDialog();
    g_nUpdateBits = -1;

    Sel_InvalidateFromLegacy();   // KIWI-UX (wholesale brush-list rebuild)
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45f2e0  Undo_Redo
// Redoes the last undone operation.
// ─────────────────────────────────────────────────────────────────────────────
void Undo_Redo()
{
    if ( !g_lastredo )
    {
        Sys_Printf( "Nothing left to redo.\n" );
        return;
    }

    if ( g_lastundo && !g_lastundo->done )
        Sys_Printf( "WARNING: last undo not finished.\n" );

    // Refresh the brush/entity list diagnostics.
    {
        char *s1 = (char *)va( "%s - active_brushes", g_lastredo->operation );
        MainFrm_BrushList( s1, &active_brushes );
        char *s2 = (char *)va( "%s - active_brushes", g_lastredo->operation );
        MainFrm_BrushList( s2, &selected_brushes );
    }

    undo_s *redo = g_lastredo;

    // Detach redo from the list tail
    if ( redo->prev )
        redo->prev->next = nullptr;
    else
        g_redolist = nullptr;
    const char *operation = redo->operation;
    g_lastredo = redo->prev;

    // Start a new undo for this redo operation
    Undo_GeneralStart( operation );
    Select_Deselect( 1 );

    // KIWI: where the defs going to the new undo record sit, before any unlink.
    {
        std::unordered_map<const brush_t *, int> ordinals;
        Undo_OrdinalsOf( &active_brushes, ordinals );
        for ( selbrush_t *inst = active_brushes.next; inst != &active_brushes; inst = inst->next )
            if ( inst->def->def == (brush_t_def *)(intptr_t)redo->id )
            {
                auto it = ordinals.find( inst->def );
                s_undoOrdinal[inst->def] = it != ordinals.end() ? it->second : -1;
            }
    }

    // ── Phase 1: Move active brush instances with redoId == redo->id
    //            into the new undo's brushlist.
    {
        selbrush_t *v4 = active_brushes.next;
        while ( v4 != &active_brushes )
        {
            selbrush_t **pPrev = &v4->next->prev;
            brush_t *def = v4->def;

            if ( def->def == (brush_t_def *)(intptr_t)redo->id )
            {
                {
                    brush_t *brushDef = def;
                    vassert( (brushDef->refCount >= 2), "(brushDef->refCount) = %i", brushDef->refCount );   // Undo.cpp:849
                }
                def->unk1 = def->owner->numberId;
                Entity_UnlinkBrush( def );
                Undo_01_NoMoreUndo( &g_lastundo->brushlist, def );
                g_undoMemorySize += (int)Brush_MemorySize( def );
                Brush_Free( v4 );
            }
            v4 = (selbrush_t *)pPrev;
        }
    }

    // ── Phase 2: Move entity instances with redoId == redo->id
    //            into the new undo's entitylist.
    {
        entity_s *v7 = entityInsts.next;
        while ( v7 != &entityInsts )
        {
            entity_s **v9 = &v7->next->prev;
            entity_s_def *def = (entity_s_def *)v7->def;

            if ( def->redoId == redo->id )
            {
                // Unlink from redo list, link into the new undo's entitylist.
                DecRef( (entity_s *)def );
                IncRef( (entity_s *)def, &g_lastundo->entitylist );

                {
                    entity_s_def *entity = def;
                    iassert( entity->refCount == 1 );   // Undo.cpp:867
                    ++entity->refCount;
                    Entity_Free( v7 );
                    iassert( entity->refCount == 1 );   // Undo.cpp:870
                }
                g_undoMemorySize += (int)Entity_MemorySize( def );
            }
            v7 = (entity_s *)v9;
        }
    }

    // ── Phase 3: Restore entity defs from redo's entitylist.
    undo_s *v31 = redo;
    entity_s *entNode = redo->entitylist.next;
    while ( entNode )
    {
        if ( entNode == &v31->entitylist )
            break;

        entity_s_def *worldDef = (entity_s_def *)world_entity->def;
        int entEntityId = entNode->numberId;

        if ( entEntityId == worldDef->numberId )
        {
            // World entity: restore the saved worldspawn epairs onto the live world.
            // DELIBERATE DIVERGENCE from the binary: it frees world's current epairs
            // inline, sets world->epairs = entNode->epairs, then Entity_Free_R(entNode)
            // WITHOUT first zeroing entNode->epairs — but Entity_Free_R (0x483C80) calls
            // Entity_FreeEpairs, so it frees entNode->epairs, which now ALIASES
            // world->epairs and use-after-frees the freshly-restored chain (a latent
            // binary UAF on redo of a worldspawn epair edit; Undo_Undo's path zeroes
            // first and is fine).  We swap instead: entNode takes the OLD world epairs
            // so Entity_Free_R frees THOSE, and the restored chain survives.
            epair_t *tmpEpairs  = worldDef->epairs;
            worldDef->epairs    = entNode->epairs;
            entNode->epairs     = tmpEpairs;
            Entity_Free_R( entNode );
        }
        else
        {
            // Unlink from redo list, insert into entities.
            DecRef( entNode );
            IncRef( entNode, &entities );

            {
                entity_s *entity = entNode;
                iassert( entity->refCount == 1 );   // Undo.cpp:891
                Prefab_Init( (prefab_s *)&entityInsts, (entity_s_def *)entNode, &active_brushes );
                --entity->refCount;
                iassert( entity->refCount == 1 );   // Undo.cpp:894
            }

            // Same empty-sentinel check as Undo_Undo (Undo.cpp:750): brushes.prev, NOT
            // brushes.ownerPrev.
            iassert( (void *)entNode->brushes.prev == (void *)&entNode->def );   // Undo.cpp:895
            iassert( (void **)entNode->def == (void **)&entNode->def );           // Undo.cpp:896
        }
        entNode = v31->entitylist.next;
    }

    // ── Phase 4: Restore brush defs from redo's brushlist.
    // KIWI: in position order, each at its own (see Undo_Undo's phase 4).
    {
        const std::vector<brush_t *> defs = Undo_DefsInOrder( &redo->brushlist );
        for ( brush_t *onext : defs )
        {
            const int ordinal = Undo_TakeOrdinal( onext );
            Undo_Refcount( onext );

            // Find owner entity instance by saved owner numberId.
            entity_s *v17 = nullptr;
            for ( entity_s *e = entityInsts.next; e != &entityInsts; e = e->next )
            {
                entity_s_def *eDef = (entity_s_def *)e->def;
                if ( eDef->numberId == onext->unk1 )
                {
                    v17 = e;
                    break;
                }
            }

            if ( v17 && v17 != &entityInsts )
            {
                entity_s_def *v19 = (entity_s_def *)v17->def;
                Entity_LinkBrushAt( onext, (entity_s *)v19, ordinal );   // KIWI: was appended
            }
            else
            {
                // KIWI: the restored definition and instance must share an owner.
                // Match Undo_Undo's world fallback instead of attaching to the list sentinel.
                entity_s_def *worldDef = (entity_s_def *)world_entity->def;
                Entity_LinkBrush( onext, (entity_s *)worldDef );
                v17 = world_entity;
            }

            selbrush_t *newInst = Brush_AddToList( onext, v17 );
            if ( newInst->next || newInst->prev )
                Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
            Brush_AddToList2( newInst );
        }
    }

    Undo_End();

    Sys_Printf( "%s redone.\n", redo->operation );
    --g_redoId;
    ::operator delete( redo );

    g_bScreenUpdates = 1;
    char *v24 = (char *)va( "%s - active_brushes", "after redo" );
    MainFrm_BrushList( v24, &active_brushes );
    char *v25 = (char *)va( "%s - active_brushes", "after redo" );
    MainFrm_BrushList( v25, &selected_brushes );
    g_nUpdateBits = -1;

    Sel_InvalidateFromLegacy();   // KIWI-UX (wholesale brush-list rebuild)
}

// KIWI (TERRAIN): a brush CREATED inside the open record is stamped with the record's
// id - what Undo_EndBrushList does per def - WITHOUT saving a copy of it.  Undo_Undo's
// phase 1 then frees it and phase 4 has nothing to put back.  Registering it through
// Undo_AddBrush instead would restore that copy after the undo and leave the new
// chunk behind (kiwi_terrain.cpp ExpandUnderBrush).
void Undo_KiwiMarkCreated( brush_t *def )
{
    if ( !g_lastundo || g_lastundo->done || !def )
        return;
    def->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;
    entity_s *owner = (entity_s *)def->owner;
    if ( owner && *(int *)&owner->eclass->fixedsize )
        owner->epairEdits = g_lastundo->id;
}
