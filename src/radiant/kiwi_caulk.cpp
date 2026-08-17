#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_caulk.cpp — KIWI-UX (ROUND BH, ITEMS 1 + 2).  See kiwi_caulk.h for the
// user directives, the material-resolution argument (D-BH-A), the reason the
// apply reuses the browser funnel rather than writing a second one (D-BH-B),
// the two-undo-record ruling for the auto-fit (D-BH-C), why only caulk is
// fitted (D-BH-D) and how this composes with ROUND BH ITEM 4 (D-BH-E).
//
// NEW code over ported cores.  Nothing here writes a MaterialDef or a texdef
// itself: the material goes through TexWnd_ApplyMaterialAtIndex and the fit
// through Brush_FitTexture, exactly as a thumbnail click and the Surface
// Inspector's Fit button do.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_caulk.h"
#include "kiwi_command.h"
#include "kiwi_uv.h"        // KiwiUv_CanEdit (the shared canExecute, D-BH note)
#include "kiwi_selection.h" // ROUND BJ, ITEM 1 — the mixed-selection split

#include <string.h>
#include <vector>

// ── ported / cross-file entry points (each verified against its definition) ──
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:112   int Sys_Printf(const char*,...)
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773  int g_nUpdateBits = 0
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1340  bool Radiant_RegisterCommand(const char*,byte,byte,int)
// texwnd.cpp — the browser accessors round AZ added for the Sky tab, reused verbatim
// (kiwi_skybox.cpp:79-83 declares the same three).  texwnd_s is TU-local, so every
// out-of-TU reader goes through these.
extern int         TexWnd_MaterialCount();                                   // texwnd.cpp:2548   int TexWnd_MaterialCount()
extern qtexture_s *TexWnd_MaterialAt( int idx );                             // texwnd.cpp:2550   qtexture_s *TexWnd_MaterialAt(int)
extern void        TexWnd_ApplyMaterialAtIndex( int idx );                   // texwnd.cpp:1250   void TexWnd_ApplyMaterialAtIndex(int)
extern qtexture_s *Texture_GetHandle( const char *name );                    // texwnd.cpp:313    qtexture_s *Texture_GetHandle(const char*)
// select.cpp — the Surface Inspector's Fit funnel, IDB 0x4939E0.  Declared exactly as
// mainfrm.cpp:4810 / surfacedlg.cpp:44 / patchdialog.cpp:356 declare it.
extern void        Brush_FitTexture( float x, float y, int a4 );             // select.cpp:3667   void Brush_FitTexture(float,float,int)
// ── KIWI-UX (ROUND BJ, ITEM 1): the AUTO-CAULK half ─────────────────────────
// The ONE id->action entry point (kiwi_command.h); id 33220 is Selection->CSG->Auto
// Caulk, whose handler Cmd_OnSelectionAutoCaulk (mainfrm.cpp:2817-2824) is `static`
// and brackets Brush_AutoCaulk itself.  Reaching it by id rather than re-spelling
// its four-line bracket here is the same choice kiwi_command.cpp's Cut makes with
// Copy / Delete (kiwi_command.cpp:1105-1106) and kiwi_join.cpp makes with CSG_Merge.
extern void        Radiant_ExecCommand( unsigned int cmdId );                // mainfrm.cpp:4083  void Radiant_ExecCommand(unsigned int)
// materialdef.cpp — the per-face material name the before/after count reads.  It
// carries the MtlDef_IsValid L0 assert (materialdef.cpp:53), so it is called behind the
// same "exactly one of lyrMtl / radMtl" guard kiwi_uv.cpp:724 states, and its result is
// used as a name exactly as csg.cpp:721 and kiwi_uveditor.cpp:3557 use it.
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );         // materialdef.cpp:159  LayerMaterialDef *Materialdef_GetName(MaterialDef*)

namespace
{
    // D-BH-A.  Spelled ONCE, here; csg.cpp's ported AutoCaulk uses the same literal
    // (csg.cpp:665 / :679 / :722).
    const char *const KCAULK_NAME = "caulk";

    // The leaf of a registered material name.  Browser names are stored lowercase
    // (Texture_GetHandle's _strlwr, texwnd.cpp:321) and may or may not carry a folder,
    // so both spellings have to answer the same.
    const char *LeafName( const char *name )
    {
        const char *slash = strrchr( name, '/' );
        if ( slash )
            return slash + 1;
        const char *back = strrchr( name, '\\' );
        return back ? back + 1 : name;
    }

    // Resolve caulk to its index in the browser's sorted_materials.  Texture_GetHandle
    // FIRST so a set that has not referenced caulk yet still registers it (it appends
    // through Editor_AddRadiantMaterial, texwnd.cpp:302-304, which is the same array
    // TexWnd_MaterialAt reads) — then a pointer scan, which is exact where a name
    // compare would only be probable.
    int CaulkIndex()
    {
        qtexture_s *q = Texture_GetHandle( KCAULK_NAME );
        if ( !q )
            return -1;
        const int n = TexWnd_MaterialCount();
        for ( int i = 0; i < n; ++i )
            if ( TexWnd_MaterialAt( i ) == q )
                return i;
        return -1;
    }
}

bool KiwiCaulk_IsCaulkName( const char *name )
{
    if ( !name || !name[0] )
        return false;
    return _stricmp( LeafName( name ), KCAULK_NAME ) == 0;
}

bool KiwiCaulk_CanExecute()
{
    // D-BH note on the declaration: the SAME predicate, not a copy of it.
    return KiwiUv_CanEdit();
}

void KiwiCaulk_AutoFitApplied( const char *appliedName )
{
    if ( !KiwiCaulk_IsCaulkName( appliedName ) )
        return;
    // D-BH-C.  The Surface Inspector's own call (mainfrm.cpp:4813 makes the identical
    // one for Ctrl+F).  1 x 1 = one repeat across the face; a4 == 0 is "fit per face",
    // the only value OnFit / Brush_FitTexture ever pass (select.cpp:2104).
    Brush_FitTexture( 1.0f, 1.0f, 0 );
    Sys_Printf( "Caulk: fitted one repeat per face (the apply and the fit are separate "
                "undo records - two Ctrl+Z).\n" );
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BJ, ITEM 1) — the two halves of the End key
// ═════════════════════════════════════════════════════════════════════════════
// See kiwi_caulk.h D-BJ-A (why the key never arrived on a face selection),
// D-BJ-B (the split) and D-BJ-C (what the AUTO half can and cannot compose with).
namespace
{
    // Whole-selected instances, and how many of them AutoCaulk can actually act on.
    // Brush_AutoCaulk skips fixed-size entities and patches outright
    // (Brush_IsFixedOrPatch, csg.cpp:146-152 / :706), so a selection made entirely of
    // those has no AUTO arm and falls back to the plain one.
    int SelectedBrushCount( int *outSolids )
    {
        int n = 0, solids = 0;
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            ++n;
            brush_t  *def   = b->def;
            entity_s *owner = def ? def->owner : nullptr;
            const bool fixed = owner && owner->eclass && owner->eclass->fixedsize;
            if ( def && !def->patch && !fixed )
                ++solids;
        }
        if ( outSolids )
            *outSolids = solids;
        return n;
    }

    // Faces of the WHOLE-SELECTED brushes whose current-layer material is not
    // already caulk.  Brush_AutoCaulk reports a count of its own but does not
    // return one, and csg.cpp is a ported file this round does not touch, so the
    // number this verb prints is measured either side of the call instead.
    int NonCaulkFaceCount()
    {
        const int layer = g_qeglobals.current_edit_layer;
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            brush_t *def = b->def;
            if ( !def || !def->faces || def->patch )
                continue;
            for ( int fi = 0; fi < def->faceCount; ++fi )
            {
                MaterialDef *md = &def->faces[fi].mtldef[layer];
                // The MtlDef_IsValid invariant (materialdef.cpp:53) is an L0 assert
                // inside Materialdef_GetName; test it rather than trip it.
                if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
                    continue;
                const char *name = (const char *)Materialdef_GetName( md );
                if ( !KiwiCaulk_IsCaulkName( name ) )
                    ++n;
            }
        }
        return n;
    }

    // The AUTO half: Selection->CSG->Auto Caulk, through its own id so its undo
    // bracket is the ported one and there is exactly one Auto Caulk in the editor.
    // Returns how many faces it converted.
    int RunAutoCaulk()
    {
        const int before = NonCaulkFaceCount();
        Radiant_ExecCommand( 33220u );          // Cmd_OnSelectionAutoCaulk
        const int after  = NonCaulkFaceCount();
        return ( before > after ) ? ( before - after ) : 0;
    }

    // Install `items` as the whole KIWI selection and push it at the legacy globals.
    // Sel_SyncToLegacy opens with Select_Deselect(1), so this is a REPLACE.
    void SetSelection( const std::vector<sel_item_t> &items, const sel_item_t &active )
    {
        selection_t &sel = KiwiSel();
        Sel_Clear( sel );
        for ( size_t i = 0; i < items.size(); ++i )
            Sel_Add( sel, items[i] );
        if ( Sel_ItemValid( active ) )
            sel.active = active;
        Sel_SyncToLegacy();
    }
}

// ── the verb ────────────────────────────────────────────────────────────────
// FACE arm: one call of real work (D-BH-B) — the funnel does the gesture end
// (BH ITEM 3), the validate, the apply, the patch re-lay and the auto-fit (BH
// ITEM 2).  SOLID arm: the ported AutoCaulk through its own command id.
static bool KiwiCaulk_ApplyToSelection()
{
    if ( !KiwiCaulk_CanExecute() )
    {
        Sys_Printf( "Caulk: nothing selected - select brushes or faces first.\n" );
        return true;
    }

    const int nFaces = g_SelectedFaces.GetSize();
    int       nSolids = 0;
    const int nBrushes = SelectedBrushCount( &nSolids );

    // ── SOLID / GROUP: the stock AUTO-caulk, and nothing else ────────────────
    // No index resolution and no fit on this arm: Brush_AutoCaulkFace writes the
    // material itself with SetMaterial( "caulk", &mat ) (csg.cpp:665/:679), and it
    // is the only thing that knows WHICH faces it converted — see D-BJ-C.
    if ( nFaces == 0 && nSolids > 0 )
    {
        // The BH ITEM 3 handshake, which the FACE arm gets for free from the browser
        // funnel: a live gesture is ended (record-free when it applied nothing)
        // before anything writes, and given back afterwards with a fresh baseline.
        // A gesture that refuses says so on the console and nothing happens.
        if ( !KiwiUv_EndGestureBeforeApply( "Caulk" ) )
            return true;
        const int did = RunAutoCaulk();
        KiwiUv_RestoreGestureAfterApply();
        Sys_Printf( "Caulk: auto-caulked %d hidden face%s on %d selected solid%s "
                    "(faces concealed by neighbouring geometry only).\n",
                    did, did == 1 ? "" : "s", nSolids, nSolids == 1 ? "" : "s" );
        g_nUpdateBits = -1;
        return true;
    }

    const int idx = CaulkIndex();
    if ( idx < 0 )
    {
        Sys_Printf( "Caulk: no \"%s\" material in the registry - this asset set is "
                    "missing main/materials/%s.\n", KCAULK_NAME, KCAULK_NAME );
        return true;
    }

    // ── PURE FACE SELECTION: plain caulk + fit, exactly those faces ──────────
    // …and the ONE other case that takes this arm: a whole-brush selection with no
    // AUTO-caulkable solid in it at all (patches, fixed-size entities).  Those have
    // no hidden-face test to run, so they keep round BH's behaviour rather than
    // silently doing nothing.
    if ( nBrushes == 0 || nSolids == 0 )
    {
        TexWnd_ApplyMaterialAtIndex( idx );
        if ( nFaces > 0 )
            Sys_Printf( "Caulk: caulked %d selected face%s and fitted %s.\n",
                        nFaces, nFaces == 1 ? "" : "s", nFaces == 1 ? "it" : "them" );
        else
            Sys_Printf( "Caulk: caulked and fitted %d selected patch%s / entit%s "
                        "(nothing here has hidden faces to auto-caulk).\n",
                        nBrushes, nBrushes == 1 ? "" : "es", nBrushes == 1 ? "y" : "ies" );
        g_nUpdateBits = -1;
        return true;
    }

    // ── MIXED: faces get plain caulk, whole brushes get AUTO-caulk ───────────
    // The two halves cannot run over one legacy selection, because Brush_SetTexture
    // walks BOTH g_SelectedFaces AND selected_brushes in the same record
    // (select.cpp:1820-1876) — a mixed apply would plain-caulk every face of every
    // selected solid, which is the opposite of what the AUTO half is for.  So the
    // selection is split through the typed layer's own crossing (Sel_SyncToLegacy,
    // kiwi_selection.h) and restored afterwards.  Values are captured first: the
    // items hold live selbrush_t*, and neither half frees a brush.
    // The handshake runs ONCE here, before the first selection swap: a parked face
    // push holds a face index into a selection this arm is about to replace twice.
    // (The FACE half's funnel runs its own; with nothing live by then it is a no-op,
    // which is also why the face gizmo is not re-armed on this arm — D-BJ-C.)
    if ( !KiwiUv_EndGestureBeforeApply( "Caulk" ) )
        return true;

    std::vector<sel_item_t> faceItems, objItems, allItems;
    const sel_item_t savedActive = KiwiSel().active;
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( !Sel_ItemValid( it ) || !Sel_BrushLive( it.brush ) )
                continue;
            allItems.push_back( it );
            if ( it.kind == SEL_FACE )       faceItems.push_back( it );
            else if ( it.kind == SEL_OBJECT ) objItems.push_back( it );
        }
    }

    // AUTO first, PLAIN second, so the round-BH handshake that the funnel runs at
    // its tail (KiwiUv_RestoreGestureAfterApply, texwnd.cpp:1330) is the LAST thing
    // to touch the gesture state instead of being unwound by a selection swap.
    int autoDid = 0;
    if ( !objItems.empty() )
    {
        SetSelection( objItems, sel_item_t() );
        autoDid = RunAutoCaulk();
    }
    if ( !faceItems.empty() )
    {
        SetSelection( faceItems, sel_item_t() );
        TexWnd_ApplyMaterialAtIndex( idx );
    }
    SetSelection( allItems, savedActive );

    Sys_Printf( "Caulk: %d face%s caulked and fitted; %d hidden face%s auto-caulked "
                "on %d solid%s.\n",
                (int)faceItems.size(), faceItems.size() == 1 ? "" : "s",
                autoDid, autoDid == 1 ? "" : "s",
                (int)objItems.size(), objItems.size() == 1 ? "" : "s" );
    g_nUpdateBits = -1;
    return true;
}

void KiwiCaulk_RegisterCommands()
{
    // UNBOUND here; kiwi_keymap.cpp's ApplyModern gives it End and moves the classic
    // occupant (View->Center, 32953) to Shift+End in that profile only.
    Radiant_RegisterCommand( "KiwiCaulkSelection", 0, 0, KIWI_CMD_CAULK_FACES );
}

bool KiwiCaulk_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_CAULK_FACES )
        return false;
    return KiwiCaulk_ApplyToSelection();
}
