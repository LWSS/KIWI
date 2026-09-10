#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Plasticity-style outliner over live KIWI scene and selection state.
// Group creation mirrors xywnd.cpp:3411 plus pmesh.cpp:7396's new-entity undo id.
// Reparenting preserves Entity_UnlinkBrush -> Entity_LinkBrush ->
// Entity_LinkBrush_0_extern and the ported deselect/rebuild/reselect envelope.
// Undo_AddBrush must run before each relink because it records the old owner's id;
// recording an unchanged owner entity would make undo delete and recreate it.
// Ungroup mirrors Select_Ungroup for one row entity; rename uses targetname.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include "kiwi_outliner.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_hover.h"         // viewport row hover
#include "kiwi_refimage.h"
#include "kiwi_selection.h"
#include "kiwi_visibility.h"    // shared hidden state and undo
#include "kiwi_windows.h"

#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points; locations anchor the exact ABI used here.
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773
extern entity_s   *world_entity;                                             // map.cpp:62
extern entity_s    entityInsts;                                              // entity.cpp:299
extern eclass_t   *Eclass_ForName( int hasBrushes, const char *name );       // eclass.cpp:1096
extern entity_s   *Entity_Create( eclass_t *eclass );                        // entity.cpp:1629
extern void        Entity_Free( char *a1 );                                  // entity.cpp:1500
extern void        Entity_LinkBrush( brush_t *b, entity_s *world_ent );      // entity.cpp:445
extern void        Entity_UnlinkBrush( brush_t *b );                         // entity.cpp:464
extern selbrush_t *Entity_LinkBrush_0_extern( entity_s *e, entity_brush_s *b );// brush.cpp:635
extern void        Brush_BuildWindings( brush_t *def, int bFull );           // brush.cpp:1434
extern void        SetupVertexSelection();                                   // engine_stubs.cpp
extern void        MarkMapModified();                                        // win_qe3.cpp
extern void        sub_476330( selbrush_t *b );                              // brush.cpp:851; Brush_Deselect_Helper
extern void        sub_476470( selbrush_t *b );                              // brush.cpp:970; Brush_Select_Helper
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:212
extern char       *ValueForKey2( const entity_s *e, const char *key );                   // entity.cpp:89; "" when absent
extern void        Undo_ClearRedo();                                         // undo.cpp:176
extern void        Undo_GeneralStart( const char *operation );               // undo.cpp:367
extern void        Undo_AddBrush( entity_brush_s *pBrushInst );              // undo.cpp:494; takes brush def
extern void        Undo_AddBrushList( selbrush_t *sb );                      // undo.cpp:551
extern void        Undo_AddEntity_W( entity_s *a1 );                         // undo.cpp:633
extern void        Undo_SetIdForEntity( entity_s_def *ent );                 // undo.cpp:663
extern void        Undo_End();                                               // undo.cpp:686
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
// Live dockspace id used to re-dock a newly opened panel (imgui_shell.cpp:476).
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp

// selected_brushes is passed only to Undo_AddBrushList, matching xywnd.cpp:3402.

namespace
{
// Row model
// Equal-height rows are flattened from live state each frame so ImGuiListClipper
// can skip off-screen entries; section headers therefore use ordinary rows too.
enum koutKind_t
{
    KOUT_SECTION_BRUSHES = 0,   // worldspawn brushes + func_group folders
    KOUT_SECTION_TERRAIN,       // worldspawn terrain meshes (PATCH_TERRAIN patches)
    KOUT_SECTION_CURVES,        // construction objects
    KOUT_SECTION_ENTITIES,      // ordinary brush and point entities
    KOUT_SECTION_LIGHTS,        // CLASS_LIGHT entities
    KOUT_SECTION_MODELS,        // model and prefab entities
    KOUT_SECTION_IMAGES,        // editor-only reference images
    KOUT_GROUP_ENTITY,          // a func_group folder
    KOUT_ENTITY,                // any other brush/point entity, shown by classname
    KOUT_BRUSH,                 // one brush instance
    KOUT_CON_GROUP,             // a construction-group folder
    KOUT_CON_OBJECT,            // one construction object
    KOUT_IMAGE_GROUP,           // an image-group folder (KIWI 2026-09-10)
    KOUT_IMAGE,                 // one reference image
};

struct koutRow_t
{
    koutKind_t  kind;
    int         indent;
    selbrush_t *inst;       // KOUT_BRUSH             — the INSTANCE node
    entity_s   *ent;        // KOUT_GROUP_ENTITY / KOUT_ENTITY — the INSTANCE node
    int         conIndex;   // KOUT_CON_OBJECT        — store index
    int         conGroup;   // KOUT_CON_GROUP         — group id
    int         imageIndex; // KOUT_IMAGE store index
    int         imageGroup; // KOUT_IMAGE_GROUP       — group id (also set on member rows)
    int         ordinal;    // the display number ("Brush 12", "Line 3")
    int         count;      // folders: how many children
    unsigned    key;        // collapse-set key; 0 = not collapsible
};

// numberId is stable for an entity's lifetime. Tags separate key namespaces and
// keep valid id 0 distinct from the non-collapsible sentinel.
inline unsigned EntKey       ( int numberId ) { return 0xD0000000u | (unsigned)numberId; }
inline unsigned ConGroupKey  ( int group    ) { return 0xE0000000u | (unsigned)group; }
inline unsigned ImageGroupKey( int group    ) { return 0xA0000000u | (unsigned)group; }
const unsigned KOUT_KEY_BRUSHES  = 0xF0000001u;
const unsigned KOUT_KEY_CURVES   = 0xF0000002u;
const unsigned KOUT_KEY_ENTITIES = 0xF0000003u;
const unsigned KOUT_KEY_LIGHTS   = 0xF0000004u;
const unsigned KOUT_KEY_MODELS   = 0xF0000005u;

const unsigned KOUT_KEY_IMAGES = 0xF0000006u;
const unsigned KOUT_KEY_TERRAIN = 0xF0000007u;

// Which of an entity's brushes a section lists: worldspawn splits its terrain meshes
// out of "Brushes" into "Terrain"; every other entity folder lists all of its own.
enum koutFilter_t { KOUT_FILTER_ALL = 0, KOUT_FILTER_NOT_TERRAIN, KOUT_FILTER_TERRAIN };

bool IsTerrainNode( const selbrush_t *b )
{
    return b && b->def && b->def->patch && ( b->def->patch->type & PATCH_TERRAIN ) != 0;
}

bool PassesFilter( const selbrush_t *b, koutFilter_t filter )
{
    if ( filter == KOUT_FILTER_ALL )
        return true;
    return IsTerrainNode( b ) == ( filter == KOUT_FILTER_TERRAIN );
}

const int KOUT_CLASS_LIGHT      = 0x01;
const int KOUT_CLASS_MODELCLASS = 0x08;
const int KOUT_CLASS_PREFAB     = 0x10;

// Ported selection and visibility values (select.cpp:5112, :4168, :4180).
const unsigned KOUT_BRUSHFLAG_SELECTED = 0x80u;
const unsigned KOUT_HIDDEN_BIT = 4u;

// Panel-local state
std::vector<unsigned>  s_collapsed;        // keys of the folders that are CLOSED
std::vector<koutRow_t> s_rows;             // rebuilt every frame
unsigned               s_lastSelGen = 0;   // for the auto-expand pass
unsigned               s_lastImageGen = 0;

// Row indices shift with the flatten. Brush anchors store identity; construction
// anchors retain a store-generation-scoped index.
struct koutAnchor_t
{
    koutKind_t  kind     = KOUT_BRUSH;
    selbrush_t *inst     = nullptr;
    int         conIndex = -1;
    int         imageIndex = -1;
    bool        valid    = false;
};
koutAnchor_t s_anchor;

// Inline rename targets use stable identities. Entities store targetname and
// construction rows use sidecar names. World brushes cannot be named safely
// because the map format has no persistent per-brush id; ordinals can reorder.
struct koutRename_t
{
    bool        active   = false;
    koutKind_t  kind     = KOUT_BRUSH;
    unsigned    key      = 0;        // folders (entity / construction group)
    int         conIndex = -1;       // KOUT_CON_OBJECT
    int         imageIndex = -1;     // KOUT_IMAGE
};
koutRename_t s_rename;
char     s_renameBuf[64] = { 0 };
bool     s_renameFocus  = false;

// Folder keys are stable; construction indices are valid only within one store
// generation, which commands cannot change while text input owns the keyboard.
bool RenamingRow( const koutRow_t &r )
{
    if ( !s_rename.active || s_rename.kind != r.kind )
        return false;
    if ( r.kind == KOUT_CON_OBJECT )
        return s_rename.conIndex == r.conIndex;
    if ( r.kind == KOUT_IMAGE )
        return s_rename.imageIndex == r.imageIndex;
    return s_rename.key != 0 && s_rename.key == r.key;
}

bool Renameable( koutKind_t k )
{
    return k == KOUT_GROUP_ENTITY || k == KOUT_ENTITY
        || k == KOUT_CON_GROUP    || k == KOUT_CON_OBJECT || k == KOUT_IMAGE
        || k == KOUT_IMAGE_GROUP;
}

// Seed the editor with the current label (kiwi_viewcube.cpp:502-508).
void BeginRename( const koutRow_t &r, const char *seed )
{
    s_rename.active   = true;
    s_rename.kind     = r.kind;
    s_rename.key      = r.key;
    s_rename.conIndex = r.conIndex;
    s_rename.imageIndex = r.imageIndex;
    s_renameFocus     = true;
    _snprintf( s_renameBuf, sizeof( s_renameBuf ), "%s", seed ? seed : "" );
    s_renameBuf[sizeof( s_renameBuf ) - 1] = '\0';
}

// Freeing an entity or reordering the store invalidates later flattened rows;
// stop drawing and rebuild next frame rather than dereference stale identities.
bool s_structural = false;

// Shift+drag paint selection is mutually exclusive with row drag-and-drop.
bool s_paintSelecting = false;

// Payload validation uses Sel_BrushLive for brush pointers and pairs construction
// indices with the store generation.
const char *KOUT_PAYLOAD = "KIWI_OUTLINER_ROW";
struct koutDrag_t
{
    int         kind;
    selbrush_t *inst;
    int         conIndex;
    unsigned    conGen;
    int         imageIndex;     // KOUT_IMAGE rows (KIWI 2026-09-10): paired with the
    unsigned    imageGen;       // image-store generation, like the construction pair
};

// Outliner MMB opacity drag (KIWI 2026-09-10, user: "allow opacity changes in the
// outliner for images only by MMB clicking and going left and right for each image or
// group section header"): press MMB on an image row, an image-group folder or the
// Images section header, then move left/right.  One picture, the group's members or
// every picture follow; the whole drag is one ref-image undo record (SettleEdit).
struct koutOpacityDrag_t
{
    bool  active = false;
    int   imageIndex = -1;      // one picture, else
    int   imageGroup = -1;      // its members, else every picture
    float lastX = 0.0f;
    float shown = -1.0f;        // the opacity last reported, for the tooltip
};
koutOpacityDrag_t s_opacityDrag;
const float KOUT_OPACITY_PER_PIXEL = 0.005f;   // 200 px = the full 0..1 range

// Small helpers
bool Collapsed( unsigned key )
{
    if ( !key )
        return false;
    for ( size_t i = 0; i < s_collapsed.size(); ++i )
        if ( s_collapsed[i] == key )
            return true;
    return false;
}

void SetCollapsed( unsigned key, bool closed )
{
    if ( !key )
        return;
    for ( size_t i = 0; i < s_collapsed.size(); ++i )
    {
        if ( s_collapsed[i] != key )
            continue;
        if ( !closed )
            s_collapsed.erase( s_collapsed.begin() + i );
        return;
    }
    if ( closed )
        s_collapsed.push_back( key );
}

entity_s_def *DefOf( entity_s *inst )
{
    return inst ? (entity_s_def *)inst->def : 0;
}

const char *ClassOf( entity_s *inst )
{
    entity_s_def *def = DefOf( inst );
    if ( def && def->eclass && def->eclass->name )
        return def->eclass->name;
    return "";
}

bool IsFuncGroup( entity_s *inst )
{
    const char *cn = ClassOf( inst );
    return cn[0] && !_stricmp( cn, "func_group" );
}

// Centralized so every section uses the same entity classification.
koutKind_t EntitySection( entity_s *inst )
{
    if ( IsFuncGroup( inst ) )
        return KOUT_SECTION_BRUSHES;

    entity_s_def *def = DefOf( inst );
    eclass_t *ec = def ? def->eclass : 0;
    const int classType = ec ? ec->classtype : 0;
    if ( ( classType & KOUT_CLASS_LIGHT ) != 0 )
        return KOUT_SECTION_LIGHTS;

    if ( ( classType & ( KOUT_CLASS_MODELCLASS | KOUT_CLASS_PREFAB ) ) != 0 )
        return KOUT_SECTION_MODELS;

    const char *cn = ClassOf( inst );
    const bool namedModel = !_stricmp( cn, "misc_model" )
                         || !_stricmp( cn, "misc_prefab" );
    if ( namedModel )
        return KOUT_SECTION_MODELS;

    if ( !_stricmp( cn, "script_model" ) && def )
    {
        const char *model = ValueForKey2( def, "model" );
        if ( model && model[0] )
            return KOUT_SECTION_MODELS;
    }

    return KOUT_SECTION_ENTITIES;
}

unsigned SectionKey( koutKind_t kind )
{
    switch ( kind )
    {
    case KOUT_SECTION_BRUSHES:  return KOUT_KEY_BRUSHES;
    case KOUT_SECTION_TERRAIN:  return KOUT_KEY_TERRAIN;
    case KOUT_SECTION_CURVES:   return KOUT_KEY_CURVES;
    case KOUT_SECTION_ENTITIES: return KOUT_KEY_ENTITIES;
    case KOUT_SECTION_LIGHTS:   return KOUT_KEY_LIGHTS;
    case KOUT_SECTION_MODELS:   return KOUT_KEY_MODELS;
    case KOUT_SECTION_IMAGES:   return KOUT_KEY_IMAGES;
    default:                    return 0;
    }
}

// targetname with Plasticity's "<class> <id>" fallback (Outliner.tsx:158).
void FolderName( entity_s *inst, char *out, int outSize )
{
    entity_s_def *def = DefOf( inst );
    const char   *tn  = def ? ValueForKey2( def, "targetname" ) : "";
    if ( tn && tn[0] )
    {
        _snprintf( out, (size_t)outSize, "%s", tn );
    }
    else if ( IsFuncGroup( inst ) )
    {
        _snprintf( out, (size_t)outSize, "Group %i", def ? def->numberId : 0 );
    }
    else
    {
        _snprintf( out, (size_t)outSize, "%s %i", ClassOf( inst ), def ? def->numberId : 0 );
    }
    out[outSize - 1] = '\0';
}

const char *ConTypeName( kconType_t t )
{
    switch ( t )
    {
    case KCON_LINE:     return "Line";
    case KCON_POLYLINE: return "Polyline";
    case KCON_RECT:     return "Rect";
    case KCON_CIRCLE:   return "Circle";
    case KCON_ARC:      return "Arc";
    default:            return "Curve";
    }
}

int EntityBrushCount( entity_s *inst, koutFilter_t filter = KOUT_FILTER_ALL )
{
    int n = 0;
    if ( !inst )
        return 0;
    for ( selbrush_t *b = inst->brushes.ownerNext; b && b != &inst->brushes; b = b->ownerNext )
        if ( PassesFilter( b, filter ) )
            ++n;
    return n;
}

// Patch meshes retain their visible type even though the map stores them as brushes.
void BrushName( selbrush_t *inst, int ordinal, char *out, int outSize )
{
    const bool isPatch = ( inst && inst->def && inst->def->patch != 0 );
    const char *what = IsTerrainNode( inst ) ? "Terrain" : ( isPatch ? "Patch" : "Brush" );
    _snprintf( out, (size_t)outSize, "%s %i", what, ordinal );
    out[outSize - 1] = '\0';
}

const char *ImageBaseName( const std::string &file )
{
    const char *base = file.c_str();
    for ( const char *p = base; *p; ++p )
        if ( *p == '/' || *p == '\\' ) base = p + 1;
    return base;
}

const char *ImageAxisName( int axis )
{
    return axis == 2 ? "XY" : ( axis == 1 ? "XZ" : "YZ" );
}

bool BrushHidden( const selbrush_t *b )
{
    return b && ( ( (unsigned)b->brushFlags & KOUT_HIDDEN_BIT ) != 0 );
}

// Use the shared writer so the eye and H-family update the same hide state;
// callers bracket visibility undo around the whole gesture.
void SetBrushHidden( selbrush_t *b, bool hidden )
{
    KiwiVis_SetHidden( b, hidden );
}

bool BrushSelected( selbrush_t *b )
{
    // Query live typed selection; the panel owns no selection cache.
    return b && Sel_Contains( KiwiSel(), Sel_MakeObject( b ) );
}

// Flatten (FlattenOutline.ts:13)
void PushRow( koutKind_t kind, int indent, unsigned key )
{
    koutRow_t r;
    r.kind     = kind;
    r.indent   = indent;
    r.inst     = 0;
    r.ent      = 0;
    r.conIndex = -1;
    r.conGroup = -1;
    r.imageIndex = -1;
    r.imageGroup = -1;
    r.ordinal  = 0;
    r.count    = 0;
    r.key      = key;
    s_rows.push_back( r );
}

void FlattenEntityBrushes( entity_s *inst, int indent, koutFilter_t filter = KOUT_FILTER_ALL )
{
    int ordinal = 0;
    for ( selbrush_t *b = inst->brushes.ownerNext; b && b != &inst->brushes; b = b->ownerNext )
    {
        if ( !PassesFilter( b, filter ) )
            continue;
        ++ordinal;
        PushRow( KOUT_BRUSH, indent, 0 );
        s_rows.back().inst    = b;
        s_rows.back().ent     = inst;
        s_rows.back().ordinal = ordinal;
    }
}

void FlattenEntitySection( koutKind_t sectionKind )
{
    const unsigned sectionKey = SectionKey( sectionKind );
    PushRow( sectionKind, 0, sectionKey );
    const size_t sectionRow = s_rows.size() - 1;

    int count = 0;
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        if ( e != world_entity && EntitySection( e ) == sectionKind )
            ++count;
    }
    if ( sectionKind == KOUT_SECTION_BRUSHES && world_entity )
        count += EntityBrushCount( world_entity, KOUT_FILTER_NOT_TERRAIN );
    s_rows[sectionRow].count = count;

    if ( Collapsed( sectionKey ) )
        return;

    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        if ( e == world_entity || EntitySection( e ) != sectionKind )
            continue;

        entity_s_def *def = DefOf( e );
        const unsigned key = def ? EntKey( def->numberId ) : 0u;
        PushRow( IsFuncGroup( e ) ? KOUT_GROUP_ENTITY : KOUT_ENTITY, 1, key );
        s_rows.back().ent   = e;
        s_rows.back().count = EntityBrushCount( e );
        if ( !Collapsed( key ) )
            FlattenEntityBrushes( e, 2 );
    }

    if ( sectionKind == KOUT_SECTION_BRUSHES && world_entity )
        FlattenEntityBrushes( world_entity, 1, KOUT_FILTER_NOT_TERRAIN );
}

// Worldspawn's terrain meshes (Terrain Sculpt chunks, Terrain-dialog patches) get
// their own section; terrain inside a group or entity stays in that folder.
void FlattenTerrain()
{
    PushRow( KOUT_SECTION_TERRAIN, 0, KOUT_KEY_TERRAIN );
    s_rows.back().count = world_entity ? EntityBrushCount( world_entity, KOUT_FILTER_TERRAIN ) : 0;
    if ( Collapsed( KOUT_KEY_TERRAIN ) || !world_entity )
        return;
    FlattenEntityBrushes( world_entity, 1, KOUT_FILTER_TERRAIN );
}

void FlattenCurves()
{
    PushRow( KOUT_SECTION_CURVES, 0, KOUT_KEY_CURVES );
    const size_t curvesRow = s_rows.size() - 1;
    int curvesCount = KiwiCon_GroupCount();
    const int objectCount = KiwiCon_Count();
    for ( int i = 0; i < objectCount; ++i )
        if ( KiwiCon_Group( i ) < 0 )
            ++curvesCount;
    s_rows[curvesRow].count = curvesCount;

    if ( !Collapsed( KOUT_KEY_CURVES ) )
    {
        const int nGroups = KiwiCon_GroupCount();
        for ( int g = 0; g < nGroups; ++g )
        {
            const int      gid = KiwiCon_GroupIdAt( g );
            const unsigned key = ConGroupKey( gid );
            PushRow( KOUT_CON_GROUP, 1, key );
            s_rows.back().conGroup = gid;
            s_rows.back().count    = KiwiCon_GroupMemberCount( gid );

            if ( Collapsed( key ) )
                continue;
            int ordinal = 0;
            const int n = KiwiCon_Count();
            for ( int i = 0; i < n; ++i )
            {
                if ( KiwiCon_Group( i ) != gid )
                    continue;
                ++ordinal;
                PushRow( KOUT_CON_OBJECT, 2, 0 );
                s_rows.back().conIndex = i;
                s_rows.back().conGroup = gid;
                s_rows.back().ordinal  = ordinal;
            }
        }

        int ordinal = 0;
        const int n = KiwiCon_Count();
        for ( int i = 0; i < n; ++i )
        {
            if ( KiwiCon_Group( i ) >= 0 )
                continue;
            ++ordinal;
            PushRow( KOUT_CON_OBJECT, 1, 0 );
            s_rows.back().conIndex = i;
            s_rows.back().ordinal  = ordinal;
        }
    }
}

void FlattenImages()
{
    PushRow( KOUT_SECTION_IMAGES, 0, KOUT_KEY_IMAGES );
    s_rows.back().count = KiwiRefImage_Count();
    if ( Collapsed( KOUT_KEY_IMAGES ) )
        return;
    // Groups first (like the Curves section), then the ungrouped pictures.
    const int nGroups = KiwiRefImage_GroupCount();
    for ( int g = 0; g < nGroups; ++g )
    {
        const int      gid = KiwiRefImage_GroupIdAt( g );
        const unsigned key = ImageGroupKey( gid );
        PushRow( KOUT_IMAGE_GROUP, 1, key );
        s_rows.back().imageGroup = gid;
        s_rows.back().count      = KiwiRefImage_GroupMemberCount( gid );
        if ( Collapsed( key ) )
            continue;
        int ordinal = 0;
        for ( int i = 0; i < KiwiRefImage_Count(); ++i )
        {
            if ( KiwiRefImage_Group( i ) != gid )
                continue;
            PushRow( KOUT_IMAGE, 2, 0 );
            s_rows.back().imageIndex = i;
            s_rows.back().imageGroup = gid;
            s_rows.back().ordinal    = ++ordinal;
        }
    }
    int ordinal = 0;
    for ( int i = 0; i < KiwiRefImage_Count(); ++i )
    {
        if ( KiwiRefImage_Group( i ) >= 0 )
            continue;
        PushRow( KOUT_IMAGE, 1, 0 );
        s_rows.back().imageIndex = i;
        s_rows.back().ordinal = ++ordinal;
    }
}

// Divergence from Plasticity: KIWI emits seven fixed top-level sections, including
// empty ones; Plasticity emits only non-empty type sections within each group.
void Flatten()
{
    s_rows.clear();
    FlattenEntitySection( KOUT_SECTION_BRUSHES );
    FlattenTerrain();
    FlattenCurves();
    FlattenEntitySection( KOUT_SECTION_ENTITIES );
    FlattenEntitySection( KOUT_SECTION_LIGHTS );
    FlattenEntitySection( KOUT_SECTION_MODELS );
    FlattenImages();
}

// Expand brush ancestors after the typed brush-selection generation changes
// (Outliner.tsx:89-100).
void AutoExpandForSelection()
{
    // Fold pending legacy changes in before reading the generation they may bump.
    KiwiSel();
    const unsigned gen = Sel_Generation();
    if ( gen == s_lastSelGen )
        return;
    s_lastSelGen = gen;

    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        bool any = false, anyTerrain = false, anyOther = false;
        for ( selbrush_t *b = e->brushes.ownerNext; b && b != &e->brushes; b = b->ownerNext )
        {
            if ( BrushSelected( b ) )
            {
                any = true;
                if ( IsTerrainNode( b ) ) anyTerrain = true;
                else                      anyOther   = true;
                if ( anyTerrain && anyOther )
                    break;
            }
        }
        if ( !any )
            continue;

        if ( e == world_entity )
        {
            // Worldspawn lists its terrain under "Terrain", the rest under "Brushes".
            // KIWI (2026-09-10, user: "when clicking on a terrain piece, do NOT expand the
            // terrain group"): a selected terrain chunk never opens the Terrain section -
            // it holds hundreds of sculpt chunks and stays closed until opened by hand.
            (void)anyTerrain;
            if ( anyOther )   SetCollapsed( KOUT_KEY_BRUSHES, false );
            continue;
        }

        SetCollapsed( SectionKey( EntitySection( e ) ), false );
        entity_s_def *def = DefOf( e );
        if ( def )
            SetCollapsed( EntKey( def->numberId ), false );
    }
}

void ObserveImageGeneration()
{
    const unsigned gen = KiwiRefImage_Generation();
    if ( gen == s_lastImageGen )
        return;
    s_lastImageGen = gen;
    if ( KiwiRefImage_SelectedCount() > 0 )
    {
        SetCollapsed( KOUT_KEY_IMAGES, false );
        // A selected picture opens its own group folder too.
        for ( int i = 0; i < KiwiRefImage_SelectedCount(); ++i )
        {
            const int g = KiwiRefImage_Group( KiwiRefImage_SelectedAt( i ) );
            if ( g >= 0 )
                SetCollapsed( ImageGroupKey( g ), false );
        }
    }
    if ( s_anchor.kind == KOUT_IMAGE
      && ( s_anchor.imageIndex < 0 || s_anchor.imageIndex >= KiwiRefImage_Count() ) )
        s_anchor = koutAnchor_t();
    if ( s_rename.active && s_rename.kind == KOUT_IMAGE
      && ( s_rename.imageIndex < 0 || s_rename.imageIndex >= KiwiRefImage_Count() ) )
        s_rename = koutRename_t();
}

// Selection: outliner -> scene
// Rows use the viewport's Sel_* / Sel_SyncToLegacy and KiwiConSel_ApplyClick
// funnels; the panel owns no second selection state.
void SelectBrushRow( selbrush_t *b, bool additive, bool toggle )
{
    // Stale flattened rows fail audibly.
    if ( !b || !Sel_BrushLive( b ) )
    {
        Sys_Printf( "Outliner: that brush no longer exists — nothing selected.\n" );
        return;
    }
    selection_t &sel = KiwiSel();
    if ( !additive && !toggle )
    {
        Sel_Clear( sel );
        KiwiConSel_Clear();          // a plain click owns the WHOLE selection
    }
    const sel_item_t it = Sel_MakeObject( b );
    if ( toggle )
        Sel_Toggle( sel, it );
    else
        Sel_Add( sel, it );
    Sel_SyncToLegacy();
    g_nUpdateBits = -1;
}

void SelectConRow( int index, bool additive, bool toggle )
{
    // Stale flattened rows fail audibly.
    if ( index < 0 || index >= KiwiCon_Count() )
    {
        Sys_Printf( "Outliner: that curve row is stale (index %i of %i) — "
                    "nothing selected.\n", index, KiwiCon_Count() );
        return;
    }
    if ( !additive && !toggle )
    {
        // A plain curve click clears the brush half of the shared selection.
        Sel_Clear( KiwiSel() );
        Sel_SyncToLegacy();
    }
    kconSelItem_t it;
    it.object = index;
    it.kind   = KCONSEL_OBJECT;
    it.index  = -1;
    KiwiConSel_ApplyClick( it, additive, toggle );
    g_nUpdateBits = -1;
}

void SelectImageRow( int index, bool additive, bool toggle )
{
    if ( index < 0 || index >= KiwiRefImage_Count() )
    {
        Sys_Printf( "Outliner: that image row is stale (index %i of %i) - nothing selected.\n",
                    index, KiwiRefImage_Count() );
        return;
    }
    KiwiRefImage_ApplyClick( index, additive, toggle, false );
}

bool ConObjectRowSelected( int index )
{
    for ( int i = 0; i < KiwiConSel_Count(); ++i )
    {
        const kconSelItem_t *it = KiwiConSel_At( i );
        if ( it && it->object == index && it->kind == KCONSEL_OBJECT )
            return true;
    }
    return false;
}

// Shift ranges cover leaves in the current flatten; folders would implicitly add
// hidden descendants and change the gesture's meaning.
void SelectRange( int a, int b )
{
    if ( a > b )
    {
        const int t = a; a = b; b = t;
    }
    if ( a < 0 )
        a = 0;
    if ( b >= (int)s_rows.size() )
        b = (int)s_rows.size() - 1;

    selection_t &sel = KiwiSel();
    bool touchedBrush = false;
    for ( int i = a; i <= b; ++i )
    {
        const koutRow_t &r = s_rows[i];
        if ( r.kind == KOUT_BRUSH && r.inst && Sel_BrushLive( r.inst ) )
        {
            Sel_Add( sel, Sel_MakeObject( r.inst ) );
            touchedBrush = true;
        }
        else if ( r.kind == KOUT_CON_OBJECT && r.conIndex >= 0 )
        {
            kconSelItem_t it;
            it.object = r.conIndex;
            it.kind   = KCONSEL_OBJECT;
            it.index  = -1;
            KiwiConSel_ApplyClick( it, true, false );   // shift = add
        }
    }
    if ( touchedBrush )
        Sel_SyncToLegacy();
    g_nUpdateBits = -1;
}

int FindAnchorRow()
{
    if ( !s_anchor.valid )
        return -1;
    for ( size_t i = 0; i < s_rows.size(); ++i )
    {
        const koutRow_t &r = s_rows[i];
        if ( s_anchor.kind == KOUT_BRUSH && r.kind == KOUT_BRUSH && r.inst == s_anchor.inst )
            return (int)i;
        if ( s_anchor.kind == KOUT_CON_OBJECT && r.kind == KOUT_CON_OBJECT
          && r.conIndex == s_anchor.conIndex )
            return (int)i;
        if ( s_anchor.kind == KOUT_IMAGE && r.kind == KOUT_IMAGE
          && r.imageIndex == s_anchor.imageIndex )
            return (int)i;
    }
    return -1;
}

void SetAnchor( const koutRow_t &r )
{
    s_anchor.kind     = r.kind;
    s_anchor.inst     = r.inst;
    s_anchor.conIndex = r.conIndex;
    s_anchor.imageIndex = r.imageIndex;
    s_anchor.valid    = ( r.kind == KOUT_BRUSH || r.kind == KOUT_CON_OBJECT
                       || r.kind == KOUT_IMAGE );
}

// Brush reparenting
// Snapshot instances before mutating their owner chains. One undo bracket covers
// the move, with each Undo_AddBrush taken before its owner changes.
bool ReparentBrushes( std::vector<selbrush_t *> &insts, entity_s *targetInst, const char *op )
{
    if ( !targetInst )
    {
        Sys_Printf( "Outliner: the drop target no longer exists — move cancelled.\n" );
        return false;
    }
    entity_s_def *targetDef = DefOf( targetInst );
    if ( !targetDef )
    {
        Sys_Printf( "Outliner: the drop target has no definition — move cancelled.\n" );
        return false;
    }
    // A point entity's brush is its bounding box, not user geometry
    // (Entity_Create, entity.cpp:1640-1645).
    if ( targetDef->eclass && targetDef->eclass->fixedsize )
    {
        Sys_Printf( "Outliner: %s is a point entity — it cannot hold brushes.\n",
                    ClassOf( targetInst ) );
        return false;
    }

    std::vector<selbrush_t *> move;
    for ( size_t i = 0; i < insts.size(); ++i )
    {
        selbrush_t *sb = insts[i];
        if ( !sb || !Sel_BrushLive( sb ) )      // the liveness guard, before any deref
            continue;
        if ( sb->owner == targetInst )
            continue;
        move.push_back( sb );
    }
    if ( move.empty() )
    {
        // Report a valid no-op drop instead of appearing to ignore it.
        Sys_Printf( "Outliner: nothing to move — the %i dragged brush(es) are "
                    "already in %s.\n", (int)insts.size(), ClassOf( targetInst ) );
        return false;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( op );
    for ( size_t i = 0; i < move.size(); ++i )
        Undo_AddBrush( (entity_brush_s *)move[i]->def );     // BEFORE the move

    for ( size_t i = 0; i < move.size(); ++i )
    {
        selbrush_t *sb = move[i];
        // select.cpp:5112: selected instances leave bookkeeping across the relink.
        const bool wasSelected = ( ( (unsigned)sb->brushFlags & KOUT_BRUSHFLAG_SELECTED ) != 0 );
        if ( wasSelected )
            sub_476330( sb );

        brush_t *bDef = sb->def;
        Entity_UnlinkBrush( bDef );                          // entity.cpp:465
        Entity_LinkBrush( bDef, (entity_s *)targetDef );     // entity.cpp:437
        Entity_LinkBrush_0_extern( targetInst, sb );         // brush.cpp:633

        Brush_BuildWindings( bDef, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();                          // select.cpp:5130's gate
        MarkMapModified();
        ++bDef->version;

        if ( wasSelected )
            sub_476470( sb );
    }
    Undo_End();

    g_nUpdateBits = -1;
    Sel_InvalidateFromLegacy();     // the instances did not move, but their owners did
    s_structural = true;            // the owner chains the flatten walked have changed
    Sys_Printf( "Outliner: moved %i brush(es).\n", (int)move.size() );
    return true;
}

// The current SEL_OBJECT selection as instance pointers, live-guarded.
void GatherSelectedBrushes( std::vector<selbrush_t *> &out )
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        if ( sel.items[i].kind != SEL_OBJECT )
            continue;
        selbrush_t *b = sel.items[i].brush;
        if ( b && Sel_BrushLive( b ) )
            out.push_back( b );
    }
}

// Select_Ungroup for one entity (select.cpp:5106-5146). Undo_AddEntity_W snapshots
// entity and brushes; undo restores the entity before relinking its brushes.
bool UngroupEntity( entity_s *inst, const char *op )
{
    if ( !inst || inst == world_entity || !world_entity )
    {
        Sys_Printf( "Outliner: worldspawn is not a group — nothing to ungroup.\n" );
        return false;
    }
    entity_s_def *def = DefOf( inst );
    if ( !def || !def->eclass || def->eclass->fixedsize )
    {
        Sys_Printf( "Outliner: %s is a point entity — it holds no brushes to "
                    "ungroup.\n", ClassOf( inst ) );
        return false;
    }

    entity_s_def *worldDef = (entity_s_def *)world_entity->def;

    Undo_ClearRedo();
    Undo_GeneralStart( op );
    Undo_AddEntity_W( (entity_s *)def );

    int n = 0;
    for ( selbrush_t *b = inst->brushes.ownerNext; b && b != &inst->brushes; )
    {
        selbrush_t *next = b->ownerNext;                 // the relink rewrites b's links
        const bool wasSelected = ( ( (unsigned)b->brushFlags & KOUT_BRUSHFLAG_SELECTED ) != 0 );
        if ( wasSelected )
            sub_476330( b );

        brush_t *bDef = b->def;
        Entity_UnlinkBrush( bDef );
        Entity_LinkBrush( bDef, (entity_s *)worldDef );
        Entity_LinkBrush_0_extern( world_entity, b );

        Brush_BuildWindings( bDef, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++bDef->version;

        if ( wasSelected )
            sub_476470( b );
        ++n;
        b = next;
    }
    Entity_Free( (char *)inst );                          // select.cpp:5146
    Undo_End();
    s_structural = true;                                  // this frame's rows are stale

    g_nUpdateBits = -1;
    Sel_InvalidateFromLegacy();
    Sys_Printf( "Outliner: ungrouped %i brush(es).\n", n );
    return true;
}

// Section eyes: a category row's eye reads "everything in it is hidden" and one
// click applies the inverse to every member.  Members per section:
//   Brushes  = worldspawn's non-terrain brushes + every func_group's brushes
//   Terrain  = worldspawn's terrain meshes
//   Entities / Lights / Models = every brush of every entity classed there
//   Curves   = every construction object;  Images = every reference image.
bool SectionIsBrushBacked( koutKind_t k )
{
    return k == KOUT_SECTION_BRUSHES || k == KOUT_SECTION_TERRAIN || k == KOUT_SECTION_ENTITIES
        || k == KOUT_SECTION_LIGHTS  || k == KOUT_SECTION_MODELS;
}

// Walk the brushes a section lists.  `apply` < 0 counts (n / hidden), else sets hidden.
void SectionBrushes( koutKind_t k, int apply, int *n, int *nh )
{
    if ( n )  *n  = 0;
    if ( nh ) *nh = 0;
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        koutFilter_t filter = KOUT_FILTER_ALL;
        if ( e == world_entity )
        {
            if ( k == KOUT_SECTION_BRUSHES )      filter = KOUT_FILTER_NOT_TERRAIN;
            else if ( k == KOUT_SECTION_TERRAIN ) filter = KOUT_FILTER_TERRAIN;
            else                                  continue;
        }
        else if ( EntitySection( e ) != k )
            continue;
        for ( selbrush_t *b = e->brushes.ownerNext; b && b != &e->brushes; b = b->ownerNext )
        {
            if ( !PassesFilter( b, filter ) )
                continue;
            if ( apply < 0 )
            {
                if ( n )  ++*n;
                if ( nh && BrushHidden( b ) ) ++*nh;
            }
            else
                SetBrushHidden( b, apply != 0 );
        }
    }
}

void SectionEyeState( koutKind_t k, bool *hidden, bool *hasEye )
{
    int n = 0, nh = 0;
    if ( SectionIsBrushBacked( k ) )
        SectionBrushes( k, -1, &n, &nh );
    else if ( k == KOUT_SECTION_CURVES )
    {
        n = KiwiCon_Count();
        for ( int i = 0; i < n; ++i )
            if ( KiwiCon_Hidden( i ) ) ++nh;
    }
    else if ( k == KOUT_SECTION_IMAGES )
    {
        n = KiwiRefImage_Count();
        for ( int i = 0; i < n; ++i )
        {
            const krefImage_t *image = KiwiRefImage_At( i );
            if ( image && image->hidden ) ++nh;
        }
    }
    *hidden = ( n > 0 && nh == n );
    *hasEye = ( n > 0 );
}

void SectionSetHidden( koutKind_t k, bool want )
{
    if ( SectionIsBrushBacked( k ) )
    {
        // One visibility record covers the whole section.
        KiwiVis_UndoPush( "hide section (outliner)" );
        SectionBrushes( k, want ? 1 : 0, nullptr, nullptr );
        KiwiVis_UndoCommit();
    }
    else if ( k == KOUT_SECTION_CURVES )
    {
        KiwiCon_UndoPush();
        const int n = KiwiCon_Count();
        for ( int i = 0; i < n; ++i )
            KiwiCon_SetHidden( i, want );
    }
    else if ( k == KOUT_SECTION_IMAGES )
    {
        const int n = KiwiRefImage_Count();
        for ( int i = 0; i < n; ++i )
            KiwiRefImage_SetHidden( i, want );
    }
}

// Row drawing
// The shell has no icon atlas, so the eye is drawn directly. Two-argument
// PathStroke is unambiguous across the vendored ImGui signature swap (imgui.h:3551).
// Open = an almond outline with a filled iris, a darker pupil and a highlight;
// closed = the lower lid with three lashes.
void DrawEye( ImDrawList *dl, ImVec2 c, float r, bool hidden, ImU32 col )
{
    if ( !hidden )
    {
        // Almond: two arcs of radius 1.55r whose centres sit 0.9r above/below the
        // pupil; they meet 1.26r either side of it (sqrt(1.55^2 - 0.9^2)).
        dl->PathClear();
        dl->PathArcTo( ImVec2( c.x, c.y + r * 0.90f ), r * 1.55f, -2.52f, -0.62f, 14 );
        dl->PathArcTo( ImVec2( c.x, c.y - r * 0.90f ), r * 1.55f,  0.62f,  2.52f, 14 );
        dl->PathFillConvex( ( col & 0x00FFFFFFu ) | 0x30000000u );      // faint white
        dl->PathClear();
        dl->PathArcTo( ImVec2( c.x, c.y + r * 0.90f ), r * 1.55f, -2.52f, -0.62f, 14 );
        dl->PathArcTo( ImVec2( c.x, c.y - r * 0.90f ), r * 1.55f,  0.62f,  2.52f, 14 );
        dl->PathStroke( col, 1.2f, ImDrawFlags_Closed );   // vendored order: (col, thickness, flags)
        // Iris, pupil, glint.
        dl->AddCircleFilled( c, r * 0.58f, col, 12 );
        dl->AddCircleFilled( c, r * 0.28f, IM_COL32( 30, 30, 36, 255 ), 10 );
        dl->AddCircleFilled( ImVec2( c.x - r * 0.20f, c.y - r * 0.22f ), r * 0.14f,
                             IM_COL32( 255, 255, 255, 230 ), 8 );
    }
    else
    {
        // A lower lid and lashes remain legible as closed at this size.
        dl->PathClear();
        dl->PathArcTo( ImVec2( c.x, c.y - r * 1.15f ), r * 1.55f, 1.20f, 1.94f, 12 );
        dl->PathStroke( col, 1.3f );
        dl->AddLine( ImVec2( c.x - r * 0.75f, c.y + r * 0.25f ),
                     ImVec2( c.x - r * 1.00f, c.y + r * 0.70f ), col, 1.1f );
        dl->AddLine( ImVec2( c.x,             c.y + r * 0.45f ),
                     ImVec2( c.x,             c.y + r * 0.95f ), col, 1.1f );
        dl->AddLine( ImVec2( c.x + r * 0.75f, c.y + r * 0.25f ),
                     ImVec2( c.x + r * 1.00f, c.y + r * 0.70f ), col, 1.1f );
    }
}

void DrawArrow( ImDrawList *dl, ImVec2 c, float r, bool open, ImU32 col )
{
    if ( open )
        dl->AddTriangleFilled( ImVec2( c.x - r, c.y - r * 0.55f ),
                               ImVec2( c.x + r, c.y - r * 0.55f ),
                               ImVec2( c.x,     c.y + r * 0.70f ), col );
    else
        dl->AddTriangleFilled( ImVec2( c.x - r * 0.55f, c.y - r ),
                               ImVec2( c.x - r * 0.55f, c.y + r ),
                               ImVec2( c.x + r * 0.70f, c.y     ), col );
}

// Drop-target classification.
bool RowAcceptsBrushes( const koutRow_t &r )
{
    return r.kind == KOUT_SECTION_BRUSHES      // -> worldspawn (ungroup)
        || r.kind == KOUT_SECTION_TERRAIN      // -> worldspawn as well
        || r.kind == KOUT_GROUP_ENTITY
        || r.kind == KOUT_ENTITY;
}

bool RowAcceptsCurves( const koutRow_t &r )
{
    return r.kind == KOUT_SECTION_CURVES       // -> ungrouped
        || r.kind == KOUT_CON_GROUP;
}

bool RowAcceptsImages( const koutRow_t &r )
{
    return r.kind == KOUT_SECTION_IMAGES       // -> ungrouped
        || r.kind == KOUT_IMAGE_GROUP;
}

void ApplyDrop( const koutDrag_t &drag, const koutRow_t &target )
{
    if ( drag.kind == KOUT_IMAGE )
    {
        if ( !RowAcceptsImages( target ) )
            return;
        if ( drag.imageGen != KiwiRefImage_Generation() )
        {
            Sys_Printf( "Outliner: the image store changed — drop cancelled.\n" );
            return;
        }
        if ( drag.imageIndex < 0 || drag.imageIndex >= KiwiRefImage_Count() )
        {
            Sys_Printf( "Outliner: the dragged image is gone (index %i of %i) — "
                        "drop cancelled.\n", drag.imageIndex, KiwiRefImage_Count() );
            return;
        }
        const int gid = ( target.kind == KOUT_SECTION_IMAGES ) ? -1 : target.imageGroup;
        // Dragging a selected row moves the whole image selection.
        std::vector<int> moving;
        if ( KiwiRefImage_IsSelected( drag.imageIndex ) )
            for ( int i = 0; i < KiwiRefImage_SelectedCount(); ++i )
                moving.push_back( KiwiRefImage_SelectedAt( i ) );
        if ( moving.empty() )
            moving.push_back( drag.imageIndex );
        int moved = 0;
        for ( size_t i = 0; i < moving.size(); ++i )
            if ( KiwiRefImage_SetGroup( moving[i], gid ) )
                ++moved;
        s_structural = true;
        Sys_Printf( "Outliner: moved %i image(s)%s.\n", moved,
                    gid >= 0 ? " into the group" : " out of their group" );
        return;
    }

    if ( drag.kind == KOUT_BRUSH )
    {
        if ( !RowAcceptsBrushes( target ) )
            return;
        if ( !drag.inst || !Sel_BrushLive( drag.inst ) )   // the guard, before any deref
        {
            Sys_Printf( "Outliner: the dragged brush no longer exists.\n" );
            return;
        }
        // Dragging a selected row moves the whole current selection.
        std::vector<selbrush_t *> moving;
        if ( BrushSelected( drag.inst ) )
            GatherSelectedBrushes( moving );
        if ( moving.empty() )
            moving.push_back( drag.inst );

        const bool toWorld = target.kind == KOUT_SECTION_BRUSHES || target.kind == KOUT_SECTION_TERRAIN;
        entity_s *targetInst = toWorld ? world_entity : target.ent;
        ReparentBrushes( moving, targetInst, toWorld ? "outliner ungroup" : "outliner group" );
        return;
    }

    if ( drag.kind == KOUT_CON_OBJECT )
    {
        if ( !RowAcceptsCurves( target ) )
            return;
        // Refuse indices from another store generation; they may name new objects.
        if ( drag.conGen != KiwiCon_Generation() )
        {
            Sys_Printf( "Outliner: the construction store changed — drop cancelled.\n" );
            return;
        }
        if ( drag.conIndex < 0 || drag.conIndex >= KiwiCon_Count() )
        {
            Sys_Printf( "Outliner: the dragged curve is gone (index %i of %i) — "
                        "drop cancelled.\n", drag.conIndex, KiwiCon_Count() );
            return;
        }

        const int gid = ( target.kind == KOUT_SECTION_CURVES ) ? -1 : target.conGroup;

        std::vector<int> moving;
        if ( KiwiConSel_ObjectSelected( drag.conIndex ) )
        {
            for ( int i = 0; i < KiwiConSel_Count(); ++i )
            {
                const kconSelItem_t *it = KiwiConSel_At( i );
                if ( it && it->object >= 0 && it->object < KiwiCon_Count() )
                    moving.push_back( it->object );
            }
        }
        if ( moving.empty() )
            moving.push_back( drag.conIndex );

        KiwiCon_UndoPush();                    // ONE store snapshot for the whole move
        for ( size_t i = 0; i < moving.size(); ++i )
            KiwiCon_SetGroup( moving[i], gid );
        s_structural = true;
        Sys_Printf( "Outliner: moved %i construction object(s).\n", (int)moving.size() );
    }
}

} // namespace

// Collapse identities are document-local. Reset arms a post-sidecar-load pass
// that closes folders while leaving top-level sections open.
static bool s_collapseFoldersPending = false;

static void CollapseFoldersOnly()
{
    // KIWI (2026-09-10, user): the Terrain section is ALWAYS collapsed when a map loads —
    // the only top-level section that starts closed (its rows are hundreds of sculpt chunks).
    SetCollapsed( KOUT_KEY_TERRAIN, true );
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        if ( e == world_entity )
            continue;
        entity_s_def *def = DefOf( e );
        if ( def )
            SetCollapsed( EntKey( def->numberId ), true );
    }
    const int nGroups = KiwiCon_GroupCount();
    for ( int g = 0; g < nGroups; ++g )
        SetCollapsed( ConGroupKey( KiwiCon_GroupIdAt( g ) ), true );
    const int nImageGroups = KiwiRefImage_GroupCount();
    for ( int g = 0; g < nImageGroups; ++g )
        SetCollapsed( ImageGroupKey( KiwiRefImage_GroupIdAt( g ) ), true );
}

void KiwiOutliner_ResetForNewMap()
{
    s_collapsed.clear();
    SetCollapsed( KOUT_KEY_TERRAIN, true );   // closed from the first frame (KIWI 2026-09-10)
    s_collapseFoldersPending = true;
    s_rows.clear();
    s_lastSelGen = 0;
    s_lastImageGen = 0;
    s_anchor = koutAnchor_t();
    s_rename = koutRename_t();
    s_renameBuf[0] = '\0';
    s_renameFocus = false;
    s_structural = false;
    s_paintSelecting = false;
}

// Panel
void KiwiOutliner_Draw()
{
    // Clear before the early-out so closing the panel cannot retain viewport hover.
    KiwiHover_OutlinerClear();

    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_OUTLINER );
    if ( !open || !*open )
        return;                              // closed: no Begin, no End, no cost

    if ( KiwiWindows_JustOpened( KIWI_WIN_OUTLINER ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_OUTLINER ), open ) )
    {
        // KIWI adds Ungroup beside Plasticity's New Group action. Both run before
        // flattening because they can restructure rows.
        ImGui::TextUnformatted( "Scene" );
        ImGui::SameLine();
        {
            const bool can = KiwiOutliner_CanGroup();
            ImGui::BeginDisabled( !can );
            if ( ImGui::SmallButton( "New Group" ) )
                KiwiOutliner_GroupSelection();
            ImGui::EndDisabled();
            if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                ImGui::SetTooltip( "Create a group from the selection\n"
                                   "(brushes -> a func_group entity)" );
        }
        ImGui::SameLine();
        {
            const bool can = KiwiOutliner_CanUngroup();
            ImGui::BeginDisabled( !can );
            if ( ImGui::SmallButton( "Ungroup" ) )
                KiwiOutliner_UngroupSelection();
            ImGui::EndDisabled();
        }
        ImGui::Separator();

        s_structural = false;
        AutoExpandForSelection();
        ObserveImageGeneration();
        if ( s_collapseFoldersPending )
        {
            s_collapseFoldersPending = false;
            CollapseFoldersOnly();
        }
        Flatten();

        ImGuiIO &io = ImGui::GetIO();
        // Clear the shift-drag latch before rows process this frame's press.
        if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            s_paintSelecting = false;

        // An MMB opacity drag in flight follows the mouse wherever it goes; release
        // settles it into one undo record.  Rows below only START a drag.
        if ( s_opacityDrag.active )
        {
            if ( ImGui::IsMouseDown( ImGuiMouseButton_Middle ) )
            {
                const float x  = io.MousePos.x;
                const float dx = x - s_opacityDrag.lastX;
                s_opacityDrag.lastX = x;
                if ( dx != 0.0f )
                {
                    const float shown = KiwiRefImage_NudgeOpacity( s_opacityDrag.imageIndex,
                                                                    s_opacityDrag.imageGroup,
                                                                    dx * KOUT_OPACITY_PER_PIXEL );
                    if ( shown >= 0.0f )
                        s_opacityDrag.shown = shown;
                }
                if ( s_opacityDrag.shown >= 0.0f )
                    ImGui::SetTooltip( "Opacity %.2f  (MMB drag left / right)", (double)s_opacityDrag.shown );
            }
            else
            {
                KiwiRefImage_SettleEdit();
                s_opacityDrag = koutOpacityDrag_t();
            }
        }

        // Items use itemH, but the clipper needs item height plus ItemSpacing.y;
        // otherwise hit rows drift by one spacing per entry.
        const float itemH   = ImGui::GetTextLineHeight();
        const float rowH    = ImGui::GetTextLineHeightWithSpacing();
        // Derive glyph centers from gutter widths so layout changes stay centered.
        const float indentW    = 14.0f;   // per indent level
        const float eyeW       = 18.0f;   // the eye column's hit width
        const float eyeCx      = eyeW * 0.5f;
        const float arrowW     = indentW; // the disclosure arrow shares the indent step
        const float arrowCx    = arrowW * 0.5f;
        const float glyphR     = 4.0f;    // eye + arrow glyph radius
        const float colGapX    = 2.0f;    // arrow/spacer -> label

        ImGui::BeginChild( "##outlinerrows", ImVec2( 0, 0 ), 0,
                           ImGuiWindowFlags_HorizontalScrollbar );

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 dim  = ImGui::GetColorU32( ImGuiCol_TextDisabled );
        const ImU32 lit  = ImGui::GetColorU32( ImGuiCol_Text );

        // Large maps still submit only the visible rows.
        ImGuiListClipper clipper;
        clipper.Begin( (int)s_rows.size(), rowH );
        while ( clipper.Step() )
        {
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
            {
                if ( i < 0 || i >= (int)s_rows.size() )
                    continue;
                const koutRow_t r = s_rows[i];       // isolate against mid-draw re-entry
                // Row indices shift on collapse. Widget ids use folder keys, brush
                // pointers, or tagged construction indices to preserve ImGui state.
                if ( r.key != 0 )
                    ImGui::PushID( (int)r.key );
                else if ( r.kind == KOUT_BRUSH && r.inst )
                    ImGui::PushID( (const void *)r.inst );
                else if ( r.kind == KOUT_CON_OBJECT )
                    ImGui::PushID( (int)( 0xC0000000u | (unsigned)r.conIndex ) );
                else if ( r.kind == KOUT_IMAGE )
                    ImGui::PushID( (int)( 0xB0000000u | (unsigned)r.imageIndex ) );
                else
                    ImGui::PushID( i );

                // Eye
                bool hidden = false;
                bool hasEye = false;
                switch ( r.kind )
                {
                case KOUT_BRUSH:
                    hidden = BrushHidden( r.inst );
                    hasEye = true;
                    break;
                case KOUT_CON_OBJECT:
                    hidden = KiwiCon_Hidden( r.conIndex );
                    hasEye = true;
                    break;
                case KOUT_IMAGE:
                {
                    const krefImage_t *image = KiwiRefImage_At( r.imageIndex );
                    hidden = image ? image->hidden : false;
                    hasEye = image != nullptr;
                    break;
                }
                case KOUT_IMAGE_GROUP:
                    hidden = KiwiRefImage_GroupAllHidden( r.imageGroup );
                    hasEye = KiwiRefImage_GroupMemberCount( r.imageGroup ) > 0;
                    break;
                case KOUT_GROUP_ENTITY:
                case KOUT_ENTITY:
                {
                    // Folder eye state is "all hidden"; one click applies its inverse.
                    int n = 0, nh = 0;
                    for ( selbrush_t *b = r.ent->brushes.ownerNext;
                          b && b != &r.ent->brushes; b = b->ownerNext )
                    {
                        ++n;
                        if ( BrushHidden( b ) )
                            ++nh;
                    }
                    hidden = ( n > 0 && nh == n );
                    hasEye = ( n > 0 );
                    break;
                }
                case KOUT_CON_GROUP:
                {
                    int n = 0, nh = 0;
                    const int cnt = KiwiCon_Count();
                    for ( int k = 0; k < cnt; ++k )
                    {
                        if ( KiwiCon_Group( k ) != r.conGroup )
                            continue;
                        ++n;
                        if ( KiwiCon_Hidden( k ) )
                            ++nh;
                    }
                    hidden = ( n > 0 && nh == n );
                    hasEye = ( n > 0 );
                    break;
                }
                case KOUT_SECTION_BRUSHES:
                case KOUT_SECTION_TERRAIN:
                case KOUT_SECTION_CURVES:
                case KOUT_SECTION_ENTITIES:
                case KOUT_SECTION_LIGHTS:
                case KOUT_SECTION_MODELS:
                case KOUT_SECTION_IMAGES:
                    SectionEyeState( r.kind, &hidden, &hasEye );
                    break;
                default:
                    break;
                }

                const ImVec2 eyePos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton( "##eye", ImVec2( eyeW, itemH ) );
                const bool eyeHover = ImGui::IsItemHovered();
                if ( hasEye )
                {
                    DrawEye( dl, ImVec2( eyePos.x + eyeCx, eyePos.y + itemH * 0.5f ),
                             glyphR, hidden, ( hidden || !eyeHover ) ? dim : lit );
                    if ( ImGui::IsItemClicked( ImGuiMouseButton_Left ) )
                    {
                        const bool want = !hidden;
                        if ( r.kind == KOUT_BRUSH )
                        {
                            // One visibility record per click.
                            KiwiVis_UndoPush( "hide (outliner)" );
                            SetBrushHidden( r.inst, want );
                            KiwiVis_UndoCommit();      // commit the captured gesture
                        }
                        else if ( r.kind == KOUT_CON_OBJECT )
                        {
                            KiwiCon_UndoPush();
                            KiwiCon_SetHidden( r.conIndex, want );
                        }
                        else if ( r.kind == KOUT_GROUP_ENTITY || r.kind == KOUT_ENTITY )
                        {
                            // One visibility record covers every child brush.
                            KiwiVis_UndoPush( "hide group (outliner)" );
                            for ( selbrush_t *b = r.ent->brushes.ownerNext;
                                  b && b != &r.ent->brushes; b = b->ownerNext )
                                SetBrushHidden( b, want );
                            KiwiVis_UndoCommit();      // commit the captured gesture
                        }
                        else if ( r.kind == KOUT_CON_GROUP )
                        {
                            KiwiCon_UndoPush();
                            const int cnt = KiwiCon_Count();
                            for ( int k = 0; k < cnt; ++k )
                                if ( KiwiCon_Group( k ) == r.conGroup )
                                    KiwiCon_SetHidden( k, want );
                        }
                        else if ( r.kind == KOUT_IMAGE )
                        {
                            KiwiRefImage_SetHidden( r.imageIndex, want );
                        }
                        else if ( r.kind == KOUT_IMAGE_GROUP )
                        {
                            KiwiRefImage_SetGroupHidden( r.imageGroup, want );   // one record
                        }
                        else if ( r.key >= KOUT_KEY_BRUSHES )      // a category row
                        {
                            SectionSetHidden( r.kind, want );
                            g_nUpdateBits = -1;
                        }
                    }
                }
                ImGui::SameLine( 0.0f, 0.0f );

                // Indent and disclosure arrow
                if ( r.indent > 0 )
                {
                    ImGui::Dummy( ImVec2( indentW * (float)r.indent, itemH ) );
                    ImGui::SameLine( 0.0f, 0.0f );
                }

                const bool isFolder = ( r.key != 0 );
                if ( isFolder )
                {
                    const ImVec2 ap = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton( "##arrow", ImVec2( arrowW, itemH ) );
                    DrawArrow( dl, ImVec2( ap.x + arrowCx, ap.y + itemH * 0.5f ), glyphR,
                               !Collapsed( r.key ), dim );
                    if ( ImGui::IsItemClicked( ImGuiMouseButton_Left ) )
                        SetCollapsed( r.key, !Collapsed( r.key ) );
                    ImGui::SameLine( 0.0f, colGapX );
                }
                else
                {
                    ImGui::Dummy( ImVec2( arrowW, itemH ) );      // the arrow's own slot
                    ImGui::SameLine( 0.0f, colGapX );
                }

                // Label
                char label[128];
                bool selected = false;
                switch ( r.kind )
                {
                case KOUT_SECTION_BRUSHES:
                    _snprintf( label, sizeof( label ), "Brushes (%i)", r.count );
                    break;
                case KOUT_SECTION_TERRAIN:
                    _snprintf( label, sizeof( label ), "Terrain (%i)", r.count );
                    break;
                case KOUT_SECTION_CURVES:
                    _snprintf( label, sizeof( label ), "Curves (%i)", r.count );
                    break;
                case KOUT_SECTION_ENTITIES:
                    _snprintf( label, sizeof( label ), "Entities (%i)", r.count );
                    break;
                case KOUT_SECTION_LIGHTS:
                    _snprintf( label, sizeof( label ), "Lights (%i)", r.count );
                    break;
                case KOUT_SECTION_MODELS:
                    _snprintf( label, sizeof( label ), "Models (%i)", r.count );
                    break;
                case KOUT_SECTION_IMAGES:
                    _snprintf( label, sizeof( label ), "Images (%i)", r.count );
                    break;
                case KOUT_GROUP_ENTITY:
                case KOUT_ENTITY:
                {
                    char nm[80];
                    FolderName( r.ent, nm, sizeof( nm ) );
                    _snprintf( label, sizeof( label ), "%s (%i)", nm, r.count );
                    break;
                }
                case KOUT_BRUSH:
                    BrushName( r.inst, r.ordinal, label, sizeof( label ) );
                    selected = BrushSelected( r.inst );
                    break;
                case KOUT_CON_GROUP:
                    _snprintf( label, sizeof( label ), "%s (%i)",
                               KiwiCon_GroupName( r.conGroup ), r.count );
                    break;
                case KOUT_CON_OBJECT:
                {
                    const kconObject_t *o = KiwiCon_At( r.conIndex );
                    // Unnamed objects use Plasticity's "<type> <ordinal>" fallback.
                    const char *nm = KiwiCon_Name( r.conIndex );
                    if ( nm && nm[0] )
                        _snprintf( label, sizeof( label ), "%s", nm );
                    else
                        _snprintf( label, sizeof( label ), "%s %i",
                                   o ? ConTypeName( o->type ) : "Curve", r.ordinal );
                    selected = KiwiConSel_ObjectSelected( r.conIndex );
                    break;
                }
                case KOUT_IMAGE_GROUP:
                    _snprintf( label, sizeof( label ), "%s (%i)",
                               KiwiRefImage_GroupName( r.imageGroup ), r.count );
                    break;
                case KOUT_IMAGE:
                {
                    const krefImage_t *image = KiwiRefImage_At( r.imageIndex );
                    const char *nm = image ? ( image->name.empty()
                        ? ImageBaseName( image->file ) : image->name.c_str() ) : "Image";
                    _snprintf( label, sizeof( label ), "%s  [%s]%s", nm,
                               image ? ImageAxisName( image->axis ) : "?",
                               image && image->locked ? " (locked)" : "" );
                    selected = image && KiwiRefImage_IsSelected( r.imageIndex );
                    break;
                }
                }
                label[sizeof( label ) - 1] = '\0';

                // Renameable rows swap their label for an editor (OutlinerItems.tsx:41-58).
                if ( RenamingRow( r ) )
                {
                    ImGui::SetNextItemWidth( -1.0f );
                    if ( s_renameFocus )
                    {
                        ImGui::SetKeyboardFocusHere();
                        s_renameFocus = false;
                    }
                    const bool done = ImGui::InputText( "##rename", s_renameBuf,
                                                        sizeof( s_renameBuf ),
                                                        ImGuiInputTextFlags_EnterReturnsTrue );
                    // Divergence from Plasticity: Esc cancels, matching other editor fields.
                    const bool esc  = ImGui::IsKeyPressed( ImGuiKey_Escape, false );
                    const bool lost = ImGui::IsItemDeactivated();
                    if ( esc )
                    {
                        s_rename.active = false;
                    }
                    else if ( done || lost )
                    {
                        if ( s_renameBuf[0] || r.kind == KOUT_IMAGE )
                        {
                            if ( r.kind == KOUT_IMAGE )
                            {
                                KiwiRefImage_SetName( r.imageIndex, s_renameBuf );
                            }
                            else if ( r.kind == KOUT_CON_GROUP )
                            {
                                KiwiCon_SetGroupName( r.conGroup, s_renameBuf );
                            }
                            else if ( r.kind == KOUT_IMAGE_GROUP )
                            {
                                KiwiRefImage_SetGroupName( r.imageGroup, s_renameBuf );
                            }
                            else if ( r.kind == KOUT_CON_OBJECT )
                            {
                                // One construction snapshot covers the object rename.
                                KiwiCon_UndoPush();
                                KiwiCon_SetName( r.conIndex, s_renameBuf );
                            }
                            else if ( entity_s_def *def = DefOf( r.ent ) )
                            {
                                // Match the entity editor's epair undo bracket (win_ent.cpp:278).
                                Undo_ClearRedo();
                                Undo_GeneralStart( "outliner rename" );
                                Undo_AddEntity_W( (entity_s *)def );
                                SetKeyValue( def, "targetname", s_renameBuf );
                                Undo_End();
                                g_nUpdateBits = -1;
                            }
                        }
                        s_rename.active = false;
                    }
                    ImGui::PopID();
                    continue;
                }

                const bool dimImageRow = r.kind == KOUT_IMAGE && hidden;
                if ( dimImageRow )
                    ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
                const bool rowClicked = ImGui::Selectable( label, selected,
                                                           ImGuiSelectableFlags_AllowDoubleClick,
                                                           ImVec2( 0.0f, itemH ) );
                if ( dimImageRow )
                    ImGui::PopStyleColor();
                if ( rowClicked )
                {
                    if ( r.kind == KOUT_IMAGE )
                    {
                        SelectImageRow( r.imageIndex, io.KeyShift, io.KeyCtrl );
                        if ( !io.KeyShift && !io.KeyCtrl
                          && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                            KiwiRefImage_Focus( r.imageIndex );
                        SetAnchor( r );
                    }
                    else if ( r.kind == KOUT_BRUSH || r.kind == KOUT_CON_OBJECT )
                    {
                        // Modifiers reserve range/toggle gestures; an unmodified
                        // double-click renames after its first click selected the row.
                        if ( !io.KeyShift && !io.KeyCtrl
                          && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                        {
                            if ( r.kind == KOUT_CON_OBJECT )
                            {
                                const char *nm = KiwiCon_Name( r.conIndex );
                                BeginRename( r, ( nm && nm[0] ) ? nm : "" );
                            }
                            else
                            {
                                // World brushes have no persistent naming key; report it.
                                Sys_Printf( "Outliner: a brush has no name field — "
                                            "put it in a group (New Group from "
                                            "Selection) and name the group.\n" );
                            }
                        }
                        else if ( io.KeyShift )
                        {
                            const int anchor = FindAnchorRow();
                            if ( anchor >= 0 )
                                SelectRange( anchor, i );
                            else if ( r.kind == KOUT_BRUSH )
                                SelectBrushRow( r.inst, true, false );
                            else
                                SelectConRow( r.conIndex, true, false );
                        }
                        else if ( r.kind == KOUT_BRUSH )
                        {
                            SelectBrushRow( r.inst, false, io.KeyCtrl );
                            SetAnchor( r );
                        }
                        else
                        {
                            SelectConRow( r.conIndex, false, io.KeyCtrl );
                            SetAnchor( r );
                        }
                    }
                    else if ( isFolder )
                    {
                        // Double-click renames a folder; single-click selects its children.
                        if ( !io.KeyShift && !io.KeyCtrl
                          && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left )
                          && Renameable( r.kind ) )
                        {
                            if ( r.kind == KOUT_CON_GROUP )
                            {
                                BeginRename( r, KiwiCon_GroupName( r.conGroup ) );
                            }
                            else if ( r.kind == KOUT_IMAGE_GROUP )
                            {
                                BeginRename( r, KiwiRefImage_GroupName( r.imageGroup ) );
                            }
                            else
                            {
                                char nm[80];
                                FolderName( r.ent, nm, sizeof( nm ) );
                                BeginRename( r, nm );
                            }
                        }
                        else if ( r.kind == KOUT_IMAGE_GROUP )
                        {
                            // Plain: the group's pictures replace the selection; Shift adds
                            // them; Ctrl on a fully selected group removes them.
                            const int n = KiwiRefImage_Count();
                            bool any = false, all = true;
                            for ( int k = 0; k < n; ++k )
                            {
                                if ( KiwiRefImage_Group( k ) != r.imageGroup )
                                    continue;
                                any = true;
                                if ( !KiwiRefImage_IsSelected( k ) )
                                    all = false;
                            }
                            const bool remove = io.KeyCtrl && !io.KeyShift && any && all;
                            bool first = !io.KeyShift && !io.KeyCtrl;
                            for ( int k = 0; k < n; ++k )
                            {
                                if ( KiwiRefImage_Group( k ) != r.imageGroup )
                                    continue;
                                if ( remove )
                                    KiwiRefImage_SelectRemove( k );
                                else if ( first )
                                {
                                    KiwiRefImage_ApplyClick( k, false, false, false );   // replaces
                                    first = false;
                                }
                                else
                                    KiwiRefImage_SelectAdd( k );
                            }
                            g_nUpdateBits = -1;
                        }
                        else if ( r.kind == KOUT_GROUP_ENTITY || r.kind == KOUT_ENTITY )
                        {
                            selection_t &sel = KiwiSel();
                            bool any = false;
                            bool all = true;
                            for ( selbrush_t *b = r.ent->brushes.ownerNext;
                                  b && b != &r.ent->brushes; b = b->ownerNext )
                            {
                                if ( !Sel_BrushLive( b ) )
                                    continue;
                                any = true;
                                if ( !BrushSelected( b ) )
                                    all = false;
                            }
                            // Ctrl toggles the group as a unit; Shift adds it.
                            const bool remove = io.KeyCtrl && !io.KeyShift && any && all;
                            if ( !io.KeyShift && !io.KeyCtrl )
                            {
                                Sel_Clear( sel );
                                KiwiConSel_Clear();
                            }
                            for ( selbrush_t *b = r.ent->brushes.ownerNext;
                                  b && b != &r.ent->brushes; b = b->ownerNext )
                            {
                                if ( !Sel_BrushLive( b ) )
                                    continue;
                                if ( remove ) Sel_Remove( sel, Sel_MakeObject( b ) );
                                else          Sel_Add   ( sel, Sel_MakeObject( b ) );
                            }
                            Sel_SyncToLegacy();
                            g_nUpdateBits = -1;
                        }
                        else if ( r.kind == KOUT_CON_GROUP )
                        {
                            const int cnt = KiwiCon_Count();
                            bool any = false;
                            bool all = true;
                            for ( int k = 0; k < cnt; ++k )
                            {
                                if ( KiwiCon_Group( k ) != r.conGroup )
                                    continue;
                                any = true;
                                if ( !ConObjectRowSelected( k ) )
                                    all = false;
                            }
                            const bool remove = io.KeyCtrl && !io.KeyShift && any && all;
                            if ( !io.KeyShift && !io.KeyCtrl )
                            {
                                KiwiConSel_Clear();
                                Sel_Clear( KiwiSel() );
                                Sel_SyncToLegacy();
                            }
                            for ( int k = 0; k < cnt; ++k )
                            {
                                if ( KiwiCon_Group( k ) != r.conGroup )
                                    continue;
                                kconSelItem_t it;
                                it.object = k;
                                it.kind   = KCONSEL_OBJECT;
                                it.index  = -1;
                                KiwiConSel_ApplyClick( it, !remove, remove );
                            }
                            g_nUpdateBits = -1;
                        }
                    }
                }

                // Publish row hover to the viewport immediately after Selectable:
                // IsItemHovered reads the last item, and later widgets replace it.
                // Folder hover publishes all members (kiwi_hover.h).
                if ( ImGui::IsItemHovered() )
                {
                    switch ( r.kind )
                    {
                    case KOUT_BRUSH:
                        KiwiHover_OutlinerBrush( r.inst );
                        break;
                    case KOUT_ENTITY:
                    case KOUT_GROUP_ENTITY:
                        KiwiHover_OutlinerEntity( r.ent );
                        break;
                    case KOUT_CON_OBJECT:
                        KiwiHover_OutlinerCon( r.conIndex );
                        break;
                    case KOUT_CON_GROUP:
                        KiwiHover_OutlinerConGroup( r.conGroup );
                        break;
                    default:
                        break;
                    }
                }

                // Folder context menu must immediately follow Selectable because
                // BeginPopupContextItem tests the last submitted item.
                if ( r.kind == KOUT_GROUP_ENTITY || r.kind == KOUT_CON_GROUP )
                {
                    if ( ImGui::BeginPopupContextItem( "##folderctx" ) )
                    {
                        if ( ImGui::MenuItem( "Rename" ) )
                        {
                            if ( r.kind == KOUT_CON_GROUP )
                            {
                                BeginRename( r, KiwiCon_GroupName( r.conGroup ) );
                            }
                            else
                            {
                                char nm[80];
                                FolderName( r.ent, nm, sizeof( nm ) );
                                BeginRename( r, nm );
                            }
                        }
                        if ( ImGui::MenuItem( "Ungroup" ) )
                        {
                            if ( r.kind == KOUT_GROUP_ENTITY )
                            {
                                UngroupEntity( r.ent, "outliner ungroup" );
                            }
                            else
                            {
                                KiwiCon_UndoPush();
                                KiwiCon_RemoveGroup( r.conGroup );
                                s_structural = true;
                            }
                            SetCollapsed( r.key, false );
                        }
                        ImGui::EndPopup();
                    }
                }
                // Construction objects expose rename only; grouping uses drag and
                // deletion uses the selection's Delete command.
                else if ( r.kind == KOUT_CON_OBJECT )
                {
                    if ( ImGui::BeginPopupContextItem( "##conctx" ) )
                    {
                        if ( ImGui::MenuItem( "Rename" ) )
                        {
                            const char *nm = KiwiCon_Name( r.conIndex );
                            BeginRename( r, ( nm && nm[0] ) ? nm : "" );
                        }
                        ImGui::EndPopup();
                    }
                }
                else if ( r.kind == KOUT_IMAGE_GROUP )
                {
                    if ( ImGui::BeginPopupContextItem( "##imagegroupctx" ) )
                    {
                        if ( ImGui::MenuItem( "Rename" ) )
                            BeginRename( r, KiwiRefImage_GroupName( r.imageGroup ) );
                        const bool allHidden = KiwiRefImage_GroupAllHidden( r.imageGroup );
                        if ( ImGui::MenuItem( allHidden ? "Show all" : "Hide all" ) )
                            KiwiRefImage_SetGroupHidden( r.imageGroup, !allHidden );
                        if ( ImGui::MenuItem( "Ungroup" ) )
                        {
                            KiwiRefImage_RemoveGroup( r.imageGroup );   // members stay, ungrouped
                            s_structural = true;
                        }
                        ImGui::EndPopup();
                    }
                }
                else if ( r.kind == KOUT_IMAGE )
                {
                    if ( ImGui::BeginPopupContextItem( "##imagectx" ) )
                    {
                        const krefImage_t *image = KiwiRefImage_At( r.imageIndex );
                        if ( image && ImGui::MenuItem( image->hidden ? "Show" : "Hide" ) )
                            KiwiRefImage_SetHidden( r.imageIndex, !image->hidden );
                        image = KiwiRefImage_At( r.imageIndex );
                        if ( image && ImGui::MenuItem( image->locked ? "Unlock" : "Lock" ) )
                            KiwiRefImage_SetLocked( r.imageIndex, !image->locked );
                        image = KiwiRefImage_At( r.imageIndex );
                        if ( image && ImGui::MenuItem( "Rename" ) )
                            BeginRename( r, image->name.c_str() );
                        ImGui::Separator();
                        // Grouping (KIWI 2026-09-10): a new group takes the selected pictures
                        // (this one when it is not selected); "Move to" re-homes the same set.
                        if ( ImGui::MenuItem( "New group from selected" ) )
                        {
                            if ( !KiwiRefImage_IsSelected( r.imageIndex ) )
                                KiwiRefImage_ApplyClick( r.imageIndex, false, false, false );
                            const int gid = KiwiRefImage_GroupFromSelection( nullptr );
                            if ( gid >= 0 )
                                SetCollapsed( ImageGroupKey( gid ), false );
                            s_structural = true;
                        }
                        if ( KiwiRefImage_GroupCount() > 0 && ImGui::BeginMenu( "Move to group" ) )
                        {
                            for ( int g = 0; g < KiwiRefImage_GroupCount(); ++g )
                            {
                                const int gid = KiwiRefImage_GroupIdAt( g );
                                ImGui::PushID( gid );
                                if ( ImGui::MenuItem( KiwiRefImage_GroupName( gid ), 0,
                                                      KiwiRefImage_Group( r.imageIndex ) == gid ) )
                                {
                                    if ( KiwiRefImage_IsSelected( r.imageIndex ) )
                                    {
                                        std::vector<int> set;
                                        for ( int k = 0; k < KiwiRefImage_SelectedCount(); ++k )
                                            set.push_back( KiwiRefImage_SelectedAt( k ) );
                                        for ( size_t k = 0; k < set.size(); ++k )
                                            KiwiRefImage_SetGroup( set[k], gid );
                                    }
                                    else
                                        KiwiRefImage_SetGroup( r.imageIndex, gid );
                                    s_structural = true;
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndMenu();
                        }
                        if ( KiwiRefImage_Group( r.imageIndex ) >= 0 && ImGui::MenuItem( "Remove from group" ) )
                        {
                            KiwiRefImage_SetGroup( r.imageIndex, -1 );
                            s_structural = true;
                        }
                        ImGui::Separator();
                        if ( KiwiRefImage_At( r.imageIndex ) && ImGui::MenuItem( "Delete" ) )
                        {
                            KiwiRefImage_DeleteAt( r.imageIndex );
                            s_structural = true;
                        }
                        ImGui::EndPopup();
                    }
                }

                // MMB opacity drag start (KIWI 2026-09-10): the row under the middle press
                // names the target; the drag itself runs above the clipper each frame.
                if ( !s_opacityDrag.active
                  && ( r.kind == KOUT_IMAGE || r.kind == KOUT_IMAGE_GROUP || r.kind == KOUT_SECTION_IMAGES )
                  && ImGui::IsItemHovered() && ImGui::IsMouseClicked( ImGuiMouseButton_Middle ) )
                {
                    s_opacityDrag.active     = true;
                    s_opacityDrag.imageIndex = ( r.kind == KOUT_IMAGE ) ? r.imageIndex : -1;
                    s_opacityDrag.imageGroup = ( r.kind == KOUT_IMAGE_GROUP ) ? r.imageGroup : -1;
                    s_opacityDrag.lastX      = io.MousePos.x;
                    s_opacityDrag.shown      = -1.0f;
                    if ( r.kind == KOUT_IMAGE )
                    {
                        const krefImage_t *image = KiwiRefImage_At( r.imageIndex );
                        if ( image )
                            s_opacityDrag.shown = image->opacity;
                    }
                }

                // Shift paints selection; unmodified dragging moves rows.
                if ( io.KeyShift && ImGui::IsItemHovered()
                  && ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
                {
                    s_paintSelecting = true;
                    if ( r.kind == KOUT_BRUSH && r.inst && Sel_BrushLive( r.inst )
                      && !BrushSelected( r.inst ) )
                    {
                        Sel_Add( KiwiSel(), Sel_MakeObject( r.inst ) );
                        Sel_SyncToLegacy();
                        g_nUpdateBits = -1;
                    }
                    else if ( r.kind == KOUT_CON_OBJECT && r.conIndex >= 0
                           && !KiwiConSel_ObjectSelected( r.conIndex ) )
                    {
                        kconSelItem_t it;
                        it.object = r.conIndex;
                        it.kind   = KCONSEL_OBJECT;
                        it.index  = -1;
                        KiwiConSel_ApplyClick( it, true, false );
                        g_nUpdateBits = -1;
                    }
                }

                // Drag source
                if ( !io.KeyShift && !s_paintSelecting
                  && ( r.kind == KOUT_BRUSH || r.kind == KOUT_CON_OBJECT || r.kind == KOUT_IMAGE ) )
                {
                    if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
                    {
                        koutDrag_t d;
                        d.kind       = (int)r.kind;
                        d.inst       = r.inst;
                        d.conIndex   = r.conIndex;
                        d.conGen     = KiwiCon_Generation();
                        d.imageIndex = r.imageIndex;
                        d.imageGen   = KiwiRefImage_Generation();
                        ImGui::SetDragDropPayload( KOUT_PAYLOAD, &d, sizeof( d ) );
                        ImGui::TextUnformatted( label );
                        ImGui::EndDragDropSource();
                    }
                }

                // Drop target
                if ( RowAcceptsBrushes( r ) || RowAcceptsCurves( r ) || RowAcceptsImages( r ) )
                {
                    if ( ImGui::BeginDragDropTarget() )
                    {
                        const ImGuiPayload *p = ImGui::AcceptDragDropPayload( KOUT_PAYLOAD );
                        if ( p && p->DataSize == (int)sizeof( koutDrag_t ) )
                        {
                            koutDrag_t d;
                            memcpy( &d, p->Data, sizeof( d ) );
                            ApplyDrop( d, r );
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                ImGui::PopID();
                if ( s_structural )
                    break;              // see s_structural: the rest of s_rows is stale
            }
            if ( s_structural )
                break;
        }
        clipper.End();

        ImGui::EndChild();
    }
    ImGui::End();
}

// Group commands
bool KiwiOutliner_CanGroup()
{
    // Brush grouping is create-only and therefore requires all-world ownership.
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    if ( !brushes.empty() )
    {
        bool allWorld = true;
        for ( size_t i = 0; i < brushes.size(); ++i )
            if ( brushes[i]->owner != world_entity )
                allWorld = false;
        if ( allWorld )
            return true;
    }
    return KiwiConSel_Count() > 0;
}

bool KiwiOutliner_GroupSelection()
{
    bool did = false;

    // Brush half: func_group entity.
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    if ( !brushes.empty() )
    {
        eclass_t *ec = Eclass_ForName( 0, "func_group" );     // eclass.cpp:1138
        if ( !ec )
        {
            Sys_Printf( "Outliner: no func_group entity definition — cannot group.\n" );
        }
        else
        {
            // Entity_Create allocates only for an all-world selection; mixed owners
            // merge or refuse (entity.cpp:1628-1756). Stamping a returned existing
            // entity would make undo delete it, so existing groups are drag targets.
            bool allWorld = true;
            for ( size_t i = 0; i < brushes.size(); ++i )
                if ( brushes[i]->owner != world_entity )
                    allWorld = false;

            if ( !allWorld )
            {
                Sys_Printf( "Outliner: part of the selection already belongs to an "
                            "entity.  Ungroup it first, or DRAG the rows onto the "
                            "group folder you want them in.\n" );
            }
            else
            {
                // xywnd.cpp:3400-3404 bracket plus pmesh.cpp:7396 entity-id tail;
                // Entity_Create performs the relink.
                Undo_ClearRedo();
                Undo_GeneralStart( "outliner group" );
                Undo_AddBrushList( &selected_brushes );
                entity_s *inst = Entity_Create( ec );
                if ( entity_s_def *def = DefOf( inst ) )
                {
                    Undo_SetIdForEntity( def );
                    // numberId makes the generated group_N name session-unique.
                    char nm[64];
                    _snprintf( nm, sizeof( nm ), "group_%i", def->numberId );
                    nm[sizeof( nm ) - 1] = '\0';
                    SetKeyValue( def, "targetname", nm );
                    SetCollapsed( EntKey( def->numberId ), false );
                    did = true;
                }
                Undo_End();
                g_nUpdateBits = -1;
                Sel_InvalidateFromLegacy();
                s_structural = true;
                if ( did )
                    Sys_Printf( "Outliner: grouped %i brush(es) into a func_group.\n",
                                (int)brushes.size() );
                else
                    Sys_Printf( "Outliner: could not create the group "
                                "(see the message above).\n" );
            }
        }
    }

    // Construction half: sidecar group.
    if ( KiwiConSel_Count() > 0 )
    {
        std::vector<int> objs;
        for ( int i = 0; i < KiwiConSel_Count(); ++i )
        {
            const kconSelItem_t *it = KiwiConSel_At( i );
            if ( !it || it->object < 0 || it->object >= KiwiCon_Count() )
                continue;
            bool dup = false;
            for ( size_t k = 0; k < objs.size(); ++k )
                if ( objs[k] == it->object )
                {
                    dup = true;
                    break;
                }
            if ( !dup )
                objs.push_back( it->object );
        }
        if ( !objs.empty() )
        {
            KiwiCon_UndoPush();
            const int gid = KiwiCon_NewGroup( 0 );
            for ( size_t i = 0; i < objs.size(); ++i )
                KiwiCon_SetGroup( objs[i], gid );
            SetCollapsed( ConGroupKey( gid ), false );
            Sys_Printf( "Outliner: grouped %i construction object(s) into \"%s\".\n",
                        (int)objs.size(), KiwiCon_GroupName( gid ) );
            did = true;
        }
    }

    if ( !did )
        Sys_Printf( "Outliner: select something first.\n" );
    return did;
}

bool KiwiOutliner_CanUngroup()
{
    // Brush side: any selected brush owned by a non-world brush entity.
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    for ( size_t i = 0; i < brushes.size(); ++i )
    {
        entity_s *e = brushes[i]->owner;
        if ( !e || e == world_entity )
            continue;
        entity_s_def *def = DefOf( e );
        if ( def && def->eclass && !def->eclass->fixedsize )
            return true;
    }
    // Construction side: any selected grouped object.
    for ( int i = 0; i < KiwiConSel_Count(); ++i )
    {
        const kconSelItem_t *it = KiwiConSel_At( i );
        if ( it && KiwiCon_Group( it->object ) >= 0 )
            return true;
    }
    return false;
}

bool KiwiOutliner_UngroupSelection()
{
    bool did = false;

    // Collect distinct owners before ungroup frees them and invalidates selection.
    std::vector<entity_s *> ents;
    {
        std::vector<selbrush_t *> brushes;
        GatherSelectedBrushes( brushes );
        for ( size_t i = 0; i < brushes.size(); ++i )
        {
            entity_s *e = brushes[i]->owner;
            if ( !e || e == world_entity )
                continue;
            entity_s_def *def = DefOf( e );
            if ( !def || !def->eclass || def->eclass->fixedsize )
                continue;
            bool dup = false;
            for ( size_t k = 0; k < ents.size(); ++k )
                if ( ents[k] == e )
                {
                    dup = true;
                    break;
                }
            if ( !dup )
                ents.push_back( e );
        }
    }
    for ( size_t i = 0; i < ents.size(); ++i )
        if ( UngroupEntity( ents[i], "outliner ungroup" ) )
            did = true;

    // Ungroup selected construction objects and remove groups left empty.
    {
        std::vector<int> groups;
        std::vector<int> objs;
        for ( int i = 0; i < KiwiConSel_Count(); ++i )
        {
            const kconSelItem_t *it = KiwiConSel_At( i );
            if ( !it )
                continue;
            const int g = KiwiCon_Group( it->object );
            if ( g < 0 )
                continue;
            objs.push_back( it->object );
            bool dup = false;
            for ( size_t k = 0; k < groups.size(); ++k )
                if ( groups[k] == g )
                {
                    dup = true;
                    break;
                }
            if ( !dup )
                groups.push_back( g );
        }
        if ( !objs.empty() )
        {
            KiwiCon_UndoPush();
            for ( size_t i = 0; i < objs.size(); ++i )
                KiwiCon_SetGroup( objs[i], -1 );
            for ( size_t i = 0; i < groups.size(); ++i )
                if ( KiwiCon_GroupMemberCount( groups[i] ) == 0 )
                    KiwiCon_RemoveGroup( groups[i] );
            Sys_Printf( "Outliner: ungrouped %i construction object(s).\n", (int)objs.size() );
            did = true;
        }
    }

    if ( !did )
        Sys_Printf( "Outliner: nothing in the selection belongs to a group.\n" );
    return did;
}

// Commands
void KiwiOutliner_RegisterCommands()
{
    // Unbound by default but searchable and remappable through radiant.ini.
    Radiant_RegisterCommand( "KiwiGroupSelection",   0, 0, KIWI_CMD_GROUP_CREATE );
    Radiant_RegisterCommand( "KiwiUngroupSelection", 0, 0, KIWI_CMD_GROUP_UNGROUP );
}

bool KiwiOutliner_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == (unsigned int)KIWI_CMD_GROUP_CREATE )
    {
        KiwiOutliner_GroupSelection();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_GROUP_UNGROUP )
    {
        KiwiOutliner_UngroupSelection();
        return true;
    }
    return false;
}
