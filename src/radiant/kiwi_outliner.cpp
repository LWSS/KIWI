#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_outliner.cpp — ROUND W implementation.  See kiwi_outliner.h for the user
// directive, the Plasticity sources this ports and the ruling that a brush group
// IS a func_group entity.
//
// NEW code over the KIWI layers plus FOUR ported cores, each called exactly the
// way its existing caller calls it:
//
//   CREATE A GROUP     xywnd.cpp:3411 CreateEntityFromName's bracket, verbatim:
//                        Undo_ClearRedo(); Undo_GeneralStart( op );
//                        Undo_AddBrushList( &selected_brushes );
//                        Entity_Create( Eclass_ForName( 0, "func_group" ) );
//                      plus pmesh.cpp:7396's Undo_SetIdForEntity( newDef ) tail,
//                      which is what makes the NEW entity part of the record
//                      (Undo_Undo phase 2 removes entity instances whose def
//                      carries the record id — undo.cpp:853).
//
//   REPARENT A BRUSH   the three-call triple that is the ONLY spelling of it in
//                      this tree — Entity_Create's own loop (entity.cpp:1697-1699
//                      and :1743-1745) and Select_Ungroup's (select.cpp:5119-5126)
//                      are the same three calls in the same order:
//                        Entity_UnlinkBrush( def );                 // def-list out
//                        Entity_LinkBrush( def, targetDef );        // def-list in
//                        Entity_LinkBrush_0_extern( targetInst, inst ); // owner chain
//                      wrapped in the same Brush_Deselect_Helper / rebuild /
//                      Brush_Select_Helper envelope both of them use.
//
//   UNGROUP            Select_Ungroup's body (select.cpp:5079) reduced to ONE
//                      entity: the same triple back to worldspawn, then
//                      Entity_Free.  It is NOT a call to Select_Ungroup, because
//                      that walks the SELECTION and the outliner's ungroup acts on
//                      the folder row the user right-clicked.
//
//   RENAME             SetKeyValue( def, "targetname", text ) — entity.cpp:209,
//                      the same setter win_ent.cpp:278 EntSetKey_Apply drives.
//
// ── UNDO, AND WHY THE REPARENT IS ACTUALLY COVERED ─────────────────────────
// This was checked in the restore code rather than assumed.  Undo_AddBrush
// (undo.cpp:494) clones the brush def and stores `clone->unk1 =
// brush->owner->numberId` (undo.cpp:527) — the OWNING ENTITY's unique number.
// Undo_Undo's phase 4 (undo.cpp:942-968) re-links each restored def into the
// entity instance whose def carries that numberId, falling back to worldspawn.
// So OWNERSHIP IS PART OF THE RECORD, and a reparent bracketed with an
// Undo_AddBrush per moved brush — taken BEFORE the move, so the OLD owner is what
// gets stored — is undone completely.  Nothing extra is needed and nothing extra
// is added: the entities themselves are deliberately NOT fed to Undo_AddEntity in
// the reparent path, because Undo_AddEntity stamps the LIVE entity
// (undo.cpp:620) and phase 2 would then delete and re-create an entity the user
// never touched.
//
// ── ONE RECORD PER ACT ─────────────────────────────────────────────────────
// Every verb here opens exactly one Undo_GeneralStart / Undo_End bracket, so the
// §39 journal (kiwi_undo.h) mints exactly one ticket and one Ctrl+Z undoes the
// whole regroup, however many brushes it moved.  The construction half pushes
// exactly one KiwiCon_UndoPush before mutating, which is the same promise in the
// other domain.
//
// ── WHAT THIS FILE DOES NOT DO ─────────────────────────────────────────────
// It does not delete, move, duplicate or reshape geometry, and it owns no
// selection of its own.  Every row reads live state as it is drawn and every
// click leaves through the funnels the viewport already uses.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include "kiwi_outliner.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_hover.h"         // ROUND AG, ITEM 3 — the row hover, in 3D
#include "kiwi_selection.h"
#include "kiwi_visibility.h"    // ROUND AG, ITEM 7 — KiwiVis_SetHidden / UndoPush
#include "kiwi_windows.h"

#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118   int Sys_Printf(const char*,...)
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773  int g_nUpdateBits
extern entity_s   *world_entity;                                             // map.cpp:62        entity_s *world_entity
extern entity_s    entityInsts;                                              // entity.cpp:299    entity_s entityInsts{}
extern eclass_t   *Eclass_ForName( int hasBrushes, const char *name );       // eclass.cpp:1096   eclass_t *Eclass_ForName(int,const char*)
extern entity_s   *Entity_Create( eclass_t *eclass );                        // entity.cpp:1629   entity_s *Entity_Create(eclass_t*)
extern void        Entity_Free( char *a1 );                                  // entity.cpp:1500   void Entity_Free(char*)
extern void        Entity_LinkBrush( brush_t *b, entity_s *world_ent );      // entity.cpp:445    void Entity_LinkBrush(brush_t*,entity_s*)
extern void        Entity_UnlinkBrush( brush_t *b );                         // entity.cpp:464    void Entity_UnlinkBrush(brush_t*)
extern selbrush_t *Entity_LinkBrush_0_extern( entity_s *e, entity_brush_s *b );// brush.cpp:635   selbrush_t *Entity_LinkBrush_0_extern(entity_s*,entity_brush_s*)
extern void        Brush_BuildWindings( brush_t *def, int bFull );           // brush.cpp:1434    void Brush_BuildWindings(brush_t*,int)
extern void        SetupVertexSelection();                                   // engine_stubs      void SetupVertexSelection()
extern void        MarkMapModified();                                        // win_qe3.cpp       void MarkMapModified()
extern void        sub_476330( selbrush_t *b );                              // brush.cpp:851     void sub_476330(selbrush_t*)  Brush_Deselect_Helper
extern void        sub_476470( selbrush_t *b );                              // brush.cpp:970     void sub_476470(selbrush_t*)  Brush_Select_Helper
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:212  void SetKeyValue(entity_s_def*,const char*,const char*)
extern char       *ValueForKey2( int e, const char *key );                   // entity.cpp:89     char *ValueForKey2(int,const char*)  ("" when absent)
extern void        Undo_ClearRedo();                                         // undo.cpp:176      void Undo_ClearRedo()
extern void        Undo_GeneralStart( const char *operation );               // undo.cpp:367      void Undo_GeneralStart(const char*)
extern void        Undo_AddBrush( entity_brush_s *pBrushInst );              // undo.cpp:494      void Undo_AddBrush(entity_brush_s*)  -- takes the brush DEF
extern void        Undo_AddBrushList( selbrush_t *sb );                      // undo.cpp:551      void Undo_AddBrushList(selbrush_t*)
extern void        Undo_AddEntity_W( entity_s *a1 );                         // undo.cpp:633      void Undo_AddEntity_W(entity_s*)
extern void        Undo_SetIdForEntity( entity_s_def *ent );                 // undo.cpp:663      void Undo_SetIdForEntity(entity_s_def*)
extern void        Undo_End();                                               // undo.cpp:686      void Undo_End()
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
// KIWI-UX (ROUND W): the live dockspace id, so the JustOpened latch can re-dock
// this window the way the shell re-docks its own (imgui_shell.cpp:476).
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp

// `active_brushes` / `selected_brushes` are the DISPLAY-list sentinels, declared in
// qe3.h:1053/1054 and defined in engine_stubs.cpp:777/778.  Only `selected_brushes`
// is named here, and only as Undo_AddBrushList's argument — exactly as
// xywnd.cpp:3402 passes it.

namespace
{
// ═════════════════════════════════════════════════════════════════════════════
//  THE ROW MODEL
// ═════════════════════════════════════════════════════════════════════════════
// One flat array per frame, built from the live scene plus the collapse set —
// FlattenOutline.ts:13 in C++.  Every row is the SAME HEIGHT, which is what lets
// ImGuiListClipper skip the ones off screen; that is also why the section headers
// are ordinary rows with an arrow glyph rather than ImGui::CollapsingHeader.
enum koutKind_t
{
    KOUT_SECTION_BRUSHES = 0,   // worldspawn brushes + func_group folders
    KOUT_SECTION_CURVES,        // construction objects
    KOUT_SECTION_ENTITIES,      // ordinary brush and point entities
    KOUT_SECTION_LIGHTS,        // CLASS_LIGHT entities
    KOUT_SECTION_MODELS,        // model and prefab entities
    KOUT_GROUP_ENTITY,          // a func_group folder
    KOUT_ENTITY,                // any other brush/point entity, shown by classname
    KOUT_BRUSH,                 // one brush instance
    KOUT_CON_GROUP,             // a construction-group folder
    KOUT_CON_OBJECT,            // one construction object
};

struct koutRow_t
{
    koutKind_t  kind;
    int         indent;
    selbrush_t *inst;       // KOUT_BRUSH             — the INSTANCE node
    entity_s   *ent;        // KOUT_GROUP_ENTITY / KOUT_ENTITY — the INSTANCE node
    int         conIndex;   // KOUT_CON_OBJECT        — store index
    int         conGroup;   // KOUT_CON_GROUP         — group id
    int         ordinal;    // the display number ("Brush 12", "Line 3")
    int         count;      // folders: how many children
    unsigned    key;        // collapse-set key; 0 = not collapsible
};

// ── collapse keys ───────────────────────────────────────────────────────────
// Entity keys are the entity DEF's `numberId` (qe3.h:539, minted by
// dword_739DC4++ in entity.cpp:1716) — unique for the life of the entity and
// stable across frames, which an index or a pointer would not be.
//
// EVERY key is TAGGED into its own high range, and that is not decoration:
// dword_739DC4 starts at ZERO (entity.cpp:289), so an untagged numberId of 0 would
// collide with this file's "0 == this row is not collapsible" sentinel and produce
// one folder in the map that could never be collapsed.  The tags also keep the
// three namespaces apart; both id spaces are small counters, so nothing can carry
// into the tag bits.
inline unsigned EntKey     ( int numberId ) { return 0xD0000000u | (unsigned)numberId; }
inline unsigned ConGroupKey( int group    ) { return 0xE0000000u | (unsigned)group; }
const unsigned KOUT_KEY_BRUSHES  = 0xF0000001u;
const unsigned KOUT_KEY_CURVES   = 0xF0000002u;
const unsigned KOUT_KEY_ENTITIES = 0xF0000003u;
const unsigned KOUT_KEY_LIGHTS   = 0xF0000004u;
const unsigned KOUT_KEY_MODELS   = 0xF0000005u;

const int KOUT_CLASS_LIGHT      = 0x01;
const int KOUT_CLASS_MODELCLASS = 0x08;
const int KOUT_CLASS_PREFAB     = 0x10;

// The BRUSHFLAG_SELECTED bit, spelled as select.cpp:5112 spells it.
const unsigned KOUT_BRUSHFLAG_SELECTED = 0x80u;
// The hidden bit + its depth field, spelled as select.cpp:4168/4180 spell them
// (and as kiwi_visibility.cpp:35 re-states).  ONE state: a brush hidden from this
// panel is hidden to H, to Ctrl+H and to "Show Hidden", because it is the same
// two fields.
const unsigned KOUT_HIDDEN_BIT = 4u;

// ═════════════════════════════════════════════════════════════════════════════
//  PANEL STATE (all of it panel-local; none of it is scene state)
// ═════════════════════════════════════════════════════════════════════════════
std::vector<unsigned>  s_collapsed;        // keys of the folders that are CLOSED
std::vector<koutRow_t> s_rows;             // rebuilt every frame
unsigned               s_lastSelGen = 0;   // for the auto-expand pass

// The shift-range anchor, stored as an IDENTITY rather than a row index: the
// flatten changes shape whenever anything is expanded, deleted or created, and an
// index into last frame's array is a lie the moment it does.
struct koutAnchor_t
{
    koutKind_t  kind     = KOUT_BRUSH;
    selbrush_t *inst     = nullptr;
    int         conIndex = -1;
    bool        valid    = false;
};
koutAnchor_t s_anchor;

// ── INLINE RENAME ───────────────────────────────────────────────────────────
// ROUND W shipped this for FOLDERS only and keyed it on the folder's collapse key,
// which every non-folder row leaves at 0.
//
// ── KIWI-UX (ROUND X, ITEM 10): EVERY ROW THAT HAS SOMEWHERE TO PUT A NAME ──
// USER DIRECTIVE, verbatim: "Move curves into their own section in the 'outliner'.
// Allow renaming of items in the outliner by double clicking the item and typing."
//
// Plasticity renames every RealNodeItem — Solids, Curves, Groups and Empties — with
// the same handler, because `render()`'s double-click hook is unconditional
// (plasticity/src/components/outliner/OutlinerItems.tsx:75-80) and the storage is
// one key→string map that does not care what kind the node is (Nodes.ts:84-98).
// Only its two virtual SECTION headers are excluded, and they are excluded by being
// a different component entirely (Outliner.tsx:164-174, hardcoded 'Curves'/'Solids').
//
// KIWI matches that set except for one kind, and the exception is a storage fact
// rather than a choice: a worldspawn BRUSH has nowhere to keep a name.  It is not an
// entity, so it has no epair; it is not in the sidecar, so it has no editor-side
// record; and the .map format has no per-brush id to key an external table on.  The
// obvious workaround — key names by the brush's ordinal in the worldspawn list — is
// refused deliberately: that ordinal is reordered by CSG, clone, delete, undo and
// by the load order of the .map itself, so a name would silently move to a
// DIFFERENT brush, which is worse than having no name.  See RADIANT_KNOWN_ISSUES.
//
// So the renameable set is: entity folders and entity rows (targetname), construction
// group folders (the store's group-name table) and construction objects (the sidecar
// name added this round, kiwi_construct.h kconObject_t::name).
//
// The row being renamed is held as an IDENTITY rather than as an index, for exactly
// the reason s_anchor is (the flatten changes shape whenever anything is expanded,
// created or deleted).
struct koutRename_t
{
    bool        active   = false;
    koutKind_t  kind     = KOUT_BRUSH;
    unsigned    key      = 0;        // folders (entity / construction group)
    int         conIndex = -1;       // KOUT_CON_OBJECT
};
koutRename_t s_rename;
char     s_renameBuf[64] = { 0 };
bool     s_renameFocus  = false;

// Is THIS row the one being renamed?  Folders compare by collapse key (unique and
// stable per entity / group); a construction object compares by store index, which
// is only meaningful inside one store generation — and the generation cannot move
// while an InputText has focus, because every path that bumps it is a command and
// no command runs while io.WantTextInput is true.
bool RenamingRow( const koutRow_t &r )
{
    if ( !s_rename.active || s_rename.kind != r.kind )
        return false;
    if ( r.kind == KOUT_CON_OBJECT )
        return s_rename.conIndex == r.conIndex;
    return s_rename.key != 0 && s_rename.key == r.key;
}

// True for the kinds that have somewhere to put a name (see the block above).
bool Renameable( koutKind_t k )
{
    return k == KOUT_GROUP_ENTITY || k == KOUT_ENTITY
        || k == KOUT_CON_GROUP    || k == KOUT_CON_OBJECT;
}

// Enter rename mode on `r`, seeded with the CURRENT label so the field doubles as
// a readout the user can edit rather than one they must retype (the grid pill's
// own rule, kiwi_viewcube.cpp:502-508).  `seed` is that label.
void BeginRename( const koutRow_t &r, const char *seed )
{
    s_rename.active   = true;
    s_rename.kind     = r.kind;
    s_rename.key      = r.key;
    s_rename.conIndex = r.conIndex;
    s_renameFocus     = true;
    _snprintf( s_renameBuf, sizeof( s_renameBuf ), "%s", seed ? seed : "" );
    s_renameBuf[sizeof( s_renameBuf ) - 1] = '\0';
}

// Set by any row action that FREES an entity or reorders the construction store.
// The row array was flattened at the top of the frame, so once one of those has
// run every row after it in this frame's list may name something that no longer
// exists — the draw loop STOPS rather than reading it.  It is a one-frame stall
// and the next frame re-flattens; the alternative is a dangling deref inside an
// ImGui loop, which is the exact shape of bug the liveness discipline exists for.
bool s_structural = false;

// Shift+drag paint-select: latched on the press, cleared on release.  It is
// deliberately exclusive with the ImGui drag-drop SOURCE (see the row draw) —
// one gesture cannot mean both "add these rows" and "move this row".
bool s_paintSelecting = false;

// ── the drag payload ────────────────────────────────────────────────────────
// NOT a bare pointer by contract: the descriptor carries what the drop needs to
// re-validate before it dereferences anything.  For a brush that is
// Sel_BrushLive (kiwi_selection.h:117 — pointer comparison against the two
// display lists, safe on freed memory); for a construction object it is the
// store generation, because indices are only meaningful within one generation
// (kiwi_construct.h "nothing else may cache across it").
const char *KOUT_PAYLOAD = "KIWI_OUTLINER_ROW";
struct koutDrag_t
{
    int         kind;
    selbrush_t *inst;
    int         conIndex;
    unsigned    conGen;
};

// ═════════════════════════════════════════════════════════════════════════════
//  SMALL HELPERS
// ═════════════════════════════════════════════════════════════════════════════
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

// KIWI: keep entity bucketing in one place so every section uses the same rule.
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
        const char *model = ValueForKey2( (int)(intptr_t)def, "model" );
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
    case KOUT_SECTION_CURVES:   return KOUT_KEY_CURVES;
    case KOUT_SECTION_ENTITIES: return KOUT_KEY_ENTITIES;
    case KOUT_SECTION_LIGHTS:   return KOUT_KEY_LIGHTS;
    case KOUT_SECTION_MODELS:   return KOUT_KEY_MODELS;
    default:                    return 0;
    }
}

// A folder row's display name: the entity's own `targetname` when it has one,
// otherwise "<klass> N" — Outliner.tsx:158's `getName(object) ?? klass id`.
void FolderName( entity_s *inst, char *out, int outSize )
{
    entity_s_def *def = DefOf( inst );
    const char   *tn  = def ? ValueForKey2( (int)(intptr_t)def, "targetname" ) : "";
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

int EntityBrushCount( entity_s *inst )
{
    int n = 0;
    if ( !inst )
        return 0;
    for ( selbrush_t *b = inst->brushes.ownerNext; b && b != &inst->brushes; b = b->ownerNext )
        ++n;
    return n;
}

// A brush row's own label.  A patch mesh is called a patch — it is still a brush
// to the map format, but calling it "Brush" would be a lie the user can see.
void BrushName( selbrush_t *inst, int ordinal, char *out, int outSize )
{
    const bool isPatch = ( inst && inst->def && inst->def->patch != 0 );
    _snprintf( out, (size_t)outSize, "%s %i", isPatch ? "Patch" : "Brush", ordinal );
    out[outSize - 1] = '\0';
}

bool BrushHidden( const selbrush_t *b )
{
    return b && ( ( (unsigned)b->brushFlags & KOUT_HIDDEN_BIT ) != 0 );
}

// Set/clear the hide state of ONE brush instance, writing the SAME two fields the
// ported family writes: Select_Hide's third pass (select.cpp:4179-4180) sets the
// bit and depth 1, ShowHidden (select.cpp:4249-4250) clears both.
//
// KIWI-UX (ROUND AG, ITEM 7): the body moved to KiwiVis_SetHidden and this is a
// forwarder.  It had been a verbatim private copy, and the round-AG undo made
// that a liability rather than a duplication: a second spelling of "hidden" is a
// second thing the snapshot store would have to be kept in step with.  The
// "no undo bracket, deliberately" sentence that stood here is GONE — the eye
// brackets now, at the click (see the four arms in the row handler).
void SetBrushHidden( selbrush_t *b, bool hidden )
{
    KiwiVis_SetHidden( b, hidden );
}

bool BrushSelected( selbrush_t *b )
{
    // Outliner.tsx:150-152 re-asks per row per render; so does this.  The typed
    // selection is the authority for the UX layer, and Sel_Contains is a short
    // scan over a list that is only ever as long as the user's selection.
    return b && Sel_Contains( KiwiSel(), Sel_MakeObject( b ) );
}

// ═════════════════════════════════════════════════════════════════════════════
//  FLATTEN  (FlattenOutline.ts:13)
// ═════════════════════════════════════════════════════════════════════════════
void PushRow( koutKind_t kind, int indent, unsigned key )
{
    koutRow_t r;
    r.kind     = kind;
    r.indent   = indent;
    r.inst     = 0;
    r.ent      = 0;
    r.conIndex = -1;
    r.conGroup = -1;
    r.ordinal  = 0;
    r.count    = 0;
    r.key      = key;
    s_rows.push_back( r );
}

void FlattenEntityBrushes( entity_s *inst, int indent )
{
    int ordinal = 0;
    for ( selbrush_t *b = inst->brushes.ownerNext; b && b != &inst->brushes; b = b->ownerNext )
    {
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
        count += EntityBrushCount( world_entity );
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
        FlattenEntityBrushes( world_entity, 1 );
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

void Flatten()
{
    s_rows.clear();
    FlattenEntitySection( KOUT_SECTION_BRUSHES );
    FlattenCurves();
    FlattenEntitySection( KOUT_SECTION_ENTITIES );
    FlattenEntitySection( KOUT_SECTION_LIGHTS );
    FlattenEntitySection( KOUT_SECTION_MODELS );
}

// Outliner.tsx:89-100: anything that becomes selected has its ancestors expanded,
// so a viewport selection is never hiding inside a closed folder.  Cheap and only
// runs when the selection generation actually moved.
void AutoExpandForSelection()
{
    // KiwiSel() FIRST: it folds in any pending legacy change (and bumps the
    // generation when it does), so reading the counter before that call would
    // compare against a number the very next line invalidates.
    KiwiSel();
    const unsigned gen = Sel_Generation();
    if ( gen == s_lastSelGen )
        return;
    s_lastSelGen = gen;

    for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
    {
        bool any = false;
        for ( selbrush_t *b = e->brushes.ownerNext; b && b != &e->brushes; b = b->ownerNext )
        {
            if ( BrushSelected( b ) )
            {
                any = true;
                break;
            }
        }
        if ( !any )
            continue;

        if ( e == world_entity )
        {
            SetCollapsed( KOUT_KEY_BRUSHES, false );
            continue;
        }

        SetCollapsed( SectionKey( EntitySection( e ) ), false );
        entity_s_def *def = DefOf( e );
        if ( def )
            SetCollapsed( EntKey( def->numberId ), false );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SELECTION  (the outliner -> scene direction)
// ═════════════════════════════════════════════════════════════════════════════
// Everything leaves through the SAME funnels the viewport click uses:
// Sel_* + Sel_SyncToLegacy for brushes (kiwi_selection.h:132-146),
// KiwiConSel_ApplyClick for construction (kiwi_conselect.h:125).  The outliner is
// not a second selection owner and holds no list of its own.
void SelectBrushRow( selbrush_t *b, bool additive, bool toggle )
{
    // KIWI-UX (CLEANUP, C-54): a click on a row whose brush has been deleted under
    // the list did nothing and said nothing.  Same early-out, now audible.
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
    // KIWI-UX (CLEANUP, C-54): the construction store shrank under the list, so the
    // row names an object that is gone.  Same early-out, now audible.
    if ( index < 0 || index >= KiwiCon_Count() )
    {
        Sys_Printf( "Outliner: that curve row is stale (index %i of %i) — "
                    "nothing selected.\n", index, KiwiCon_Count() );
        return;
    }
    if ( !additive && !toggle )
    {
        // Same rule as above, from the other side: a plain click on a curve row
        // clears the brush selection too, so the two halves cannot drift into
        // disagreeing about what "the selection" is.
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

// Add every selectable row in [a,b] of the CURRENT flatten — the shift-click
// range.  Sections and folders are skipped: a range is over leaves, because
// including a folder would silently mean "and everything inside it", which is a
// different act with a different undo story.
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
    }
    return -1;
}

void SetAnchor( const koutRow_t &r )
{
    s_anchor.kind     = r.kind;
    s_anchor.inst     = r.inst;
    s_anchor.conIndex = r.conIndex;
    s_anchor.valid    = ( r.kind == KOUT_BRUSH || r.kind == KOUT_CON_OBJECT );
}

// ═════════════════════════════════════════════════════════════════════════════
//  THE REPARENT (the func_group half of the directive)
// ═════════════════════════════════════════════════════════════════════════════
// `insts` is a SNAPSHOT taken before any mutation — the owner chains this walks
// are the very lists the triple relinks, so iterating them live would be
// iterating a list while it changes under the cursor.
//
// One Undo_GeneralStart / Undo_End bracket for the whole move (kiwi_command.h's
// bracket contract), with the per-brush Undo_AddBrush taken BEFORE the first
// relink so the record stores the OLD owner (see the file header).
bool ReparentBrushes( std::vector<selbrush_t *> &insts, entity_s *targetInst, const char *op )
{
    // KIWI-UX (CLEANUP, C-54): audible, same early-out.
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
    // A point entity's brush is its bounding box and is not the user's to move —
    // Entity_Create refuses the same case (entity.cpp:1640-1645).
    if ( targetDef->eclass && targetDef->eclass->fixedsize )
    {
        Sys_Printf( "Outliner: %s is a point entity — it cannot hold brushes.\n",
                    ClassOf( targetInst ) );
        return false;
    }

    // Drop the dead and the already-there.
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
        // KIWI-UX (CLEANUP, C-54): this arm prints because the SUCCESS arm does.
        // Dragging a row onto the folder it already belongs to used to produce
        // nothing at all, which reads as "the outliner ignored my drag".
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
        // select.cpp:5112 — the ported envelope: a SELECTED instance is taken out
        // of the selection bookkeeping across the relink and put back after.
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

// Dissolve ONE func_group: every brush back to worldspawn, then free the entity.
// Select_Ungroup's body (select.cpp:5106-5146) reduced to a single entity, plus
// the undo bracket it does not have.  Undo_AddEntity_W (undo.cpp:633) clones the
// entity AND every brush def it owns, and Undo_Undo restores the entity in phase 3
// before it re-links the brushes in phase 4 — which is the order that makes the
// group come back with its members, not just its name.
bool UngroupEntity( entity_s *inst, const char *op )
{
    // KIWI-UX (CLEANUP, C-54): audible, same early-out.  "Ungroup did nothing and
    // said nothing" is indistinguishable from "ungroup is broken".
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

// ═════════════════════════════════════════════════════════════════════════════
//  ROW DRAWING
// ═════════════════════════════════════════════════════════════════════════════
// The eye.  Drawn on the window draw list rather than from a font glyph, because
// the shell ships no icon atlas and a letter would read as text.  Open = an
// almond outline with a pupil; CLOSED = the same almond flattened to its lower
// lid with a lash, which is what Plasticity's `eye-off` reads as at 16px
// (OutlinerItems.tsx:87).
// NOTE on PathStroke: the vendored ImGui SWAPPED its last two parameters in
// 1.92.8 — it is now PathStroke( col, thickness, flags ) (imgui.h:3551) and the
// old (col, flags, thickness) spelling is `= delete` (imgui.h:3607).  Passing two
// arguments is the only form that is unambiguous under both.
void DrawEye( ImDrawList *dl, ImVec2 c, float r, bool hidden, ImU32 col )
{
    if ( !hidden )
    {
        dl->PathClear();
        dl->PathArcTo( ImVec2( c.x, c.y + r * 1.15f ), r * 1.55f, -1.20f, -1.94f, 12 );
        dl->PathStroke( col, 1.3f );
        dl->PathClear();
        dl->PathArcTo( ImVec2( c.x, c.y - r * 1.15f ), r * 1.55f, 1.20f, 1.94f, 12 );
        dl->PathStroke( col, 1.3f );
        dl->AddCircleFilled( c, r * 0.42f, col, 10 );
    }
    else
    {
        // The lower lid alone plus three short lashes — an unmistakably CLOSED eye
        // at this size, where a drawn-through slash would just look like a strike.
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

// A folder / section disclosure triangle.
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

// Is this row a legal DROP TARGET, and what does dropping on it mean?
bool RowAcceptsBrushes( const koutRow_t &r )
{
    return r.kind == KOUT_SECTION_BRUSHES      // -> worldspawn (ungroup)
        || r.kind == KOUT_GROUP_ENTITY
        || r.kind == KOUT_ENTITY;
}

bool RowAcceptsCurves( const koutRow_t &r )
{
    return r.kind == KOUT_SECTION_CURVES       // -> ungrouped
        || r.kind == KOUT_CON_GROUP;
}

// Apply a dropped payload to a row.
void ApplyDrop( const koutDrag_t &drag, const koutRow_t &target )
{
    if ( drag.kind == KOUT_BRUSH )
    {
        if ( !RowAcceptsBrushes( target ) )
            return;
        if ( !drag.inst || !Sel_BrushLive( drag.inst ) )   // the guard, before any deref
        {
            Sys_Printf( "Outliner: the dragged brush no longer exists.\n" );
            return;
        }
        // Multi-drag: dragging a row that is PART OF THE SELECTION moves the whole
        // selection, which is what every outliner in every DCC does and what the
        // directive's "allow multiple" implies for the drag half.
        std::vector<selbrush_t *> moving;
        if ( BrushSelected( drag.inst ) )
            GatherSelectedBrushes( moving );
        if ( moving.empty() )
            moving.push_back( drag.inst );

        entity_s *targetInst = ( target.kind == KOUT_SECTION_BRUSHES ) ? world_entity : target.ent;
        ReparentBrushes( moving, targetInst,
                         ( target.kind == KOUT_SECTION_BRUSHES ) ? "outliner ungroup"
                                                                 : "outliner group" );
        return;
    }

    if ( drag.kind == KOUT_CON_OBJECT )
    {
        if ( !RowAcceptsCurves( target ) )
            return;
        // The construction store's indices are only meaningful within one
        // generation (kiwi_construct.h): a store that changed under the drag makes
        // the payload's index name a different object, so the drop is refused
        // rather than applied to whatever now sits there.
        if ( drag.conGen != KiwiCon_Generation() )
        {
            Sys_Printf( "Outliner: the construction store changed — drop cancelled.\n" );
            return;
        }
        // KIWI-UX (CLEANUP, C-54): the generation matched but the index is out of
        // range anyway — say so, as the generation arm above already does.
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

// KIWI: collapse keys and row identities belong to one map document.
// KIWI: "When loading a map, load with all the groups in the outliner collapsed"
// (earlier directive) — but only the FOLDERS.  The first version of that one-shot
// also closed the top-level sections, which is exactly how a freshly loaded map
// came up with "Solids (54)" and nothing under it until the user toggled it.
// Armed by the map reset, consumed on the next draw (after the sidecar load has
// minted the curve groups it closes).
static bool s_collapseFoldersPending = false;

static void CollapseFoldersOnly()
{
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
}

void KiwiOutliner_ResetForNewMap()
{
    s_collapsed.clear();
    s_collapseFoldersPending = true;
    s_rows.clear();
    s_lastSelGen = 0;
    s_anchor = koutAnchor_t();
    s_rename = koutRename_t();
    s_renameBuf[0] = '\0';
    s_renameFocus = false;
    s_structural = false;
    s_paintSelecting = false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  THE PANEL
// ═════════════════════════════════════════════════════════════════════════════
void KiwiOutliner_Draw()
{
    // ROUND AG, ITEM 3: the hover target lives for exactly ONE outliner draw.
    // Cleared HERE — before the early-out, so closing the panel with a row
    // hovered cannot leave a highlight burned into the viewport.
    KiwiHover_OutlinerClear();

    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_OUTLINER );
    if ( !open || !*open )
        return;                              // closed: no Begin, no End, no cost

    if ( KiwiWindows_JustOpened( KIWI_WIN_OUTLINER ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_OUTLINER ), open ) )
    {
        // ── the header: a title and ONE button (Outliner.tsx:196-205) ───────
        // Drawn BEFORE the flatten on purpose: the two buttons here restructure the
        // scene, and a row array built before them would be one frame out of date
        // the instant either is pressed.  The header reads no rows, so it can.
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
        if ( s_collapseFoldersPending )
        {
            s_collapseFoldersPending = false;
            CollapseFoldersOnly();
        }
        Flatten();

        ImGuiIO &io = ImGui::GetIO();
        // The shift-drag latch.  It is checked BEFORE the rows so a press that
        // began on a row this frame still paints on the next one.
        if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            s_paintSelecting = false;

        // TWO heights, and the difference matters: ImGui advances the cursor by
        // (item height + ItemSpacing.y) after each line, so an ImGuiListClipper told
        // the ITEM height would drift by one spacing per row and put the wrong rows
        // under the mouse a screenful down.  The items are itemH tall; the clipper is
        // told rowH, which is itemH + the spacing it cannot see.
        const float itemH   = ImGui::GetTextLineHeight();
        const float rowH    = ImGui::GetTextLineHeightWithSpacing();
        // ── KIWI-UX (CLEANUP, C-55): the row layout, NAMED ──────────────────
        // `indentW` was already named and the rest of the row was bare literals.
        // The two HALF constants are written as arithmetic rather than as their own
        // literals, because each is the CENTRE of the gutter above it — changing a
        // gutter width and forgetting its centre is how a glyph ends up off-centre.
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

        // THE CLIPPER: thousands of brushes, ~40 submitted rows.
        ImGuiListClipper clipper;
        clipper.Begin( (int)s_rows.size(), rowH );
        while ( clipper.Step() )
        {
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
            {
                if ( i < 0 || i >= (int)s_rows.size() )
                    continue;
                const koutRow_t r = s_rows[i];       // by value: the row list must not
                                                     // be re-entered mid-draw
                // ── KIWI-UX (CLEANUP, C-43): A STABLE WIDGET ID, NOT THE ROW INDEX ──
                // This file refuses to store an index for the selection anchor
                // (:184-186, "an index into last frame's array is a lie the moment it
                // does") and for the rename target (:224-226) — and then handed that
                // very index to ImGui as the identity of every widget scoped under it
                // (the rename box, both context menus, the drag source).  Collapsing a
                // folder above an open rename box shifted `i` and ImGui lost the
                // widget's state.  The identity each row already carries is used
                // instead: the collapse KEY for a folder (which is exactly what
                // koutRename_t keys on), the instance POINTER for a brush, and a
                // tagged store index for a construction object (which is what
                // koutAnchor_t / koutRename_t key on).
                if ( r.key != 0 )
                    ImGui::PushID( (int)r.key );
                else if ( r.kind == KOUT_BRUSH && r.inst )
                    ImGui::PushID( (const void *)r.inst );
                else if ( r.kind == KOUT_CON_OBJECT )
                    ImGui::PushID( (int)( 0xC0000000u | (unsigned)r.conIndex ) );
                else
                    ImGui::PushID( i );

                // ── the eye ────────────────────────────────────────────────
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
                case KOUT_GROUP_ENTITY:
                case KOUT_ENTITY:
                {
                    // A folder's eye reads "is EVERY child hidden" and writes the
                    // opposite to all of them — the only reading that makes one
                    // click on a folder a complete act.
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
                            // ROUND AG, ITEM 7: ONE record per CLICK.  The
                            // construction arms below have bracketed since round U
                            // and the brush arms did not, which the user saw as
                            // "Ctrl+Z undoes hiding a line but not a brush".
                            KiwiVis_UndoPush( "hide (outliner)" );
                            SetBrushHidden( r.inst, want );
                            KiwiVis_UndoCommit();      // KIWI-UX (CLEANUP, C-41)
                        }
                        else if ( r.kind == KOUT_CON_OBJECT )
                        {
                            KiwiCon_UndoPush();
                            KiwiCon_SetHidden( r.conIndex, want );
                        }
                        else if ( r.kind == KOUT_GROUP_ENTITY || r.kind == KOUT_ENTITY )
                        {
                            // ROUND AG, ITEM 7: ONE record for the whole ENTITY,
                            // taken before the loop — a group's eye is one gesture
                            // however many brushes it owns.
                            KiwiVis_UndoPush( "hide group (outliner)" );
                            for ( selbrush_t *b = r.ent->brushes.ownerNext;
                                  b && b != &r.ent->brushes; b = b->ownerNext )
                                SetBrushHidden( b, want );
                            KiwiVis_UndoCommit();      // KIWI-UX (CLEANUP, C-41)
                        }
                        else if ( r.kind == KOUT_CON_GROUP )
                        {
                            KiwiCon_UndoPush();
                            const int cnt = KiwiCon_Count();
                            for ( int k = 0; k < cnt; ++k )
                                if ( KiwiCon_Group( k ) == r.conGroup )
                                    KiwiCon_SetHidden( k, want );
                        }
                    }
                }
                ImGui::SameLine( 0.0f, 0.0f );

                // ── the indent + the disclosure arrow ───────────────────────
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

                // ── the label ──────────────────────────────────────────────
                char label[128];
                bool selected = false;
                switch ( r.kind )
                {
                case KOUT_SECTION_BRUSHES:
                    _snprintf( label, sizeof( label ), "Brushes (%i)", r.count );
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
                    // ROUND X, ITEM 10: a NAMED object shows its name; an unnamed one
                    // falls back to the generated "<type> <ordinal>".  That is
                    // Plasticity's own fallback shape — `${klass} ${id}` when the
                    // names map has no entry (Outliner.tsx:158).
                    const char *nm = KiwiCon_Name( r.conIndex );
                    if ( nm && nm[0] )
                        _snprintf( label, sizeof( label ), "%s", nm );
                    else
                        _snprintf( label, sizeof( label ), "%s %i",
                                   o ? ConTypeName( o->type ) : "Curve", r.ordinal );
                    selected = KiwiConSel_ObjectSelected( r.conIndex );
                    break;
                }
                }
                label[sizeof( label ) - 1] = '\0';

                // Renaming this row?  Draw the edit box instead of the label — the
                // same swap Plasticity's row does (OutlinerItems.tsx:41-58).
                // ROUND X, ITEM 10: any renameable KIND, not folders only.
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
                    // ROUND X, ITEM 10: Esc CANCELS.  Plasticity has no cancel at all
                    // — its only key handler is `e.code === "Enter"` and blur commits
                    // (OutlinerItems.tsx:153-167), so clicking away saves an edit you
                    // were abandoning.  That is a gap in theirs, not a rule to copy:
                    // every other text field in this editor (the grid pill, the command
                    // palette) cancels on Esc, and a rename that cannot be backed out
                    // of would be the only one that does not.
                    const bool esc  = ImGui::IsKeyPressed( ImGuiKey_Escape, false );
                    const bool lost = ImGui::IsItemDeactivated();
                    if ( esc )
                    {
                        s_rename.active = false;
                    }
                    else if ( done || lost )
                    {
                        if ( s_renameBuf[0] )
                        {
                            if ( r.kind == KOUT_CON_GROUP )
                            {
                                KiwiCon_SetGroupName( r.conGroup, s_renameBuf );
                            }
                            else if ( r.kind == KOUT_CON_OBJECT )
                            {
                                // ROUND X, ITEM 10: ONE store snapshot for the edit,
                                // the same bracket every other construction-store
                                // mutation takes (kiwi_conselect.h UNDO) — a rename
                                // is undoable exactly like a hide or a group move.
                                KiwiCon_UndoPush();
                                KiwiCon_SetName( r.conIndex, s_renameBuf );
                            }
                            else if ( entity_s_def *def = DefOf( r.ent ) )
                            {
                                // ONE record: the epair edit alone.  Undo_AddEntity_W
                                // is the setter's own bracket shape (win_ent.cpp:278).
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

                if ( ImGui::Selectable( label, selected,
                                        ImGuiSelectableFlags_AllowDoubleClick,
                                        ImVec2( 0.0f, itemH ) ) )
                {
                    if ( r.kind == KOUT_BRUSH || r.kind == KOUT_CON_OBJECT )
                    {
                        // ── ROUND X, ITEM 10: DOUBLE-CLICK RENAMES A LEAF TOO ──
                        // Tested BEFORE the selection arms and only without
                        // modifiers: Shift and Ctrl are the range / toggle gestures
                        // and a second click while building a selection means "add
                        // another one", never "rename this".  A double-click still
                        // leaves the row SELECTED, because the first click of the
                        // pair already ran the ordinary single-click arm.
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
                                // A worldspawn brush has nowhere to keep a name —
                                // see the koutRename_t block for why an ordinal key
                                // is refused rather than shipped.  Say so once per
                                // attempt rather than doing nothing silently.
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
                        // Double-click a FOLDER = rename (OutlinerItems.tsx:77).
                        // Single-click = select everything inside it, which is the
                        // one thing a folder row can usefully mean for selection.
                        if ( !io.KeyShift && !io.KeyCtrl
                          && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left )
                          && Renameable( r.kind ) )
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
                            // Folder rows follow the outliner's documented group
                            // convention: Ctrl toggles the group as a unit; Shift
                            // (and Shift+Ctrl) adds it.
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

                // ── ROUND AG, ITEM 3: THE ROW HOVER IS A VIEWPORT HOVER ────
                // USER DIRECTIVE: "While mousing over the brushes in the
                // outliner, it should highlight them in 3D so I can find them
                // easier."  Plasticity's own Outliner.tsx does exactly this by
                // pushing the row's item into the SAME hover collection the
                // viewport raycast writes to (`selection.hovered.add`); the KIWI
                // equivalent publishes a one-frame target that
                // KiwiHover_DrawWorld consumes — see kiwi_hover.h.
                //
                // HERE, right after the Selectable, because IsItemHovered reads
                // the LAST SUBMITTED ITEM and the drag-drop and context-menu
                // calls below submit their own.  A folder publishes ALL its
                // members, which is the whole reason a mapper hovers a folder.
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

                // ── the folder context menu (rename / ungroup) ─────────────
                // Immediately after the Selectable ON PURPOSE:
                // BeginPopupContextItem's open test reads the LAST SUBMITTED ITEM
                // even when it is given an explicit id, so the drag-drop calls
                // below must not come between them.
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
                // ── ROUND X, ITEM 10: the CONSTRUCTION-OBJECT context menu ─────
                // Same position and the same reason as the folder menu above (it has
                // to be the item immediately after the Selectable).  Rename only —
                // grouping a curve is already the drag-to-folder gesture and delete
                // is the Delete key over the viewport selection this row drives.
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

                // ── shift+DRAG paint-select (the directive's "shift dragging") ──
                // Deliberately exclusive with the drag-drop source below: one
                // gesture, one meaning.  Shift held = paint; shift free = move.
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

                // ── drag SOURCE ────────────────────────────────────────────
                if ( !io.KeyShift && !s_paintSelecting
                  && ( r.kind == KOUT_BRUSH || r.kind == KOUT_CON_OBJECT ) )
                {
                    if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
                    {
                        koutDrag_t d;
                        d.kind     = (int)r.kind;
                        d.inst     = r.inst;
                        d.conIndex = r.conIndex;
                        d.conGen   = KiwiCon_Generation();
                        ImGui::SetDragDropPayload( KOUT_PAYLOAD, &d, sizeof( d ) );
                        ImGui::TextUnformatted( label );
                        ImGui::EndDragDropSource();
                    }
                }

                // ── drop TARGET ────────────────────────────────────────────
                if ( RowAcceptsBrushes( r ) || RowAcceptsCurves( r ) )
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

// ═════════════════════════════════════════════════════════════════════════════
//  THE TWO GROUP VERBS
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiOutliner_CanGroup()
{
    // The predicate matches what the verb will actually DO, so the palette row and
    // the header button grey out instead of printing a refusal after the click.
    // The brush half needs a selection that is ENTIRELY worldspawn-owned — see
    // KiwiOutliner_GroupSelection for why this verb is create-only.
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

    // ── the SOLID half: a func_group entity ─────────────────────────────────
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
            // WHICH ARM Entity_Create WILL TAKE, decided BEFORE the bracket opens.
            // Entity_Create has three arms (entity.cpp:1628-1756) and only ONE of
            // them allocates: with every selected brush owned by worldspawn it
            // builds a NEW entity (:1714) and reparents the selection into it
            // (:1739-1753).  With ANY non-world brush in the selection it instead
            // MERGES the world brushes into that brush's EXISTING entity
            // (:1669-1710) and returns THAT — a return indistinguishable from a
            // create afterwards.  Two things go wrong if this verb takes that arm:
            //   * Undo_SetIdForEntity would stamp a pre-existing entity, and one
            //     Ctrl+Z would DELETE a group the user never created (Undo_Undo
            //     phase 2, undo.cpp:853);
            //   * the merge can also REFUSE outright (":1648 Can't merge entities",
            //     ":1661 ...ungroup first"), which would leave an opened bracket
            //     around a no-op — kiwi_command.h: "a command that mutates nothing
            //     must NOT open a bracket at all".
            // So "Group Selection" is CREATE-ONLY, and adding brushes to an
            // existing group is the DRAG, which names its target unambiguously.
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
                // xywnd.cpp:3400-3404's bracket, verbatim, plus pmesh.cpp:7396's
                // Undo_SetIdForEntity tail.  Entity_Create does the reparenting
                // itself, so there is nothing to relink here.
                Undo_ClearRedo();
                Undo_GeneralStart( "outliner group" );
                Undo_AddBrushList( &selected_brushes );
                entity_s *inst = Entity_Create( ec );
                if ( entity_s_def *def = DefOf( inst ) )
                {
                    Undo_SetIdForEntity( def );
                    // "named 'group_N'" — the directive's own spelling.  numberId is
                    // the entity's unique number (qe3.h:539), so two groups made in
                    // one session can never share a name.
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

    // ── the CURVE half: a sidecar group ─────────────────────────────────────
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
    // Brush side: any selected brush whose owner is a non-worldspawn brush entity.
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
    // Curve side: any selected construction object that is in a group.
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

    // Collect the DISTINCT owning entities first — the ungroup frees them, so the
    // selection they came from must not be walked while that happens.
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

    // The construction half: drop every selected object out of its group, and
    // remove a group that ends up with nothing in it.
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

// ═════════════════════════════════════════════════════════════════════════════
//  COMMANDS
// ═════════════════════════════════════════════════════════════════════════════
void KiwiOutliner_RegisterCommands()
{
    // UNBOUND in both keymap profiles, on the same argument kiwi_windows.cpp makes
    // for its own five: registering makes them searchable in the §15 palette and
    // remappable from radiant.ini, and a group verb is not worth a letter while
    // the modern profile's budget is spent on the modelling verbs.
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
