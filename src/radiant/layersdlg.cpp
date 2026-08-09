#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Layer-management UI over layers.cpp. The original tree resource is unavailable,
// so the port uses a flat modeless list while preserving full layer paths and commands.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <utility>

extern void       Select_BrushByLayer( char *layer_str );    // select.cpp 0x48EE10
extern void       Brush_SetInstanceLayerString( selbrush_t *inst, const char *str ); // brush.cpp 0x4758E0
extern int        g_nUpdateBits;                              // engine_stubs 0x25D5A74
extern void       Assert( const char *file, int line, int type, const char *fmt, ... );
extern BOOL       LoadRegistryInfo( const char *pszName, void *pvBuf, long *plSize ); // win_qe3.cpp 0x4999C0
void              LayersDlg_RefreshIfOpen();                  // below (dialog refresh hook)

// ══════════════════════════════════════════════════════════════════════════════
//  CORE OPERATIONS (UI-independent — the gate exercises these directly)
// ══════════════════════════════════════════════════════════════════════════════

// 0x41C850  CLayersDlg::AssignSelectionToLayer — assign every selected brush to
// `layerName`.  IDA: walk selected_brushes; free def->parent_layer_string; dup the
// layer string into it; reset the instance's xx7 faceVis flag.  We route through
// Brush_SetInstanceLayerString (sub_4758E0), which does exactly that.
void Layers_AssignSelectionToLayer( const char *layerName )
{
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        // (the binary inlines Brush_SetInstanceLayerString here; the helper carries the
        // brush.cpp:2365/2354 checks itself)
        Brush_SetInstanceLayerString( i, layerName );   // free old + dup new + xx7=0
    }
    g_nUpdateBits = -1;
}

// 0x41CA60..0x41CB80  the Hide/Show layer handlers (via sub_41D0F0): set the layer's
// flag in the map (layers_01/layers_02) AND toggle its runtime FilterBrush
// visibility.  `hidden` true → hide.  Returns false if the layer doesn't exist.
bool Layers_SetHidden( const char *layerName, bool hidden )
{
    if ( !Layers_Exists( layerName ) )
        return false;

    // (1) the persisted layer flag (round-trips in the .map "flags" line).
    // sub_41D0F0 (hide)  = Layers_ClearFlag(name, 8) then Layers_SetFlag(name, 1)
    // sub_41D270 (show)  = Layers_ClearFlag(name, 9)   ← clears hidden AND frozen
    // i.e. hiding UNFREEZES and showing UNFREEZES — the tree's three visual states
    // (0x1000 shown / 0x2000 hidden / 0x3000 frozen) are mutually exclusive.
    // [audit U7: the port previously only touched bit 1, so a frozen layer stayed
    //  frozen after Show and Hide+Show could not clear a freeze.]
    if ( hidden )
    {
        Layers_ClearFlag( layerName, 8 );
        Layers_SetFlag( layerName, 1 );
    }
    else
    {
        Layers_ClearFlag( layerName, 9 );
    }

    // (2) the runtime visibility filter (drives FilterBrush → views).  Rebuild the
    // script_layer filter set first so a freshly-relevant layer has an entry, then
    // flip its isShown.  (No-op when the layer carries no script_layer brushes —
    // matches the binary, where only layers present in the CCheckListBox toggle.)
    Layers_RebuildVisibilityFilters();
    Layers_SetLayerVisible( layerName, !hidden );
    return true;
}

// 0x41CB20 / 0x41CB50 via sub_41D1B0 — FREEZE a layer.  The binary's tree helper sets
// the item state image to 0x3000 then Layers_ClearFlag(name,1) + Layers_SetFlag(name,8):
// freezing implies NOT hidden (the three states are exclusive, see Layers_SetHidden).
// There is no separate "unfreeze" command in MENU233 — Show Layer clears bit 8 too.
bool Layers_SetFrozen( const char *layerName )
{
    if ( !Layers_Exists( layerName ) )
        return false;
    Layers_ClearFlag( layerName, 1 );
    Layers_SetFlag( layerName, 8 );
    // Frozen layers still DRAW (they are just not selectable — Layers_KeyIsFrozen in
    // layers.cpp drives that), so unlike Hide there is no visibility-filter flip here.
    return true;
}

// 0x41CB80 / 0x41CBB0 via sub_41D3D0 / sub_41D320 — collapse / expand a layer's subtree.
// The binary does TVM_EXPAND on the CTreeCtrl item (guarded by a re-entrancy byte) and
// then Layers_ClearFlag(name,4) / Layers_SetFlag(name,4) — the "expanded" bit that
// round-trips in the .map layer flags.
// UI REDUCTION (documented): this port's layers pane is a FLAT CListBox (see the file
// header), so there is no tree node to expand or collapse.  The BACKEND flag write is
// reproduced faithfully — the expanded state persists through save/load exactly as the
// binary's does — but nothing visually folds.  Both binary handlers pass bRecurse = 1,
// so the flag is written for the layer AND every descendant.
bool Layers_SetExpanded( const char *layerName, bool expanded )
{
    if ( !Layers_Exists( layerName ) )
        return false;
    if ( expanded ) Layers_SetFlag( layerName, 4 );
    else            Layers_ClearFlag( layerName, 4 );
    return true;
}

// The "and children" recursion.  The binary walks the CTreeCtrl (TVGN_CHILD/TVGN_NEXT);
// here the hierarchy lives in the layer PATH, so a child of "a/b" is any layer whose
// full path starts with "a/b/".  Applies `fn` to `layerName` first, then to every
// descendant — same pre-order as sub_41D0F0/1B0/270/320/3D0.
static void Layers_ForSelfAndChildren( const char *layerName, bool recurse,
                                       void ( *fn )( const char * ) )
{
    fn( layerName );
    if ( !recurse )
        return;

    char prefix[1100];
    _snprintf( prefix, sizeof( prefix ) - 1, "%s/", layerName );
    prefix[sizeof( prefix ) - 1] = '\0';
    const size_t plen = strlen( prefix );

    std::vector<std::pair<std::string,int>> layers;
    Layers_Enumerate( layers );
    for ( const auto &kv : layers )
    {
        if ( kv.first.size() > plen && !strncmp( kv.first.c_str(), prefix, plen ) )
            fn( kv.first.c_str() );
    }
}

// UI-independent action behind MENU233's Hide / Show / Freeze / Collapse / Expand picks
// (and their "and Children" variants): run `fn` over the layer and, when `recurse`, its
// whole subtree, then invalidate every view.
void LayerSubtreeOp_Apply( const char *layerName, bool recurse, void ( *fn )( const char * ) )
{
    Layers_ForSelfAndChildren( layerName, recurse, fn );
    g_nUpdateBits = -1;
}

// UI-independent action behind CLayerDlg's New button / MENU233 "Create New Layer".
// Faithful subset of CLayers::NewLayer (0x41C690): reject empty / "./\\" / "prefabs"
// names; if a layer is selected and isn't "The Map", nest under it ("parent/name");
// Layers_AddLayerPath + refresh.  `parentLayer` is "" (or "The Map") for a root layer;
// returns false when the name was rejected and nothing changed.
bool LayerNew_Apply( const char *name, const char *parentLayer )
{
    if ( !name[0] || strpbrk( name, "./\\" ) || !strncmp( name, "prefabs", 7 ) )
        return false;

    char full[1100];
    if ( parentLayer[0] && strcmp( parentLayer, "The Map" ) )
        _snprintf( full, sizeof( full ) - 1, "%s/%s", parentLayer, name );
    else
        _snprintf( full, sizeof( full ) - 1, "%s", name );
    full[sizeof( full ) - 1] = '\0';

    Layers_AddLayerPath( full, 0 );
    g_nUpdateBits = -1;
    return true;
}

// UI-independent guard behind Delete Layer: the two built-ins are not deletable.  Split
// out so the caller can skip its confirmation prompt for a layer that cannot be deleted.
bool Layers_CanDeleteLayer( const char *layerName )
{
    return strcmp( layerName, "000_Global" ) != 0 && strcmp( layerName, "The Map" ) != 0;
}

// UI-independent action behind CLayerDlg's Delete button / MENU233 "Delete Layer"
// (CLayers::DeleteLayer 0x41CBE0): Layers_DeleteLayer re-layers the victim's brushes onto
// the active layer.  Returns false when the layer is a built-in and nothing changed.
bool LayerDelete_Apply( const char *layerName )
{
    if ( !Layers_CanDeleteLayer( layerName ) )
        return false;
    Layers_DeleteLayer( layerName );
    g_nUpdateBits = -1;
    return true;
}

// UI-independent action behind CLayerDlg's Rename button / MENU233 "Rename Layer"
// (CLayers::RenameLayer 0x41CC60): reject 000_Global and the same bad leaf names New
// rejects, then splice `newLeaf` onto the picked layer's parent path.  Returns false when
// the rename was rejected and nothing changed.
bool LayerRename_Apply( const char *oldFull, const char *newLeaf )
{
    if ( !strcmp( oldFull, "000_Global" ) )
        return false;
    if ( !newLeaf[0] || strpbrk( newLeaf, "./\\" ) || !strncmp( newLeaf, "prefabs", 7 ) )
        return false;

    // Build the new full path = (old's parent path) + newLeaf.
    char newFull[1100];
    const char *slash = strrchr( oldFull, '/' );
    if ( slash )
    {
        int plen = (int)( slash - oldFull );
        _snprintf( newFull, sizeof( newFull ) - 1, "%.*s/%s", plen, oldFull, newLeaf );
    }
    else
        _snprintf( newFull, sizeof( newFull ) - 1, "%s", newLeaf );
    newFull[sizeof( newFull ) - 1] = '\0';

    Layers_RenameLayer( oldFull, newFull );
    g_nUpdateBits = -1;
    return true;
}

// UI-independent read behind the layers pane's list population: the sorted (full path,
// flags) rows plus the active layer's name.
void LayerList_Gather( std::vector<std::pair<std::string,int>> &layers, const char *&active )
{
    Layers_Enumerate( layers );
    active = Layers_GetActive();
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CLayerDlg, the hand-built modeless popup (CSurfaceDlg / CFindTextureDlg
//  pattern).  Every core above stays COMMON (imgui_panel_layers.cpp calls
//  LayerList_Gather / Layer*_Apply / Layers_* directly and re-gathers each frame).
// ══════════════════════════════════════════════════════════════════════════════

// ── backend → UI refresh entry ────────────────────────────────────────────────
// The binary's Layers_02 (0x41A0E0) wipes the CTreeCtrl (TVM_DELETEITEM/TVI_ROOT),
// re-inserts the "The Map" root (sub_41A070), walks layerMap re-inserting each layer
// (maybeLayers), then bolds+icons the active item (sub_41C9C0).  In the port's flat
// listbox form ALL of that is CLayerDlg::Refresh().  This free entry lets the
// backend (layers.cpp Layers_02 / sub_41C9C0) reach the singleton without exposing it.
void LayersDlg_RefreshIfOpen()
{
    // NO-MFC: no-op — the ImGui layers panel pull-populates from LayerList_Gather every
    // frame, so there is nothing to push at.
}

