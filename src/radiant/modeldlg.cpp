#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Modeless bulk model replacement dialog. It replaces matching misc_model,
// dyn_model, or misc_prefab "model" keys with a random entry from the target set.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <cstring>
#include <cstdlib>

extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ── externs (per-TU, matching vehicledlg.cpp / undo.cpp) ──────────────────────
extern entity_s *world_entity;                                              // 0x25D5B30 (map.cpp)
extern void      SetKeyValue( entity_s_def *e, const char *key, const char *value ); // 0x483690 (entity.cpp)
extern bool      Entity_HasEpairMatch( entity_s *e, const char *key, const char *value ); // 0x483930 (entity.cpp)
extern char      FilterBrush( selbrush_t *b, int a2 );                      // 0x46A1F0 (filters.cpp)
extern int       g_nUpdateBits;                                            // 0x25D5A74 (engine_stubs.cpp)
// active_brushes / selected_brushes sentinels are declared in qe3.h.

// ═════════════════════════════════════════════════════════════════════════════
//  THE CORE — 0x434EC0  CModelDlg::OnReplace (BN_CLICKED ctrl 0x627/1575)
//  Replace every matching entity's "model" with a random TO entry.
//    classFlags bit0 = touch misc_model, bit1 = touch dyn_model, bit2 = touch misc_prefab
//    (in the binary these are the three BM_GETCHECK reads off the dialog's CButton members
//     at this+0x98 / +0xEC / +0x140).
// ═════════════════════════════════════════════════════════════════════════════
void ModelDlg_DoReplace( const char *const *fromSet, int fromCount,
                         const char *const *toSet,   int toCount,
                         int classFlags )
{
    if ( fromCount <= 0 || toCount <= 0 )       // 0x434EDE/EF2: either set empty → bail
    {
        g_nUpdateBits = -1;
        return;
    }

    const bool doModel  = ( classFlags & 1 ) != 0;
    const bool doDyn    = ( classFlags & 2 ) != 0;
    const bool doPrefab = ( classFlags & 4 ) != 0;

    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; )
    {
        selbrush_t *next = b->next->prev;       // pre-save (faithful sentinel-walk; +0x04 then +0x00)

        // 0x434F1B: skip filtered.  0x434F2B: skip brushflags & 2 and & 0x20.  skip world.
        if ( FilterBrush( b, 0 ) )                              { b = next; continue; }
        if ( ( b->brushFlags & 2 ) || ( b->brushFlags & 0x20 ) ){ b = next; continue; }

        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )                 { b = next; continue; }

        // 0x434F55 (type 0 → log+continue).  brush = the binary's local.
        entity_s_def *ent = (entity_s_def *)owner->def;
        selbrush_t *brush = b;
        if ( brush->def )
            iassert( brush->owner->def == brush->def->owner );   // ModelDlg.cpp:292
        if ( !ent )                                            { b = next; continue; }

        // 0x434FA6..: per FROM entry, test this entity by class + the class checkbox.
        const char *cls = ent->eclass->name;                   // eclass@0x60, name@0x04
        const bool isModel  = ( strcmp( cls, "misc_model" )  == 0 );
        const bool isDyn    = ( strcmp( cls, "dyn_model" )   == 0 );
        const bool isPrefab = ( strcmp( cls, "misc_prefab" ) == 0 );

        // The checkbox gate: only process this entity if its class is checked.  (In the
        // binary the per-class BM_GETCHECK short-circuits to the NEXT class test / next
        // brush; here the combined predicate is equivalent.)
        const bool process =
            ( isModel  && doModel  ) ||
            ( isDyn    && doDyn    ) ||
            ( isPrefab && doPrefab );

        if ( process )
        {
            for ( int i = 0; i < fromCount; ++i )
            {
                char fromBuf[256];
                const char *fromVal;
                if ( isPrefab )
                {
                    _snprintf( fromBuf, sizeof( fromBuf ), "prefabs/misc_models/%s.map", fromSet[i] );
                    fromVal = fromBuf;
                }
                else
                {
                    fromVal = fromSet[i];
                }

                if ( Entity_HasEpairMatch( (entity_s *)ent, "model", fromVal ) )
                {
                    int pick = ( rand() % toCount );           // 0x4350B7: random TO entry
                    char toBuf[256];
                    const char *toVal;
                    if ( isPrefab )
                    {
                        _snprintf( toBuf, sizeof( toBuf ), "prefabs/misc_models/%s.map", toSet[pick] );
                        toVal = toBuf;
                    }
                    else
                    {
                        toVal = toSet[pick];
                    }
                    SetKeyValue( ent, "model", toVal );
                    break;                                     // 0x435031: one replace per brush
                }
            }
        }

        b = next;
    }

    g_nUpdateBits = -1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  CModelDlg — the hand-built modeless popup (CVehicleDlg / CDynEntityDlg pattern)
// ══════════════════════════════════════════════════════════════════════════════
//  Two list boxes (FROM / TO), each with [Add file...] (→ a CFileDialog at main_shared\
//  xmodel\, matching the binary's CModelFileDialog 0x434330/0x43464D) and [Remove], plus
//  the three class checkboxes (misc_model / dyn_model / misc_prefab) and [Replace].
//  radiant.rc has no IDD template, so it is hand-built (the established sibling pattern).
//  U-GUARD: the control-id enum is MFC-shell-only; modelReplaceState_t + ModelReplace_Apply
//  below stay COMMON (imgui_panel_model.cpp externs the action and mirrors the struct).


// One [Replace] pass snapshot: the two model-set list boxes and the three class check
// boxes the replace consults.
struct modelReplaceState_t
{
    const char *const *fromSet;    // IDC_MDL_FROM_LIST contents (binary ctrl 1566)
    int                fromCount;
    const char *const *toSet;      // IDC_MDL_TO_LIST contents   (binary ctrl 1564)
    int                toCount;
    int                classFlags; // IDC_MDL_CHK_MODEL / _DYN / _PREFAB → bit0 / bit1 / bit2
};

// UI-independent action behind CModelDlg's [Replace] button.
void ModelReplace_Apply( const modelReplaceState_t &st )
{
    ModelDlg_DoReplace( st.fromSet, st.fromCount, st.toSet, st.toCount, st.classFlags );
    g_nUpdateBits |= 1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CModelDlg (the hand-built popup) and its listbox/file-picker helpers.
//  Everything below only feeds ModelReplace_Apply above, which the ImGui panel calls.
// ══════════════════════════════════════════════════════════════════════════════

