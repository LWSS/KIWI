#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\entity.cpp — entity DEFs and INSTANCES: epairs, the def/inst brush
// lists, prefab instancing, and the .map entity parse/write halves.

#include "stdafx.h"
#include "qe3.h"
#include <universal/q_parse.h>   // Com_ParseExt, parseInfo_t, Com_GetParseThreadInfo
#include <universal/assertive.h> // iassert (USE_ASSERTS always on; same handler as Assert)
#include "kiwi_ported.h"         // KIWI: shared engine/editor externs
extern void KiwiPrefabPrefix_Invalidate();   // brush.cpp:7110
extern void KiwiShadowCache_Invalidate();    // kiwi_shadowcache.cpp:46
extern void KiwiLightCache_EntityKeyChanged( entity_s_def *lightDef, const char *key );
extern void KiwiWalkCache_MarkStructural();  // kiwi_walkcache.h

// ─── helpers declared by other Radiant files ─────────────────────────────────
extern char *AllocMaterialString( const char *src );
extern void  Epair_SetRaw( epair_t **head, const char *key, const char *value );   // eclass.cpp: raw epair set
extern void  Error( const char *fmt, ... );    // wraps Com_Error (engine_stubs.cpp)
namespace SurfaceInspector { void UpdateSurfaceDialog(); }

// ─── globals from engine_stubs.cpp / other radiant files ─────────────────────
extern char          g_activeLayer_string[];    // engine_stubs.cpp
#include "mainfrm.h"
#include "xywnd.h"                             // xywndState_t / Ed_ActiveXY (U-GLOBALS)
#include "kiwi_fmt.h"                          // KIWI: KiwiFmt_Exact (entity origins)
extern void XYWnd_PasteClip();                 // mainfrm.cpp (U-CMD-1 free fn)

// ─── forward declarations for functions defined later in this file ────────────
struct prefab_s;  // defined in qe3.h; forward-declare for use in Prefab_Init decl
void            Entity_Free( entity_s *e );
void            Entity_Free_R( entity_s *e );
entity_s       *Prefab_Init( prefab_s *instList, entity_s_def *entDef, selbrush_t *activeList );
void            EntityAssignModel( entity_s_def *e );
void            Entity_LinkBrush( brush_t *b, entity_s *world_ent );
void            Entity_UnlinkBrush( brush_t *b );
unsigned int    Brush_ApplyEntityColor( brush_t *b_in );
void            Entity_RebuildBounds( entity_s *e );

// ─── declarations for ported-elsewhere functions used here ───────────────────
// brush.cpp
extern void          Brush_SetLayerString( brush_t *b, const char *str );  // 0x4758A0
extern void          Brush_Build( brush_t *b, float *mins = nullptr, float *maxs = nullptr );
// Brush_Write is defined in brush.cpp as int Brush_Write(WriteWriter_t, brush_t*).
// WriteWriter_t = WriteFunc_t** — see brush.cpp.  Declared here with compatible type.
typedef int (*WriteFunc_entity_t)( void *ctx, const char *fmt, ... );
extern int           Brush_Write( WriteFunc_entity_t *writer, brush_t *b );
extern selbrush_t   *Entity_LinkBrush_0_extern( entity_s *e, entity_brush_s *b );
extern void          Brush_Deselect_Helper( selbrush_t *b );   // brush.cpp 0x476330
extern void          Brush_Select_Helper( selbrush_t *b );     // brush.cpp 0x476470

// eclass.cpp
// Eclass_ForModel (0x482300): the per-map model class for a model name; reads the
// entity's eclass and writes its modelClass.
extern models_t     *Eclass_ForModel( const char *modelName, entity_s *ent );

// ─── epair access ────────────────────────────────────────────────────────────
//     The engine `zero` empty-string is stubbed nullptr, so we return a real ""
//     for missing keys (avoids strcmp/sscanf NULL deref).

static char s_emptyKeyValue[] = "";

// ValueForKey2 (0x4825C0) — case-insensitive (_stricmp) key walk; a missing key returns ""
// (the engine's `zero` global; s_emptyKeyValue is safer than the stubbed null).
char *ValueForKey2( const entity_s *e, const char *key )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, key ) )
        {
            return ep->value ? ep->value : s_emptyKeyValue;   // KIWI: "" for a NULL value too
        }
    }
    return s_emptyKeyValue;
}

// Entity_GetVec3ForKey (0x483860) — case-insensitive key; sscanf "%f %f %f" of the value
// ("" when missing); returns 1 iff all 3 parsed.
int Entity_GetVec3ForKey( entity_s_def *e, float *out, const char *key )
{
    const char *value = s_emptyKeyValue;
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !_stricmp( ep->key, key ) ) { value = ep->value; break; }
    return sscanf( value, "%f %f %f", out, out + 1, out + 2 ) == 3;
}

// Entity_GetFloatValueForKey (0x4837C0) — case-insensitive key, (float)atof; missing = 0.0.
float Entity_GetFloatValueForKey( const entity_s *e, const char *key )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, key ) )
        {
            return (float)atof( ep->value );
        }
    }
    return (float)atof( s_emptyKeyValue );
}

// Entity_GetIntValueForKey (0x483820) — the same case-insensitive walk as ValueForKey2 with
// an atol on the value; missing = atol("") = 0.  camwnd.cpp, vehiclepath.cpp, shadowvolume.cpp
// and kiwi_light.cpp declare it extern.
int Entity_GetIntValueForKey( const entity_s *e, const char *key )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, key ) )
        {
            return atol( ep->value );
        }
    }
    return atol( s_emptyKeyValue );
}

// HasKeyValuePair (0x4838B0) — case-sensitive (strcmp) key walk; returns bool.
bool HasKeyValuePair( entity_s_def *e, const char *key )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !strcmp( ep->key, key ) )
            return true;
    return false;
}

// Entity_FreeEpairs (0x483BB0) — free the whole epair list (key, value, node) and null the head.
void Entity_FreeEpairs( entity_s_def *e )
{
    epair_t **head = &e->epairs;
    for ( epair_t *ep = *head; ep; )
    {
        epair_t *next = ep->next;
        free( ep->key );
        free( ep->value );
        free( ep );
        ep = next;
    }
    *head = nullptr;
}

// DeleteKey (0x483720) — remove the FIRST epair whose key matches (case-sensitive),
// freeing the node + its key/value strings.  Only the first match is removed.
void DeleteKey( epair_t **head, const char *key )
{
    for ( epair_t **pp = head; *pp; pp = &( *pp )->next )
    {
        epair_t *cur = *pp;
        if ( !strcmp( cur->key, key ) )
        {
            *pp = cur->next;
            free( cur->key );
            free( cur->value );
            free( cur );
            return;
        }
    }
}

// Forward decls for SetKeyValue's side-effect chain (defined later in this file).
void  SetKeyValue( entity_s_def *e, const char *key, const char *value );  // 0x483690 (defined below)
void  Checkkey_Model( entity_s_def *e, const char *key );        // 0x482f70 (model rebuild on key change)
void  MapLoad_ParsePrefab( const char *classname, entity_s_def *e ); // 0x483010
void  Checkkey_Color( entity_s_def *e, const char *key );        // 0x483210
void  Entity_UpdateCylinder( const char *key, entity_s_def *e );  // 0x483320

// Radius/height clamps, part of SetKeyValue's side-effect chain.  Only act on the cylinder
// classes (classtype & 0xC0 = trigger_radius / trigger_disk; height: & 0x40, trigger_radius);
// for plain fixed-size and brush entities these are no-ops.
static void Entity_ClampRadius( const char *key, entity_s_def *e )   // 0x483440
{
    if ( !_stricmp( key, "radius" ) && ( e->eclass->classtype & 0xC0 ) != 0 )
    {
        if ( Entity_GetFloatValueForKey( e, "radius" ) > 0.0f )
        {
            Entity_UpdateCylinder( key, e );
        }
        else
        {
            SetKeyValue( e, "radius", "256" );
        }
    }
}
static void Entity_ClampHeight( const char *key, entity_s_def *e )   // 0x4834A0
{
    if ( !_stricmp( key, "height" ) && ( e->eclass->classtype & 0x40 ) != 0 )
    {
        if ( Entity_GetFloatValueForKey( e, "height" ) > 0.0f )
        {
            Entity_UpdateCylinder( key, e );
        }
        else
        {
            SetKeyValue( e, "height", "128" );
        }
    }
}

// SetKeyValue (0x483690) — set key=value in entity e's epair list AND fire the side-effect
// chain: raw set (Epair_SetRaw), Checkkey_Model (model rebuild on angles/modelscale/model),
// MapLoad_ParsePrefab on "classname", EntityAssignModel on "model", Checkkey_Color, then the
// radius/height clamps.
void SetKeyValue( entity_s_def *e, const char *key, const char *value )
{
    if ( !e || !key || !*key )
        return;

    // The prefab/walk caches derive from this epair list (prefab layer prefix from "model",
    // the walk recording from origin/angles/modelscale/spawnflags): any key set drops the
    // walk epoch.  KIWI (2026-09-09): the prefab layer prefix reads ONLY "model" (and the
    // class, via "classname"), so a drag that rewrites "origin"/"angles" every frame no
    // longer rebuilds every prefab's prefix string per frame.
    if ( !_stricmp( key, "model" ) || !_stricmp( key, "classname" ) )
        KiwiPrefabPrefix_Invalidate();
    KiwiShadowCache_Invalidate();
    // KIWI-UX: A light epair edit evicts that light while retaining unrelated light previews.
    KiwiLightCache_EntityKeyChanged( e, key );
    // "classname" re-runs the eclass lookup, which is what decides whether a brush counts
    // as a fixed-size entity — the one key that changes the SET of objects rather than one
    // object's state (kiwi_walkcache.h KiwiWalkCache_MarkStructural).
    if ( !_stricmp( key, "classname" ) )
        KiwiWalkCache_MarkStructural();

    Epair_SetRaw( &e->epairs, key, value );   // raw set (primary effect)

    Checkkey_Model( e, key );                            // model rebuild
    if ( !_stricmp( key, "classname" ) )
    {
        MapLoad_ParsePrefab( value, e );
    }
    if ( !_stricmp( key, "model" ) )
        EntityAssignModel( e );
    Checkkey_Color( e, key );
    Entity_ClampRadius( key, e );
    Entity_ClampHeight( key, e );
}

// math (engine or engine_stubs)
extern void          OrientationConcatenate( const orientation_t *a, const orientation_t *b, orientation_t *out );
extern void          Vec3RotateTranspose( vec3_t in, float (*axis)[3], vec3_t out );
extern void          AxisToAngles( float *angles, float (*axis)[3] );    // 0x4A8A00 (engine_stubs)
// VectorMaxValues (0x4a8240) lives in brush.cpp's file-local `namespace edwind` — NOT a
// global symbol — so it can't be reached via a global extern from here. entity.cpp callers
// (Entity_UpdateModelInst, Entity_UpdateCylinder) inline the expand-AABB loop instead.

// map load helpers (stubs / unported)
extern void          Map_Free();
extern void          Map_NewMap();
extern void          Map_InitlLayers();
extern void          Pointfile_Clear();
extern void          Layers_SetMapLayers();
extern void          Layers_RefreshDialog();
extern void          Com_BeginParseSession( const char *name );
// Map header parsers (layers.cpp).
extern int           Map_ReadVersion( const void **text );                 // 0x4861F0
extern void          Map_ParseLayers( const void **text, int skipFlags );  // 0x486280
extern void          Map_ParseEntityLayerKey( const char **text, const char *def, char *out, const char *key ); // 0x42F8A0
extern brush_t      *Brush_Parse( const char **text, int version );

// model-instance helpers (r_ed_scene.cpp — the editor model-inst buffer / model-render epic).
extern int           AddModelToModelInstBuff( XModel *model, float *axis, float scale );
extern void          ModelInstUpdate( int inst, float (*axis)[3], float scale );
extern void          RemoveModelInstFromBuf( int inst );
extern void          Entity_GetModelInstBounds( int inst, float *out_mins, float *out_maxs ); // 0x4FDE10
extern void          Brush_AccumulateWorldBounds( const orientation_t *orient, const brush_t *b, float *mins, float *maxs ); // 0x47ABE0

// brushFlags / classtype (local enum).  Bit values come from Eclass_InitFromText
// (eclass.cpp):
//   light 0x1, not-worldspawn 0x2, path 0x4, model-class 0x8 (misc_model/script_*/
//   dyn_model), misc_prefab 0x10, node_ 0x20, trigger_radius 0x40, trigger_disk 0x80,
//   reflection_probe 0x100.
// SetupModelInst gates Entity_InitPrefabInst on `classtype & CLASS_PREFAB`; SetupModelInst /
// Entity_InitPrefabInst / Eclass_ForModel all test `& 0x10`.
enum CLASS_FLAGS
{
    CLASS_LIGHT       = 0x01,
    CLASS_MODELCLASS  = 0x08,
    CLASS_PREFAB      = 0x10,
    CLASS_SCRIPTNODE  = 0x20,
};

// ─── globals defined in this translation unit ────────────────────────────────
// Entity DEFINITION circular doubly-linked list head (sentinel).
entity_s    entities{};          // 0x23F17A0

// Entity INSTANCE circular list (prefab instance list, headed by entityInsts).
entity_s    entityInsts{};       // 0x23F1748

// Running entity numberId counter (0x739DC4).
int         dword_739DC4 = 0;

// Screen-update guard (0x739B0F).
bool        g_bScreenUpdates = true;

// Between-map clipboard flag (0x25D5B07).
bool        g_bRestoreBetween = false;

// World-space identity orientation matrix (the binary's read-only .data at 0x6DE290).
float       world_orient_matrix[4][3] = {
    { 0.0f, 0.0f, 0.0f },   // origin
    { 1.0f, 0.0f, 0.0f },   // axis[0]
    { 0.0f, 1.0f, 0.0f },   // axis[1]
    { 0.0f, 0.0f, 1.0f },   // axis[2]
};

// ─────────────────────────────────────────────────────────────────────────────
// 0x483bf0  IncRef  (entity.cpp:690)
// Links an entity into a list (circular, TAIL insertion).  In the binary every use
// is inlined (Entity_Create / Map_New / Map_LoadEntities / undo).
// ─────────────────────────────────────────────────────────────────────────────
void IncRef( entity_s *e, entity_s *list )
{
    iassert( !e->next && !e->prev );
    e->next          = list;
    e->prev          = list->prev;
    list->prev->next = e;
    list->prev       = e;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x483c30  DecRef  (entity.cpp:701)
// Unlinks an entity from the global entities linked list.
// ─────────────────────────────────────────────────────────────────────────────
void DecRef( entity_s *e )
{
    iassert( e->next && e->prev );
    e->next->prev = e->prev;
    e->prev->next = e->next;
    e->prev = nullptr;
    e->next = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x4825F0  Entity_FreePrefab — frees the child entity instances of a prefab (its
// circular instance list, down to the self-sentinel), then the prefab_s; e->prefab is
// cleared.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_FreePrefab( entity_s *e )
{
    prefab_s *pf = (prefab_s *)e->prefab;
    if ( !pf )
        return;
    // Free all entity instances in the prefab's own list until only the sentinel remains.
    while ( pf->next_entity != (entity_s *)pf )
        Entity_Free( pf->next_entity );
    free( pf );
    e->prefab = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x483c80  Entity_Free_R  (entity.cpp:712)
// Frees an entity DEFINITION and all its brush defs (unlink, --refCount, zero the links,
// Brush_Free_R), then its epairs.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_Free_R( entity_s *e )
{
    iassert( e );
    vassert( (e->refCount == 0), "(e->refCount) = %i", e->refCount );
    // KIWI (2026-09-09): the prefab-prefix memo is keyed by DEF POINTER; a def freed here
    // can be re-allocated for a different prefab, so drop the memo now that SetKeyValue
    // no longer bumps it on every key (only "model"/"classname").
    KiwiPrefabPrefix_Invalidate();
    if ( e->next )
        DecRef( e );

    // Walk the brush-def list (circular via onext/oprev; &e->def is the sentinel).  Every
    // iteration re-reads e->brushes.prev (the def-list FIRST element / oldest-inserted brush),
    // freeing the list from that end; after each unlink the sentinel's onext slot (=
    // e->brushes.prev) advances to the next first element.  The b->oprev == sentinel and
    // post-decrement refCount == 0 asserts only make sense from this end.
    for ( brush_t *b = (brush_t *)e->brushes.prev;
          b != (brush_t *)&e->def;
          b = (brush_t *)e->brushes.prev )
    {
        iassert( b->owner == e );                          // entity.cpp:721
        iassert( (void *)b->oprev == (void *)&e->def );    // :722 - the binary's &e->brushes is `def` here
        if ( !b->onext || !b->oprev )
            Com_Error( ERR_FATAL, "Entity_UnlinkBrush: Not currently linked" );
        vassert( (b->refCount > 0), "(b->refCount) = %i", b->refCount );

        // Unlink b from the def-list:
        b->onext->oprev = b->oprev;
        b->oprev->onext = b->onext;
        int newRef = --b->refCount;
        b->oprev  = nullptr;
        b->onext  = nullptr;
        b->owner  = nullptr;
        // Post-decrement invariant: b->refCount == newRef here (post-dec, pre-free); the
        // message prints the post-dec value.
        vassert( (b->refCount == 0), "(b->refCount) = %i", newRef );   // entity.cpp:724
        Brush_Free_R( b );
    }

    Entity_FreeEpairs( e );
    free( e );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x484fc0  Entity_LinkBrush  (entity.cpp:1190)
// Links a brush_t DEFINITION into an entity's active brush-def list (the binary also
// inlines it at brush.cpp's Split_LinkCloneToEntity).
// ─────────────────────────────────────────────────────────────────────────────
void Entity_LinkBrush( brush_t *b, entity_s *world_ent )
{
    KiwiWalkCache_MarkStructural();   // ownership move: the tree's shape changed
    if ( b->oprev || b->onext )
        Com_Error( ERR_FATAL, "Entity_LinkBrush: Allready linked" );
    vassert( (b->refCount >= 0), "(b->refCount) = %i", b->refCount );
    b->owner = world_ent;
    ++b->refCount;
    // Insert into the def-list.  The list HEAD is &world_ent->def; the per-entity "first
    // element" pointer is the overlapping brushes.prev and is updated ONLY on the first
    // insert.  This writes old_head->NEXT, NOT ->prev: on the empty list old_head is the
    // sentinel, whose ->next is brushes.prev, the "first" pointer MapFile_WriteEntity reads
    // (writing ->prev left the def-list looking empty to the writer: zero brushes saved).
    b->onext                          = (brush_t *)&world_ent->def;
    b->oprev                          = (brush_t *)world_ent->def;
    ( (entity_s *)world_ent->def )->next = (entity_s *)b;
    world_ent->def                        = (entity_s *)b;
    Brush_ApplyEntityColor( b );
}

// KIWI: Entity_LinkBrush at a position - before the brush now `ordinal`-th in the entity's
// list (appended when the list is shorter).  Undo / redo put a brush back where it was:
// appending moved every undone brush to the end of the saved map.  The sentinel is
// &ent->def as a brush_t: its oprev is the last brush, its onext ent->brushes.prev (the first).
void Entity_LinkBrushAt( brush_t *b, entity_s *ent, int ordinal )
{
    brush_t *const sentinel = (brush_t *)&ent->def;
    brush_t       *at       = (brush_t *)ent->brushes.prev;
    for ( int i = 0; i < ordinal && at != sentinel; ++i )
        at = at->onext;
    if ( ordinal < 0 || at == sentinel )
    {
        Entity_LinkBrush( b, ent );
        return;
    }
    KiwiWalkCache_MarkStructural();
    if ( b->oprev || b->onext )
        Com_Error( ERR_FATAL, "Entity_LinkBrush: Allready linked" );
    vassert( (b->refCount >= 0), "(b->refCount) = %i", b->refCount );
    b->owner = ent;
    ++b->refCount;
    b->onext         = at;
    b->oprev         = at->oprev;
    at->oprev->onext = b;       // the sentinel's onext is ent->brushes.prev
    at->oprev        = b;
    Brush_ApplyEntityColor( b );
}

// KIWI: `b`'s position in its owner's brush list, or -1.
int Entity_BrushOrdinal( const brush_t *b )
{
    entity_s *ent = b ? b->owner : nullptr;
    if ( !ent )
        return -1;
    brush_t *const sentinel = (brush_t *)&ent->def;
    int i = 0;
    for ( brush_t *d = (brush_t *)ent->brushes.prev; d && d != sentinel; d = d->onext, ++i )
        if ( d == b )
            return i;
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485020  Entity_UnlinkBrush  (entity.cpp:1208)
// Unlinks a brush_t from its entity's def-list, decrements refCount and zeroes the brush's
// oprev/onext/owner (brush.cpp's Entity_UnlinkBrush_def forwards here).
// ─────────────────────────────────────────────────────────────────────────────
void Entity_UnlinkBrush( brush_t *b )
{
    KiwiWalkCache_MarkStructural();   // see Entity_LinkBrush
    if ( !b->onext || !b->oprev )
        Com_Error( ERR_FATAL, "Entity_UnlinkBrush: Not currently linked" );
    vassert( (b->refCount > 0), "(b->refCount) = %i", b->refCount );
    b->onext->oprev = b->oprev;
    b->oprev->onext = b->onext;
    --b->refCount;
    b->oprev = nullptr;
    b->onext = nullptr;
    b->owner = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485390  Entity_RebuildBounds  (entity.cpp:1329)
// Rebuilds the AABB brush for a fixed-size entity using eclass mins/maxs + origin.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_RebuildBounds( entity_s *e )
{
    iassert( e );
    eclass_t *ec = e->eclass;
    float mins[3], maxs[3];
    mins[0] = ec->mins[0] + e->origin[0];
    mins[1] = ec->mins[1] + e->origin[1];
    mins[2] = ec->mins[2] + e->origin[2];
    maxs[0] = ec->maxs[0] + e->origin[0];
    maxs[1] = ec->maxs[1] + e->origin[1];
    maxs[2] = ec->maxs[2] + e->origin[2];
    // Get the sole brush def for this fixed-size entity from e->brushes.prev (the def-list
    // first element), as the binary does; with a single bbox brush it is also `def`.
    brush_t *b = (brush_t *)e->brushes.prev;
    Brush_Build( b, mins, maxs );
}

// ─────────────────────────────────────────────────────────────────────────────
// Entity_HasEpairMatch (0x483930) — does the entity have epair `key` with value `val`?
// Short-circuits on the FIRST key match (case-sensitive strcmp on key and value).
// ─────────────────────────────────────────────────────────────────────────────
bool Entity_HasEpairMatch( entity_s *e, const char *key, const char *val )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
    {
        if ( strcmp( ep->key, key ) == 0 )
            return strcmp( ep->value, val ) == 0;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Substring-aware epair matchers used by Select_ByKeyValue (select.cpp 0x490C00).
// All eight matchers walk e->epairs; the six below are the substring variants of
// HasKeyValuePair / Entity_HasEpairMatch (which are the exact-match pair).  NOTE the
// substring matchers keep walking past a key match whose value mismatches, unlike
// Entity_HasEpairMatch which short-circuits on the first key hit.
// ─────────────────────────────────────────────────────────────────────────────

// 0x483900 — key is a substring of an epair key (no value test).
bool Entity_HasKeySubstr( entity_s_def *e, const char *key )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( strstr( ep->key, key ) )
            return true;
    return false;
}

// 0x483AC0 — value exact (no key test).
bool Entity_HasValue( entity_s_def *e, const char *val )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !strcmp( ep->value, val ) )
            return true;
    return false;
}

// 0x483B10 — value is a substring of an epair value (no key test).
bool Entity_HasValueSubstr( entity_s_def *e, const char *val )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( strstr( ep->value, val ) )
            return true;
    return false;
}

// 0x4839C0 — key is a substring AND value matches exactly.
bool Entity_HasKeySubstrValue( entity_s_def *e, const char *key, const char *val )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( strstr( ep->key, key ) && !strcmp( ep->value, val ) )
            return true;
    return false;
}

// 0x483A20 — key matches exactly AND value is a substring.
bool Entity_HasKeyValueSubstr( entity_s_def *e, const char *key, const char *val )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !strcmp( ep->key, key ) && strstr( ep->value, val ) )
            return true;
    return false;
}

// 0x483A80 — key is a substring AND value is a substring.
bool Entity_HasKeySubstrValueSubstr( entity_s_def *e, const char *key, const char *val )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( strstr( ep->key, key ) && strstr( ep->value, val ) )
            return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x482940  Entity_GetOrientationMatrix  (entity.cpp:123)
// Builds a 4x3 rotation matrix for a fixed-size entity from its "angles" and origin.
// ─────────────────────────────────────────────────────────────────────────────
// non-static: also called by the prefab ray trace (Brush_TraceTest) in select.cpp.
void Entity_GetOrientationMatrix( entity_s *ent, float (*axis)[3] )
{
    iassert( ent );
    iassert( ent->eclass );
    iassert( ent->eclass->fixedsize );
    iassert( axis );
    float angles[3];
    if ( !Entity_GetVec3ForKey( (entity_s_def *)ent, angles, "angles" ) )
        angles[0] = angles[1] = angles[2] = 0.0f;
    // AnglesToAxis fills axis[1..3] (the rotation part); axis[0] gets origin.
    AnglesToAxis( angles, axis + 1 );
    axis[0][0] = ent->origin[0];
    axis[0][1] = ent->origin[1];
    axis[0][2] = ent->origin[2];
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x482a70  Entity_GetOrientation  (entity.cpp:148)
// The entity's orientation matrix concatenated onto orParent.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_GetOrientation( entity_s_def *ent, orientation_t *orParent, orientation_t *orOut )
{
    iassert( ent );
    iassert( ent->eclass );
    iassert( ent->eclass->fixedsize );
    iassert( orParent );
    iassert( orOut );
    orientation_t local;
    Entity_GetOrientationMatrix( (entity_s *)ent, (float (*)[3])&local );
    OrientationConcatenate( &local, orParent, orOut );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x482b50  Entity_GetOrientationInverse  (entity.cpp:163)
// The orientation matrix with Vec3RotateTranspose( origin ) taken off its origin, then
// concatenated onto orParent.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_GetOrientationInverse( entity_s_def *ent, orientation_t *orParent, orientation_t *orOut )
{
    iassert( ent );
    iassert( ent->eclass );
    iassert( ent->eclass->fixedsize );
    iassert( orParent );
    iassert( orOut );
    orientation_t local;
    Entity_GetOrientationMatrix( (entity_s *)ent, (float (*)[3])&local );
    float out[3];
    Vec3RotateTranspose( ent->origin, local.axis, out );
    local.origin[0] -= out[0];
    local.origin[1] -= out[1];
    local.origin[2] -= out[2];
    OrientationConcatenate( &local, orParent, orOut );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x482650  Entity_UpdateModelInst  (entity.cpp:65)
// Updates (or creates) the model instance for a fixed-size entity, and (with out-params)
// accumulates its world-space AABB over the 8 local-bounds corners.
// ─────────────────────────────────────────────────────────────────────────────
// KIWI: the binary names the bounds out-params the other way round (its "maxs" holds the
// running MIN); here they are named for what they hold.
void Entity_UpdateModelInst( entity_s *ent, float *_org, float *mins, float *maxs )
{
    iassert( ent );
    entity_s_def *def = (entity_s_def *)ent->def;
    iassert( ent->def );
    entitymodel_t *emc = (entitymodel_t *)def->modelClass;
    iassert( ent->def->modelClass );   // entity.cpp:67
    // modelClass (entitymodel_t*) ->model is the models_t* sub-node; its handle holds the
    // XModel* after Model_load → R_RegisterModel.
    modelnode_t *mc = emc->model;
    iassert( ent->def->modelClass->model );   // entity.cpp:68
    XModel *xmodel = (XModel *)mc->handle;
    iassert( ent->def->modelClass->model->handle );   // entity.cpp:69

    // modelscale epair (default 1.0; <= 0 → 1.0).
    const float keyScale = (float)atof( ValueForKey2( def, "modelscale" ) );
    const float scale    = keyScale > 0.0f ? keyScale : 1.0f;

    // World orientation = the entity's own orientation matrix concatenated with the parent
    // orientation passed in _org (a 12-float orientation_t: origin[3] then axis[3][3]).
    orientation_t entOrient, worldOrient;
    Entity_GetOrientationMatrix( (entity_s *)def, (float (*)[3])&entOrient );
    OrientationConcatenate( &entOrient, (const orientation_t *)_org, &worldOrient );

    if ( ent->modelInst )
        ModelInstUpdate( ent->modelInst, (float (*)[3])&worldOrient, scale );
    else
        ent->modelInst = AddModelToModelInstBuff( xmodel, &worldOrient.origin[0], scale );

    // Bounds out-params (both NULL or both non-NULL).
    iassert( (mins == NULL && maxs == NULL) || (mins != NULL && maxs != NULL) );   // entity.cpp:91

    int modelInst = ent->modelInst;
    if ( modelInst && mins && maxs )
    {
        // Local-space model bounds → world-space AABB over the 8 corners.
        float lmin[3], lmax[3];
        Entity_GetModelInstBounds( modelInst, lmin, lmax );
        mins[0] = mins[1] = mins[2] =  131072.0f;
        maxs[0] = maxs[1] = maxs[2] = -131072.0f;
        for ( int c = 0; c < 8; ++c )
        {
            float corner[3] = { (c & 1) ? lmin[0] : lmax[0],
                                (c & 2) ? lmin[1] : lmax[1],
                                (c & 4) ? lmin[2] : lmax[2] };
            // OrientationWorldPosToLocalPos (0x4ba610, q_shared.cpp:1638) inlined -- the INVERSE
            // world->local transform  worldPt[i] = axis_row[i] . (corner - origin) -- NOT the
            // forward OrientationPosToWorldPos (0x4ba430, axis^T*pos + origin).  (map.cpp
            // inlines the same inverse.)
            const orientation_t *o = (const orientation_t *)_org;
            float rel[3] = { corner[0] - o->origin[0],
                             corner[1] - o->origin[1],
                             corner[2] - o->origin[2] };
            float worldPt[3] = {
                o->axis[0][0]*rel[0] + o->axis[0][1]*rel[1] + o->axis[0][2]*rel[2],
                o->axis[1][0]*rel[0] + o->axis[1][1]*rel[1] + o->axis[1][2]*rel[2],
                o->axis[2][0]*rel[0] + o->axis[2][1]*rel[1] + o->axis[2][2]*rel[2] };
            // VectorMaxValues(pt, mins, maxs) inlined.
            for ( int k = 0; k < 3; ++k )
            {
                if ( mins[k] > worldPt[k] ) mins[k] = worldPt[k];
                if ( maxs[k] < worldPt[k] ) maxs[k] = worldPt[k];
            }
        }
        // Pad a thin axis to 8 units; the assert fires only on an inverted box.
        for ( int axis = 0; axis < 3; ++axis )
        {
            iassert( maxs[axis] >= mins[axis] );   // entity.cpp:107
            if ( maxs[axis] - mins[axis] <= 8.0f )
            {
                float mid = ( maxs[axis] + mins[axis] ) * 0.5f;
                maxs[axis] = mid + 4.0f;
                mins[axis] = mid - 4.0f;
            }
        }
    }
}

// Model_SetModel (brush.cpp/0x478780) — assigns/loads the model for a brush's owning
// entity.  Entity_InitPrefabInst calls it ONLY to recurse into a child instance whose
// own eclass is a prefab (nested prefabs); for plain brush prefabs the recursion branch
// is not taken.

extern void __cdecl XModelGetBounds( const XModel *model, float *mins, float *maxs );          // xmodel.cpp

// KIWI: a prefab instance's proxy box from each child's OWN local geometry, through `orient`
// (the prefab's frame in its parent's).  The binary accumulated every child's bbox DEF windings,
// but a model's / nested prefab's bbox def is shared by every instance of the prefab and holds
// whichever instance last set its model up (an AABB of that instance's world AABB, mapped
// back) - so one prefab's instances took each other's boxes.  Brushes: their windings (as
// before); a resolved model: XModel bounds x modelscale through its own orientation; a realized
// nested prefab: its children, recursively; any other fixed-size child (a light, a model not
// resolved yet): its eclass box around its origin - what Entity_RebuildBounds builds.
static void Prefab_AccumulateLocalBounds( prefab_s *pf, const orientation_t *orient,
                                          float *mins, float *maxs, int depth )
{
    for ( selbrush_t *bi = pf->active_brushlist_next;
          bi != (selbrush_t *)&pf->active_brushlist;
          bi = bi->next )
    {
        entity_s     *ce   = bi->owner;
        entity_s_def *cdef = ce ? (entity_s_def *)ce->def : nullptr;
        if ( !cdef || !cdef->eclass || !*(int *)&cdef->eclass->fixedsize )
        {
            Brush_AccumulateWorldBounds( orient, bi->def, mins, maxs );
            continue;
        }

        orientation_t local, composed;
        Entity_GetOrientationMatrix( (entity_s *)cdef, (float (*)[3])&local );
        OrientationConcatenate( &local, orient, &composed );
        if ( ce->prefab && depth < 16 )
        {
            Prefab_AccumulateLocalBounds( (prefab_s *)ce->prefab, &composed, mins, maxs, depth + 1 );
            continue;
        }

        const entitymodel_t *emc = (const entitymodel_t *)cdef->modelClass;
        const XModel *xm = ( emc && emc->model && !( cdef->eclass->classtype & CLASS_PREFAB ) )
                         ? (const XModel *)emc->model->handle : nullptr;
        float lmin[3], lmax[3];
        const orientation_t *frame = &composed;
        if ( xm )
        {
            XModelGetBounds( xm, lmin, lmax );
            const float keyScale = (float)atof( ValueForKey2( cdef, "modelscale" ) );
            const float scale    = keyScale > 0.0f ? keyScale : 1.0f;
            for ( int k = 0; k < 3; ++k )
            {
                lmin[k] *= scale;
                lmax[k] *= scale;
            }
        }
        else
        {
            // the unrotated eclass box around the origin, in the prefab's frame
            for ( int k = 0; k < 3; ++k )
            {
                lmin[k] = cdef->eclass->mins[k] + cdef->origin[k];
                lmax[k] = cdef->eclass->maxs[k] + cdef->origin[k];
            }
            frame = orient;
        }
        for ( int c = 0; c < 8; ++c )
        {
            const float corner[3] = { ( c & 1 ) ? lmax[0] : lmin[0],
                                      ( c & 2 ) ? lmax[1] : lmin[1],
                                      ( c & 4 ) ? lmax[2] : lmin[2] };
            float w[3];
            OrientationPosToWorldPos( w, corner, frame );
            for ( int k = 0; k < 3; ++k )
            {
                if ( w[k] < mins[k] ) mins[k] = w[k];
                if ( w[k] > maxs[k] ) maxs[k] = w[k];
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x482c60  Entity_InitPrefabInst
// Build the prefab_s for a misc_prefab entity and populate it with brush INSTANCES of
// every entity-def in the loaded prefab model, then compute the placed prefab's overall
// bounds (mins/maxs) which SetupModelInst feeds to Brush_Build for the bbox.
//
//   ent       = the misc_prefab entity instance
//   identMtx  = world orientation matrix (forwarded to Model_SetModel for nested prefabs)
//   mins/maxs = OUT: the placed prefab's world-space bounding box
//
// The prefab model was loaded into ent->def->modelClass->model (a models_t sub-node) by
// Model_SetModel→Eclass_ForModel→Eclass_LoadModel→Prefab_Load→Eclass_RealizeModel.  Its loaded
// entity-defs form a circular list: sentinel &model->entities, first model->entities.next,
// each linked via entity_s.next.  Prefab_Init instances one def's brushes into the prefab_s
// active list.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_InitPrefabInst( entity_s *ent, const float *identMtx, float *mins, float *maxs )
{
    iassert( ent );
    entity_s_def *def = (entity_s_def *)ent->def;
    iassert( ent->def );
    models_t *modelClass = (models_t *)def->modelClass;
    iassert( ent->def->modelClass );   // entity.cpp:184
    modelnode_t *model = modelClass->model;
    iassert( ent->def->modelClass->model );   // entity.cpp:185
    iassert( ent->def->modelClass->model->entities.next );   // entity.cpp:186
    iassert( mins );
    iassert( maxs );
    iassert( ent->prefab == NULL );

    // Allocate + init the prefab_s, with both entity-list and brush-list as empty
    // circular sentinels.
    prefab_s *pf = (prefab_s *)operator new( sizeof(prefab_s) );
    ent->prefab = pf;
    memset( pf, 0, sizeof(prefab_s) );
    pf->next_entity           = (entity_s *)pf;
    pf->prev_entity           = (entity_s *)pf;
    pf->active_brushlist_next = (selbrush_t *)&pf->active_brushlist;
    pf->active_brushlist      = (selbrush_t *)&pf->active_brushlist;

    // Walk the loaded prefab's entity-defs and instance each (Prefab_Init links the
    // brush instances into pf->active_brushlist).
    entity_s_def *sentinel = &model->entities;
    for ( entity_s_def *entDef = (entity_s_def *)model->entities.next;
          entDef != sentinel;
          entDef = entDef->next )
    {
        entity_s *inst = Prefab_Init( pf, entDef, (selbrush_t *)&pf->active_brushlist );

        // If the instanced child is itself a prefab (classtype & 0x10), recurse via
        // Model_SetModel on its (single) brush instance (nested prefabs).
        if ( ( ( (entity_s_def *)inst->def )->eclass->classtype & CLASS_PREFAB ) != 0 )
        {
            entity_brush_s *b = (entity_brush_s *)inst->brushes.ownerNext;
            iassert( b );
            if ( b != (entity_brush_s *)&inst->brushes )
                Model_SetModel( b, identMtx );
        }
    }

    // Compute the placed prefab's bounds: orient by THIS entity's matrix, then accumulate
    // every child (the binary: each instanced brush's def windings, sub_47ABE0).
    float orientMtx[4][3];
    Entity_GetOrientationMatrix( (entity_s *)def, orientMtx );

    mins[0] = mins[1] = mins[2] =  131072.0f;
    maxs[0] = maxs[1] = maxs[2] = -131072.0f;
    Prefab_AccumulateLocalBounds( pf, (const orientation_t *)orientMtx, mins, maxs, 0 );
}

// ─────────────────────────────────────────────────────────────────────────────
// KIWI: models resolve on their first draw, AFTER Entity_InitPrefabInst built the instance's
// proxy box from eclass stand-ins - and the version gate never rebuilds it.  Model_SetModel
// bumps g_modelResolveSerial when an entity's model first resolves; the next idle tick
// re-derives every realized instance's box (rebuilt only when it moved).  Deferred while a
// KIWI tool is live or an undo bracket is open: the proxy's planes are a G/R/S baseline then.
// ─────────────────────────────────────────────────────────────────────────────
unsigned g_modelResolveSerial;
class KiwiEditorCommand;
extern KiwiEditorCommand *KiwiCmd_Active();                 // kiwi_command.cpp

static bool Prefab_RefreshInstance( entity_s *inst, int depth )
{
    prefab_s *pf = (prefab_s *)inst->prefab;
    selbrush_t *proxy = inst->brushes.ownerNext;
    if ( !pf || !proxy || proxy == (selbrush_t *)&inst->brushes || !proxy->def )
        return false;

    bool moved = false;
    for ( selbrush_t *bi = pf->active_brushlist_next;          // nested instances first
          bi != (selbrush_t *)&pf->active_brushlist;
          bi = bi->next )
    {
        if ( bi->owner && bi->owner->prefab && depth < 16 )
            moved |= Prefab_RefreshInstance( bi->owner, depth + 1 );
    }

    float orientMtx[4][3];
    Entity_GetOrientationMatrix( (entity_s *)inst->def, orientMtx );
    float mins[3] = {  131072.0f,  131072.0f,  131072.0f };
    float maxs[3] = { -131072.0f, -131072.0f, -131072.0f };
    Prefab_AccumulateLocalBounds( pf, (const orientation_t *)orientMtx, mins, maxs, 0 );
    if ( mins[0] > maxs[0] )
        return moved;                                       // no geometry - leave it
    brush_t *def = proxy->def;
    for ( int k = 0; k < 3; ++k )
    {
        if ( fabsf( def->mins[k] - mins[k] ) > 0.01f || fabsf( def->maxs[k] - maxs[k] ) > 0.01f )
        {
            Brush_Build( def, mins, maxs );
            return true;
        }
    }
    return moved;
}

void Prefab_RefreshProxyBounds()
{
    static unsigned s_done;
    if ( s_done == g_modelResolveSerial )
        return;
    if ( KiwiCmd_Active() || ( g_lastundo && !g_lastundo->done ) )
        return;                                              // retried next tick
    s_done = g_modelResolveSerial;

    bool moved = false;
    for ( selbrush_t *list : { &active_brushes, &selected_brushes, &filtered_brushes } )
        for ( selbrush_t *b = list->next; b && b != list; b = b->next )
            if ( b->owner && b->owner->prefab )
                moved |= Prefab_RefreshInstance( b->owner, 0 );
    if ( moved )
        g_nUpdateBits = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x483010  MapLoad_ParsePrefab  (the binary's "Checkkey_Model"; entity.cpp:285)
// Classname-change handler: re-class a fixed-size entity and (re)build its model +
// bbox brush.  Called by SetKeyValue when "classname" changes.  Checkkey_Model (below) is
// the SIMPLE epair handler.  Inert on Entity_Create/Map_New (eclass still NULL at the
// classname set).
// ─────────────────────────────────────────────────────────────────────────────
void MapLoad_ParsePrefab( const char *classname, entity_s_def *e )
{

    // Bail unless the entity already has a FIXED-SIZE eclass.
    eclass_t *cur = e->eclass;
    if ( !cur )
        return;
    if ( !*(int *)&cur->fixedsize )
        return;

    // Look up the eclass for the NEW classname (no-brushes variant).
    eclass_t *newClass = Eclass_ForName( 0, classname );
    if ( !*(int *)&newClass->fixedsize )                  // only fixed-size classes get a bbox brush
        return;

    // If NEITHER the current eclass NOR the new eclass is "misc_prefab",
    // it's a plain re-class — set the new eclass and let EntityAssignModel rebuild.
    if ( I_stricmp( e->eclass->name, "misc_prefab" ) && I_stricmp( newClass->name, "misc_prefab" ) )
    {
        e->eclass = newClass;
        EntityAssignModel( e );
    }
    else
    {
        // A model/prefab class — resolve the model name epair.
        char *modelName = ValueForKey2( e, "model" );
        iassert( modelName );                            // entity.cpp:292

        // For misc_model / script_model whose model isn't already a .map,
        // synthesise the prefab path "prefabs/misc_models/<name>.map", store it back, and
        // refresh the selection (UpdateSelection(-1, 0)).
        if ( ( !I_stricmp( e->eclass->name, "misc_model" ) || !I_stricmp( e->eclass->name, "script_model" ) )
             && !strstr( modelName, ".map" ) )
        {
            char modelPath[516];
            _snprintf( modelPath, 0x200u, "prefabs/misc_models/%s.map", modelName );
            SetKeyValue( e, "model", modelPath );
            modelName = ValueForKey2( e, "model" );
            UpdateSelection( 0xFFFFFFFF, 0 );
        }

        // Assign the new eclass, clear modelClass + the brush's
        // model-failed flag, then (re)build the model entity class via Eclass_ForModel.
        brush_t *brush = (brush_t *)e->brushes.prev;
        e->eclass      = newClass;
        e->modelClass  = nullptr;
        *(unsigned __int8 *)&brush->unk01 = 0;           // low byte = modelFailed
        Eclass_ForModel( modelName, e );
    }

    // Bump the def version and rebuild the bbox brush around
    // the new eclass mins/maxs offset by the entity origin.
    ++*(unsigned __int16 *)&e->version_prob_wrong;

    float bmins[3];
    float bmaxs[3];
    bmins[0] = e->origin[0] + newClass->mins[0];
    bmins[1] = newClass->mins[1] + e->origin[1];
    bmins[2] = newClass->mins[2] + e->origin[2];
    bmaxs[0] = e->origin[0] + newClass->maxs[0];
    bmaxs[1] = newClass->maxs[1] + e->origin[1];
    bmaxs[2] = newClass->maxs[2] + e->origin[2];

    brush_t *brush = (brush_t *)e->brushes.prev;
    Brush_Create( bmins, bmaxs, brush, newClass );
    Brush_BuildWindings( brush, 1 );                     // bsnap=1

    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();

    MarkMapModified();
    ++*(unsigned __int16 *)&brush->version;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x483210  Checkkey_Color  (entity.cpp:340)
// Called when an entity epair changes; refreshes brush color if "_color" or "targetname"
// (the latter only for script nodes, classtype & 0x20).
// ─────────────────────────────────────────────────────────────────────────────
void Checkkey_Color( entity_s_def *e, const char *key )
{
    if ( _stricmp( key, "_color" ) && _stricmp( key, "targetname" ) )
        return;

    eclass_t *ec = e->eclass;
    if ( !*(int *)&ec->fixedsize )
        return;

    // The sole brush for a fixed-size entity is the first def in the def-list,
    // e->brushes.prev (for a single-brush fixedsize entity it is also `def`).  Three asserts:
    // !b (344), b == sentinel (345), b->onext != sentinel (346); the sentinel is the binary's
    // ent->brushes head, &e->def in this layout.
    brush_t *b = (brush_t *)e->brushes.prev;
    iassert( b );
    iassert( b != (brush_t *)&e->def );
    iassert( !b || (void *)b->onext == (void *)&e->def );

    if ( !_stricmp( key, "_color" ) )
    {
        Brush_ApplyEntityColor( b );
        return;
    }
    if ( ( ec->classtype & CLASS_SCRIPTNODE ) != 0 )
    {
        if ( strncmp( ec->name, "node_scr", 8 ) != 0 )
        {
            char *tn = ValueForKey2( e, "targetname" );
            if ( tn && strncmp( tn, "auto", 4 ) != 0 )
                Brush_ApplyEntityColor( b );
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x483320  Entity_UpdateCylinder  (entity.cpp:374)
// When "radius" or "height" changes on a cylinder entity (classtype & 0xC0 = trigger_radius /
// trigger_disk, gated by Entity_ClampRadius/Height), recomputes the entity brush's bounds so
// the 2D/camera bbox tracks the cylinder cap radius + height. The brush winding is rebuilt
// first (Brush_BuildWindings), then the AABB is EXPANDED to enclose the two cylinder
// corners: XY = origin ± radius, Z = origin.z + eclass->mins[2] .. + height.
// VectorMaxValues is namespace-local to brush.cpp (edwind::), so the expand is INLINED here
// (as in Entity_UpdateModelInst above). VectorMaxValues only ever shrinks mins / grows
// maxs — it expands the post-winding bbox, it does not reset it.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_UpdateCylinder( const char *key, entity_s_def *e )
{
    iassert( !stricmp( key, "radius" ) || !stricmp( key, "height" ) );   // entity.cpp:374

    float radius = Entity_GetFloatValueForKey( e, "radius" );
    if ( radius <= 0.0f )                                    // only act on a real radius
        return;

    // height: classtype & 0x80 (trigger_disk, the FLAT disk) => fixed 32.0; otherwise
    // (trigger_radius, classtype 0x40) read the "height" key.  0x80 is a bit of the LOW
    // byte of classtype, NOT the int sign bit.
    float height;
    if ( ( e->eclass->classtype & 0x80 ) != 0 )              // trigger_disk => fixed height
        height = 32.0f;
    else
    {
        height = Entity_GetFloatValueForKey( e, "height" );
        if ( height <= 0.0f )                                // bail on non-positive height
            return;
    }

    // e->brushes.prev is the entity's first brush DEF, treated as a brush_t* (as
    // Map_ImportBuffer does: `(brush_t *)eDef->brushes.prev`). A trigger_radius/disk owns
    // exactly one brush, so this is the cylinder geometry brush.
    brush_t *brush = (brush_t *)e->brushes.prev;
    Brush_BuildWindings( brush, 1 );                         // rebuild winding + base bounds

    const float baseZ = e->eclass->mins[2] + e->origin[2];
    const float lower[3] = { e->origin[0] - radius, e->origin[1] - radius, baseZ };
    const float upper[3] = { e->origin[0] + radius, e->origin[1] + radius, baseZ + height };

    // VectorMaxValues(corner, &brush->mins, &brush->maxs): expand AABB to enclose corner.
    for ( int i = 0; i < 3; ++i )
    {
        if ( brush->mins[i] > lower[i] ) brush->mins[i] = lower[i];
        if ( brush->maxs[i] < lower[i] ) brush->maxs[i] = lower[i];
    }
    for ( int i = 0; i < 3; ++i )
    {
        if ( brush->mins[i] > upper[i] ) brush->mins[i] = upper[i];
        if ( brush->maxs[i] < upper[i] ) brush->maxs[i] = upper[i];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x482f70  Checkkey_Model (the binary's "Checkkey_Model_0") — an entity epair changed; if
// the key affects model geometry, trigger a rebuild.  It bumps the 16-bit DEF version
// (version_prob_wrong), NOT the 32-bit version.  (The COMPLEX classname handler, the
// binary's "Checkkey_Model", is a DIFFERENT function — MapLoad_ParsePrefab above.)
// ─────────────────────────────────────────────────────────────────────────────
void Checkkey_Model( entity_s_def *e, const char *key )
{
    iassert( e );
    eclass_t *ec = e->eclass;
    if ( ec && *(int *)&ec->fixedsize )
    {
        if ( !_stricmp( key, "angles" ) || !_stricmp( key, "modelscale" ) )
        {
            Entity_RebuildBounds( (entity_s *)e );
            ++*(unsigned __int16 *)&e->version_prob_wrong;
        }
    }
    if ( !_stricmp( key, "model" ) )
    {
        e->modelClass = nullptr;
        // Clear the model-failed flag of e->brushes.prev (the def-list FIRST element; for a
        // fixedsize entity also `def`).  The binary clears it UNCONDITIONALLY; the sentinel
        // guard skips its harmless write to the sentinel-as-brush on an empty list.
        brush_t *b = (brush_t *)e->brushes.prev;
        if ( b != (brush_t *)&e->def )
            *(unsigned __int8 *)&b->unk01 = 0;   // low byte only (modelFailed)
        ++*(unsigned __int16 *)&e->version_prob_wrong;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x483e70  ParseEntity — parse one entity block { ... } from the text stream.
// SUBSET: the fixed-size model-offset tail is documented at its site below.  hasBrushes /
// empty checks read brushes.prev (the binary's field), not def.
// rawOnly: return the epair-only entity (no eclass / bbox; the project file).
// keepFileLayers: brushes keep the file's layers (a load); else they join the active layer.
// ─────────────────────────────────────────────────────────────────────────────
entity_s *ParseEntity( const char **text, int version, char rawOnly, char keepFileLayers )
{
    ParseThreadInfo *parseInfo = Com_GetParseThreadInfo();
    parseInfo_t     *pi        = &parseInfo->parseInfo[parseInfo->parseInfoNum];

    // Read opening brace with Com_Parse — required so the '{' that Map_ParseLayers ungot
    // (after the layer block) is honoured. (Raw Com_ParseExt skips the unget and would
    // read the token *after* '{'.)
    parseInfo_t *tok = Com_Parse( text );
    if ( !tok->token[0] )
        return nullptr;
    if ( strcmp( tok->token, "{" ) )
        Com_Error( ERR_FATAL, "ParseEntity: { not found" );

    entity_s *ent = (entity_s *)operator new( sizeof(entity_s) );
    memset( ent, 0, sizeof(entity_s) );
    // Unique entity numberId — the id undo.cpp matches on.
    ent->numberId = dword_739DC4++;
    // Initialise the brush-def circular list sentinel: BOTH the head (def) AND the embedded
    // brushes node's prev link point at &def — an empty circular list — so functions that
    // read ent->brushes.prev (Entity_Free/WriteSelected) see the sentinel.
    ent->def = (entity_s *)&ent->def;
    ent->brushes.prev = (selbrush_t *)&ent->def;

    char layerBuf[1028];
    Map_ParseEntityLayerKey( text, "000_Global", layerBuf, "layer" );

    // Parse key/value pairs and brush blocks. Each key is read with Com_Parse (honours the
    // unget left by Map_ParseEntityLayerKey and by each prior epair).
    // Epairs are APPENDED (a running tail pointer starting at &ent->epairs), preserving
    // file order — prepending would reverse them.
    epair_t **epairTail = &ent->epairs;
    while ( true )
    {
        parseInfo_t *tok2 = Com_Parse( text );
        if ( !tok2->token[0] )
        {
            Error( "ParseEntity: EOF without closing brace" );
            return nullptr;
        }
        if ( !strcmp( tok2->token, "}" ) )
            break;

        if ( !strcmp( tok2->token, "{" ) )
        {
            // Brush block (a patch/mesh block parses into a patch brush too).
            brush_t *b = Brush_Parse( text, version );
            if ( !b )
                break;
            if ( !keepFileLayers )
            {
                if ( b->parent_layer_string )
                    free( b->parent_layer_string );
                size_t layerLen = strlen( g_activeLayer_string );
                char  *lc       = (char *)operator new( layerLen + 1 );
                memcpy( lc, g_activeLayer_string, layerLen + 1 );
                b->parent_layer_string = lc;
            }
            Entity_LinkBrush( b, ent );
        }
        else
        {
            // tok2->token is the KEY. The value read reuses the SAME parseInfo token
            // buffer (Com_GetParseThreadInfo() returns the same slot), so the key must
            // be copied to its own storage BEFORE reading the value. The value is read on
            // the same line (Com_ParseOnLine).
            epair_t *ep = (epair_t *)operator new( sizeof(epair_t) );
            ep->next    = nullptr;
            size_t klen = strlen( tok2->token );
            ep->key     = (char *)operator new( klen + 1 );
            memcpy( ep->key, tok2->token, klen + 1 );

            parseInfo_t *pi3   = Com_ParseOnLine( text );
            const char *valStr = pi3->token;
            size_t vlen = strlen( valStr );
            ep->value   = (char *)operator new( vlen + 1 );
            memcpy( ep->value, valStr, vlen + 1 );

            // Append to epairs list (file order)
            ep->next    = nullptr;
            *epairTail  = ep;
            epairTail   = &ep->next;
        }
    }

    // KIWI (2026-10-05, load/save gauntlet): a misc_model is placed in game by R_LoadMiscModel,
    // where a non-zero "angle" (yaw) wins over "angles"; the editor (Entity_GetOrientationMatrix)
    // and its rotate tools read "angles" only, so such a model showed unrotated here and turned
    // in game.  Say it as "angles" - the orientation the game already used - and drop "angle".
    if ( !rawOnly && !_stricmp( ValueForKey2( ent, "classname" ), "misc_model" ) )
    {
        const char *angle = ValueForKey2( ent, "angle" );   // the first, as the game reads
        if ( atof( angle ) != 0.0 )
        {
            char value[96];
            _snprintf( value, sizeof( value ), "0 %s 0", angle );
            value[sizeof( value ) - 1] = '\0';
            Sys_Printf( "misc_model: \"angle\" \"%s\" read as \"angles\" \"%s\" (the game takes \"angle\" first)\n",
                        angle, value );
            Epair_SetRaw( &ent->epairs, "angles", value );
        }
        for ( int i = 0; i < 8; ++i )            // every copy; an "angle" of 0 the game ignores too
            DeleteKey( &ent->epairs, "angle" );
    }

    // Check if we should return the raw parsed entity (group_info or force-return flag)
    if ( !strcmp( ValueForKey2( ent, "classname" ), "group_info" ) || rawOnly )
        return ent;

    bool hasBrushes = ( (void *)ent->brushes.prev != (void *)&ent->def );
    Entity_GetVec3ForKey( (entity_s_def *)ent, ent->origin, "origin" );
    char     *clsName = ValueForKey2( ent, "classname" );
    eclass_t *ec      = Eclass_ForName( hasBrushes ? 1 : 0, clsName );
    ent->eclass     = ec;

    if ( *(int *)&ec->fixedsize )
    {
        // Fixed-size entity (light/spawn/prefab/...): create the bounding-box brush. The
        // bbox brush is the editor's selectable proxy; it is NEVER written to the .map
        // (MapFile_WriteEntity skips brush output for fixedsize). It IS required so the
        // def-list is non-empty (the Map_SaveFile entity-write gate) and so the per-entity
        // layer can be stored on / read back from the brush (parent_layer_string).
        if ( hasBrushes )
        {
            Sys_Printf( "Warning: Fixed size entity with brushes\n" );
            // KIWI: free the warned brushes (unlinked the way Entity_Free does) - the reset
            // below alone leaked every one of them
            for ( brush_t *b = (brush_t *)ent->brushes.prev; b != (brush_t *)&ent->def; )
            {
                brush_t *next = (brush_t *)b->onext;
                b->onext = nullptr;
                b->oprev = nullptr;
                b->owner = nullptr;
                if ( b->refCount > 0 )
                    --b->refCount;
                Brush_Free_R( b );
                b = next;
            }
            // A fixedsize entity must own NO parsed brushes — reset the def-list to the
            // empty sentinel (BOTH def and brushes.prev point back at &def), discarding the
            // warned brushes so the bbox brush below is the sole def-list element.
            ent->def = (entity_s *)&ent->def;
            ent->brushes.prev         = (selbrush_t *)&ent->def;
        }

        EntityAssignModel( (entity_s_def *)ent );

        float *origin = ent->origin;
        float  mins[3], maxs[3];
        mins[0] = ec->mins[0] + origin[0];
        mins[1] = ec->mins[1] + origin[1];
        mins[2] = ec->mins[2] + origin[2];
        maxs[0] = ec->maxs[0] + origin[0];
        maxs[1] = ec->maxs[1] + origin[1];
        maxs[2] = ec->maxs[2] + origin[2];

        // SUBSET: the binary's fixed-size tail additionally
        //   (1) for CLASS_MODELCLASS (classtype & 8) with a non-empty "model" key, reads the
        //       entity's "angles" (zero-filled when absent) and calls Eclass_ForModel(modelName,
        //       e) to realize the model class;
        //   (2) when the angles are non-zero, rotates the bbox's face planepts about the
        //       origin (0x478630) so an angled misc_model gets a ROTATED bbox.
        // Eclass_ForModel (eclass.cpp) is real; the rotation would be the face planepts turned
        // by KiwiAng_ToAxis about the origin.  The bbox brush of a fixed-size entity is never
        // written to the .map, so this is display-only.

        // Build the bounding-box brush = 0x475AC0(&matdef, ec, mins, maxs)
        //   == Brush_Alloc(&matdef, ec) + Brush_Create(mins, maxs, brush, ec).
        MaterialDef matdef;
        memset( &matdef, 0, sizeof( matdef ) );
        matdef.lyrMtl = ec->material.lyrMtl;
        matdef.radMtl = ec->material.radMtl;
        Init_MaterialLayer( &matdef, 0.25f );

        brush_t *bbox = Brush_Alloc( &matdef, ec );
        Brush_Create( mins, maxs, bbox, ec );
        if ( keepFileLayers )
            Brush_SetLayerString( bbox, layerBuf );   // store per-entity layer on the brush
        Entity_LinkBrush( bbox, (entity_s *)ent );
        return ent;
    }

    if ( (void *)ent->brushes.prev == (void *)&ent->def )
        Sys_Printf( "Warning: Brush entity with no brushes\n" );

    return ent;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x4847f0  Entity_WriteSelected  (entity.cpp:1006)
// Writes a single entity and its selected brushes to the file.
// The MemFile_fprintf handle is a pointer-to-pointer opaque writer (WriteWriter_t = WriteFunc_t**).
// ─────────────────────────────────────────────────────────────────────────────
void Entity_WriteSelected( entity_s_def *ent, WriteFunc_entity_t *writer )
{
    iassert( world_entity );

    // Check if this entity has any selected brushes (or IS the worldspawn def)
    bool isWorldDef = ( world_entity->def == (void *)ent );
    if ( !isWorldDef )
    {
        bool found = false;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        {
            if ( b->owner->def == (void *)ent )
            {
                found = true;
                break;
            }
        }
        if ( !found )
            return;
    }

    if ( *(int *)&ent->eclass->fixedsize )
    {
        // KIWI: exact origins (kiwi_fmt.h), not "%.1f": a copy / clone / paste is re-parsed
        // from this text, so every copy of a model landed on the 0.1 grid.
        char originStr[132], ox[40], oy[40], oz[40];
        sprintf( originStr, "%s %s %s", KiwiFmt_Exact( ox, sizeof( ox ), ent->origin[0] ),
                 KiwiFmt_Exact( oy, sizeof( oy ), ent->origin[1] ),
                 KiwiFmt_Exact( oz, sizeof( oz ), ent->origin[2] ) );
        SetKeyValue( ent, "origin", originStr );
    }

    // `writer` is a WriteWriter_t = WriteFunc_entity_t* (ptr-to-fn-ptr): call through the
    // function pointer, passing the handle as context.
    WriteFunc_entity_t fn = *writer;
    void *ctx = writer;

    fn( ctx, "{\n" );
    for ( epair_t *i = ent->epairs; i; i = i->next )
        fn( ctx, "\"%s\" \"%s\"\n", i->key, i->value );

    if ( !*(int *)&ent->eclass->fixedsize )
    {
        int j = 0;
        for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
        {
            if ( sb->owner->def == (void *)ent )
            {
                fn( ctx, "// brush %i\n", j++ );
                Brush_Write( writer, sb->def );
            }
        }
    }
    fn( ctx, "}\n" );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485410  EntityAssignModel  (entity.cpp:1348)
// Reads the "model" epair; if the entity has a default_model_name that differs,
// forces it. Strips the leading "xmodel/" prefix from model keys.  The version bump is
// the 16-bit DEF version, NOT version; the unk01 clear is byte-wide, not 16-bit.
// ─────────────────────────────────────────────────────────────────────────────
void EntityAssignModel( entity_s_def *e )
{
    const char *modelName = "";
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )   // kept open-coded: the NULL-value assert
    {
        if ( _stricmp( ep->key, "model" ) == 0 )
        {
            modelName = ep->value;                 // may be NULL
            iassert( modelName );                  // entity.cpp:1351 -- fires on NULL model value
            if ( !modelName ) modelName = "";      // release-safe: binary carries NULL into _stricmp (latent AV)
            break;
        }
    }

    eclass_t *ec = e->eclass;
    if ( ec && *(int *)&ec->fixedsize &&
         ec->default_model_name && *ec->default_model_name )
    {
        if ( _stricmp( modelName, ec->default_model_name ) )
        {
            SetKeyValue( e, "model", ec->default_model_name );
            // The first brush def (e->brushes.prev) loses its model-failed flag; the binary
            // writes it unconditionally, the sentinel guard skips its harmless empty-list
            // write (see Checkkey_Model above).
            brush_t *b = (brush_t *)e->brushes.prev;
            if ( b != (brush_t *)&e->def )
                *(unsigned __int8 *)&b->unk01 = 0;   // low byte only (the high byte is the live cull flag)
            ++*(unsigned __int16 *)&e->version_prob_wrong;
            e->modelClass = nullptr;
        }
    }
    else if ( I_strnicmp( modelName, "xmodel", 6 ) == 0 )
    {
        char c = modelName[6];
        if ( c == '/' || c == '\\' )
        {
            char *stripped = AllocMaterialString( modelName + 7 );
            SetKeyValue( e, "model", stripped );
            free( stripped );
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485510  Entity_SetDefaultModelKey
// Tail of CreateEntityFromName's re-class (Path B): after the owning entity has been
// repointed to the new eclass `newClass`, set the entity's "model" key to that eclass's
// default_model_name (when the eclass is a fixedsize class WITH a non-empty default
// model name) and refresh the model-render caches.
//
//   bbox     = the entity's new bbox brush DEF
//   newClass = the new eclass (the Eclass_ForName result)
//
// This is the SAME tail EntityAssignModel runs:
//   SetKeyValue("model", default) / ++entity->version_prob_wrong (16-bit) /
//   entity->modelClass = 0 / firstBrushDef->unk01 low byte = 0
// (entity->brushes.prev is a brush_t DEF, NOT a selbrush instance).
// The gate reads the ENTITY's eclass (brush->owner->eclass) for the fixedsize/non-empty
// check, but the model-name VALUE comes from the passed newClass; at the call site
// owner->def->eclass was just set to newClass, so they are the same object.
// ─────────────────────────────────────────────────────────────────────────────
eclass_t *Entity_SetDefaultModelKey( brush_t *bbox, eclass_t *newClass )
{
    entity_s_def *owner = (entity_s_def *)bbox->owner;   // the entity DEF
    eclass_t *ec = owner->eclass;
    if ( !ec )                                        return ec;
    if ( !*(int *)&ec->fixedsize )                    return ec;
    if ( !ec->default_model_name )                    return (eclass_t *)ec->default_model_name;
    if ( !*ec->default_model_name )                   return (eclass_t *)ec->default_model_name;  // first char

    SetKeyValue( owner, "model", newClass->default_model_name );
    ++*(unsigned __int16 *)&owner->version_prob_wrong;
    owner->modelClass = nullptr;
    // The low byte of the first brush DEF's unk01.  No sentinel guard here (unlike
    // EntityAssignModel).
    brush_t *firstBrushDef = (brush_t *)owner->brushes.prev;
    *(unsigned __int8 *)&firstBrushDef->unk01 = 0;
    return (eclass_t *)owner;   // the binary returns the entity; unused by the sole caller
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485630  Prefab_Init  (entity.cpp:1423)
// Creates a new entity INSTANCE from a def, links it into the prefab/entity list,
// and creates brush instances for each brush def owned by entDef.  The instance version
// is seeded from the DEF's version_prob_wrong minus 1 (16-bit), not from the DEF's version.
// ─────────────────────────────────────────────────────────────────────────────
entity_s *Prefab_Init( prefab_s *instList, entity_s_def *entDef, selbrush_t *activeList )
{
    iassert( entDef );

    entity_s *entInst = (entity_s *)operator new( sizeof(entity_s) );
    memset( entInst, 0, sizeof(entity_s) );
    entInst->def = entDef;

    iassert( entInst->modelInst == NULL );   // entity.cpp:1427
    iassert( entInst->prefab == NULL );      // entity.cpp:1428

    *(unsigned __int16 *)&entInst->version = (unsigned __int16)entDef->version_prob_wrong - 1;   // DEF version -> the instance version's low word
    entInst->mapLayer = nullptr;

    // Insert into the prefab entity list (via the instList sentinel)
    entInst->next               = (entity_s *)instList;
    entInst->prev               = instList->prev_entity;
    instList->prev_entity->next = entInst;
    instList->prev_entity       = entInst;

    // Initialise the embedded brush-instance sentinel
    entInst->brushes.ownerNext = (selbrush_t *)&entInst->brushes;
    entInst->brushes.ownerPrev = (selbrush_t *)&entInst->brushes;

    // Walk the entity's brush-DEF list and create an instance per brush.  Like
    // MapFile_WriteEntity, start at brushes.prev (the def-list FIRST element), advance via
    // brush_t.onext, and stop at the sentinel &def.  (Starting at def itself — the list
    // HEAD, whose onext jumps straight to the sentinel — creates only ONE instance.)
    brush_t *bDef = (brush_t *)entDef->brushes.prev;
    brush_t *sentinel = (brush_t *)&entDef->def;
    while ( bDef != sentinel )
    {
        selbrush_t *sb = Brush_AddToList( bDef, entInst );
        if ( sb->next || sb->prev )
            Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
        if ( activeList == &selected_brushes )
        {
            Brush_AddToList2( sb );
        }
        else
        {
            sb->next               = activeList->next;
            activeList->next->prev = sb;
            activeList->next       = sb;
            sb->prev               = activeList;
        }
        bDef = bDef->onext;
    }
    ++entDef->refCount;
    return entInst;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485750  Entity_Free  (entity.cpp:1453)
// Frees an entity INSTANCE: unlinks it, removes model instance, frees brush instances,
// decrements def refcount, frees def if last ref.
// ─────────────────────────────────────────────────────────────────────────────
void Entity_Free( entity_s *e )
{
    iassert( e );
    iassert( e->def );
    entity_s_def *def = (entity_s_def *)e->def;
    vassert( (e->def->refCount > 0), "(e->def->refCount) = %i", e->def->refCount );   // entity.cpp:1455
    iassert( e->next );
    iassert( e->prev );

    e->next->prev = e->prev;
    e->prev->next = e->next;

    Entity_FreePrefab( e );

    if ( e->modelInst )
    {
        RemoveModelInstFromBuf( e->modelInst );
        e->modelInst = 0;
    }

    while ( e->brushes.ownerNext != &e->brushes )
        Brush_Free( e->brushes.ownerNext );

    --def->refCount;
    if ( !def->refCount )
        Entity_Free_R( (entity_s *)def );
    free( e );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485890  SetupModelInst  (entity.cpp:1480)
// If the entity instance is out-of-date (version mismatch), rebuilds model instance and
// bounding brush.  It compares / writes the DEF's 16-bit version_prob_wrong, not version.
// CLASS_PREFAB gate = classtype & 0x10.
// ─────────────────────────────────────────────────────────────────────────────
void SetupModelInst( float *ident_mtx, entity_s *e )
{
    iassert( e );
    entity_s_def *def = (entity_s_def *)e->def;
    // Compare the INSTANCE version against the DEF's version_prob_wrong (the field
    // Checkkey_Model bumps), both 16-bit.
    if ( (unsigned __int16)e->version == (unsigned __int16)def->version_prob_wrong )
        return;

    Entity_FreePrefab( e );
    if ( e->modelInst )
    {
        RemoveModelInstFromBuf( e->modelInst );
        e->modelInst = 0;
    }

    float bmins[3] = {}, bmaxs[3] = {};
    if ( ( def->eclass->classtype & CLASS_PREFAB ) != 0 )
    {
        Entity_InitPrefabInst( e, ident_mtx, bmins, bmaxs );
    }
    else
    {
        Entity_UpdateModelInst( e, ident_mtx, bmins, bmaxs );
    }

    // Rebuild the sole brush instance.  Asserts the instance owns exactly one brush
    // (ownerNext != &brushes && ownerNext == ownerPrev, line 1491) and that brush has a
    // def with refCount>=2 (line 1492), then builds it UNCONDITIONALLY.
    iassert( e->brushes.ownerNext != &e->brushes && e->brushes.ownerNext == e->brushes.ownerPrev );
    selbrush_t *ownerNext = e->brushes.ownerNext;
    brush_t *brushDef = ownerNext->def;
    iassert( e->brushes.ownerNext->def && e->brushes.ownerNext->def->refCount >= 2 );
    Brush_Build( brushDef, bmins, bmaxs );
    e->version = (unsigned __int16)def->version_prob_wrong;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x4859B0  Angles_AimAlongDir — re-aim an entity's angles onto a target direction `dir`.
// Builds the angles' current FORWARD vector (cos/sin of pitch/yaw), projects it onto the
// plane perpendicular to `dir` (Gram-Schmidt), then assembles a 3x3 axis whose rows are
// [forward', dir × forward', dir] and reads pitch/yaw/roll back out via AxisToAngles.
// AxisToAngles takes the rows as one float[3][3].  MSVC may reorder separate locals, so
// this MUST be a single contiguous matrix[9], not three loose vec3s.
static void Angles_AimAlongDir( float *angles_out, float *dir )
{
    float matrix[9];   // row0 = forward', row1 = dir × forward', row2 = dir  (contiguous!)

    const double yaw   = DEG2RAD( angles_out[1] );
    const float  cy = (float)cos( yaw ),   sy = (float)sin( yaw );
    const double pitch = DEG2RAD( angles_out[0] );
    const float  cp = (float)cos( pitch ), sp = (float)sin( pitch );

    matrix[0] = cp * cy;
    matrix[1] = cp * sy;
    matrix[2] = -sp;

    // forward' = forward - dot(dir,forward)*dir  (project onto plane ⟂ dir)
    const float neg = -( dir[0] * matrix[0] + dir[1] * matrix[1] + dir[2] * matrix[2] );
    matrix[0] += dir[0] * neg;
    matrix[1] += dir[1] * neg;
    matrix[2] += dir[2] * neg;
    Vec3Normalize_R( matrix );

    Vec3Cross( dir, matrix, matrix + 3 );   // row1 = dir × forward'
    Vec3Normalize_R( matrix + 3 );

    matrix[6] = dir[0];                      // row2 = dir
    matrix[7] = dir[1];
    matrix[8] = dir[2];

    AxisToAngles( angles_out, (float (*)[3])matrix );
}

// 0x485ad0  AlignEntityToFace_OrientToFloor  (entity.cpp:1519)
// Adjusts the entity's "angles" epair toward a given face direction (Angles_AimAlongDir,
// then the angles written back).
// ─────────────────────────────────────────────────────────────────────────────
void AlignEntityToFace_OrientToFloor( entity_s_def *ent, float *dir )
{
    iassert( ent->eclass->fixedsize );   // entity.cpp:1519
    float angles[3];
    if ( !Entity_GetVec3ForKey( ent, angles, "angles" ) )
        angles[0] = angles[1] = angles[2] = 0.0f;
    Angles_AimAlongDir( angles, dir );
    char text[160];                                        // KIWI: exact, not %g
    SetKeyValue( ent, "angles", KiwiFmt_Vec3Exact( text, sizeof( text ), angles, 0 ) );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x484980  Entity_Create  (entity.cpp)
// Creates a new entity from the selected brushes (merge-into-existing OR fresh def+instance OR
// fixedsize point-entity bbox).
// ─────────────────────────────────────────────────────────────────────────────
entity_s *Entity_Create( eclass_t *eclass )
{
    char      foundNonWorld = 0;       // a non-world brush was selected
    entity_s *targetDef     = nullptr; // existing entity def to merge into
    entity_s *ownerInst     = nullptr; // owner instance / the result

    // ── Scan the current selection ───────────────────────────────────────────
    // A point entity can't be built from existing brushes; brushes of a single
    // non-world entity can be re-classed (merge). World brushes are ignored here —
    // for a point entity, the single selected world brush is the PLACEHOLDER that
    // marks where to drop the entity (consumed by Select_Delete in the tail below).
    if ( selected_brushes.next != &selected_brushes )
    {
        for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
        {
            if ( sb->owner == world_entity )
                continue;

            if ( *(int *)&eclass->fixedsize )
            {
                Sys_Printf( "Entity NOT created, can't make a point entity out of brushes\n" );
                MessageBeep( 0x40u );
                return nullptr;
            }
            if ( foundNonWorld && targetDef != (entity_s *)sb->def->owner )
            {
                Sys_Printf( "Can't merge entities\n" );
                MessageBeep( 0x40u );
                return nullptr;
            }
            entity_s_def *owner = (entity_s_def *)sb->def->owner;
            ownerInst = sb->owner;
            targetDef = (entity_s *)owner;
            if ( !Entity_HasEpairMatch( owner, "classname", eclass->name ) )
            {
                Sys_Printf( "Can't change entity type from %s to %s; ungroup first\n",
                            ValueForKey2( owner, "classname" ), eclass->name );
                MessageBeep( 0x40u );
                return nullptr;
            }
            foundNonWorld = 1;
        }

        if ( foundNonWorld )
        {
            // Re-class the selected brushes into the existing entity `targetDef`
            // (ownerInst = its instance): a WORLD brush is reparented into targetDef
            // (Entity_UnlinkBrush + Entity_LinkBrush(def, targetDef) +
            // Entity_LinkBrush_0(ownerInst, inst));
            // a brush ALREADY owned by the target is asserted-consistent and left in
            // place.  (No new entity is allocated here — the selection collapses into
            // the existing one.)
            entity_s *inst = ownerInst;              // the binary's local name
            iassert( inst );   // entity.cpp:1160

            for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
            {
                if ( sb->owner != world_entity )
                {
                    // Already in (the merge target's) instance — verify + skip.
                    {
                        selbrush_t   *brushInst = sb;         // the binary's locals
                        entity_s     *inst      = ownerInst;
                        entity_s_def *e         = targetDef;
                        iassert( brushInst->owner == inst );      // entity.cpp:1166
                        iassert( brushInst->def->owner == e );    // entity.cpp:1167
                    }
                    continue;
                }
                Brush_Deselect_Helper( sb );
                brush_t *bDef = sb->def;
                Entity_UnlinkBrush( bDef );                  // def out of worldspawn's def-list
                Entity_LinkBrush( bDef, targetDef );         // def into targetDef's def-list (+Brush_ApplyEntityColor)
                Entity_LinkBrush_0_extern( ownerInst, sb );  // instance into ownerInst's owner-chain
                Brush_RebuildAfterEdit( sb->def );
                Brush_Select_Helper( sb );
            }
            g_nUpdateBits = -1;
            return ownerInst;
        }
    }

    // ── Allocate a fresh entity def + instance ─────────────────────────────────
    entity_s_def *e = (entity_s_def *)operator new( sizeof(entity_s_def) );
    memset( e, 0, sizeof(entity_s_def) );
    e->numberId = dword_739DC4++;
    // empty def-side brush-list sentinel — BOTH the head ptr (def) AND brushes.prev
    // point at &def (see Map_New / Entity_LinkBrush).
    e->brushes.prev          = (selbrush_t *)&e->def;
    e->def  = (entity_s *)&e->def;
    SetKeyValue( e, "classname", eclass->name );
    e->eclass = eclass;
    // copy the eclass's default key/values (the eclass->xx8 epair list)
    for ( epair_t *node = eclass->xx8; node; node = node->next )
    {
        Epair_SetRaw( &e->epairs, node->key, node->value );
    }

    IncRef( (entity_s *)e, &entities );                              // inlined in the binary

    ownerInst = Prefab_Init( (prefab_s *)&entityInsts, e, &active_brushes );

    if ( !*(int *)&eclass->fixedsize )
    {
        // Brush entity (func_*/trigger_*): reparent every selected world brush into the
        // new entity `e`/`ownerInst` (Entity_UnlinkBrush(def) + Entity_LinkBrush(def, e) +
        // Entity_LinkBrush_0(ownerInst, inst)).  With no selection the entity is returned empty.
        // (The scan above already proved every selected brush is a WORLD brush — a
        // non-world selection would have early-returned via the merge/refuse paths.)
        for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
        {
            Brush_Deselect_Helper( sb );
            brush_t *bDef = sb->def;
            Entity_UnlinkBrush( bDef );                  // def out of worldspawn's def-list
            Entity_LinkBrush( bDef, (entity_s *)e );     // def into the new entity's def-list (+Brush_ApplyEntityColor)
            Entity_LinkBrush_0_extern( ownerInst, sb );  // instance into ownerInst's owner-chain
            Brush_RebuildAfterEdit( sb->def );
            Brush_Select_Helper( sb );
        }
        g_nUpdateBits = -1;
        return ownerInst;
    }

    // ── Point entity: build the bounding-box brush at the placeholder location ──
    // Origin = placeholder mins − eclass mins; the bbox spans the eclass [mins,maxs]
    // placed at that origin (== the ParseEntity fixedsize tail above). The
    // origin is written to the def's origin field; MapFile_WriteEntity writes the
    // "origin" text from it on save (fixedsize entities emit no brush).
    brush_t *refDef = selected_brushes.next->def;
    float origin[3];
    origin[0] = refDef->mins[0] - eclass->mins[0];
    origin[1] = refDef->mins[1] - eclass->mins[1];
    origin[2] = refDef->mins[2] - eclass->mins[2];
    e->origin[0] = origin[0];
    e->origin[1] = origin[1];
    e->origin[2] = origin[2];

    float mins[3], maxs[3];
    mins[0] = eclass->mins[0] + origin[0];
    mins[1] = eclass->mins[1] + origin[1];
    mins[2] = eclass->mins[2] + origin[2];
    maxs[0] = eclass->maxs[0] + origin[0];
    maxs[1] = eclass->maxs[1] + origin[1];
    maxs[2] = eclass->maxs[2] + origin[2];

    // Init_MaterialLayer only seeds the bbox texcoords, which are never emitted for a
    // fixedsize entity.
    MaterialDef matdef;
    memset( &matdef, 0, sizeof( matdef ) );
    matdef.lyrMtl = eclass->material.lyrMtl;
    matdef.radMtl = eclass->material.radMtl;
    Init_MaterialLayer( &matdef, 0.25f );

    brush_t *bbox = Brush_Alloc( &matdef, eclass );
    Brush_Create( mins, maxs, bbox, eclass );
    Entity_LinkBrush( bbox, (entity_s *)e );          // def-side link (refCount 0->1)
    Brush_RebuildAfterEdit( bbox );

    Select_Delete();                                  // consume the placeholder selection
    selbrush_t *inst = Brush_AddToList( bbox, ownerInst );  // instance (refCount 1->2)
    if ( inst->next || inst->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    Brush_AddToList2( inst );                          // add to the selection list

    g_nUpdateBits = -1;
    return ownerInst;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x4870c0  Map_New  (entity.cpp:690 asserts) — THE File→New WORLDSPAWN INIT.
// Frees the current map, re-inits the sentinel lists, and builds a fresh worldspawn
// entity DEF by hand (the text loader's per-entity build, done without .map text).
// world_entity ends valid so a drag-created brush can Entity_LinkBrush onto it.
// The entity-list insert is TAIL insertion (IncRef).
// ─────────────────────────────────────────────────────────────────────────────
void Map_New()
{
    Sys_Printf( "Map_New\n" );
    Map_Free();
    Map_NewMap();
    Pointfile_Clear();
    Map_InitlLayers();
    g_qeglobals.g_layerCount_maybe = 1;
    Sys_Printf( "Updating layers...\n" );
    Layers_SetMapLayers();
    Layers_RefreshDialog();

    // Allocate worldspawn entity def
    entity_s_def *e = (entity_s_def *)operator new( sizeof(entity_s_def) );
    memset( e, 0, sizeof(entity_s_def) );
    // Self-initialise the def-side brush-list sentinel. The head lives at def; an EMPTY
    // list needs BOTH the head ptr AND brushes.prev to point at &def — see Entity_LinkBrush.
    // (Without the brushes.prev write a walk of the empty list, e.g. saving an empty
    // worldspawn, dereferences NULL.)
    e->brushes.prev          = (selbrush_t *)&e->def;
    e->def  = (entity_s *)&e->def;

    SetKeyValue( e, "classname", "worldspawn" );
    eclass_t *ec  = Eclass_ForName( 1, "worldspawn" );
    e->eclass    = ec;

    // Link into entities list (IncRef inlined in the binary)
    IncRef( (entity_s *)e, &entities );

    world_entity = Prefab_Init( (prefab_s *)&entityInsts, e, &active_brushes );

    // Reset the camera + XY view to the new-map default. Camera goes
    // to origin (0,0,48), pitch/yaw 0 (roll untouched); the active XY view recentres.
    // U-GLOBALS: the camera + XY state come from Ed_Camera()/Ed_ActiveXY() (never NULL), so the
    // GUI guards fall away; only the CXYWnd::Paste call still needs the MFC shell.
    {
        camera_s &cam = *Ed_Camera();
        cam.angles[0] = 0.0f;
        cam.angles[1] = 0.0f;
        cam.origin[0] = 0.0f;
        cam.origin[1] = 0.0f;
        cam.origin[2] = 48.0f;
    }
    // KIWI-UX (shakeout I): a POST-INIT hook over the ported placement above, not a
    // replacement for it — the classic profile still gets (0,0,48) looking down +X.
    // USER DIRECTIVE: "Scale of the map (3d view) is still way too big."  Starting a
    // fresh map standing AT the origin looking at empty space is the worst case of
    // that: nothing is in frame, so nothing gives the world a size.  See
    // KiwiCam_DefaultSpawn (kiwi_camera.h) for the standoff and the angles.
    {
        extern bool KiwiUX_ModernInput();     // kiwi_ux.cpp
        extern void KiwiCam_DefaultSpawn();   // kiwi_camera.cpp
        if ( KiwiUX_ModernInput() )
            KiwiCam_DefaultSpawn();
    }
    Ed_ActiveXY()->m_vOrigin[0] = 0.0f;
    Ed_ActiveXY()->m_vOrigin[1] = 0.0f;
    Ed_ActiveXY()->m_vOrigin[2] = 0.0f;
    if ( g_bRestoreBetween )
        XYWnd_PasteClip();   // carry a selection across File→New (U-CMD-1 free fn)

    g_bRestoreBetween = false;
    modified          = 0;     // a fresh map is clean
    g_nUpdateBits     = -1;    // invalidate all views
}

// ─── deps for Map_ImportBuffer (ported elsewhere) ─────────────────────────────
extern int         Map_GetNextAutoTarget();                                // map.cpp 0x487950


// ─────────────────────────────────────────────────────────────────────────────
// Clipboard (CXYWnd::Copy / Paste: the in-app buffer + the Win32 clipboard mirror).
//
// The binary stores the copied selection in a global CMemFile (g_Clipboard)
// AND mirrors it to the Win32 OLE clipboard (RegisterClipboardFormatA("RadiantClippings")
// + SetClipboardData) so brushes can be pasted between two running editors.  The CMemFile
// + CString refcounting is a heap buffer here.  KIWI (2026-09-22, user: "copy/paste from 1
// instance of kiwi radiant to another"): the Win32 mirror is back - Copy also puts the map
// text on the clipboard under "RadiantClippings", Paste takes the clipboard's clippings
// when it holds some (another editor's copy, or this one's), else the in-app buffer.
// Universal groups ride along: they are the `kiwi_group` epair on every member.
// NOT byte-verified against 0x46E140 / 0x46E280 (the IDB was not open): the payload is
// the NUL-terminated map text, the same bytes the in-app buffer holds.
//
// Copy writes the selection to the buffer via Entity_WriteSelected_R (the SAME .map
// writer Map_SaveFile uses); Paste re-parses the buffer through Map_ImportBuffer.
// Reset/Append/Writer are port-authored glue with no binary counterpart.  Copy core ==
// Entity_WriteSelected_R; Paste core == Com_BeginParseSession + Map_ImportBuffer(&buf,4) +
// teardown ("paste from clipboard", spaceDelim=0 / negNum=1).
// ─────────────────────────────────────────────────────────────────────────────
// Writer handle = WriteFunc_entity_t* (pointer to the fn-ptr at slot 0); matches the
// entity.cpp Entity_WriteSelected convention (the fn is called with ctx==the handle).
extern void Entity_WriteSelected_R( WriteFunc_entity_t *writer );   // map.cpp 0x488DF0

// The clipboard text. Heap buffer grown on demand (no std::string in this TU's PCH path).
static char  *s_clipboardBuf = nullptr;
static size_t s_clipboardLen = 0;   // strlen (excludes the NUL)
static size_t s_clipboardCap = 0;
// KIWI (2026-09-09): the Win32 clipboard sequence number at the moment of the last
// in-app Copy.  Paste (mainfrm.cpp 33040) lets clipboard IMAGES win only when the OS
// clipboard changed AFTER that copy — "whichever was copied most recently wins".
// Without it a screenshot taken hours earlier hijacked every Ctrl+V into an image paste.
static DWORD  s_clipboardSysSeq = 0;

static void RadiantClipboard_Reset()
{
    s_clipboardLen = 0;
    if ( s_clipboardBuf && s_clipboardCap )
        s_clipboardBuf[0] = '\0';
}

static void RadiantClipboard_Append( const char *txt, size_t n )
{
    if ( s_clipboardLen + n + 1 > s_clipboardCap )
    {
        size_t newCap = s_clipboardCap ? s_clipboardCap * 2 : 8192;
        while ( newCap < s_clipboardLen + n + 1 )
            newCap *= 2;
        char *nb = (char *)realloc( s_clipboardBuf, newCap );
        if ( !nb )
            Com_Error( ERR_FATAL, "RadiantClipboard_Append: out of memory" );
        s_clipboardBuf = nb;
        s_clipboardCap = newCap;
    }
    memcpy( s_clipboardBuf + s_clipboardLen, txt, n );
    s_clipboardLen += n;
    s_clipboardBuf[s_clipboardLen] = '\0';
}

// The string-capturing writer. Matches the WriteWriter_t (WriteFunc_t**) contract used by
// Entity_WriteSelected_R / Entity_WriteSelected / Brush_Write: the writer is a 2-slot
// array whose [0] is this format function and whose [1] is the sink; the function is
// invoked with ctx == the writer pointer itself and recovers the sink from writer[1].
static int RadiantClipboard_Writer( void *ctx, const char *fmt, ... )
{
    char    line[4096];
    va_list ap;
    va_start( ap, fmt );
    int n = vsnprintf( line, sizeof( line ), fmt, ap );
    va_end( ap );
    if ( n < 0 )
        n = 0;
    if ( (size_t)n >= sizeof( line ) )
        n = (int)sizeof( line ) - 1;
    RadiantClipboard_Append( line, (size_t)n );
    (void)ctx;
    return n;
}

// The Win32 clipboard format the binary registers for its clippings.
static UINT RadiantClipboard_Format()
{
    static UINT fmt = 0;
    if ( !fmt )
        fmt = ::RegisterClipboardFormatA( "RadiantClippings" );
    return fmt;
}

// OpenClipboard fails while another process holds it (a clipboard manager, the other
// editor mid-copy): a few short retries.  The main window owns it - SetClipboardData
// fails after EmptyClipboard when the clipboard was opened with no owner.
static bool RadiantClipboard_OpenOS()
{
    for ( int attempt = 0; attempt < 8; ++attempt )
    {
        if ( ::OpenClipboard( g_qeglobals.d_hwndMain ) )
            return true;
        ::Sleep( 5 );
    }
    return false;
}

static bool RadiantClipboard_OSHasClippings()
{
    const UINT fmt = RadiantClipboard_Format();
    return fmt && ::IsClipboardFormatAvailable( fmt );
}

// The in-app buffer onto the Win32 clipboard as "RadiantClippings".
static void RadiantClipboard_ToOS()
{
    const UINT fmt = RadiantClipboard_Format();
    if ( !fmt || !s_clipboardBuf || !s_clipboardLen || !RadiantClipboard_OpenOS() )
        return;
    HGLOBAL mem = ::GlobalAlloc( GMEM_MOVEABLE, s_clipboardLen + 1 );
    char   *dst = mem ? (char *)::GlobalLock( mem ) : nullptr;
    if ( dst )
    {
        memcpy( dst, s_clipboardBuf, s_clipboardLen + 1 );
        ::GlobalUnlock( mem );
        ::EmptyClipboard();
        if ( ::SetClipboardData( fmt, mem ) )
            mem = nullptr;                              // the clipboard owns it now
    }
    if ( mem )
        ::GlobalFree( mem );
    ::CloseClipboard();
}

// The Win32 clipboard's clippings into the in-app buffer; false (buffer untouched) when it
// holds none.
static bool RadiantClipboard_FromOS()
{
    if ( !RadiantClipboard_OSHasClippings() || !RadiantClipboard_OpenOS() )
        return false;
    bool ok = false;
    if ( HANDLE mem = ::GetClipboardData( RadiantClipboard_Format() ) )
    {
        const char  *src = (const char *)::GlobalLock( mem );
        const size_t cap = ::GlobalSize( mem );
        if ( src )
        {
            size_t n = 0;
            while ( n < cap && src[n] )                 // (GlobalSize may round up; stop at the NUL)
                ++n;
            if ( n )
            {
                RadiantClipboard_Reset();
                RadiantClipboard_Append( src, n );
                ok = true;
            }
            ::GlobalUnlock( mem );
        }
    }
    ::CloseClipboard();
    return ok;
}

// Serialise the current selection to the in-app buffer (the writer half of Copy).
static void RadiantClipboard_Capture()
{
    RadiantClipboard_Reset();
    // 2-slot writer object: [0] = the format fn-ptr (recovered by *writer), [1] = sink
    // (unused — our writer appends to the static buffer, not writer[1]).
    WriteFunc_entity_t writer[2];
    writer[0] = &RadiantClipboard_Writer;
    writer[1] = nullptr;
    Entity_WriteSelected_R( writer );   // writer decays to WriteFunc_entity_t* (the handle)
}

// CXYWnd::Copy core (0x46E140). Serialise the current selection to the in-app
// clipboard buffer and mirror it to the Win32 clipboard.
void RadiantClipboard_Copy()
{
    RadiantClipboard_Capture();
    RadiantClipboard_ToOS();
    s_clipboardSysSeq = ::GetClipboardSequenceNumber();   // AFTER our own SetClipboardData
}

// True when the brush/entity copy is the most recent thing copied, so Paste must not be
// diverted to a clipboard image (KiwiRefImage_PasteClipboard): the Win32 clipboard holds
// clippings (this editor's or another's), or the in-app copy was made AFTER the last
// change to the Win32 clipboard.
bool RadiantClipboard_NewerThanSystem()
{
    if ( RadiantClipboard_OSHasClippings() )
        return true;
    return s_clipboardLen != 0 && s_clipboardBuf
        && ::GetClipboardSequenceNumber() == s_clipboardSysSeq;
}

// KIWI test hook (kiwi_test.cpp `clipboard forget`): drop the in-app copy the way a fresh
// editor has none, so the next paste must come off the Win32 clipboard.
void RadiantClipboard_ForgetLocal()
{
    RadiantClipboard_Reset();
    s_clipboardSysSeq = 0;
}

// KIWI (2026-09-09): Clone = Copy + Paste through the map text, which is what the
// binary's Clone_Selection did (OLE clipboard -> Map_ImportBuffer).  That route
// duplicates ENTITIES (misc_model, triggers, lights: a fresh entity with its epairs and
// the auto-target renumber), patches, and worldspawn brushes alike; the previous
// in-memory clone only deep-copied brush DEFs into their EXISTING owner entity, so a
// cloned model was a second proxy brush inside the same misc_model — invisible and
// unsaved — and fixed-size entities were skipped outright.  The user's own clipboard is
// stashed around the round trip so Clone never clobbers a pending Ctrl+V - and Clone
// stays off the Win32 clipboard entirely (Capture / Import, not Copy / Paste).
static void RadiantClipboard_Import();   // defined below

void RadiantClipboard_CloneSelection()
{
    char  *stashBuf = nullptr;
    size_t stashLen = s_clipboardLen;
    const DWORD stashSeq = s_clipboardSysSeq;
    if ( stashLen && s_clipboardBuf )
    {
        stashBuf = (char *)malloc( stashLen + 1 );
        if ( !stashBuf )
            Com_Error( ERR_FATAL, "RadiantClipboard_CloneSelection: out of memory" );
        memcpy( stashBuf, s_clipboardBuf, stashLen + 1 );
    }

    RadiantClipboard_Capture();
    RadiantClipboard_Import();

    RadiantClipboard_Reset();
    if ( stashBuf )
    {
        RadiantClipboard_Append( stashBuf, stashLen );
        free( stashBuf );
    }
    s_clipboardSysSeq = stashSeq;
}

// KIWI: is there map text to paste - in this editor or on the Win32 clipboard?
// (kiwi_pasteplace.cpp gates its command on it.)
bool RadiantClipboard_HasMapText()
{
    return ( s_clipboardLen != 0 && s_clipboardBuf != nullptr ) || RadiantClipboard_OSHasClippings();
}

// CXYWnd::Paste core (0x46E280). The Win32 clipboard's clippings replace the in-app
// buffer when it holds some (the most recent copy, from any editor); then the buffer is
// re-parsed, placed and selected.
void RadiantClipboard_Paste()
{
    RadiantClipboard_FromOS();
    RadiantClipboard_Import();
}

// Re-parse the in-app clipboard buffer. Sets up / tears down the parse session exactly
// like the binary (Com_BeginParseSession + spaceDelimited=0/negativeNumbers=1) so the
// version-4 .map tokens parse identically.
static void RadiantClipboard_Import()
{
    if ( !s_clipboardLen || !s_clipboardBuf )
        return;

    // Map_ImportBuffer consumes the buffer through a moving text pointer (const char**).
    const char *cursor = s_clipboardBuf;

    Com_BeginParseSession( "paste from clipboard" );
    ParseThreadInfo *pi = Com_GetParseThreadInfo();
    pi->parseInfo[pi->parseInfoNum].spaceDelimited  = 0;
    pi->parseInfo[pi->parseInfoNum].negativeNumbers = 1;

    extern char *Map_ImportBuffer( const char **text, int version );   // map.cpp 0x487C90
    Map_ImportBuffer( &cursor, 4 );

    Com_EndParseSession();
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x483de0  Entity_MemorySize
// Returns the total heap size (_msize) of an entity_s_def + all its epairs
// (key+value+node).  Called from undo.cpp.
// ─────────────────────────────────────────────────────────────────────────────
size_t Entity_MemorySize( entity_s *e )
{
    size_t total = _msize( e );
    epair_t *ep = e->epairs;
    while ( ep )
    {
        total += _msize( ep->key );
        total += _msize( ep->value );
        total += _msize( ep );
        ep = ep->next;
    }
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x485090  Entity_Clone
// Allocates a new entity_s_def, copies the source's eclass/origin fields, deep-copies
// all epairs.  Called from undo.cpp.
// ─────────────────────────────────────────────────────────────────────────────
entity_s *Entity_Clone( const entity_s *src )
{
    entity_s *clone = (entity_s *)::operator new( sizeof(entity_s) );
    memset( clone, 0, sizeof(entity_s) );

    // Assign unique entity ID (numberId, NOT epairEdits).
    clone->numberId = dword_739DC4++;

    // Init the DEF-side brush-list sentinel: BOTH def AND brushes.prev point at &def (the
    // empty-list sentinel; see Map_New / Entity_LinkBrush), NOT the instance-side
    // ownerNext/ownerPrev — a NULL def sentinel crashes Entity_Free_R when undo evicts the
    // entity (Undo_FreeFirstUndo).
    clone->def = (entity_s *)&clone->def;
    clone->brushes.prev         = (selbrush_t *)&clone->def;

    // Copy source fields
    clone->eclass    = src->eclass;
    clone->origin[0] = src->origin[0];
    clone->origin[1] = src->origin[1];
    clone->origin[2] = src->origin[2];

    // Deep-copy epairs
    epair_t *srcEp = src->epairs;
    while ( srcEp )
    {
        epair_t *newEp = (epair_t *)::operator new( sizeof(epair_t) );
        // both dup calls inlined in the binary (AllocMaterialString, qe3.cpp:56)
        newEp->key   = AllocMaterialString( srcEp->key );
        newEp->value = AllocMaterialString( srcEp->value );

        // Prepend to clone's epair list
        newEp->next  = clone->epairs;
        clone->epairs = newEp;

        srcEp = srcEp->next;
    }

    return clone;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x487B10  Entity_NeedsAutoExportId — "is this entity one of the auto-exported
//  placeholder kinds?"  True for a FIXED-SIZE entity whose classname contains "actor"
//  or "misc_turret".  (The binary builds a CString from ValueForKey2 before the two
//  strstr calls; the CString is pure MFC scaffolding around the same char*.)
//
//  0x487C30  Delete_Exportables — Misc→Delete exportables (cmd 206).  Deselect, then
//  select every ACTIVE brush whose owning entity qualifies, and delete the selection.
//  The `i = i->prev` step before Select_Brush is the binary's own: Select_Brush moves
//  the node out of active_brushes, so the loop resumes from the node BEFORE it.
// ═════════════════════════════════════════════════════════════════════════════

bool Entity_NeedsAutoExportId( entity_s_def *def )
{
    if ( !def || !def->eclass || !*(int *)&def->eclass->fixedsize )
        return false;
    const char *cn = ValueForKey2( def, "classname" );
    if ( !cn )
        return false;
    if ( strstr( cn, "actor" ) )
        return true;
    return strstr( cn, "misc_turret" ) != nullptr;
}

void Delete_Exportables()
{
    Select_Deselect( 1 );
    for ( selbrush_t *i = active_brushes.next; i != &active_brushes; i = i->next )
    {
        if ( !i )
            continue;
        if ( Entity_NeedsAutoExportId( (entity_s_def *)i->owner->def ) )
        {
            selbrush_t *victim = i;
            i = i->prev;                      // resume from the node before it
            Select_Brush( victim, 1, 1, 0 );
        }
    }
    Select_Delete();
}

