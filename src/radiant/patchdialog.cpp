#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Patch creation and advanced terrain-paint dialogs.

#include "stdafx.h"
#include "res/resource.h"
#include "qe3.h"
#include <stdlib.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  CAdvPatchEditDlg — the terrain-paint settings dialog (paint epic piece 6b).
//  A MODELESS CDialog over IDD_ADVPATCHEDIT (the controls carry the binary's exact
//  IDs).  The 3 radius/strength EDITs sync into the apply chain's control table
//  (g_curveEditCtrls via CurveEdit_SetCtrl) on change; the mode RADIOs and channel
//  CHECKBOXes are read LIVE by sub_401DB0 / sub_43E6F0 (piece 7) through GetDlgItem
//  on the global dialog pointer (AdvPatchEdit_GetDlg).  Modeless so it stays open
//  while the user paints; Close/Cancel just hides it.
// ═══════════════════════════════════════════════════════════════════════════════
extern void CurveEdit_SetCtrl( int slot, int id, float value );   // pmesh.cpp (gate seeder)
extern void  CurveEdit_BindData( int slot, int trackbarId, int editId, float defVal, float mn, float mx, float step );
extern float CurveEdit_SnapStore( int slot, float value );        // sub_4010D0 clamp+snap
extern int   CurveEdit_TrackbarId( int slot );
extern int   CurveEdit_EditId( int slot );
extern int   CurveEdit_StepCount( int slot );
extern int   CurveEdit_StepIndex( int slot );
extern void  CurveEdit_SetHwnds( int slot, void *hTrackbar, void *hEdit );
extern float CurveEdit_SetFromStep( int slot, int pos );
extern float CurveEdit_DisplayValue( int slot );
extern float CurveEdit_InputToValue( int slot, float typed );
extern void  CurveEdit_SetRange( int slot, float maxVal );        // sub_4015C0 range edits

// Trackbar messages (raw, to avoid an afxcmn/CSliderCtrl dependency in this TU).
#define KRAD_TBM_GETPOS    0x0400   // WM_USER+0
#define KRAD_TBM_SETPOS    0x0405   // WM_USER+5
#define KRAD_TBM_SETRANGE  0x0406   // WM_USER+6
extern unsigned int  g_paintColorBGR;   // paint target colour {B,G,R} (defined below)
extern byte g_paintColorA;     // paint target alpha       (defined below)

// UI-independent action behind CAdvPatchEditDlg's buddy edits — transform the typed value
// (sub_401CF0 for amplitude) then clamp+snap it into the slot's stored value.
void AdvPatchEdit_ApplySlotValue( int slot, float typed )
{
    CurveEdit_SnapStore( slot, CurveEdit_InputToValue( slot, typed ) );
}

// UI-independent action behind CAdvPatchEditDlg's inner/outer slider-range edits — set the
// slot's max, rebuilding its [0,64] range and re-snapping the value (sub_4015C0).
void AdvPatchEdit_ApplySlotRange( int slot, float maxVal )
{
    CurveEdit_SetRange( slot, maxVal );
}

// UI-independent action behind CAdvPatchEditDlg's slider drags — thumb pos -> slot value.
void AdvPatchEdit_ApplySlotStep( int slot, int pos )
{
    CurveEdit_SetFromStep( slot, pos );
}

// UI-independent action behind CAdvPatchEditDlg's "Color..." button — store the picked colour.
void AdvPatchEdit_ApplyPaintColor( COLORREF color )
{
    g_paintColorBGR = color;          // raw COLORREF, matching this[323]
}

// UI-independent action behind CAdvPatchEditDlg's "Alpha..." button — alpha = (R+G+B)/3.
void AdvPatchEdit_ApplyPaintAlpha( COLORREF c )
{
    g_paintColorA = (byte)( ( ( c & 0xFF ) + ( ( c >> 16 ) & 0xFF ) + ( ( c >> 8 ) & 0xFF ) ) / 3 );
}

// ── MFC shell — CAdvPatchEditDlg itself + the singleton + its raw accessor ────────
// The five AdvPatchEdit_Apply* cores above stay COMMON (imgui_panel_advpatch.cpp drives
// them).  The live-control readers below this block (AdvDlg_IsChecked /
// CurvEditDlg_OnSomeSetting / AdvPatchEdit_SetMode / AdvPatchEdit_ShowHeight) keep their
// symbols — core TUs call them — with per-body NO-MFC fences.

// Forwarder so the free paint-drag (sub_43E6F0) can push the eyedroppered height into the
// dialog's 1467 edit box (CurvEditDlg::OnHeightText) without befriending the class.
static void AdvPatchEdit_ShowHeight( float h )
{
    // NO-MFC: no-op — there is no dialog edit to push into; the eyedropper still stores the
    // picked height/colour in g_paintChannelVal / g_paintColor* for the panel to re-read.
    (void)h;
}

// CurvEditDlg::OnSomeSetting (0x401F90) — is the "apply to unselected patches too" checkbox
// (1432, IDB dword_25D6570) checked on the live dialog?  Gates paint/soft-sel onto the active
// (unselected) brush list (sub_43E4F0 / sub_43DD00) and the no-selection paint start (Drag_Begin).
int CurvEditDlg_OnSomeSetting()
{
    // NO-MFC: returns 0 (unchecked) — same as the MFC shell reports with the dialog never
    // created, so paint/soft-sel stay restricted to the SELECTED brushes.
    return 0;
}

// sub_401E10 — programmatically select the mode radio whose value == `mode` (walks the
// mode table, BM_SETCHECKs each), then repaint.  Used by Patch_FinishCurveDrag to switch
// from "Grab Value" (5) to "Flatten" (2) after an eyedropper drag.
// ── AdvPatchEdit control store (UI-rework) ──────────────────────────────────────
// The binary read the mode radios + channel checkboxes LIVE off the modeless dialog's HWND
// via BM_GETCHECK. With no MFC dialog those reads returned "unchecked" → mode 6 (Disabled) →
// terrain paint permanently OFF. This store is the shell-agnostic replacement: the ImGui
// AdvPatch panel writes it, and AdvDlg_IsChecked / sub_401DB0 read it, so paint mode + channel
// selection actually take effect. Defaults match a fresh dialog: mode 6 (Disabled), no channels.
static int  s_advMode        = 6;                          // 0..6 (sub_401DB0 index); 6 = Disabled
// Channel defaults: HEIGHT on. The stroke's mask is built from these (sub_43E6F0); with none
// ticked the mask is 0 and paint touches nothing. Height-on makes Raise/Lower/Smooth/Noise
// sculpt the terrain immediately once a mode is picked, which is the common case. [Height,
// Color(all), Blue, Green, Red, Alpha].
static bool s_advChan[6]     = { true, false, false, false, false, false };
// Channel-checkbox ids in panel order: Height, Color(all), Blue, Green, Red, Alpha.
static const int s_advChanIds[6] = { 1469, 1470, 1471, 1472, 1473, 1474 };

// sub_401E10 — programmatically select the mode radio whose value == `mode` (Patch_FinishCurve-
// Drag switches "Grab Value" 5 → "Flatten" 2 after an eyedropper drag). Now writes the store.
void AdvPatchEdit_SetMode( int mode )
{
    s_advMode = mode;
}

// Panel accessors (imgui_panel_advpatch.cpp).
int  AdvPatchEdit_GetMode()                 { return s_advMode; }
bool AdvPatchEdit_GetChannel( int idx )     { return ( idx >= 0 && idx < 6 ) ? s_advChan[idx] : false; }
void AdvPatchEdit_SetChannel( int idx, bool on ) { if ( idx >= 0 && idx < 6 ) s_advChan[idx] = on; }

// KIWI-UX: stroke DIRECTION for Raise/Lower (mode 0).  The binary signed the stroke by
// the mouse button (LMB raise, RMB lower), but Alt+RMB is the mouselook in the modern
// shell, so the RMB half is unreachable — this panel toggle is the replacement sign.
static bool s_advLower = false;
bool AdvPatchEdit_GetLower()            { return s_advLower; }
void AdvPatchEdit_SetLower( bool low )  { s_advLower = low; }


// ═══════════════════════════════════════════════════════════════════════════════
//  ShowInfoDialog (0x40BE90) — the modeless "Information" state prompt.
//
//  A caption-only popup holding one disabled multiline edit (IDD_INFORMATION, PE
//  resource 150).  Patch BEND and INSERT/DELETE mode use it to tell the user which
//  key does what for the current sub-state; the message pointer is one of the
//  g_pBendStateMsg / g_pInsDelStateMsg literals below.
//
//  The binary keeps the dialog in a static object (off_25D5BF0) and tests its m_hWnd
//  (dword_25D5C10 == object+0x20) to decide whether to Create it; this port uses a
//  lazily-new'd pointer, the same idiom as g_pAdvPatchDlg above.  SetFocus back to the
//  main frame is faithful (0x40bec7) — the prompt must never steal keyboard focus, or
//  the TAB/ENTER/ESC keys the message describes would go to the prompt instead of the
//  view that implements the mode.
// ═══════════════════════════════════════════════════════════════════════════════

// Bend-state prompts, indexed by g_nPatchBendState + 1 (IDB g_pBendStateMsg 0x73B114).
const char *g_pBendStateMsg[4] =
{
    "Use TAB to cycle through available bend axis. Press ENTER when the desired one is highlighted.",
    "Use TAB to cycle through available rotation axis. This will LOCK around that point. You may also use Shift + Middle Click to select an arbitrary point. Press ENTER when the desired one is highlighted",
    "Use TAB to choose which side to bend. Press ENTER when the desired one is highlighted.",
    "Use the MOUSE to bend the patch. It uses the same ui rules as Free Rotation. Press ENTER to accept the bend, press ESC to abandon it and exit Bend mode",
};

// Insert/delete (redisperse) prompt (IDB off_73B128[0] 0x73B128).
const char *g_pInsDelStateMsg = "Use TAB to cycle through available rows/columns for insertion/deletion. Press INS to insert at the highlight, DEL to remove the pair";


void ShowInfoDialog( const char *msg )
{
    // NO-MFC: no-op — IDD_INFORMATION is an MFC CDialog; the bend / insert-delete state
    // prompt text is unreachable in this shell (the modes themselves still work).  The
    // ImGui shell will show g_pBendStateMsg / g_pInsDelStateMsg itself.
    (void)msg;
}

// The hide half (CWnd::ShowWindow(&off_25D5BF0, SW_HIDE), guarded by dword_25D5C10).
void HideInfoDialog()
{
    // NO-MFC: no-op — ShowInfoDialog never created a prompt to hide.
}

// ═══════════════════════════════════════════════════════════════════════════════
//  sub_401DB0 (mode selector) + sub_43E6F0 (the addpoint PAINT DRAG) — paint epic 7.
//  This is the bridge from a mouse drag (Drag_MouseMoved sel_addpoint+Alt) to the apply
//  chain: pick the terrain cell (sub_43DD50), read the dialog's channel checkboxes + mode,
//  seed the per-channel paint state, and dispatch to sub_43E4F0 -> PMESH_16.
// ═══════════════════════════════════════════════════════════════════════════════
typedef float ( *PaintCallback )( float *cp, int channel, float cur, float strength, float weight );

extern char  sub_43DD50( const float *dir, byte *colorOut, const float *cam_origin, float *origin_out ); // pmesh.cpp
extern void  sub_43E4F0( PaintCallback cb, float *center, char channelMask, float cellInfo ); // pmesh.cpp
extern float sub_43E550( float *, int, float, float, float );   // raise/lower
extern float sub_43E570( float *, int, float, float, float );   // set toward target
extern float sub_43E5D0( float *, int, float, float, float );   // average gather
extern float sub_43E610( float *, int, float, float, float );   // average apply
extern float sub_43E670( float *, int, float, float, float );   // noise
extern float sub_401BB0();                                      // inner radius getter (pmesh.cpp)
extern float sub_401C00();                                      // outer radius getter (pmesh.cpp)
extern float sub_401C50();                                      // strength getter (pmesh.cpp)
extern float g_paintChannelVal[5];                              // flt_231F534[0..4] (pmesh.cpp)
extern float g_paintChannelWt[5];                               // flt_231F520[0..4] (pmesh.cpp)
extern float grid_sizes[];                                      // engine_stubs

// The paint TARGET colour (the dialog's colour control / the case-5 eyedropper); packed
// {B, G, R} like the IDB dword_25D65A4, with a separate alpha byte_25D65A8.  Default white.
unsigned int  g_paintColorBGR = 0x00FFFFFFu;   // 0x25D65A4
byte g_paintColorA   = 0xFFu;         // 0x25D65A8

// Is an AdvPatchEditDlg control checked? The binary did a live BM_GETCHECK on the dialog's
// HWND; here it reads the UI-independent store the ImGui panel drives (see above). Mode radios
// resolve against s_advMode; channel checkboxes against s_advChan[]. Controls with no store
// yet (soft-select 1431/1432) stay false, exactly as the never-created MFC dialog reported.
static bool AdvDlg_IsChecked( int id )
{
    static const struct { int id, mode; } s_modes[7] =
        { { 1435, 0 }, { 1439, 1 }, { 1437, 2 }, { 1436, 3 }, { 1438, 4 }, { 1468, 5 }, { 1440, 6 } };
    for ( int i = 0; i < 7; ++i )
        if ( s_modes[i].id == id )
            return s_advMode == s_modes[i].mode;
    for ( int i = 0; i < 6; ++i )
        if ( s_advChanIds[i] == id )
            return s_advChan[i];
    return false;
}

// sub_401DB0 (0x401DB0) — return the checked mode radio's index (6 = none/off).
int sub_401DB0()
{
    static const struct { int id, mode; } s_modes[7] =
        { { 1435, 0 }, { 1439, 1 }, { 1437, 2 }, { 1436, 3 }, { 1438, 4 }, { 1468, 5 }, { 1440, 6 } };
    for ( int i = 0; i < 7; ++i )
        if ( AdvDlg_IsChecked( s_modes[i].id ) )
            return s_modes[i].mode;
    return 6;
}

// sub_401D50 (0x401D50) - "is terrain-paint mode active?" gate (Drag_Begin).
// IDA gates on the live AdvPatchEditDlg controls: mode != Off and outer radius > inner
// radius.  It does not require the modeless dialog object's top window to report visible.
int sub_401D50()
{
    if ( sub_401DB0() == 6 )
        return 0;                                   // mode "Off"
    return ( sub_401C00() > sub_401BB0() ) ? 1 : 0; // outer radius > inner radius
}

// sub_43E6F0 (0x43E6F0) — the addpoint paint drag.  buttons 1=raise/LMB, 2=lower/RMB;
// origin/dir are float* ray endpoints (int-cast by Drag_MouseMoved).
void sub_43E6F0( int buttons, float *origin, float *dir )
{
    extern void Radiant_FL_Log( const char *fmt, ... );   // mainfrm.cpp — DIAG (terrain paint)
    if ( buttons != 1 && buttons != 2 )
        return;
    float *org = (float *)(intptr_t)origin;
    float *dr  = (float *)(intptr_t)dir;

    float center[3];
    if ( !sub_43DD50( dr, nullptr, org, center ) )      // pick the terrain cell under the cursor
    {
        Radiant_FL_Log( "PAINT sub_43E6F0 btn=%d: cell pick MISSED (no terrain under cursor)", buttons );
        return;
    }
    float sign = ( buttons == 1 ) ? 1.0f : -1.0f;
    // KIWI-UX: the panel's Lower toggle inverts the (LMB-only) stroke — see s_advLower.
    if ( AdvPatchEdit_GetLower() )
        sign = -sign;

    // Channel mask from the dialog checkboxes (1469 Height, 1470 Color-all, 1471/72/73 B/G/R, 1474 A).
    int mask = AdvDlg_IsChecked( 1469 ) ? 1 : 0;
    if ( AdvDlg_IsChecked( 1470 ) )
        mask |= 0xE;
    else
    {
        if ( AdvDlg_IsChecked( 1471 ) ) mask |= 2;
        if ( AdvDlg_IsChecked( 1472 ) ) mask |= 4;
        if ( AdvDlg_IsChecked( 1473 ) ) mask |= 8;
    }
    if ( AdvDlg_IsChecked( 1474 ) )
        mask |= 0x10;

    // DIAG: the pick hit — report the mode + channel mask actually applied. mask==0 means NO
    // channel is ticked, so the stroke touches nothing (a common "nothing happens" cause).
    Radiant_FL_Log( "PAINT sub_43E6F0 btn=%d: HIT mode=%d mask=0x%X", buttons, sub_401DB0(), mask );

    switch ( sub_401DB0() )
    {
    case 0:   // raise / lower height
    {
        float amount = grid_sizes[g_qeglobals.d_gridsize] * sign * 0.5f;
        sub_43E4F0( sub_43E550, center, (char)mask, amount );
        break;
    }
    case 2:   // set colour toward the paint target
        // [DEFERRED] CurvEditDlg::OnHeightText2() — dialog text refresh (cosmetic).
        g_paintChannelVal[0] = sign;
        if ( sign >= 0.0f )
        {
            g_paintChannelVal[1] = (float)( byte )( g_paintColorBGR );         // B
            g_paintChannelVal[2] = (float)( byte )( g_paintColorBGR >> 8 );    // G
            g_paintChannelVal[3] = (float)( byte )( g_paintColorBGR >> 16 );   // R
            g_paintChannelVal[4] = (float)g_paintColorA;                                // A
        }
        else
            g_paintChannelVal[1] = g_paintChannelVal[2] = g_paintChannelVal[3] = g_paintChannelVal[4] = 255.0f;
        sub_43E4F0( sub_43E570, center, (char)mask, 1.0f );
        break;

    case 3:   // average (gather pass -> normalise -> apply pass)
        for ( int k = 0; k < 5; ++k ) { g_paintChannelVal[k] = 0.0f; g_paintChannelWt[k] = 0.0f; }
        sub_43E4F0( sub_43E5D0, center, (char)mask, 0.0f );
        if ( g_paintChannelWt[0] == 0.0f ) mask &= ~1;    else g_paintChannelVal[0] /= g_paintChannelWt[0]; // height
        if ( g_paintChannelWt[1] == 0.0f ) mask &= ~2;    else g_paintChannelVal[1] /= g_paintChannelWt[1]; // B
        if ( g_paintChannelWt[2] == 0.0f ) mask &= ~4;    else g_paintChannelVal[2] /= g_paintChannelWt[2]; // G
        if ( g_paintChannelWt[3] == 0.0f ) mask &= ~8;    else g_paintChannelVal[3] /= g_paintChannelWt[3]; // R
        if ( g_paintChannelWt[4] == 0.0f ) mask &= ~0x10; else g_paintChannelVal[4] /= g_paintChannelWt[4]; // A
        if ( mask )
        {
            float amount = sub_401C50() * 0.009999999776482582f * grid_sizes[g_qeglobals.d_gridsize];
            sub_43E4F0( sub_43E610, center, (char)mask, amount );
        }
        break;

    case 4:   // noise: seed every channel's w-offset to the per-stroke counter, then apply
    {
        // flt_2665724 — a global float counter, +1 per stroke, so each stroke samples a
        // different 4D-noise slice (g_paintChannelWt[c] is the noise w-coord in sub_43E670).
        static float g_noiseStrokeW = 0.0f;                 // 0x2665724 (init 0.0)
        for ( int k = 0; k < 5; ++k ) g_paintChannelWt[k] = g_noiseStrokeW;
        g_noiseStrokeW += 1.0f;
        float amount = sub_401C50();                        // strength (raw, unscaled)
        sub_43E4F0( sub_43E670, center, (char)mask, amount );
        break;
    }

    case 5:   // eyedropper: pick the cell's height + colour into the paint target
    {
        byte picked[4] = { 0, 0, 0, 0 };
        if ( sub_43DD50( dr, picked, org, center ) )
        {
            g_paintChannelVal[0] = center[2];          // picked height
            g_paintChannelVal[1] = (float)picked[2];   // B
            g_paintChannelVal[2] = (float)picked[1];   // G
            g_paintChannelVal[3] = (float)picked[0];   // R
            g_paintChannelVal[4] = (float)picked[3];   // A
            AdvPatchEdit_ShowHeight( center[2] );      // CurvEditDlg::OnHeightText(picked height)
            g_paintColorBGR = picked[2] | ( picked[1] << 8 ) | ( picked[0] << 16 );   // {B,G,R}
            g_paintColorA   = picked[3];
        }
        break;
    }

    default:
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CPatchDialog — the Patch Inspector "Patch Properties" dialog (binary CWnd_PatchDialog
//  0x25D7278, PE resource 0xAC = IDD_PATCH_PROPERTIES).  A MODELESS CDialog.  This piece
//  ports the DETAILS half READ-ONLY: pick a control point with the Row/Column combos and
//  show its X/Y/Z + current-layer S/T.  The Type combo, the Apply write-back, and the whole
//  Texturing half (texdef edits/spins + CAP/Set/Natural/Fit) are later pieces.
//    DoPatchInspector (0x436d30) / g_PatchDialog.GetPatchInfo (0x436ba0) / sub_436E10
//    (0x436e10) / UpdatePatchInspector (0x436db0).
// ═══════════════════════════════════════════════════════════════════════════════
extern selbrush_t selected_brushes;     // qe3.h
extern int        QE_SingleBrush();      // select.cpp
extern int        g_nUpdateBits;         // pmesh.cpp (0x25D5A74) pending window-update mask
extern void       Patch_NaturalizeSelected( bool unk, bool cap, float x, float y );  // pmesh.cpp 0x447fd0
extern void       Brush_FitTexture( float x, float y, int a4 );                       // select.cpp 0x4939e0
extern void       Patch_SetTextureInfo( texdef_sub_t *texDef );                       // pmesh.cpp 0x447760
extern void       Patch_SetTexturing( float sx, float sy, int mode );                 // pmesh.cpp 0x446b60

// ── CTextureLayout (IDD_PATCH_TEXTURE_LAYOUT / resource 0x9F) ──────────────────────
//  The modal "Patch texture layout" dialog opened by the Patch Inspector's "Set..." button
//  (sub_459860 ctor / sub_436980).  Enter a texture x/y scale (default 4) + pick a layout mode,
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

// UI-independent core read behind the inspector's control-point display (sub_436E10) — fill
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
    ++def->version;             // +0x5040
    g_nUpdateBits = -1;         // force a full redraw (re-tessellates the dirty patch)
}

// UI-independent actions behind the inspector's texturing buttons — thin wrappers over the
// ported patch-texturing ops, scaled by the current edit layer's sample size
// (random_texture_stuff[layer].sampleSize, IDB +36).
void PatchInspector_Cap()                          // CAP (1282) -> sub_4368D0
{
    float s = g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].sampleSize;
    Patch_NaturalizeSelected( true, false, s, s );
    g_nUpdateBits = -1;
}

void PatchInspector_Natural()                       // Natural (1284) -> sub_436940
{
    float s = g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].sampleSize;
    Patch_NaturalizeSelected( false, false, s, s );
    g_nUpdateBits = -1;
}

void PatchInspector_Fit()                           // Fit (1286) -> sub_436910
{
    Brush_FitTexture( 1.0f, 1.0f, 0 );
    g_nUpdateBits = -1;
}

// "Set..." (1283) -> sub_436980.  `bAccepted` is the CTextureLayout modal result: the port
// bumps g_nUpdateBits either way (the repaint brackets the DIALOG, not the OK branch — the
// CurveThicken_Apply situation), so the flag rides along instead of splitting the update out.
void PatchInspector_SetTexturing( bool bAccepted, float sx, float sy, int mode )
{
    if ( bAccepted )
        Patch_SetTexturing( sx, sy, mode );
    g_nUpdateBits = -1;
}

// The five texdef spin STEPs as the inspector's step edits hold them (binary ctor
// flt_25D75A8..75B8: stretch/shift 0.05, rotate 45).
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
// by the step typed in the matching edit (sub_4370E0 -> Patch_SetTextureInfo): shift =
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
// `if (CWnd_PatchDialog.m_hWnd)` gate, in the AdvDlg_IsChecked style: constant false under
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
//  IDB cluster (all renamed in the IDB this session):
//    CPatchDensityDlg_Init          0x436280 — ctor (IDD 0x9D, 2 CComboBox @ +0x74/+0xC8)
//    CPatchDensityDlg_DoDataExchange 0x436330 — DDX_Control(1280→combo0, 1281→combo1)
//    CPatchDensityDlg_OnInitDialog  0x436440 — base OnInitDialog + CB_SETCURSEL(last sel)
//    CPatchDensityDlg_OnOK          0x436390 — read CB_GETCURSEL → dims[] → Patch_GenericMesh
//    CMainFrame::OnCurveSimplepatchmesh 0x429a20 — Undo bracket + DoModal + destruct
//
//  FIDELITY NOTE: the binary's OnInitDialog only CB_SETCURSELs the last selection —
//  it never adds the combo items, and the dialog template (PE rsrc 157, ground-truthed)
//  carries empty combos.  i.e. the stock dialog shows EMPTY dropdowns (the menu command
//  is also un-wired/auto-greyed in stock, per PROGRESS.md).  Since the combos must hold
//  the dims for CB_GETCURSEL→dims[sel] to mean anything, we POPULATE them from the
//  binary's own g_patchDensityDims table {3,5,7,9,11,13,15} in OnInitDialog (the
//  GtkRadiant DoNewPatchDlg confirms these exact 7 items + active index 0) — the only
//  added line vs the binary, so the feature actually works.
// ═══════════════════════════════════════════════════════════════════════════════
#include "mainfrm.h"                  // CMainFrame / CXYWnd (m_pActiveXY->m_nViewType)
#include "xywnd.h"                    // xywndState_t / Ed_ActiveXY (U-GLOBALS)
extern int         Sys_Printf( const char *fmt, ... );          // win_qe3.cpp (0x499E90)
extern int         QE_SingleBrush();                            // qe3.cpp (0x48C8B0)
extern selbrush_t  selected_brushes;                            // engine_stubs (0x23F1864)
extern void        Undo_ClearRedo();                            // undo.cpp
extern void        Undo_GeneralStart( const char *operation );  // undo.cpp
extern void        Undo_AddBrushList( selbrush_t *sb );         // undo.cpp
extern void        Undo_EndBrushList( selbrush_t *sb );         // undo.cpp
extern void        Undo_End();                                  // undo.cpp
extern selbrush_t *Patch_GenericMesh( int nWidth, int nHeight, int nOrientation,
                                      char bDeleteSource, char bOverwrite );   // pmesh.cpp (0x43b310)
// The TERRAIN arm is the binary's own Create_Terrain (0x43B660), reached in stock from
// TerrainDlg_NewPatch (0x458FB0) — NOT a PATCH_TERRAIN variant of Patch_GenericMesh (no
// such function exists).  It takes (width, height, viewType) and always replaces the
// selected brush, so it needs no bDeleteSource/bOverwrite.
extern selbrush_t *Create_Terrain( int width, int height, int orientation );   // pmesh.cpp (0x43B660)

// The binary's dimension table (g_patchDensityDims @0x73B170): combo index → patch size.
static const int s_patchDensityDims[7] = { 3, 5, 7, 9, 11, 13, 15 };

// Last combo selections, remembered across opens (IDB g_patchDensityLastWidthSel 0x25D5AFC /
// g_patchDensityLastHeightSel 0x25D5B00; both 0 = "3" on first open).
static int g_patchDensityLastWidthSel  = 0;
static int g_patchDensityLastHeightSel = 0;
static int g_terrainLastWidthSel       = 0;
static int g_terrainLastHeightSel      = 0;

// UI-independent action behind CPatchDensityDlg's OK (0x436390) — validate the two combo
// selections, build the patch/terrain mesh, then remember the selections for the next open.
// (The undo bracket stays with the caller: DoSimpleTerrainPatchMesh / IDB 0x429a20 opens it
// around the DIALOG, not the create — even a cancel opens/closes the bracket.)
void PatchDensity_Apply( bool terrain, int wSel, int hSel )
{
    const unsigned int maxSel = terrain ? 14u : 6u;
    if ( (unsigned int)wSel <= maxSel && (unsigned int)hSel <= maxSel )
    {
        if ( terrain )
            Create_Terrain( wSel + 2, hSel + 2,                         // 0x458FB0
                            Ed_ActiveXY()->m_nViewType );
        else
            Patch_GenericMesh( s_patchDensityDims[wSel], s_patchDensityDims[hSel],
                               Ed_ActiveXY()->m_nViewType, 1, 0 ); // 0x436400
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
