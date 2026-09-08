#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\layeredmaterialwnd.cpp
//
// The layered-material authoring model: the active library entry, its layers, the
// live-add flag, and the six commands the tool palette drives.  The binary's raw-Win32
// palette (WS_EX_PALETTEWINDOW frame + COMCTL32 toolbar + custom-painted
// "LayeredMaterialList" child) is gone — imgui_panel_lyrmtl.cpp is the widget layer and
// binds to the functions below.  Library writes still occur only after its CRC changes.

#include "stdafx.h"
#include "qe3.h"
#include <gfx_d3d/r_init.h>           // R_InitRendererForWindow

extern int  Sys_Printf( const char *fmt, ... );
extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// Brush-list display rebuild.
extern void sub_47D060( selbrush_t *listHead );   // 0x47D060

// Global brush lists (qe3.cpp).
extern selbrush_t active_brushes;          // 0x23F189C
extern selbrush_t selected_brushes;        // 0x23F1864
extern selbrush_t filtered_brushes;        // 0x23F182C

// Window-update bit-mask (qe3 / Sys_UpdateWindows).
extern int g_nUpdateBits;                  // 0x25D5A74

// texwnd "layered-material window active" flag (texwnd_s.unk_bool @0x10039) — set so
// the texture-window click-apply path can add a layer.  Accessor avoids exporting the
// TU-local texwnd_s.
extern "C" void TexWnd_SetLayeredMaterialActive( int active );   // texwnd.cpp

// Data layer (layeredmaterials.cpp).
extern void  LayeredMaterials_AddEntries( char *name, HWND hWnd );   // 0x417050
extern void *LayeredMaterials_texcoords( char *entry );              // 0x417190 (delete entry; ported)

// ═══════════════════════════════════════════════════════════════════════════════
//  lyrMtlWndGlob — the tool's global state (IDB struct @ 0x181F500; type in qe3.h —
//  field names from the binary's assert strings).  The two HWND members survive as
//  plumbing only: `layerList` is the hidden blank pane R_BeginRegistrationInternal
//  attaches a fifth swap chain to (radiant_main.cpp Radiant_CreateRenderWindows).
// ═══════════════════════════════════════════════════════════════════════════════
LyrMtlWndGlob_t lyrMtlWndGlob = {};

// ── 84-byte library-entry view (the realised "activeLyrMtl" points into
//    lyrMtlGlob_Layers[]).  Same layout the data layer (layeredmaterials.cpp) uses via
//    its offset enum; named here for the tool's field accesses.  Per the IDA cap
//    (LayeredMaterialWnd_RadMtl 0x4185C0) a library entry holds at most ONE layer, so
//    only layer[0] fits the 84-byte stride.
typedef LyrEntry_t LyrMtlEntry;
static_assert( sizeof( LyrMtlEntry ) == sizeof(LyrEntry_t), "native library entry" );
// The per-layer pair {id,handle} stride is 8 bytes; the tool indexes layer i at
// (0x4C + 8*i, 0x50 + 8*i).  Helpers keep the byte arithmetic identical to the IDB.
static inline int  *EntryLayerId    ( LyrMtlEntry *e, int i ) { return &e->layers[i].id; }
static inline void **EntryLayerHandle( LyrMtlEntry *e, int i ) { return (void **)&e->layers[i].handle; }
static inline LyrMtlEntry *ActiveEntry() { return (LyrMtlEntry *)(intptr_t)lyrMtlWndGlob.activeLyrMtl; }

// Forward decls (mutual references within this TU).
static BOOL sub_417710();
static int  LayeredMaterialWnd_ToggleLiveAdd();
BOOL        LayeredMaterialWnd_Layer( unsigned int newLayerIndex );

// ═══════════════════════════════════════════════════════════════════════════════
//  sub_417440  (0x417440) — toggle the "Live add layer" state and mirror it into the
//  texture window.  The binary drove the toolbar button's TBSTATE_CHECKED bit and read
//  the new state back out of it; the flag itself is the whole model.
// ═══════════════════════════════════════════════════════════════════════════════
static int LayeredMaterialWnd_ToggleLiveAdd()
{
    const int newChecked = ( lyrMtlWndGlob.liveAddActive ^ 1 ) & 1;
    lyrMtlWndGlob.liveAddActive = newChecked;
    TexWnd_SetLayeredMaterialActive( newChecked );
    return newChecked;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  sub_417710  (0x417710) — rebuild the three brush-list displays after a layered-
//  material change (it can affect drawn brushes).
// ═══════════════════════════════════════════════════════════════════════════════
static BOOL sub_417710()
{
    sub_47D060( &active_brushes );
    sub_47D060( &selected_brushes );
    sub_47D060( &filtered_brushes );
    g_nUpdateBits = -1;
    return TRUE;
}

// UI-independent action behind the layered-material tool's "New" command.  The name
// prompt's MessageBox owner is the frame (LayeredMaterials_AddEntries reports invalid /
// duplicate names through it).
void LyrMtlNewMaterial_Apply( const char *name )
{
    LayeredMaterials_AddEntries( (char *)name, g_qeglobals.d_hwndMain );
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LayeredMaterialWnd_Layer  (0x417940) — reorder: move the selected layer to
//  newLayerIndex by swapping the two {id,handle} pairs, then re-select newLayerIndex.
// ═══════════════════════════════════════════════════════════════════════════════
BOOL LayeredMaterialWnd_Layer( unsigned int newLayerIndex )
{
    LyrMtlEntry *e = ActiveEntry();
    iassert( lyrMtlWndGlob.activeLyrMtl );   // LayeredMaterialWnd.cpp:200
    bcassert( newLayerIndex, (unsigned)e->layerCount );        // LayeredMaterialWnd.cpp:201
    int sel = lyrMtlWndGlob.selectedLayerIndex;
    bcassert( (unsigned)sel, (unsigned)e->layerCount );        // LayeredMaterialWnd.cpp:202

    // Swap layer[newLayerIndex] <-> layer[sel].
    int   tmpId     = *EntryLayerId( e, newLayerIndex );
    void *tmpHandle = *EntryLayerHandle( e, newLayerIndex );
    *EntryLayerId( e, newLayerIndex )     = *EntryLayerId( e, sel );
    *EntryLayerHandle( e, newLayerIndex ) = *EntryLayerHandle( e, sel );
    *EntryLayerId( ActiveEntry(), lyrMtlWndGlob.selectedLayerIndex )     = tmpId;
    *EntryLayerHandle( ActiveEntry(), lyrMtlWndGlob.selectedLayerIndex ) = tmpHandle;

    lyrMtlWndGlob.selectedLayerIndex = newLayerIndex;
    return sub_417710();
}

// UI-independent action behind the layered-material tool's "Delete material" command.
BOOL LyrMtlDeleteMaterial_Apply()
{
    // LayeredMaterials_texcoords (0x417190, layeredmaterials.cpp) — now a real port:
    // replaces the deleted material with "$default" on the live brushes (via the ported
    // FindReplaceTextures), then compacts the in-memory library.  Its only remaining
    // FATAL is the FindReplaceTextures flag&4 prefab-recursion branch, which fires ONLY
    // on an explicit operator delete with a REFERENCING prefab present (disk-mutating
    // Map_SaveFile of stock .maps — HARD RULE).  Faithful to the binary.
    LayeredMaterials_texcoords( (char *)(intptr_t)lyrMtlWndGlob.activeLyrMtl );
    lyrMtlWndGlob.activeLyrMtl = 0;
    return sub_417710();
}

// UI-independent action behind the layered-material tool's "Remove layer" command.
BOOL LyrMtlRemoveLayer_Apply()
{
    LyrMtlEntry *e = ActiveEntry();
    iassert( lyrMtlWndGlob.activeLyrMtl );   // LayeredMaterialWnd.cpp:180
    int sel = lyrMtlWndGlob.selectedLayerIndex;
    bcassert( (unsigned)sel, (unsigned)e->layerCount );    // LayeredMaterialWnd.cpp:181
    // memcpy( &layer[sel], &layer[sel+1], 8 * (--layerCount - sel) )
    int remaining = --e->layerCount - sel;
    memmove( &e->layers[sel], &e->layers[sel + 1], sizeof(LyrEntryLayer_t) * remaining );
    if ( lyrMtlWndGlob.selectedLayerIndex )
        --lyrMtlWndGlob.selectedLayerIndex;
    return sub_417710();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LayeredMaterialWnd_Commands  (0x417A60) — toolbar / WM_COMMAND dispatch.
//    0 New, 1 Delete-material, 2 toggle-Live, 3 Remove-layer, 4 Down, 5 Up.
//  Command 0 is the name prompt and belongs to the widget layer (the panel runs the
//  ImGui modal and calls LyrMtlNewMaterial_Apply itself), so it is not handled here.
// ═══════════════════════════════════════════════════════════════════════════════
LRESULT LayeredMaterialWnd_Commands( int cmd )
{
    switch ( cmd )
    {
    case 1:   // Delete the active layered material.
        return LyrMtlDeleteMaterial_Apply();

    case 2:
        return LayeredMaterialWnd_ToggleLiveAdd();

    case 3:   // Remove the selected layer (shift the rest down, shrink layerCount).
        return LyrMtlRemoveLayer_Apply();

    case 4:
        return LayeredMaterialWnd_Layer( lyrMtlWndGlob.selectedLayerIndex + 1 );

    case 5:
        return LayeredMaterialWnd_Layer( lyrMtlWndGlob.selectedLayerIndex - 1 );
    }
    return 0;
}

// UI-independent action behind the layer list's row selection.
int LyrMtlSelectLayer_Apply( int layerIndex )
{
    lyrMtlWndGlob.selectedLayerIndex = layerIndex;
    g_nUpdateBits |= 0x10u;   // W_TEXTURE
    return layerIndex;
}

// ── panel-facing reads (imgui_panel_lyrmtl.cpp) ───────────────────────────────
// The active entry's name (the binary put it in the frame caption), its layer count,
// and one layer's radMtl handle — the walk the native list painter (sub_417D60) made.
const char *LyrMtlWnd_ActiveName()
{
    if ( !lyrMtlWndGlob.activeLyrMtl )
        return nullptr;
    return ActiveEntry()->name;
}

int LyrMtlWnd_LayerCount()
{
    if ( !lyrMtlWndGlob.activeLyrMtl )
        return 0;
    return ActiveEntry()->layerCount;
}

qtexture_s *LyrMtlWnd_LayerHandle( int i )
{
    if ( !lyrMtlWndGlob.activeLyrMtl || i < 0 || i >= ActiveEntry()->layerCount )
        return nullptr;
    return (qtexture_s *)*EntryLayerHandle( ActiveEntry(), i );
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LayeredMaterialWnd_InitRenderer  (0x418580) — attach the CoD renderer to the
//  layer-list HWND.  R_BeginRegistrationInternal (gfxwrapper.cpp) calls this; the HWND
//  is the permanently hidden blank pane, kept because the renderer wants a fifth window.
// ═══════════════════════════════════════════════════════════════════════════════
char LayeredMaterialWnd_InitRenderer()
{
    iassert( lyrMtlWndGlob.layerList );   // LayeredMaterialWnd.cpp:726
    return R_InitRendererForWindow( lyrMtlWndGlob.layerList );
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LayeredMaterialWnd_RadMtl  (0x4185C0) — add a radMtl (texture-window material) as a
//  new layer of the active layered material.  Rejects: no active material, layer cap
//  (1), or a duplicate of an existing layer.  On success, appends the layer (id = nextId++)
//  and rebuilds.  This is the texture-window click-apply target when "Live add" is armed.
// ═══════════════════════════════════════════════════════════════════════════════
LRESULT LayeredMaterialWnd_RadMtl( qtexture_s *radMtl )
{
    iassert( radMtl );   // LayeredMaterialWnd.cpp:736

    LyrMtlEntry *lyrMtl = ActiveEntry();   // the binary's local name
    iassert( lyrMtl );   // LayeredMaterialWnd.cpp:739

    int count = lyrMtl->layerCount;
    if ( count == 1 )
        return Sys_Printf( "Cannot have more than %i layers in a layered material.\n", 1 );

    // Duplicate check: scan existing layers for the same radMtl handle.
    if ( count > 0 )
    {
        for ( int i = 0; i < count; ++i )
        {
            if ( *EntryLayerHandle( lyrMtl, i ) == radMtl )
                return Sys_Printf( "Material '%s' is already a layer in '%s'.\n",
                                   radMtl->name, lyrMtl->name );
        }
    }

    // Append the new layer: handle then id (= nextId, post-incremented).
    *EntryLayerHandle( lyrMtl, count ) = radMtl;
    *EntryLayerId( lyrMtl, lyrMtl->layerCount++ ) = lyrMtl->nextId++;

    sub_47D060( &active_brushes );
    sub_47D060( &selected_brushes );
    sub_47D060( &filtered_brushes );
    g_nUpdateBits = -1;
    return 0;
}
