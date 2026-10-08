#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Patch dialogs: the bend / insert-delete state prompt, the Patch Inspector and the
// Patch Density cores.  KIWI: the AdvPatchEditDlg terrain-paint path (mode store,
// accessors, Patch_PaintDrag) is gone - Terrain Sculpt (kiwi_terrain.cpp) replaced it.

#include "stdafx.h"
#include "res/resource.h"
#include "qe3.h"
#include "kiwi_ported.h"   // KIWI: shared engine/editor externs
#include <stdlib.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  ShowInfoDialog (0x40BE90) — the modeless "Information" state prompt.
//
//  A caption-only popup holding one disabled multiline edit (IDD_INFORMATION, PE
//  resource 150).  Patch INSERT/DELETE mode uses it (the binary's BEND mode too - gone in
//  KIWI) to tell the user which key does what for the current sub-state; the message is
//  g_pInsDelStateMsg below.
//
//  The binary keeps the dialog in a static object and tests its m_hWnd to decide whether
//  to Create it; this port uses a lazily-new'd pointer.  Focus goes back to the main frame
//  (0x40bec7) — the prompt must never steal keyboard focus, or the TAB/ENTER/ESC keys the
//  message describes would go to the prompt instead of the view that implements the mode.
// ═══════════════════════════════════════════════════════════════════════════════

// Insert/delete (redisperse) prompt (0x73B128).
const char *g_pInsDelStateMsg = "Use TAB to cycle through available rows/columns for insertion/deletion. Press INS to insert at the highlight, DEL to remove the pair";


void ShowInfoDialog( const char *msg )
{
    // NO-MFC: no-op — IDD_INFORMATION is an MFC CDialog; the insert-delete state prompt
    // text is unreachable in this shell (the mode itself still works).
    (void)msg;
}

// The hide half (SW_HIDE on the static dialog, only when its m_hWnd exists).
void HideInfoDialog()
{
    // NO-MFC: no-op — ShowInfoDialog never created a prompt to hide.
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CPatchDialog — the Patch Inspector "Patch Properties" dialog (binary CWnd_PatchDialog
//  0x25D7278, PE resource 0xAC = IDD_PATCH_PROPERTIES).  A MODELESS CDialog.  This piece
//  ports the DETAILS half READ-ONLY: pick a control point with the Row/Column combos and
//  show its X/Y/Z + current-layer S/T.  The Type combo, the Apply write-back, and the whole
//  Texturing half (texdef edits/spins + CAP/Set/Natural/Fit) are later pieces.
//    DoPatchInspector (0x436d30) / g_PatchDialog.GetPatchInfo (0x436ba0) / 0x436e10 /
//    UpdatePatchInspector (0x436db0).
// ═══════════════════════════════════════════════════════════════════════════════
extern void       Patch_NaturalizeSelected( bool unk, bool cap, float x, float y );  // pmesh.cpp 0x447fd0
extern void       Brush_FitTexture( float x, float y, int acrossSelection );          // select.cpp 0x4939e0
extern void       Patch_SetTextureInfo( texdef_sub_t *texDef );                       // pmesh.cpp 0x447760
extern void       Patch_SetTexturing( float sx, float sy, int mode );                 // pmesh.cpp 0x446b60

// ── CTextureLayout (IDD_PATCH_TEXTURE_LAYOUT / resource 0x9F) ──────────────────────
//  The modal "Patch texture layout" dialog opened by the Patch Inspector's "Set..." button
//  (0x459860 ctor / 0x436980).  Enter a texture x/y scale (default 4) + pick a layout mode,
//  then OK -> Patch_SetTexturing(x, y, mode).  Modes: 2D distance->0 (natural), 3D distance->1
//  (arc-length), emulates curves->2 (grid).
// MFC shell — CTextureLayout (the modal "Set..." helper).  PatchInspector_SetTexturing
// below is the COMMON core the ImGui patch panel calls with its own scale/mode inputs.

// One patch control point as the inspector shows and edits it: which cell (the Row/Column
// combo selections), its position, and its current-layer texture coords.
struct patchPointState_t
{
    int   row, col;   // combo 1301 / 1302 selections (idx row+16*col into patchMesh_t.ctrl)
    float xyz[3];     // edits 1089 / 1142 / 1147
    float st[2];      // edits 1102 / 1106
};

// UI-independent core read behind the inspector's control-point display (0x436e10) — fill
// `pt`'s xyz/st from the row/col cell; false = no patch, or the cell is out of range.
bool PatchInspector_GatherPoint( patchMesh_t *def, patchPointState_t &pt )
{
    if ( !def )
        return false;
    if ( pt.row < 0 || pt.row >= def->height || pt.col < 0 || pt.col >= def->width )
        return false;
    drawVert_t *cp = &def->ctrl[pt.col][pt.row];                     // idx row+16*col
    pt.xyz[0] = cp->xyz[0];  pt.xyz[1] = cp->xyz[1];  pt.xyz[2] = cp->xyz[2];
    const float *st = (const float *)( (const char *)&cp->texCoord + 8 * g_qeglobals.current_edit_layer );
    pt.st[0] = st[0];  pt.st[1] = st[1];
    return true;
}

// UI-independent core read behind g_PatchDialog.GetPatchInfo — the selected single patch's
// mesh def (the binary's this+116), or null when the selection is not one patch.
patchMesh_t *PatchInspector_GatherSelectedPatch()
{
    patch_t *patch = nullptr;
    if ( QE_SingleBrush() && ( patch = selected_brushes.next->patch ) != nullptr )
        return patch->def;
    return nullptr;
}

// UI-independent action behind the inspector's Apply button.
// OnApply (0x436ef0) — write the edited X/Y/Z + current-layer S/T back into the selected
// control point, bump the patch version, and force a redraw (no explicit Patch_Rebuild —
// the version bump + g_nUpdateBits=-1 re-tessellate on the next render, as in the binary).
void PatchInspector_ApplyPoint( patchMesh_t *def, const patchPointState_t &pt )
{
    if ( !def )
        return;
    if ( pt.row < 0 || pt.row >= def->height || pt.col < 0 || pt.col >= def->width )
        return;
    drawVert_t *cp = &def->ctrl[pt.col][pt.row];
    cp->xyz[0] = pt.xyz[0];  cp->xyz[1] = pt.xyz[1];  cp->xyz[2] = pt.xyz[2];
    float *st = (float *)( (char *)&cp->texCoord + 8 * g_qeglobals.current_edit_layer );
    st[0] = pt.st[0];  st[1] = pt.st[1];
    ++def->version;
    g_nUpdateBits = -1;         // force a full redraw (re-tessellates the dirty patch)
}

// UI-independent actions behind the inspector's texturing buttons — thin wrappers over the
// ported patch-texturing ops, scaled by the current edit layer's sample size
// (random_texture_stuff[layer].sampleSize).
void PatchInspector_Cap()                          // CAP (1282) 0x4368D0
{
    float s = g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].sampleSize;
    Patch_NaturalizeSelected( true, false, s, s );
    g_nUpdateBits = -1;
}

void PatchInspector_Natural()                       // Natural (1284) 0x436940
{
    float s = g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].sampleSize;
    Patch_NaturalizeSelected( false, false, s, s );
    g_nUpdateBits = -1;
}

void PatchInspector_Fit()                           // Fit (1286) 0x436910
{
    Brush_FitTexture( 1.0f, 1.0f, 0 );
    g_nUpdateBits = -1;
}

// "Set..." (1283) 0x436980.  `bAccepted` is the CTextureLayout modal result: the port
// bumps g_nUpdateBits either way (the repaint brackets the DIALOG, not the OK branch — the
// CurveThicken_Apply situation), so the flag rides along instead of splitting the update out.
void PatchInspector_SetTexturing( bool bAccepted, float sx, float sy, int mode )
{
    if ( bAccepted )
        Patch_SetTexturing( sx, sy, mode );
    g_nUpdateBits = -1;
}

// The five texdef spin STEPs as the inspector's step edits hold them (binary ctor
// defaults: stretch/shift 0.05, rotate 45).
struct texdefSpinState_t
{
    float rotate;        // edit 1217
    float stretchHoriz;  // edit 1211
    float stretchVert;   // edit 1213
    float shiftHoriz;    // edit 1195
    float shiftVert;     // edit 1196
};

// UI-independent action behind the inspector's texdef spin arrows.
// Texdef spin arrows (1248/1251/1254/1257/1259) — apply a RELATIVE texture transform
// by the step typed in the matching edit (0x4370E0 -> Patch_SetTextureInfo): shift =
// +/-step, stretch = *(1 -/+ step), rotate = +/-step.  Up = iDelta > 0.
void PatchInspector_SpinTexdef( UINT id, bool up, const texdefSpinState_t &step )
{
    texdef_sub_t td;
    memset( &td, 0, sizeof( td ) );
    switch ( id )
    {
    case 1259: { float s = step.rotate;       td.rotate  = up ?  s        : -s;        break; } // Rotate
    case 1257: { float s = step.stretchHoriz; td.size[0] = up ? 1.0f - s  : 1.0f + s;  break; } // Horiz stretch
    case 1254: { float s = step.stretchVert;  td.size[1] = up ? 1.0f - s  : 1.0f + s;  break; } // Vert  stretch
    case 1248: { float s = step.shiftHoriz;   td.shift[0] = up ?  s        : -s;        break; } // Horiz shift
    case 1251: { float s = step.shiftVert;    td.shift[1] = up ?  s        : -s;        break; } // Vert  shift
    default:   return;
    }
    Patch_SetTextureInfo( &td );
    g_nUpdateBits |= 1;
}

// ── MFC shell — CPatchInspectorDlg (IDD_PATCH_PROPERTIES) ────────────────────────
// Every PatchInspector_* core above stays COMMON (imgui_panel_patch.cpp calls them).  The
// four free entry points after the class (DoPatchInspector / UpdatePatchInspector /
// g_PatchDialog_GetHwnd / g_PatchDialog_GetPatchInfo) are reached from CORE TUs, so they
// keep their symbols with per-body fences — except g_PatchDialog_GetHwnd, whose RETURN TYPE
// is CWnd* (see the U-GUARD report: brush.cpp / pmesh.cpp need a shell-agnostic accessor).

// DoPatchInspector (0x436d30) — create (first use) + show the modeless inspector, then populate.
void DoPatchInspector()
{
    // NO-MFC: no-op — the ImGui patch panel (imgui_panel_patch.cpp) IS the inspector and
    // pull-populates from PatchInspector_Gather*; nothing to create or show here.
}

// UpdatePatchInspector (0x436db0) — refresh the inspector if it is open.
void UpdatePatchInspector()
{
    // NO-MFC: no-op — the panel re-gathers every frame, so there is no push to make.
}

// Global Patch Inspector accessors used by brush, patch, and selection refresh paths.

// PatchDialog_IsOpen (U-GUARD-2) — the shell-agnostic choke point for the binary's
// `if (CWnd_PatchDialog.m_hWnd)` gate: constant false under
// KISAK_NO_MFC, so every guarded GetPatchInfo() push is skipped exactly as it is in the MFC
// shell with the inspector never opened (the ImGui patch panel re-gathers every frame, so
// there is no push to make).  Semantics are EXISTENCE, not visibility — deliberately the
// same test g_PatchDialog_GetHwnd's null-check performed, because the binary tests the
// dialog's m_hWnd and all 22 call sites (brush.cpp/pmesh.cpp/select.cpp) are pure boolean
// gates that must keep firing after CPatchInspectorDlg::OnOK hides (not destroys) it.
bool PatchDialog_IsOpen()
{
    return false;
}

// The pre-U-GUARD-2 accessor.  Its RETURN TYPE is CWnd*, so it cannot survive into the
// no-MFC shell; MFC-side callers may still want the object, so it keeps its symbol behind
// the fence.  Core TUs use PatchDialog_IsOpen() above.
void g_PatchDialog_GetPatchInfo()
{
    // NO-MFC: no-op — the panel re-gathers every frame (same as UpdatePatchInspector).
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CPatchDensityDlg — "Patch density" (Curve → Simple Patch Mesh, menu cmd 32856).
//  A MODAL CDialog over IDD_PATCH_DENSITY (157).  Two CBS_DROPDOWNLIST combos
//  (1280 width / 1281 height) choose the patch density from the binary's dimension
//  table {3,5,7,9,11,13,15}; OK calls Patch_GenericMesh over the selected brush.
//
//  Binary cluster:
//    CPatchDensityDlg_Init          0x436280 — ctor (IDD 0x9D, 2 CComboBox @ +0x74/+0xC8)
//    CPatchDensityDlg_DoDataExchange 0x436330 — DDX_Control(1280→combo0, 1281→combo1)
//    CPatchDensityDlg_OnInitDialog  0x436440 — base OnInitDialog + CB_SETCURSEL(last sel)
//    CPatchDensityDlg_OnOK          0x436390 — read CB_GETCURSEL → dims[] → Patch_GenericMesh
//    CMainFrame::OnCurveSimplepatchmesh 0x429a20 — Undo bracket + DoModal + destruct
//
//  NOTE: the binary's OnInitDialog only CB_SETCURSELs the last selection —
//  it never adds the combo items, and the dialog template (PE rsrc 157)
//  carries empty combos.  i.e. the stock dialog shows EMPTY dropdowns (the menu command
//  is also un-wired/auto-greyed in stock, per PROGRESS.md).  Since the combos must hold
//  the dims for CB_GETCURSEL→dims[sel] to mean anything, we POPULATE them from the
//  binary's own g_patchDensityDims table {3,5,7,9,11,13,15} in OnInitDialog (the
//  GtkRadiant DoNewPatchDlg confirms these exact 7 items + active index 0) — the only
//  added line vs the binary, so the feature actually works.
// ═══════════════════════════════════════════════════════════════════════════════
#include "mainfrm.h"                  // CMainFrame / CXYWnd (m_pActiveXY->m_nViewType)
#include "xywnd.h"                    // xywndState_t / Ed_ActiveXY (U-GLOBALS)
extern selbrush_t *Patch_GenericMesh( int nWidth, int nHeight, int nOrientation,
                                      char bDeleteSource, char bOverwrite );   // pmesh.cpp (0x43b310)
// The TERRAIN arm is the binary's own Create_Terrain (0x43B660), reached in stock from
// TerrainDlg_NewPatch (0x458FB0) — NOT a PATCH_TERRAIN variant of Patch_GenericMesh (no
// such function exists).  It takes (width, height, viewType) and always replaces the
// selected brush, so it needs no bDeleteSource/bOverwrite.
extern selbrush_t *Create_Terrain( int width, int height, int orientation );   // pmesh.cpp (0x43B660)

// The binary's dimension table (g_patchDensityDims @0x73B170): combo index → patch size.
static const int s_patchDensityDims[7] = { 3, 5, 7, 9, 11, 13, 15 };

// Last combo selections, remembered across opens (g_patchDensityLastWidthSel 0x25D5AFC /
// g_patchDensityLastHeightSel 0x25D5B00; both 0 = "3" on first open).
static int g_patchDensityLastWidthSel  = 0;
static int g_patchDensityLastHeightSel = 0;
static int g_terrainLastWidthSel       = 0;
static int g_terrainLastHeightSel      = 0;

// UI-independent action behind CPatchDensityDlg's OK (0x436390) — validate the two combo
// selections, build the patch/terrain mesh, then remember the selections for the next open.
// (The undo bracket stays with the caller: DoSimpleTerrainPatchMesh / 0x429a20 opens it
// around the DIALOG, not the create — even a cancel opens/closes the bracket.)
void PatchDensity_Apply( bool terrain, int wSel, int hSel )
{
    const unsigned int maxSel = terrain ? 14u : 6u;
    if ( (unsigned int)wSel <= maxSel && (unsigned int)hSel <= maxSel )
    {
        if ( terrain )
            Create_Terrain( wSel + 2, hSel + 2,
                            Ed_ActiveXY()->m_nViewType );
        else
            Patch_GenericMesh( s_patchDensityDims[wSel], s_patchDensityDims[hSel],
                               Ed_ActiveXY()->m_nViewType, 1, 0 );
        g_nUpdateBits = -1;
    }
    if ( terrain )
    {
        g_terrainLastWidthSel  = wSel;
        g_terrainLastHeightSel = hSel;
    }
    else
    {
        g_patchDensityLastWidthSel  = wSel;
        g_patchDensityLastHeightSel = hSel;
    }
}

// ImGui shell — imgui_panel_patchdensity.cpp replaces CPatchDensityDlg.  PatchDensity_Apply
// above is the COMMON create core; PatchDensity_Commit below is the undo-bracketed commit the
// panel's "Create" button calls (the body of the binary's OnOK path + OnCurveSimplepatchmesh's
// Undo bracket, minus the modal DoModal).  The menu commands now just OPEN the panel.
extern void ImGuiPanel_PatchDensity_Open( bool terrain );   // imgui_panel_patchdensity.cpp

// The "Create" commit: re-check the single-brush precondition (the panel is non-modal, so the
// selection could have changed since it opened), then run the create inside the SAME single
// Undo op the binary's OnCurveSimplepatchmesh (0x429a20) wrapped around the modal dialog.
void PatchDensity_Commit( bool terrain, int wSel, int hSel )
{
    if ( !QE_SingleBrush() )
    {
        Sys_Printf( "Simple %s: select exactly one brush first.\n",
                    terrain ? "terrain patch" : "patch mesh" );
        return;
    }
    Undo_ClearRedo();
    Undo_GeneralStart( terrain ? "make simple terrain patch" : "make simple patch mesh" );
    Undo_AddBrushList( &selected_brushes );
    PatchDensity_Apply( terrain, wSel, hSel );   // Patch_GenericMesh / Create_Terrain + g_nUpdateBits
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnCurveSimplepatchmesh (0x429a20, menu cmd 32856 / 0x8058 — verified from the
// CMainFrame command table {cmdId,cmdId,0x38,pfn}).  The binary ran the density dialog MODALLY
// inside an Undo bracket; here the (non-modal) ImGui panel owns the density pick and its
// "Create" button runs PatchDensity_Commit, so this just guards the precondition and opens it.
void DoSimpleTerrainPatchMesh( bool terrain )
{
    if ( !QE_SingleBrush() )
        return;
    ImGuiPanel_PatchDensity_Open( terrain );
}

void DoSimplePatchMesh()
{
    DoSimpleTerrainPatchMesh( false );
}
