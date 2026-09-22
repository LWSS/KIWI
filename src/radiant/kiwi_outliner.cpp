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
#include <map>
#include <string>
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
// File scope on purpose: declared inside the anonymous namespace it names a
// namespace-local function and the link fails (LNK2019, 2026-09-22).
extern void        DeleteKey( epair_t **head, const char *key );             // entity.cpp:175 (0x483720)
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
    // KIWI (2026-09-16, user: "barbwire needs to go into its own group in the outliner
    // (under Models)"): a fixed folder inside Models holding every entity that carries the
    // `kiwi_barbwire` epair (kiwi_barbwire.cpp).  Section-like: its eye and click cover
    // every member; it is not a drop target and cannot be renamed.
    KOUT_BARBWIRE_FOLDER,
    // Universal groups (2026-09-22, see UNIVERSAL GROUPS below): the top-level "Groups"
    // section, one folder per group, and inside it one folder per kind of member.
    KOUT_SECTION_GROUPS,
    KOUT_UGROUP,                // koutRow_t::ugroup names the node
    KOUT_UGROUP_TYPE,           // ...and ::ugType which kind (a KOUT_SECTION_* value)
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
    int         ugroup;     // KOUT_UGROUP / KOUT_UGROUP_TYPE — index into s_ug (this frame)
    int         ugType;     // KOUT_UGROUP_TYPE — the KOUT_SECTION_* kind it lists
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
const unsigned KOUT_KEY_BARBWIRE = 0xF0000008u;   // the Barbwire folder inside Models
const unsigned KOUT_KEY_GROUPS   = 0xF0000009u;   // the top-level Groups section

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
        || k == KOUT_IMAGE_GROUP  || k == KOUT_UGROUP;
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

// A misc_model laid by the barbwire tool (kiwi_barbwire.cpp writes the epair).
bool IsBarbwireEntity( entity_s *inst )
{
    entity_s_def *def = DefOf( inst );
    const char *src = def ? ValueForKey2( def, "kiwi_barbwire" ) : "";
    return src && src[0];
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
    else if ( IsBarbwireEntity( inst ) )
    {
        // The curve's name, plus the piece ordinal when the run was split.
        const char *src   = ValueForKey2( def, "kiwi_barbwire" );
        const char *piece = ValueForKey2( def, "kiwi_barbwire_piece" );
        if ( piece && piece[0] && strcmp( piece, "0" ) )
            _snprintf( out, (size_t)outSize, "%s #%s", src, piece );
        else
            _snprintf( out, (size_t)outSize, "%s", src );
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

// ═════════════════════════════════════════════════════════════════════════════════════
//  UNIVERSAL GROUPS  (KIWI 2026-09-22, user: "the group doesn't include models. You need
//  to just make universal groups so i can have doorknobs and fine details and group
//  houses together" / "redo the whole group system" / "make universal groups a top level
//  item in the outliner with brush/model/etc subgroups inside it")
//
//  A group is a PATH - "village/house_1/door" - stored as the epair `kiwi_group` on every
//  member ENTITY.  An epair is the one per-object store this editor saves in the .map,
//  clones into undo records, and carries through copy / paste and prefab export; the map
//  compilers ignore keys they do not know (the `kiwi_barbwire` precedent).  Nesting is
//  the path, so a group of groups needs no record of its own.
//
//    - point and brush entities (models, lights, triggers, ...) carry the key themselves;
//    - loose worldspawn brushes and patches have no epairs, so they join by being moved
//      into a func_group that carries the key (a "wrapper").  cod4map folds func_group
//      brushes back into the world, exactly as it always has for brush groups;
//    - EVERY func_group is a group: one without the key is the top-level group named by
//      its folder name, so brush groups made before this exist in the new system as-is.
//
//  The outliner never shows a wrapper: a group lists its nested groups, then one folder
//  per kind (Brushes / Models / Lights / Entities) holding the members themselves.
// ═════════════════════════════════════════════════════════════════════════════════════
const char *KOUT_UG_KEY = "kiwi_group";

std::string UGCleanName( const char *s )
{
    std::string out;
    for ( const char *p = s ? s : ""; *p; ++p )
        out.push_back( ( *p == '/' || *p == '\\' || *p == '"' ) ? '_' : *p );
    while ( !out.empty() && out[out.size() - 1] == ' ' ) out.erase( out.size() - 1 );
    while ( !out.empty() && out[0] == ' ' )              out.erase( 0, 1 );
    return out;
}

// Normalised stored path: '/'-separated, no empty components.
std::string UGCleanPath( const char *s )
{
    std::string out, part;
    for ( const char *p = s ? s : ""; ; ++p )
    {
        if ( *p && *p != '/' )
        {
            part.push_back( *p );
            continue;
        }
        part = UGCleanName( part.c_str() );
        if ( !part.empty() )
        {
            if ( !out.empty() ) out.push_back( '/' );
            out += part;
        }
        part.clear();
        if ( !*p )
            break;
    }
    return out;
}

// The group an entity is in; empty = none.  Worldspawn is never a member.
std::string UGPathOf( entity_s *inst )
{
    entity_s_def *def = DefOf( inst );
    if ( !def || inst == world_entity )
        return std::string();
    const char *v = ValueForKey2( def, KOUT_UG_KEY );
    if ( v && v[0] )
        return UGCleanPath( v );
    if ( IsFuncGroup( inst ) )
    {
        char nm[80];
        FolderName( inst, nm, sizeof( nm ) );
        return UGCleanName( nm );
    }
    return std::string();
}

std::string UGParent( const std::string &p )
{
    const size_t at = p.rfind( '/' );
    return at == std::string::npos ? std::string() : p.substr( 0, at );
}

std::string UGLeaf( const std::string &p )
{
    const size_t at = p.rfind( '/' );
    return at == std::string::npos ? p : p.substr( at + 1 );
}

std::string UGJoin( const std::string &parent, const std::string &leaf )
{
    if ( parent.empty() ) return leaf;
    if ( leaf.empty() )   return parent;
    return parent + "/" + leaf;
}

// p is P itself or somewhere beneath it.
bool UGIsUnder( const std::string &p, const std::string &P )
{
    if ( P.empty() )
        return true;
    return p == P || ( p.size() > P.size() && p.compare( 0, P.size(), P ) == 0 && p[P.size()] == '/' );
}

// What of `p` lies below the ancestor `P` ("" when p == P).
std::string UGBelow( const std::string &p, const std::string &P )
{
    if ( P.empty() )      return p;
    if ( p.size() <= P.size() ) return std::string();
    return p.substr( P.size() + 1 );
}

// Deepest path both are under.
std::string UGCommon( const std::string &a, const std::string &b )
{
    std::string out;
    size_t ia = 0, ib = 0;
    while ( ia < a.size() && ib < b.size() )
    {
        const size_t ea = a.find( '/', ia ), eb = b.find( '/', ib );
        const std::string ca = a.substr( ia, ea == std::string::npos ? std::string::npos : ea - ia );
        const std::string cb = b.substr( ib, eb == std::string::npos ? std::string::npos : eb - ib );
        if ( ca != cb )
            break;
        out = UGJoin( out, ca );
        if ( ea == std::string::npos || eb == std::string::npos )
            break;
        ia = ea + 1;
        ib = eb + 1;
    }
    return out;
}

// Collapse keys: tag 0xC = a group, 0xB = one of its per-kind folders.
unsigned UGHash( const std::string &s )
{
    unsigned h = 2166136261u;
    for ( size_t i = 0; i < s.size(); ++i ) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}
inline unsigned UGKey( const std::string &path ) { return 0xC0000000u | ( UGHash( path ) & 0x0FFFFFFFu ); }
inline unsigned UGTypeKey( const std::string &path, int kind )
{
    return 0xB0000000u | ( ( UGHash( path ) * 31u + (unsigned)kind ) & 0x0FFFFFFFu );
}

// The group tree, rebuilt from the entity list whenever it is needed (each outliner
// frame, each command).  Nodes are created for every ancestor a path implies.
struct ugNode_t
{
    std::string             path;
    int                     parent = -1;
    std::vector<int>        kids;
    std::vector<entity_s *> members;        // entities at EXACTLY this path
    int                     total = 0;      // members here and below
};
std::vector<ugNode_t>                s_ug;
std::map<std::string, int>           s_ugByPath;
std::map<const entity_s *, int>      s_ugOf;      // member -> its node

int UGEnsureNode( const std::string &path )
{
    std::map<std::string, int>::iterator it = s_ugByPath.find( path );
    if ( it != s_ugByPath.end() )
        return it->second;
    const std::string up = UGParent( path );
    const int parent = up.empty() ? -1 : UGEnsureNode( up );     // may grow s_ug
    ugNode_t n;
    n.path   = path;
    n.parent = parent;
    s_ug.push_back( n );
    const int idx = (int)s_ug.size() - 1;
    s_ugByPath[path] = idx;
    if ( parent >= 0 )
        s_ug[parent].kids.push_back( idx );
    return idx;
}

void UGBuildTree()
{
    s_ug.clear();
    s_ugByPath.clear();
    s_ugOf.clear();
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        const std::string p = UGPathOf( e );
        if ( p.empty() )
            continue;
        const int idx = UGEnsureNode( p );
        s_ug[idx].members.push_back( e );
        s_ugOf[e] = idx;
    }
    for ( size_t i = 0; i < s_ug.size(); ++i )
        for ( int up = (int)i; up >= 0; up = s_ug[up].parent )
            s_ug[up].total += (int)s_ug[i].members.size();
    // Children in name order (the map's), whatever order the entity list produced them in.
    for ( size_t i = 0; i < s_ug.size(); ++i )
        s_ug[i].kids.clear();
    for ( std::map<std::string, int>::const_iterator it = s_ugByPath.begin();
          it != s_ugByPath.end(); ++it )
        if ( s_ug[it->second].parent >= 0 )
            s_ug[s_ug[it->second].parent].kids.push_back( it->second );
}

// Valid after UGBuildTree.
bool UGHas( const entity_s *e )
{
    return s_ugOf.find( e ) != s_ugOf.end();
}

int UGFind( const std::string &path )
{
    std::map<std::string, int>::const_iterator it = s_ugByPath.find( path );
    return it == s_ugByPath.end() ? -1 : it->second;
}

// Members of node `idx`; `kind` < 0 = every kind, `deep` = nested groups too.
koutKind_t EntitySection( entity_s *inst );
void UGMembers( int idx, int kind, bool deep, std::vector<entity_s *> &out )
{
    if ( idx < 0 || idx >= (int)s_ug.size() )
        return;
    for ( size_t i = 0; i < s_ug[idx].members.size(); ++i )
        if ( kind < 0 || (int)EntitySection( s_ug[idx].members[i] ) == kind )
            out.push_back( s_ug[idx].members[i] );
    if ( deep )
        for ( size_t i = 0; i < s_ug[idx].kids.size(); ++i )
            UGMembers( s_ug[idx].kids[i], kind, true, out );
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
    r.ugroup   = -1;
    r.ugType   = -1;
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

    // A grouped entity is listed under its group and nowhere else, so the type sections
    // count and show only what is NOT in a group.  Every func_group is a group, which
    // leaves Brushes with the loose worldspawn brushes alone.
    int count = 0;
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        if ( e != world_entity && EntitySection( e ) == sectionKind && !UGHas( e ) )
            ++count;
    }
    if ( sectionKind == KOUT_SECTION_BRUSHES && world_entity )
        count += EntityBrushCount( world_entity, KOUT_FILTER_NOT_TERRAIN );
    s_rows[sectionRow].count = count;

    if ( Collapsed( sectionKey ) )
        return;

    // KIWI (2026-09-16): the Barbwire folder comes first inside Models; its members are
    // listed one level deeper and skipped by the flat pass below.
    if ( sectionKind == KOUT_SECTION_MODELS )
    {
        int wires = 0;
        for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
            if ( e != world_entity && IsBarbwireEntity( e ) && EntitySection( e ) == sectionKind
              && !UGHas( e ) )
                ++wires;
        if ( wires )
        {
            PushRow( KOUT_BARBWIRE_FOLDER, 1, KOUT_KEY_BARBWIRE );
            s_rows.back().count = wires;
            if ( !Collapsed( KOUT_KEY_BARBWIRE ) )
            {
                for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
                {
                    if ( e == world_entity || !IsBarbwireEntity( e ) || EntitySection( e ) != sectionKind
                      || UGHas( e ) )
                        continue;
                    entity_s_def *def = DefOf( e );
                    const unsigned key = def ? EntKey( def->numberId ) : 0u;
                    PushRow( KOUT_ENTITY, 2, key );
                    s_rows.back().ent   = e;
                    s_rows.back().count = EntityBrushCount( e );
                    if ( !Collapsed( key ) )
                        FlattenEntityBrushes( e, 3 );
                }
            }
        }
    }

    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        if ( e == world_entity || EntitySection( e ) != sectionKind )
            continue;
        if ( sectionKind == KOUT_SECTION_MODELS && IsBarbwireEntity( e ) )
            continue;                               // listed under the Barbwire folder
        if ( UGHas( e ) )
            continue;                               // listed once, under its group

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

// One group: nested groups first, then a folder per kind of member.  The Brushes folder
// lists the brushes THEMSELVES across every func_group at this path - the wrapper is
// plumbing and never gets a row; the other kinds list their entities.
void FlattenUGroup( int idx, int indent )
{
    const unsigned key = UGKey( s_ug[idx].path );
    PushRow( KOUT_UGROUP, indent, key );
    s_rows.back().ugroup = idx;
    s_rows.back().count  = s_ug[idx].total;
    if ( Collapsed( key ) )
        return;

    for ( size_t i = 0; i < s_ug[idx].kids.size(); ++i )
        FlattenUGroup( s_ug[idx].kids[i], indent + 1 );

    static const koutKind_t kinds[4] = { KOUT_SECTION_BRUSHES, KOUT_SECTION_MODELS,
                                         KOUT_SECTION_LIGHTS,  KOUT_SECTION_ENTITIES };
    for ( int k = 0; k < 4; ++k )
    {
        std::vector<entity_s *> ents;
        UGMembers( idx, (int)kinds[k], false, ents );
        if ( ents.empty() )
            continue;
        int count = (int)ents.size();
        if ( kinds[k] == KOUT_SECTION_BRUSHES )
        {
            count = 0;
            for ( size_t i = 0; i < ents.size(); ++i )
                count += EntityBrushCount( ents[i] );
        }
        const unsigned tkey = UGTypeKey( s_ug[idx].path, (int)kinds[k] );
        PushRow( KOUT_UGROUP_TYPE, indent + 1, tkey );
        s_rows.back().ugroup = idx;
        s_rows.back().ugType = (int)kinds[k];
        s_rows.back().count  = count;
        if ( Collapsed( tkey ) )
            continue;

        if ( kinds[k] == KOUT_SECTION_BRUSHES )
        {
            int ordinal = 0;
            for ( size_t i = 0; i < ents.size(); ++i )
                for ( selbrush_t *b = ents[i]->brushes.ownerNext;
                      b && b != &ents[i]->brushes; b = b->ownerNext )
                {
                    PushRow( KOUT_BRUSH, indent + 2, 0 );
                    s_rows.back().inst    = b;
                    s_rows.back().ent     = ents[i];
                    s_rows.back().ordinal = ++ordinal;
                }
            continue;
        }
        for ( size_t i = 0; i < ents.size(); ++i )
        {
            entity_s_def *def = DefOf( ents[i] );
            const unsigned ekey = def ? EntKey( def->numberId ) : 0u;
            PushRow( KOUT_ENTITY, indent + 2, ekey );
            s_rows.back().ent   = ents[i];
            s_rows.back().count = EntityBrushCount( ents[i] );
            if ( !Collapsed( ekey ) )
                FlattenEntityBrushes( ents[i], indent + 3 );
        }
    }
}

void FlattenGroups()
{
    int top = 0;
    for ( size_t i = 0; i < s_ug.size(); ++i )
        if ( s_ug[i].parent < 0 )
            ++top;
    PushRow( KOUT_SECTION_GROUPS, 0, KOUT_KEY_GROUPS );
    s_rows.back().count = top;
    if ( Collapsed( KOUT_KEY_GROUPS ) )
        return;
    // s_ugByPath is ordered, so the list is alphabetical and does not reshuffle when the
    // entity list does.
    for ( std::map<std::string, int>::const_iterator it = s_ugByPath.begin();
          it != s_ugByPath.end(); ++it )
        if ( s_ug[it->second].parent < 0 )
            FlattenUGroup( it->second, 1 );
}

// The member entities a group row stands for: everything at or below a group, or one
// kind at exactly that group for a per-kind folder.
void UGRowEntities( const koutRow_t &r, std::vector<entity_s *> &out )
{
    if ( r.kind == KOUT_UGROUP )
        UGMembers( r.ugroup, -1, true, out );
    else if ( r.kind == KOUT_UGROUP_TYPE )
        UGMembers( r.ugroup, r.ugType, false, out );
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
    UGBuildTree();                  // the sections below skip whatever is in a group
    FlattenGroups();
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

        // KIWI (2026-09-21, user: "the models tab needs to not expand each time I move a
        // model. Keep it collapsed, it's over 800"): same rule as Terrain above.  Every
        // drag step bumps the selection generation, so this pass re-opened the section
        // the moment it was closed.  The entity's own folder is still opened, so the
        // selected row is in view as soon as the section is opened by hand.
        const koutKind_t section = EntitySection( e );
        // A grouped object is listed under its group: open Groups and the chain of
        // groups down to it.  Of the per-kind folders only Brushes opens by itself - a
        // village's Models folder is the same 800 rows the Models rule above is about.
        const std::string path = UGPathOf( e );
        if ( !path.empty() )
        {
            SetCollapsed( KOUT_KEY_GROUPS, false );
            for ( std::string p = path; !p.empty(); p = UGParent( p ) )
                SetCollapsed( UGKey( p ), false );
            if ( section == KOUT_SECTION_BRUSHES )
                SetCollapsed( UGTypeKey( path, (int)section ), false );
        }
        else if ( section != KOUT_SECTION_MODELS )
            SetCollapsed( SectionKey( section ), false );
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

// What of `insts` may change owner: live, not already in `targetInst`, and not the
// bounding box of a point entity - that brush IS the model / light / node, and taking
// it out of its entity would orphan the entity (Entity_Create, entity.cpp:1829-1840).
// A selection made in the viewport routinely carries a few of those along.
void FilterMovable( const std::vector<selbrush_t *> &insts, entity_s *targetInst,
                    std::vector<selbrush_t *> &move, int *pointsSkipped )
{
    if ( pointsSkipped )
        *pointsSkipped = 0;
    for ( size_t i = 0; i < insts.size(); ++i )
    {
        selbrush_t *sb = insts[i];
        if ( !sb || !Sel_BrushLive( sb ) )      // the liveness guard, before any deref
            continue;
        entity_s_def *ownerDef = sb->owner ? DefOf( sb->owner ) : nullptr;
        if ( ownerDef && ownerDef->eclass && ownerDef->eclass->fixedsize )
        {
            if ( pointsSkipped )
                ++*pointsSkipped;
            continue;
        }
        if ( targetInst && sb->owner == targetInst )
            continue;
        move.push_back( sb );
    }
}

// Inside an open undo record, BEFORE any relink.  A func_group that this move drains
// completely is recorded whole (Undo_AddEntity_W, entity + brushes - what UngroupEntity
// records) and returned in `emptied` for the caller to free after the relinks: an empty
// func_group is a junk entity in the saved map.  Every other brush is recorded singly.
void RecordMoveForUndo( const std::vector<selbrush_t *> &move, std::vector<entity_s *> &emptied )
{
    for ( size_t i = 0; i < move.size(); ++i )
    {
        entity_s *owner = move[i]->owner;
        if ( !owner || owner == world_entity || !IsFuncGroup( owner ) )
            continue;
        bool seen = false;
        for ( size_t k = 0; k < emptied.size() && !seen; ++k )
            seen = ( emptied[k] == owner );
        if ( seen )
            continue;
        int leaving = 0;
        for ( size_t k = 0; k < move.size(); ++k )
            if ( move[k]->owner == owner )
                ++leaving;
        if ( leaving == EntityBrushCount( owner ) )
            emptied.push_back( owner );
    }
    for ( size_t i = 0; i < emptied.size(); ++i )
        if ( entity_s_def *def = DefOf( emptied[i] ) )
            Undo_AddEntity_W( (entity_s *)def );
    for ( size_t i = 0; i < move.size(); ++i )
    {
        bool whole = false;
        for ( size_t k = 0; k < emptied.size() && !whole; ++k )
            whole = ( emptied[k] == move[i]->owner );
        if ( !whole )
            Undo_AddBrush( (entity_brush_s *)move[i]->def );
    }
}

// One brush into `targetInst`: the ported unlink / link / instance-link / rebuild tail.
void RelinkBrush( selbrush_t *sb, entity_s *targetInst, entity_s_def *targetDef )
{
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
    int points = 0;
    FilterMovable( insts, targetInst, move, &points );
    if ( move.empty() )
    {
        // Report a valid no-op drop instead of appearing to ignore it.
        if ( points > 0 && points == (int)insts.size() )
            Sys_Printf( "Outliner: models, lights and other point entities cannot go in a "
                        "brush group — nothing moved.\n" );
        else
            Sys_Printf( "Outliner: nothing to move — the %i brush(es) are already in %s.\n",
                        (int)insts.size(), ClassOf( targetInst ) );
        return false;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( op );
    std::vector<entity_s *> emptied;
    RecordMoveForUndo( move, emptied );                      // BEFORE the move
    for ( size_t i = 0; i < move.size(); ++i )
        RelinkBrush( move[i], targetInst, targetDef );
    for ( size_t i = 0; i < emptied.size(); ++i )
        Entity_Free( (char *)emptied[i] );                   // select.cpp:5146
    Undo_End();

    g_nUpdateBits = -1;
    Sel_InvalidateFromLegacy();     // the instances did not move, but their owners did
    s_structural = true;            // the owner chains the flatten walked have changed
    Sys_Printf( "Outliner: moved %i brush(es)%s.\n", (int)move.size(),
                emptied.empty() ? "" : "; the group(s) left empty were removed" );
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

// ── Universal group operations ───────────────────────────────────────────────────────
// What a set of picked brushes MEANS for grouping: an object in the viewport is a brush
// instance, but the thing that joins a group is its entity - except for worldspawn
// brushes and for PART of a func_group, which travel as brushes into a wrapper.
struct ugUnits_t
{
    std::vector<entity_s *>   ents;     // models, lights, brush entities, func_groups picked whole
    std::vector<selbrush_t *> loose;    // worldspawn brushes / patches, and part of a func_group
};

void UGUnitsFrom( const std::vector<selbrush_t *> &picked, ugUnits_t &u )
{
    std::map<entity_s *, int> pickedOf;
    for ( size_t i = 0; i < picked.size(); ++i )
        if ( picked[i] && Sel_BrushLive( picked[i] ) && picked[i]->owner )
            ++pickedOf[picked[i]->owner];

    std::map<entity_s *, bool> added;
    for ( size_t i = 0; i < picked.size(); ++i )
    {
        selbrush_t *sb = picked[i];
        if ( !sb || !Sel_BrushLive( sb ) || !sb->owner )
            continue;
        entity_s *e = sb->owner;
        if ( e == world_entity )
        {
            u.loose.push_back( sb );
            continue;
        }
        // Only a func_group can be taken apart; a trigger or a script_brushmodel is one
        // object however many of its brushes were clicked.
        if ( IsFuncGroup( e ) && pickedOf[e] < EntityBrushCount( e ) )
        {
            u.loose.push_back( sb );
            continue;
        }
        if ( !added[e] )
        {
            added[e] = true;
            u.ents.push_back( e );
        }
    }
}

// One legacy undo record for a whole group edit.  The legacy undo has no nesting, so every
// step below assumes the record is open and nothing else opens one until UGTxnEnd.
struct ugTxn_t
{
    std::vector<entity_s *>   recorded;     // snapshotted (or born) inside this record
    std::vector<entity_s *>   freeAfter;    // func_groups drained by it
    std::vector<selbrush_t *> reselect;     // the selection to put back
};

void UGTxnBegin( ugTxn_t &t, const char *op, bool clearSelection )
{
    // Entity_Create reads the legacy selection and merges into / refuses over whatever
    // entity it touches (entity.cpp:1666-1749).  A wrapper has to be born EMPTY, so the
    // selection is parked for the length of the record.
    if ( clearSelection )
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
            if ( sel.items[i].kind == SEL_OBJECT && sel.items[i].brush )
                t.reselect.push_back( sel.items[i].brush );
        Sel_Clear( KiwiSel() );          // construction curves are not Entity_Create's business
        Sel_SyncToLegacy();
    }
    Undo_ClearRedo();
    Undo_GeneralStart( op );
}

void UGTxnRecord( ugTxn_t &t, entity_s *e )
{
    for ( size_t i = 0; i < t.recorded.size(); ++i )
        if ( t.recorded[i] == e )
            return;
    if ( entity_s_def *def = DefOf( e ) )
        Undo_AddEntity_W( (entity_s *)def );         // BEFORE the first change to it
    t.recorded.push_back( e );
}

void UGTxnSetPath( ugTxn_t &t, entity_s *e, const std::string &path )
{
    entity_s_def *def = DefOf( e );
    if ( !def )
        return;
    UGTxnRecord( t, e );
    if ( path.empty() )
        DeleteKey( &def->epairs, KOUT_UG_KEY );
    else
        SetKeyValue( def, KOUT_UG_KEY, path.c_str() );
}

// A func_group back into worldspawn; the emptied entity is freed when the record closes.
// A func_group cannot simply lose its key - untagged it is still a group, by name.
void UGTxnDissolve( ugTxn_t &t, entity_s *e )
{
    if ( !e || e == world_entity || !world_entity )
        return;
    UGTxnRecord( t, e );
    entity_s_def *worldDef = (entity_s_def *)world_entity->def;
    for ( selbrush_t *b = e->brushes.ownerNext; b && b != &e->brushes; )
    {
        selbrush_t *next = b->ownerNext;                 // the relink rewrites b's links
        RelinkBrush( b, world_entity, worldDef );
        b = next;
    }
    t.freeAfter.push_back( e );
}

// Snapshot `loose` for a move into `target` (null = a wrapper made later); returns what
// will actually move.  Split from the relink because the proven order is brushes first,
// then the new entity and its id (KiwiOutliner's original group path, pmesh.cpp:7396).
void UGTxnRecordLoose( ugTxn_t &t, const std::vector<selbrush_t *> &loose, entity_s *target,
                       std::vector<selbrush_t *> &move )
{
    FilterMovable( loose, target, move, nullptr );
    std::vector<entity_s *> emptied;
    RecordMoveForUndo( move, emptied );
    for ( size_t i = 0; i < emptied.size(); ++i )
    {
        t.recorded.push_back( emptied[i] );
        t.freeAfter.push_back( emptied[i] );
    }
}

entity_s *UGTxnNewWrapper( ugTxn_t &t, const std::string &path )
{
    eclass_t *ec = Eclass_ForName( 0, "func_group" );            // eclass.cpp:1138
    if ( !ec || selected_brushes.next != &selected_brushes )
        return nullptr;
    entity_s     *inst = Entity_Create( ec );                    // empty: its LABEL_73 tail
    entity_s_def *def  = DefOf( inst );
    if ( !def )
        return nullptr;
    Undo_SetIdForEntity( def );
    SetKeyValue( def, KOUT_UG_KEY, path.c_str() );
    t.recorded.push_back( inst );                                // born here: no snapshot
    return inst;
}

void UGTxnEnd( ugTxn_t &t )
{
    for ( size_t i = 0; i < t.freeAfter.size(); ++i )
        Entity_Free( (char *)t.freeAfter[i] );                   // select.cpp:5146
    Undo_End();
    Sel_InvalidateFromLegacy();
    if ( !t.reselect.empty() )
    {
        selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < t.reselect.size(); ++i )
            if ( Sel_BrushLive( t.reselect[i] ) )
                Sel_Add( sel, Sel_MakeObject( t.reselect[i] ) );
        Sel_SyncToLegacy();
    }
    g_nUpdateBits = -1;
    s_structural  = true;
}

void UGReveal( const std::string &path )
{
    SetCollapsed( KOUT_KEY_GROUPS, false );
    for ( std::string p = path; !p.empty(); p = UGParent( p ) )
        SetCollapsed( UGKey( p ), false );
}

// Loose brushes into group `path`: the group's existing wrapper when it has one, else a
// new one.  Brushes already in ANY wrapper of that group stay where they are.
int UGTxnLooseInto( ugTxn_t &t, const std::vector<selbrush_t *> &loose, const std::string &path )
{
    std::vector<selbrush_t *> want;
    for ( size_t i = 0; i < loose.size(); ++i )
        if ( loose[i]->owner == world_entity || UGPathOf( loose[i]->owner ) != path )
            want.push_back( loose[i] );
    if ( want.empty() )
        return 0;

    entity_s *target = nullptr;
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts && !target; e = e->next )
        if ( e != world_entity && IsFuncGroup( e ) && UGPathOf( e ) == path )
        {
            bool dying = false;
            for ( size_t k = 0; k < t.freeAfter.size() && !dying; ++k )
                dying = ( t.freeAfter[k] == e );
            if ( !dying )
                target = e;
        }

    std::vector<selbrush_t *> move;
    UGTxnRecordLoose( t, want, target, move );
    if ( move.empty() )
        return 0;
    if ( !target )
        target = UGTxnNewWrapper( t, path );
    if ( !target )
    {
        Sys_Printf( "Groups: could not make a brush holder for \"%s\" - its brushes were "
                    "left where they are.\n", path.c_str() );
        return 0;
    }
    for ( size_t i = 0; i < move.size(); ++i )
        RelinkBrush( move[i], target, DefOf( target ) );
    return (int)move.size();
}

// Requires UGBuildTree.
std::string UGUniqueChild( const std::string &parent )
{
    for ( int n = 1; ; ++n )
    {
        char nm[32];
        _snprintf( nm, sizeof( nm ), "group_%i", n );
        nm[sizeof( nm ) - 1] = '\0';
        if ( UGFind( UGJoin( parent, nm ) ) < 0 )
            return nm;
    }
}

// A NEW group from `picked`.  It is made INSIDE the deepest group everything picked
// already shares, and whatever structure the members had below that point comes along:
//   loose brushes + models                      -> group_1
//   parts of house_1 (a knob's brushes + model) -> house_1/group_1        (a sub-group)
//   all of house_1 and all of house_2           -> group_1/house_1, group_1/house_2
bool UGCreate( const std::vector<selbrush_t *> &picked, std::string *outPath )
{
    ugUnits_t u;
    UGUnitsFrom( picked, u );
    if ( u.ents.empty() && u.loose.empty() )
    {
        Sys_Printf( "Groups: select the brushes, models and other objects to group first.\n" );
        return false;
    }
    UGBuildTree();

    std::vector<std::string> entPath( u.ents.size() ), loosePath( u.loose.size() );
    std::string common;
    bool        first = true;
    for ( size_t i = 0; i < u.ents.size(); ++i )
    {
        entPath[i] = UGPathOf( u.ents[i] );
        common     = first ? entPath[i] : UGCommon( common, entPath[i] );
        first      = false;
    }
    for ( size_t i = 0; i < u.loose.size(); ++i )
    {
        loosePath[i] = UGPathOf( u.loose[i]->owner );            // "" for worldspawn
        common       = first ? loosePath[i] : UGCommon( common, loosePath[i] );
        first        = false;
    }
    const std::string path = UGJoin( common, UGUniqueChild( common ) );

    ugTxn_t t;
    UGTxnBegin( t, "group", true );
    for ( size_t i = 0; i < u.ents.size(); ++i )
        UGTxnRecord( t, u.ents[i] );

    std::map<std::string, std::vector<selbrush_t *> > byBelow;
    for ( size_t i = 0; i < u.loose.size(); ++i )
        byBelow[UGBelow( loosePath[i], common )].push_back( u.loose[i] );
    int brushes = 0;
    for ( std::map<std::string, std::vector<selbrush_t *> >::iterator it = byBelow.begin();
          it != byBelow.end(); ++it )
        brushes += UGTxnLooseInto( t, it->second, UGJoin( path, it->first ) );

    for ( size_t i = 0; i < u.ents.size(); ++i )
        UGTxnSetPath( t, u.ents[i], UGJoin( path, UGBelow( entPath[i], common ) ) );
    UGTxnEnd( t );

    UGReveal( path );
    Sys_Printf( "Groups: \"%s\" - %i object(s) and %i loose brush(es).\n", path.c_str(),
                (int)u.ents.size(), brushes );
    if ( outPath )
        *outPath = path;
    return true;
}

// `picked` joins the existing group `path`, flat: whatever groups it was in are left.
bool UGAddTo( const std::string &path, const std::vector<selbrush_t *> &picked )
{
    if ( path.empty() )
        return false;
    ugUnits_t u;
    UGUnitsFrom( picked, u );
    std::vector<entity_s *> ents;
    for ( size_t i = 0; i < u.ents.size(); ++i )
        if ( UGPathOf( u.ents[i] ) != path )
            ents.push_back( u.ents[i] );
    bool anyLoose = false;
    for ( size_t i = 0; i < u.loose.size() && !anyLoose; ++i )
        anyLoose = ( u.loose[i]->owner == world_entity || UGPathOf( u.loose[i]->owner ) != path );
    if ( ents.empty() && !anyLoose )
    {
        Sys_Printf( "Groups: nothing to add - it is all in \"%s\" already.\n", path.c_str() );
        return false;
    }

    ugTxn_t t;
    UGTxnBegin( t, "add to group", true );
    for ( size_t i = 0; i < ents.size(); ++i )
        UGTxnRecord( t, ents[i] );
    const int brushes = UGTxnLooseInto( t, u.loose, path );
    for ( size_t i = 0; i < ents.size(); ++i )
        UGTxnSetPath( t, ents[i], path );
    UGTxnEnd( t );

    UGReveal( path );
    Sys_Printf( "Groups: added %i object(s) and %i loose brush(es) to \"%s\".\n",
                (int)ents.size(), brushes, path.c_str() );
    return true;
}

// `picked` leaves every group.  Brushes go back to worldspawn.
bool UGRemove( const std::vector<selbrush_t *> &picked )
{
    ugUnits_t u;
    UGUnitsFrom( picked, u );
    std::vector<entity_s *>   ents;
    std::vector<selbrush_t *> loose;
    for ( size_t i = 0; i < u.ents.size(); ++i )
        if ( !UGPathOf( u.ents[i] ).empty() )
            ents.push_back( u.ents[i] );
    for ( size_t i = 0; i < u.loose.size(); ++i )
        if ( u.loose[i]->owner != world_entity )
            loose.push_back( u.loose[i] );
    if ( ents.empty() && loose.empty() )
    {
        Sys_Printf( "Groups: nothing selected is in a group.\n" );
        return false;
    }

    ugTxn_t t;
    UGTxnBegin( t, "remove from group", false );
    std::vector<selbrush_t *> move;
    UGTxnRecordLoose( t, loose, world_entity, move );
    for ( size_t i = 0; i < move.size(); ++i )
        RelinkBrush( move[i], world_entity, (entity_s_def *)world_entity->def );
    for ( size_t i = 0; i < ents.size(); ++i )
    {
        if ( IsFuncGroup( ents[i] ) ) UGTxnDissolve( t, ents[i] );
        else                          UGTxnSetPath( t, ents[i], std::string() );
    }
    UGTxnEnd( t );
    Sys_Printf( "Groups: %i object(s) and %i brush(es) taken out of their groups.\n",
                (int)ents.size(), (int)move.size() );
    return true;
}

// Dissolve ONE level, inside an open record: members of `path` move up to its parent (out
// of all groups when it has none) and its sub-groups become the parent's.
int UGTxnUngroup( ugTxn_t &t, const std::string &path )
{
    const std::string up = UGParent( path );
    std::vector<entity_s *>  ents;
    std::vector<std::string> paths;
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        const std::string p = UGPathOf( e );
        if ( p.empty() || !UGIsUnder( p, path ) )
            continue;
        ents.push_back( e );
        paths.push_back( p );
    }
    for ( size_t i = 0; i < ents.size(); ++i )
    {
        const std::string np = UGJoin( up, UGBelow( paths[i], path ) );
        if ( !np.empty() )                  UGTxnSetPath( t, ents[i], np );
        else if ( IsFuncGroup( ents[i] ) )  UGTxnDissolve( t, ents[i] );
        else                                UGTxnSetPath( t, ents[i], std::string() );
    }
    return (int)ents.size();
}

bool UGUngroup( const std::string &path )
{
    if ( path.empty() )
        return false;
    ugTxn_t t;
    UGTxnBegin( t, "ungroup", false );
    const int n = UGTxnUngroup( t, path );
    UGTxnEnd( t );
    Sys_Printf( "Groups: \"%s\" dissolved (%i object(s) moved %s).\n", path.c_str(), n,
                UGParent( path ).empty() ? "out of groups" : "up one level" );
    return n > 0;
}

bool UGRename( const std::string &path, const char *newLeaf )
{
    const std::string leaf = UGCleanName( newLeaf );
    if ( path.empty() || leaf.empty() )
        return false;
    const std::string np = UGJoin( UGParent( path ), leaf );
    if ( np == path )
        return false;
    UGBuildTree();
    if ( UGFind( np ) >= 0 )
    {
        Sys_Printf( "Groups: there is already a group called \"%s\" there.\n", np.c_str() );
        return false;
    }
    std::vector<entity_s *>  ents;
    std::vector<std::string> paths;
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        const std::string p = UGPathOf( e );
        if ( !p.empty() && UGIsUnder( p, path ) )
        {
            ents.push_back( e );
            paths.push_back( p );
        }
    }
    const bool wasClosed = Collapsed( UGKey( path ) );
    ugTxn_t t;
    UGTxnBegin( t, "rename group", false );
    for ( size_t i = 0; i < ents.size(); ++i )
        UGTxnSetPath( t, ents[i], UGJoin( np, UGBelow( paths[i], path ) ) );
    UGTxnEnd( t );
    SetCollapsed( UGKey( np ), wasClosed );
    return true;
}

// Every live brush of every member at or below `path`.
void UGBrushesOf( const std::string &path, std::vector<selbrush_t *> &out )
{
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        if ( e == world_entity )
            continue;
        const std::string p = UGPathOf( e );
        if ( p.empty() || !UGIsUnder( p, path ) )
            continue;
        for ( selbrush_t *b = e->brushes.ownerNext; b && b != &e->brushes; b = b->ownerNext )
            if ( Sel_BrushLive( b ) )
                out.push_back( b );
    }
}

// The group path of a picked brush: its entity's, "" for a worldspawn brush.
std::string UGPathOfBrush( const selbrush_t *sb )
{
    return ( sb && sb->owner && sb->owner != world_entity ) ? UGPathOf( sb->owner ) : std::string();
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
        || k == KOUT_SECTION_LIGHTS  || k == KOUT_SECTION_MODELS  || k == KOUT_BARBWIRE_FOLDER
        || k == KOUT_SECTION_GROUPS;
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
        else if ( k == KOUT_BARBWIRE_FOLDER )
        {
            if ( !IsBarbwireEntity( e ) || EntitySection( e ) != KOUT_SECTION_MODELS )
                continue;
        }
        else if ( k == KOUT_SECTION_GROUPS )
        {
            if ( UGPathOf( e ).empty() )            // everything that is in any group
                continue;
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
        || r.kind == KOUT_SECTION_GROUPS       // -> a NEW group
        || r.kind == KOUT_UGROUP               // -> into that group
        || r.kind == KOUT_UGROUP_TYPE          // -> likewise (its group)
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

        if ( target.kind == KOUT_SECTION_GROUPS )
        {
            UGCreate( moving, nullptr );
            return;
        }
        if ( target.kind == KOUT_UGROUP || target.kind == KOUT_UGROUP_TYPE )
        {
            if ( target.ugroup >= 0 && target.ugroup < (int)s_ug.size() )
            {
                const std::string path = s_ug[target.ugroup].path;   // copy: the op rebuilds s_ug
                UGAddTo( path, moving );
            }
            return;
        }

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
    SetCollapsed( KOUT_KEY_MODELS, true );    // same, 2026-09-21: 800+ model rows
    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        if ( e == world_entity )
            continue;
        entity_s_def *def = DefOf( e );
        if ( def )
            SetCollapsed( EntKey( def->numberId ), true );
    }
    // Universal groups load closed: a map with forty houses lists forty lines.
    UGBuildTree();
    for ( size_t g = 0; g < s_ug.size(); ++g )
        SetCollapsed( UGKey( s_ug[g].path ), true );
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
    SetCollapsed( KOUT_KEY_MODELS, true );    // same (KIWI 2026-09-21)
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
                case KOUT_BARBWIRE_FOLDER:
                case KOUT_SECTION_GROUPS:
                    SectionEyeState( r.kind, &hidden, &hasEye );
                    break;
                case KOUT_UGROUP:
                case KOUT_UGROUP_TYPE:
                {
                    std::vector<entity_s *> ents;
                    UGRowEntities( r, ents );
                    int n = 0, nh = 0;
                    for ( size_t k = 0; k < ents.size(); ++k )
                        for ( selbrush_t *b = ents[k]->brushes.ownerNext;
                              b && b != &ents[k]->brushes; b = b->ownerNext )
                        {
                            ++n;
                            if ( BrushHidden( b ) )
                                ++nh;
                        }
                    hidden = ( n > 0 && nh == n );
                    hasEye = ( n > 0 );
                    break;
                }
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
                        else if ( r.kind == KOUT_UGROUP || r.kind == KOUT_UGROUP_TYPE )
                        {
                            // The whole house (or just its models) in one record.
                            std::vector<entity_s *> ents;
                            UGRowEntities( r, ents );
                            KiwiVis_UndoPush( "hide group (outliner)" );
                            for ( size_t k = 0; k < ents.size(); ++k )
                                for ( selbrush_t *b = ents[k]->brushes.ownerNext;
                                      b && b != &ents[k]->brushes; b = b->ownerNext )
                                    SetBrushHidden( b, want );
                            KiwiVis_UndoCommit();
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
                case KOUT_BARBWIRE_FOLDER:
                    _snprintf( label, sizeof( label ), "Barbwire (%i)", r.count );
                    break;
                case KOUT_SECTION_GROUPS:
                    _snprintf( label, sizeof( label ), "Groups (%i)", r.count );
                    break;
                case KOUT_UGROUP:
                    _snprintf( label, sizeof( label ), "%s (%i)",
                               UGLeaf( s_ug[r.ugroup].path ).c_str(), r.count );
                    break;
                case KOUT_UGROUP_TYPE:
                    _snprintf( label, sizeof( label ), "%s (%i)",
                               r.ugType == KOUT_SECTION_BRUSHES ? "Brushes"
                             : r.ugType == KOUT_SECTION_MODELS  ? "Models"
                             : r.ugType == KOUT_SECTION_LIGHTS  ? "Lights" : "Entities",
                               r.count );
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
                            else if ( r.kind == KOUT_UGROUP )
                            {
                                // A copy: the rename rebuilds the tree this row indexes.
                                const std::string path = s_ug[r.ugroup].path;
                                UGRename( path, s_renameBuf );
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
                    if ( s_structural )
                        break;          // a group rename rebuilt the tree the rows index
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
                            else if ( r.kind == KOUT_UGROUP )
                            {
                                BeginRename( r, UGLeaf( s_ug[r.ugroup].path ).c_str() );
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
                        else if ( r.kind == KOUT_GROUP_ENTITY || r.kind == KOUT_ENTITY
                               || r.kind == KOUT_BARBWIRE_FOLDER || r.kind == KOUT_SECTION_GROUPS
                               || r.kind == KOUT_UGROUP || r.kind == KOUT_UGROUP_TYPE )
                        {
                            // The Barbwire and Groups folders act on every member entity
                            // at once; an entity row on its own brushes.
                            std::vector<entity_s *> ents;
                            if ( r.kind == KOUT_BARBWIRE_FOLDER )
                            {
                                for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
                                    if ( e != world_entity && IsBarbwireEntity( e ) )
                                        ents.push_back( e );
                            }
                            else if ( r.kind == KOUT_SECTION_GROUPS )
                            {
                                for ( size_t g = 0; g < s_ug.size(); ++g )
                                    if ( s_ug[g].parent < 0 )
                                        UGMembers( (int)g, -1, true, ents );
                            }
                            else if ( r.kind == KOUT_UGROUP || r.kind == KOUT_UGROUP_TYPE )
                                UGRowEntities( r, ents );   // the house, or just its models
                            else
                                ents.push_back( r.ent );

                            selection_t &sel = KiwiSel();
                            bool any = false;
                            bool all = true;
                            for ( size_t k = 0; k < ents.size(); ++k )
                                for ( selbrush_t *b = ents[k]->brushes.ownerNext;
                                      b && b != &ents[k]->brushes; b = b->ownerNext )
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
                            for ( size_t k = 0; k < ents.size(); ++k )
                                for ( selbrush_t *b = ents[k]->brushes.ownerNext;
                                      b && b != &ents[k]->brushes; b = b->ownerNext )
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
                // Universal groups, without dragging.  On a brush / entity row the menu acts
                // on that object, or on the whole selection when the row is part of it (the
                // drag rule); on a group row it acts on the group.  Every action that
                // edits groups sets s_structural, and nothing below it touches a row again.
                else if ( r.kind == KOUT_BRUSH || r.kind == KOUT_ENTITY
                       || r.kind == KOUT_SECTION_GROUPS || r.kind == KOUT_UGROUP
                       || r.kind == KOUT_UGROUP_TYPE )
                {
                    if ( ImGui::BeginPopupContextItem( "##groupctx" ) )
                    {
                        const bool objectRow = ( r.kind == KOUT_BRUSH || r.kind == KOUT_ENTITY );
                        selbrush_t *rowBrush = r.inst;
                        if ( r.kind == KOUT_ENTITY && r.ent && r.ent->brushes.ownerNext != &r.ent->brushes )
                            rowBrush = r.ent->brushes.ownerNext;     // any brush names its entity
                        std::vector<selbrush_t *> picked;
                        if ( !objectRow || ( rowBrush && Sel_BrushLive( rowBrush ) && BrushSelected( rowBrush ) ) )
                            GatherSelectedBrushes( picked );
                        else if ( rowBrush && Sel_BrushLive( rowBrush ) )
                            picked.push_back( rowBrush );

                        const std::string rowPath = ( r.ugroup >= 0 && r.ugroup < (int)s_ug.size() )
                                                  ? s_ug[r.ugroup].path : std::string();
                        char item[96];

                        if ( r.kind == KOUT_UGROUP )
                        {
                            _snprintf( item, sizeof( item ), "Add selection to \"%s\"",
                                       UGLeaf( rowPath ).c_str() );
                            item[sizeof( item ) - 1] = '\0';
                            if ( ImGui::MenuItem( item, 0, false, !picked.empty() ) )
                                UGAddTo( rowPath, picked );
                            if ( !s_structural && ImGui::MenuItem( "Rename" ) )
                                BeginRename( r, UGLeaf( rowPath ).c_str() );
                            if ( !s_structural && ImGui::MenuItem( "Ungroup" ) )
                                UGUngroup( rowPath );
                            if ( !s_structural )
                                ImGui::Separator();
                        }

                        if ( !s_structural && r.kind != KOUT_UGROUP_TYPE )
                        {
                            _snprintf( item, sizeof( item ), "New group from %s",
                                       ( objectRow && picked.size() == 1 ) ? "this" : "selection" );
                            item[sizeof( item ) - 1] = '\0';
                            if ( ImGui::MenuItem( item, 0, false, !picked.empty() ) )
                                UGCreate( picked, nullptr );
                        }

                        if ( !s_structural && objectRow && !picked.empty()
                          && ImGui::BeginMenu( "Add to group", !s_ugByPath.empty() ) )
                        {
                            const std::string mine = UGPathOfBrush( rowBrush );
                            // Full paths, in name order: "village/house_1/door".
                            std::string chosen;
                            for ( std::map<std::string, int>::const_iterator it = s_ugByPath.begin();
                                  it != s_ugByPath.end(); ++it )
                                if ( ImGui::MenuItem( it->first.c_str(), 0, it->first == mine ) )
                                    chosen = it->first;
                            ImGui::EndMenu();
                            if ( !chosen.empty() )
                                UGAddTo( chosen, picked );      // after the loop: it rebuilds the map
                        }
                        if ( !s_structural && objectRow && !UGPathOfBrush( rowBrush ).empty()
                          && ImGui::MenuItem( "Remove from group" ) )
                            UGRemove( picked );
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
                // An entity row (a model, a light) drags as its first brush: a picked brush
                // already names its entity everywhere groups are concerned (UGUnitsFrom),
                // and the plain brush drops refuse a point entity's box (FilterMovable).
                const bool entityDrag = r.kind == KOUT_ENTITY && r.ent
                                     && r.ent->brushes.ownerNext != &r.ent->brushes;
                if ( !io.KeyShift && !s_paintSelecting
                  && ( r.kind == KOUT_BRUSH || r.kind == KOUT_CON_OBJECT || r.kind == KOUT_IMAGE
                    || entityDrag ) )
                {
                    if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
                    {
                        koutDrag_t d;
                        d.kind       = entityDrag ? (int)KOUT_BRUSH : (int)r.kind;
                        d.inst       = entityDrag ? r.ent->brushes.ownerNext : r.inst;
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
    // Anything selectable as an object can be grouped: brushes, patches, models, lights,
    // other entities, existing groups.
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    return !brushes.empty() || KiwiConSel_Count() > 0;
}

bool KiwiOutliner_GroupSelection()
{
    bool did = false;

    // Object half: a universal group (UGCreate has the nesting rules).
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    if ( !brushes.empty() && UGCreate( brushes, nullptr ) )
        did = true;

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

// The ONE group the selection should join: every selected object that is in a group is
// in the same one, and at least one selected object is not in it.  "" otherwise.
static std::string JoinTargetOfSelection( const std::vector<selbrush_t *> &brushes )
{
    std::string target;
    bool        outsider = false;
    for ( size_t i = 0; i < brushes.size(); ++i )
    {
        const std::string p = UGPathOfBrush( brushes[i] );
        if ( p.empty() )                          outsider = true;
        else if ( target.empty() )                target = p;
        else if ( p != target )                   return std::string();
    }
    return outsider ? target : std::string();
}

bool KiwiOutliner_CanAddToGroup()
{
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    return !JoinTargetOfSelection( brushes ).empty();
}

// Select some of a house plus the new door handle, run this: the handle joins the house.
bool KiwiOutliner_AddSelectionToGroup()
{
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    const std::string target = JoinTargetOfSelection( brushes );
    if ( target.empty() )
    {
        Sys_Printf( "Groups: select members of ONE group together with the ungrouped objects "
                    "that should join it (or right-click the group in the outliner > Add "
                    "selection).\n" );
        return false;
    }
    return UGAddTo( target, brushes );
}

bool KiwiOutliner_CanSelectGroup()
{
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    for ( size_t i = 0; i < brushes.size(); ++i )
        if ( !UGPathOfBrush( brushes[i] ).empty() )
            return true;
    return false;
}

// Grow the selection to whole groups: first the group each selected object is in; run
// again once that is fully selected and it grows to the group containing it (knob ->
// door -> house -> village).
bool KiwiOutliner_SelectWholeGroup()
{
    std::vector<selbrush_t *> brushes;
    GatherSelectedBrushes( brushes );
    std::map<const selbrush_t *, bool> have;
    for ( size_t i = 0; i < brushes.size(); ++i )
        have[brushes[i]] = true;

    std::map<std::string, bool> grow;
    for ( size_t i = 0; i < brushes.size(); ++i )
    {
        std::string p = UGPathOfBrush( brushes[i] );
        if ( p.empty() )
            continue;
        for ( ;; )
        {
            std::vector<selbrush_t *> all;
            UGBrushesOf( p, all );
            bool full = true;
            for ( size_t k = 0; k < all.size() && full; ++k )
                full = have.find( all[k] ) != have.end();
            if ( !full || UGParent( p ).empty() )
                break;
            p = UGParent( p );
        }
        grow[p] = true;
    }
    if ( grow.empty() )
    {
        Sys_Printf( "Groups: nothing selected is in a group.\n" );
        return false;
    }
    selection_t &sel = KiwiSel();
    int added = 0;
    for ( std::map<std::string, bool>::const_iterator it = grow.begin(); it != grow.end(); ++it )
    {
        std::vector<selbrush_t *> all;
        UGBrushesOf( it->first, all );
        for ( size_t k = 0; k < all.size(); ++k )
            if ( have.find( all[k] ) == have.end() )
            {
                Sel_Add( sel, Sel_MakeObject( all[k] ) );
                have[all[k]] = true;
                ++added;
            }
    }
    Sel_SyncToLegacy();
    g_nUpdateBits = -1;
    return added > 0;
}

// Test DSL exports (kiwi_test.cpp).
int KiwiOutliner_TestGroupCount()
{
    UGBuildTree();
    return (int)s_ug.size();
}

int KiwiOutliner_TestGroupedBrushCount()
{
    std::vector<selbrush_t *> all;
    UGBrushesOf( std::string(), all );
    return (int)all.size();
}

bool KiwiOutliner_CanUngroup()
{
    // Object side: anything selected that is in a group.
    if ( KiwiOutliner_CanSelectGroup() )
        return true;
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

    // Object half: dissolve ONE level - the group each selected object is directly in.
    // Paths are taken before anything changes and handled deepest first, so dissolving
    // "house/door" cannot re-home objects that "house" is about to move as well.
    {
        std::vector<selbrush_t *> brushes;
        GatherSelectedBrushes( brushes );
        std::vector<std::string> paths;
        for ( size_t i = 0; i < brushes.size(); ++i )
        {
            const std::string p = UGPathOfBrush( brushes[i] );
            bool dup = p.empty();
            for ( size_t k = 0; k < paths.size() && !dup; ++k )
                dup = ( paths[k] == p );
            if ( !dup )
                paths.push_back( p );
        }
        // A group nested inside another selected group goes with its parent's pass.
        std::vector<std::string> tops;
        for ( size_t i = 0; i < paths.size(); ++i )
        {
            bool nested = false;
            for ( size_t k = 0; k < paths.size() && !nested; ++k )
                nested = ( k != i && paths[i] != paths[k] && UGIsUnder( paths[i], paths[k] ) );
            if ( !nested )
                tops.push_back( paths[i] );
        }
        if ( !tops.empty() )
        {
            ugTxn_t t;
            UGTxnBegin( t, "ungroup", false );
            int moved = 0;
            for ( size_t i = 0; i < tops.size(); ++i )
                moved += UGTxnUngroup( t, tops[i] );
            UGTxnEnd( t );
            Sys_Printf( "Groups: dissolved %i group(s), %i object(s) moved up.\n",
                        (int)tops.size(), moved );
            did = moved > 0;
        }
    }

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
    Radiant_RegisterCommand( "KiwiGroupAddSelection", 0, 0, KIWI_CMD_GROUP_ADD );
    Radiant_RegisterCommand( "KiwiGroupSelectWhole",  0, 0, KIWI_CMD_GROUP_SELECT );
}

bool KiwiOutliner_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == (unsigned int)KIWI_CMD_GROUP_ADD )
    {
        KiwiOutliner_AddSelectionToGroup();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_GROUP_SELECT )
    {
        KiwiOutliner_SelectWholeGroup();
        return true;
    }
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
