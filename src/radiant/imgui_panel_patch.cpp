// imgui_panel_patch.cpp — UI-rework Phase 3 unit P-7: ImGui "Patch inspector" over the
// patchdialog.cpp CPatchInspectorDlg core actions. New KISAK code; visible only under -imgui.
//
// Panel semantics vs CPatchInspectorDlg (patchdialog.cpp:657-774): the SAME core calls its
// handlers make, in the same order —
//   * the DETAILS half : GetPatchInfo (patchdialog.cpp:692-698) binds the selected single
//     patch via PatchInspector_GatherSelectedPatch and sizes the Row/Column combos from
//     def->height / def->width; ShowSelectedPoint (patchdialog.cpp:676-688) blanks the five
//     X/Y/Z/S/T fields, then fills them from PatchInspector_GatherPoint for the (row, col)
//     cell.  Row/Column change → ShowSelectedPoint, exactly ON_CBN_SELCHANGE 1301/1302
//     (patchdialog.cpp:712/766-767).
//   * [Apply]          : OnApply (patchdialog.cpp:713-721) — read the two combo selections
//     and the five edits into a patchPointState_t, then PatchInspector_ApplyPoint.  Nothing
//     dialog-side follows it: the version bump and `g_nUpdateBits = -1` live INSIDE the
//     action (patchdialog.cpp:588-589), so this file stamps no update bits of its own.
//   * CAP / Natural / Fit : the three no-arg actions, verbatim from OnCap / OnNatural / OnFit
//     (patchdialog.cpp:722-724).  Each bumps g_nUpdateBits itself.
//   * [Set...]         : OnSetTexturing (patchdialog.cpp:725-730) runs the modal
//     CTextureLayout (patchdialog.cpp:513-538) and hands its result to
//     PatchInspector_SetTexturing( accepted, x, y, mode ).  The panel IS that dialog — see
//     the Set block below.
//   * texdef spins     : OnSpin (patchdialog.cpp:731-742) reads the five STEP edits into a
//     texdefSpinState_t and calls PatchInspector_SpinTexdef with the spin's control id;
//     up = iDelta > 0.  The `g_nUpdateBits |= 1` is inside the action (patchdialog.cpp:654).
//
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md, unit P-7):
//   * the panel stays open; hiding it is OnOK/OnCancel's ShowWindow( SW_HIDE ) equivalent
//     (patchdialog.cpp:743-744), so [Done] has no analogue.  Re-opening re-runs the
//     OnInitDialog sequence (step defaults, then a GetPatchInfo-equivalent) via the
//     first-open latch, which the modeless MFC popup would NOT do — its step edits keep
//     whatever the user typed.  The latch is the house pattern (imgui_panel_surface.cpp:109).
//   * [Set...] does not open a second window: the CTextureLayout scale edits + mode radios
//     live inline and the [Set] click IS the acceptance, so PatchInspector_SetTexturing is
//     always called with bAccepted = true.  There is therefore no cancel path here — and
//     with it dies the MFC quirk that a CANCELLED CTextureLayout still stamps
//     g_nUpdateBits = -1 (patchdialog.cpp:618-623: the action bumps the bits either way,
//     because the repaint brackets the DIALOG, not the OK branch).  A panel that never
//     cancels can never hit that arm, so the observable behaviour is unchanged.
//   * Row/Column are INT INPUTS, not combos, and they stay USER-OWNED: GetPatchInfo
//     re-fills both combos on every refresh and FillCombo ends in CB_SETCURSEL( 0 )
//     (patchdialog.cpp:673), i.e. every MFC refresh silently snaps the inspected cell back
//     to (0,0).  The panel instead CLAMPS the two fields into the live patch's
//     0..height-1 / 0..width-1 range and re-shows the point when the clamp moved them, so an
//     unrelated selection refresh does not lose the user's cell.
//   * the "Type:" combo (1303) and the "Name:" edit (1033) of the real template
//     (res/radiant.rc:785 / :789) are absent here because they are absent from the MFC port
//     too — CPatchInspectorDlg never wires either one (no DDX, no message-map entry), and
//     there is no core action to bind.  Inventing one would be new behaviour.
//
// PER-FRAME RE-GATHER: the cached patchMesh_t* would dangle the moment the selection changes,
// so the patch pointer is re-Gathered at the top of every Draw.  That is a field read, not a
// map walk — PatchInspector_GatherSelectedPatch is QE_SingleBrush + `selected_brushes.next->
// patch->def` (patchdialog.cpp:566-572) — unlike the mapinfo panel's O(map) snapshot.  It is
// gated by PI_GatherPatch below purely to keep the console quiet; see there.
//
// NO HWND/MFC anywhere in this file: every bind is a UI-independent action, and the panel
// re-Gathers its own fields instead of going through the dialog's SetDlgItemText path.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>

// ── patchdialog.cpp bindings (the struct-coupled inspector core; not in
//    radiant_ui_actions.h — this panel is its first consumer) ────────────────────
// patchMesh_t / drawVert_t / selbrush_t / entity_s / eclass_t all come from qe3.h itself
// (qe3.h:736 patchMesh_t, :72 drawVert_t, :429 selbrush_t, :515 entity_s, :579 eclass_t) —
// not from a header it pulls in — so "qe3.h" above is the whole type dependency.
// `selected_brushes` is likewise already declared by qe3.h:1054, so it is not re-externed.

// MUST MATCH patchdialog.cpp:542 verbatim (shared-header consolidation pending)
// One patch control point as the inspector shows and edits it: which cell (the Row/Column
// combo selections), its position, and its current-layer texture coords.
struct patchPointState_t
{
    int   row, col;   // combo 1301 / 1302 selections (idx row+16*col into patchMesh_t.ctrl)
    float xyz[3];     // edits 1089 / 1142 / 1147
    float st[2];      // edits 1102 / 1106
};

// MUST MATCH patchdialog.cpp:627 verbatim (shared-header consolidation pending)
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

extern bool         PatchInspector_GatherPoint( patchMesh_t *def, patchPointState_t &pt );        // patchdialog.cpp:551 (sub_436E10 read)
extern patchMesh_t *PatchInspector_GatherSelectedPatch();                                         // patchdialog.cpp:566 (GetPatchInfo read)
extern void         PatchInspector_ApplyPoint( patchMesh_t *def, const patchPointState_t &pt );   // patchdialog.cpp:578 (0x436ef0)
extern void         PatchInspector_Cap();                                                         // patchdialog.cpp:595 (0x4368d0)
extern void         PatchInspector_Natural();                                                     // patchdialog.cpp:602 (0x436940)
extern void         PatchInspector_Fit();                                                         // patchdialog.cpp:609 (0x436910)
extern void         PatchInspector_SetTexturing( bool bAccepted, float sx, float sy, int mode );  // patchdialog.cpp:618 (0x436980)
extern void         PatchInspector_SpinTexdef( UINT id, bool up, const texdefSpinState_t &step ); // patchdialog.cpp:640 (0x436ae0 -> sub_4370E0)

// The texdef spin control ids PatchInspector_SpinTexdef dispatches on (patchdialog.cpp:646-650),
// copied so this panel needs no MFC resource header.  res/resource.h only carries these five
// numbers under the colliding IDC_SPIN2 alias (resource.h:254-261, "dlg172" = this dialog), so
// the IDC names quoted below are the surface inspector's aliases for the SAME numeric ids
// (resource.h:58-62) — the two dialogs share the resource ids and the roles match row for row.
static const int PI_ID_HORZ_SHIFT_SPIN   = 1248;   // IDC_SURFACE_INSP_HORZ_SHIFT_SPIN   (edit 1195)
static const int PI_ID_VERT_SHIFT_SPIN   = 1251;   // IDC_SURFACE_INSP_VERT_SHIFT_SPIN   (edit 1196)
static const int PI_ID_VERT_STRETCH_SPIN = 1254;   // IDC_SURFACE_INSP_VERT_STRETCH_SPIN (edit 1213)
static const int PI_ID_HORZ_STRETCH_SPIN = 1257;   // IDC_SURFACE_INSP_HORZ_STRETCH_SPIN (edit 1211)
static const int PI_ID_ROTATE_SPIN       = 1259;   // IDC_SURFACE_INSP_ROTATE_SPIN       (edit 1217)

// CTextureLayout's layout modes — the values its OnOK folds the three radios into
// (patchdialog.cpp:535) and hands to Patch_SetTexturing.  Radio ids/captions from
// res/radiant.rc:833-835.
static const int PI_MODE_2D     = 0;   // 1449  IDC_TERRAIN_USES_2D_DISTANCE__GOOD_FOR_GROUN (default)
static const int PI_MODE_3D     = 1;   // 1475  IDC_TERRAIN_USES_3D_DISTANCE__GOOD_FOR_CURVY
static const int PI_MODE_CURVES = 2;   // 1476  IDC_TERRAIN_EMULATES_CURVES__BACKWARDS_COMPA

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showPatch = false;
static bool s_opened    = false;                 // first-open latch (OnInitDialog equivalent)

// The inspected patch, re-Gathered every frame (see PER-FRAME RE-GATHER above).  Held in a
// static only so the row/col change and [Apply] paths inside the same frame can see it; it is
// never trusted across frames.
static patchMesh_t *s_def = nullptr;

// The DETAILS half's control values: the two combo selections plus the five edits.  User-owned
// between refreshes, exactly like the dialog's own controls.
static patchPointState_t s_pt = { 0, 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } };

// The five texdef STEP edits, seeded with the MFC ctor defaults (patchdialog.cpp:706-708:
// stretch/shift 0.05, rotate 45 — binary flt_25D75A8..75B8).
static texdefSpinState_t s_step = { 45.0f, 0.05f, 0.05f, 0.05f, 0.05f };

// The inlined CTextureLayout controls: scale edits 1089/1142 (ctor default 4.0,
// patchdialog.cpp:517/525) and the mode radio (default 2D distance, patchdialog.cpp:526).
static float s_setScale[2] = { 4.0f, 4.0f };
static int   s_setMode     = PI_MODE_2D;

// Bind the selected single patch, WITHOUT the console noise.
// PatchInspector_GatherSelectedPatch goes through QE_SingleBrush, which Sys_Printf's on
// every rejection ("you must have a single brush selected" / "you cannot manipulate fixed
// size entities", qe3.cpp:342/352) — and Sys_Printf both writes stdout and EM_REPLACESELs
// the console edit (win_qe3.cpp:68-100).  Called once per frame with nothing (or a light)
// selected that is a per-frame console flood, so the two conditions QE_SingleBrush would
// print about are pre-tested here silently.  This gate is a strict SUBSET: whenever it
// returns null, QE_SingleBrush would have returned 0 as well, so no patch the MFC dialog
// would accept is rejected here.  The null checks are the one addition (qe3.cpp:349 derefs
// blind); they only make the gate stricter, and stricter beats a per-frame fault.
static patchMesh_t *PI_GatherPatch()
{
    // Consolidated: the silent predicate half of QE_SingleBrush (qe3.cpp) — the same
    // two tests the printing gate runs, no console traffic. 2 == single non-fixed brush.
    extern int QE_SingleBrush_Check();
    if ( QE_SingleBrush_Check() != 2 )
        return nullptr;
    return PatchInspector_GatherSelectedPatch();
}

// sub_436E10 / ShowSelectedPoint (patchdialog.cpp:676-688) — blank the five X/Y/Z/S/T fields,
// then fill them from the (row, col) cell; a failed gather leaves them blank.
static void PI_ShowSelectedPoint()
{
    s_pt.xyz[0] = s_pt.xyz[1] = s_pt.xyz[2] = 0.0f;
    s_pt.st[0]  = s_pt.st[1]  = 0.0f;
    PatchInspector_GatherPoint( s_def, s_pt );
}

// One texdef spin row: the STEP edit plus the up/down arrows (UDN_DELTAPOS →
// PatchInspector_SpinTexdef with this row's control id and iDelta's sign; "+" = up).
// The whole step snapshot is passed, as OnSpin does — the action picks the field the id maps to.
static void PI_SpinRow( const char *label, float *step, int spinId )
{
    ImGui::PushID( spinId );
    ImGui::SetNextItemWidth( 90.0f );
    ImGui::InputFloat( "##step", step );
    ImGui::SameLine();
    if ( ImGui::SmallButton( "-" ) )
        PatchInspector_SpinTexdef( (UINT)spinId, false, s_step );
    ImGui::SameLine();
    if ( ImGui::SmallButton( "+" ) )
        PatchInspector_SpinTexdef( (UINT)spinId, true, s_step );
    ImGui::SameLine();
    ImGui::TextUnformatted( label );
    ImGui::PopID();
}

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_Patch_MenuItem()
{
    ImGui::Checkbox( "Patch inspector", &s_showPatch );
}

void ImGuiPanel_Patch_Draw()
{
    if ( !s_showPatch )
    {
        s_opened = false;      // a re-open re-runs the OnInitDialog sequence
        return;
    }

    if ( !s_opened )
    {
        // OnInitDialog (patchdialog.cpp:703-711): seed the five STEP edits, then GetPatchInfo.
        s_opened = true;
        s_step.shiftHoriz   = 0.05f;   // edit 1195
        s_step.shiftVert    = 0.05f;   // edit 1196
        s_step.stretchHoriz = 0.05f;   // edit 1211
        s_step.stretchVert  = 0.05f;   // edit 1213
        s_step.rotate       = 45.0f;   // edit 1217
        s_pt.row = 0;                  // FillCombo's CB_SETCURSEL( 0 ) on a fresh bind
        s_pt.col = 0;
        s_def = PI_GatherPatch();
        PI_ShowSelectedPoint();
    }

    // "Patch Properties" = the dialog's CAPTION (res/radiant.rc:766).
    if ( ImGui::Begin( "Patch Properties", &s_showPatch, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // ── the selection bind (GetPatchInfo's first half) ─────────────────────
        patchMesh_t *prevDef = s_def;
        s_def = PI_GatherPatch();

        if ( s_def )
        {
            // The combo RANGES: Row 0..height-1, Column 0..width-1 — the two FillCombo
            // counts (patchdialog.cpp:695-696).  Clamping instead of resetting to 0 is the
            // user-owned-fields divergence noted in the header.
            int row = s_pt.row, col = s_pt.col;
            if ( s_pt.row > s_def->height - 1 ) s_pt.row = s_def->height - 1;
            if ( s_pt.col > s_def->width  - 1 ) s_pt.col = s_def->width  - 1;
            if ( s_pt.row < 0 ) s_pt.row = 0;
            if ( s_pt.col < 0 ) s_pt.col = 0;
            if ( s_def != prevDef || s_pt.row != row || s_pt.col != col )
                PI_ShowSelectedPoint();     // a new patch (or a moved cell) → re-show it

            ImGui::Text( "patch %d x %d  (width x height)", s_def->width, s_def->height );
        }
        else
        {
            if ( prevDef )
                PI_ShowSelectedPoint();     // selection lost → blank the five fields
            ImGui::TextUnformatted( "(select a single patch)" );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Refresh" ) )   // the UpdatePatchInspector → GetPatchInfo path
        {
            s_def = PI_GatherPatch();
            PI_ShowSelectedPoint();
        }

        // ── Details ("Details" group box, res/radiant.rc:769) ─────────────────
        ImGui::SeparatorText( "Details" );

        // Row: / Column: — combos 1301 / 1302.  A change re-shows the point, exactly what
        // ON_CBN_SELCHANGE → ShowSelectedPoint does.
        bool cellChanged = false;
        ImGui::SetNextItemWidth( 90.0f );
        cellChanged |= ImGui::InputInt( "Row", &s_pt.row );
        ImGui::SetNextItemWidth( 90.0f );
        cellChanged |= ImGui::InputInt( "Column", &s_pt.col );
        if ( cellChanged )
        {
            // Clamp here too so a typed-out-of-range cell never reaches the gather (which
            // would blank the fields) when no patch is bound to clamp against above.
            if ( s_pt.row < 0 ) s_pt.row = 0;
            if ( s_pt.col < 0 ) s_pt.col = 0;
            if ( s_def )
            {
                if ( s_pt.row > s_def->height - 1 ) s_pt.row = s_def->height - 1;
                if ( s_pt.col > s_def->width  - 1 ) s_pt.col = s_def->width  - 1;
            }
            PI_ShowSelectedPoint();
        }

        // X / Y / Z / S / T — edits 1089 / 1142 / 1147 / 1102 / 1106.  S/T are the CURRENT
        // EDIT LAYER's texture coords: both the gather and the apply index
        // &cp->texCoord + 8 * g_qeglobals.current_edit_layer (patchdialog.cpp:559 / :586).
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::InputFloat( "X", &s_pt.xyz[0] );
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::InputFloat( "Y", &s_pt.xyz[1] );
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::InputFloat( "Z", &s_pt.xyz[2] );
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::InputFloat( "S", &s_pt.st[0] );
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::InputFloat( "T", &s_pt.st[1] );
        ImGui::TextDisabled( "S/T are the current edit layer's coords" );

        // [Apply] (button id 3) — OnApply verbatim: the state struct IS the five edits plus
        // the two combo selections, so it goes straight into the action.  The action itself
        // range-checks the cell, bumps def->version and forces the redraw.
        if ( ImGui::Button( "Apply" ) )
            PatchInspector_ApplyPoint( s_def, s_pt );

        // ── Texturing ("Texturing" group box, res/radiant.rc:787) ─────────────
        ImGui::SeparatorText( "Texturing" );

        // The four buttons, in template order (res/radiant.rc:805-808).  CAP and Natural
        // scale by the current layer's sampleSize inside the action; Fit is a fixed 1x1.
        if ( ImGui::Button( "CAP" ) )
            PatchInspector_Cap();
        ImGui::SameLine();
        if ( ImGui::Button( "Natural" ) )
            PatchInspector_Natural();
        ImGui::SameLine();
        if ( ImGui::Button( "Fit" ) )
            PatchInspector_Fit();

        // "Set..." — the inlined CTextureLayout.  Help text and radio captions verbatim from
        // the template (res/radiant.rc:826 / :833-835).  [Set] IS the OK, hence bAccepted =
        // true; there is no cancel arm here (see the header note).
        ImGui::SeparatorText( "Set... (patch texture layout)" );
        ImGui::TextDisabled( "Texture will be fit across the patch based on the x and y\n"
                             "values given. Values of 1x1 will \"fit\" the texture. 2x2\n"
                             "will repeat it twice, etc." );
        ImGui::SetNextItemWidth( 90.0f );
        ImGui::InputFloat( "Texture x", &s_setScale[0] );
        ImGui::SetNextItemWidth( 90.0f );
        ImGui::InputFloat( "Texture y", &s_setScale[1] );
        ImGui::RadioButton( "Terrain uses 2D distance (good for ground)",         &s_setMode, PI_MODE_2D );
        ImGui::RadioButton( "Terrain uses 3d distance (good for curvy walls)",    &s_setMode, PI_MODE_3D );
        ImGui::RadioButton( "Terrain emulates curves (backwards compatibility)",  &s_setMode, PI_MODE_CURVES );
        if ( ImGui::Button( "Set" ) )
            PatchInspector_SetTexturing( true, s_setScale[0], s_setScale[1], s_setMode );

        // ── the texdef spin rows ──────────────────────────────────────────────
        // Row order and labels from the template (res/radiant.rc:790-804): each edit holds
        // the STEP the matching spin applies as a RELATIVE transform (shift +/-step, stretch
        // *(1 -/+ step), rotate +/-step — patchdialog.cpp:646-650).
        ImGui::SeparatorText( "Texdef spin steps" );
        PI_SpinRow( "Horizontal shift",   &s_step.shiftHoriz,   PI_ID_HORZ_SHIFT_SPIN );
        PI_SpinRow( "Vertical shift",     &s_step.shiftVert,    PI_ID_VERT_SHIFT_SPIN );
        PI_SpinRow( "Horizontal stretch", &s_step.stretchHoriz, PI_ID_HORZ_STRETCH_SPIN );
        PI_SpinRow( "Vertical stretch",   &s_step.stretchVert,  PI_ID_VERT_STRETCH_SPIN );
        PI_SpinRow( "Rotate",             &s_step.rotate,       PI_ID_ROTATE_SPIN );
    }
    ImGuiShell_CloseOnFocusLoss( &s_showPatch );
    ImGui::End();
}
