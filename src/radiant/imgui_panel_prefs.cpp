// imgui_panel_prefs.cpp — UI-rework Phase 3 unit P-7: ImGui "Preferences" over the
// prefs.cpp core pair (PrefsDlg_Gather / Prefs_ApplyFromDialogState). New KISAK code;
// visible only under -imgui.
//
// Panel semantics vs CPrefsDlg: the SAME two core calls in the SAME order the dialog uses —
//   * open / "Refresh"  → PrefsDlg_Gather( g_PrefsDlg, state )   (prefs.cpp:559-560, OnInitDialog)
//   * "Apply"           → Prefs_ApplyFromDialogState( g_PrefsDlg, state )
//                                                                (prefs.cpp:659, OnOK)
// Both take the SAME prefData_t* the dialog uses: the editor-wide singleton g_PrefsDlg
// (prefs.h:117 / prefs.cpp:11 — a prefData_t* aimed at a file-static instance, non-NULL from
// static init on). There is no per-dialog prefData_t: CPrefsDlg's OnInitDialog/OnOK both
// name g_PrefsDlg directly, so the panel does too.
//
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md):
//   * the panel stays open; Apply commits (and persists — Prefs_ApplyFromDialogState ends in
//     Prefs_SavePrefs, prefs.cpp:467) without closing anything;
//   * "Refresh" is the Cancel equivalent: it re-Gathers from g_PrefsDlg, discarding staged
//     edits. Nothing is written until Apply, so there is no undo bracket to manage;
//   * the panel does NOT call Prefs_LoadPrefs first. Prefs_ShowDialog does that (prefs.cpp:727,
//     mirroring OnPrefs 0x426950) because a MODAL open is a natural re-sync point. A panel that
//     can be up all session must not: many prefs are also written straight into g_PrefsDlg by
//     toolbar toggles without a save, and a registry re-load would silently revert them;
//   * Apply re-Gathers afterwards, so the CLAMPS that Prefs_SavePrefs applies in place
//     (ScaleBase/ScaleRange prefs.cpp:323-328, Fov 2..160 prefs.cpp:329-330) become visible.
//     The modal never showed them — it was closing.
//
// NO HWND/MFC-dialog path here: nothing in this file touches a control, so the dialog's
// OnSetGamePrefs EnableWindow pair (prefs.cpp:699-705) is reproduced with BeginDisabled, and
// the two "..." CFileDialog browse handlers (prefs.cpp:670-694) are NOT ported — they stay
// MFC-side for now. The two path fields are instead directly typeable here (staging buffers,
// see below), which is the only way a panel-only session could ever change them.
//
// LABELS: taken from the real dialog template IDD_COD4RADIANT_PREFERENCES
// (res/radiant.rc:592-682) — the CONTROL text for checkboxes, and the adjacent IDC_STATIC
// caption for the edit fields (those have no text of their own). The GROUPING below follows
// that template's group boxes ("Views / Rendering", "Camera ", "Texturing", "New
// functionality:", "Maps / Undo / INI") so the panel A/Bs against the MFC dialog side by side.
// Where the control carries no text at all — the three view-mode radios 1006/1009/1014 are
// bitmap radios with picture statics next to them — the label is descriptive and the registry
// key ("QE4StyleWindows") is quoted instead; close-enough labels are fine, because the
// FIELD→PREF mapping correctness lives entirely in PrefsDlg_Gather /
// Prefs_ApplyFromDialogState, not in what this file prints.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include "prefs.h"                  // prefData_t + g_PrefsDlg (the Gather/Apply subject)
#include <imgui/imgui.h>
#include <cstring>
#include <string>

// MUST MATCH prefs.cpp:348 verbatim (shared-header consolidation pending)
// PHASE-4 SHIM FLAG: sUserIni / sUserFilters are MFC CStrings. That compiles here only
// because stdafx.h pulls afxwin.h into every editor TU; this is another entry in the plan's
// "CString needs a decision" list (RADIANT_UI_REWORK_PLAN.md:108-116). When the shim lands,
// this copy and the two staging-buffer conversions in PP_Refresh / PP_Apply are the places to
// retarget. (Unlike mapinfo's CMap member, CString is copyable, so this struct can and does
// live in a file static — same static-init category as prefs.cpp's own s_radiantPrefs, which
// already holds eleven CStrings.)
struct prefsDlgState_t
{
    // Radio group indices (DDX_Radio): 0-based position of the checked button.
    int  rMouse;
    int  rView;
    // Checkboxes (BOOL).
    BOOL bLoadLast, bFace, bRightClick, bAutoSave, bLoadLastMap, bTexSubset;
    BOOL bSnapshots, bLoseChanges, bCamXYUpdate, bUseWheel, bAltAlwaysMove;
    BOOL bSnapTGrid, bLinkKeepSel, bPaintSizing, bCullSky, bDontClamp;
    BOOL bTexToolbar;
    BOOL bChaseMouse, bTexScrollbar, bThickLines, bColoredEnts, bTexBrush2d;
    BOOL bTexMesh2d, bFast2dDrag, bDetachWin, bTransBg;
    // Edit fields (ints / floats).
    int   nAutoSaveMin, nStatusSize, nRotation, nFarplane, nUndoLevels;
    int   nTolerantWeld, nSplay, nDropHeight, nScaleBase, nScaleRange;
    int   nVehArrowTime, nVehArrowSize;
    float fFov, fModelOrg, fPrefabOrg;
    std::string sUserIni, sUserFilters;   // was MFC CString before U-SHIM removal
};

// ── prefs.cpp bindings ────────────────────────────────────────────────────────
// The struct-coupled DDX core of CPrefsDlg, de-static'd 2026-08-07 for this unit and not in
// any header yet (prefsDlgState_t is still local to prefs.cpp). Verified non-static at
// prefs.cpp:369 and prefs.cpp:419. Everything else the dialog owns is HWND-side
// (DoDataExchange, the CFileDialog browse handlers, OnSetGamePrefs), so nothing else is bound.
extern void PrefsDlg_Gather( const prefData_t *p, prefsDlgState_t &out );        // prefs.cpp:362
extern void Prefs_ApplyFromDialogState( prefData_t *p, const prefsDlgState_t &st ); // prefs.cpp:412

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showPrefs = false;
static bool s_opened    = false;      // first-open latch (OnInitDialog equivalent)

// The panel IS the dialog's control set, so the state struct persists across frames
// (it is the m_* member block of CPrefsDlg).
static prefsDlgState_t s_state;

// The two CString fields need a char buffer to be typeable; 260 = MAX_PATH, matching what the
// CFileDialog browse handlers would hand back. Copied in by PP_Refresh, out by PP_Apply.
static char s_userIni[260]     = { 0 };
static char s_userFilters[260] = { 0 };

// ── helpers ───────────────────────────────────────────────────────────────────
// prefsDlgState_t's checkboxes are BOOL (a 4-byte int), so they cannot be handed to
// ImGui::Checkbox directly — stage through a real bool.
struct prefCheck_t
{
    const char *label;
    BOOL       *value;
};

// Two-column checkbox block; TableNextColumn wraps to the next row on its own, so the items
// fill row-major in declaration order. _SizingFixedFit (not _StretchSame) because these
// tables sit in a scrolling, user-resizable window and fixed-fit columns settle in one frame.
static void PP_CheckGrid( const char *id, const prefCheck_t *items, int count )
{
    if ( ImGui::BeginTable( id, 2, ImGuiTableFlags_SizingFixedFit ) )
    {
        for ( int i = 0; i < count; ++i )
        {
            ImGui::TableNextColumn();
            bool on = ( *items[i].value != 0 );
            if ( ImGui::Checkbox( items[i].label, &on ) )
                *items[i].value = on ? TRUE : FALSE;
        }
        ImGui::EndTable();
    }
}

// Same bounded copy the sibling panels use for their edit-field staging buffers
// (cf. imgui_panel_layers.cpp:71 / imgui_panel_entity.cpp:100).
static void PP_CopyField( char *dst, size_t dstSz, const char *src )
{
    dst[0] = '\0';
    if ( src )
    {
        strncpy( dst, src, dstSz - 1 );
        dst[dstSz - 1] = '\0';
    }
}

// The numeric edits, at the narrow width the dialog's edit boxes have.
static void PP_Int( const char *label, int *v )
{
    ImGui::SetNextItemWidth( 120.0f );
    ImGui::InputInt( label, v );
}

static void PP_Float( const char *label, float *v )
{
    ImGui::SetNextItemWidth( 120.0f );
    ImGui::InputFloat( label, v );
}

// OnInitDialog's load pass (prefs.cpp:558-606) minus UpdateData: prefData_t → panel fields,
// plus the CString → staging-buffer copies.
static void PP_Refresh()
{
    PrefsDlg_Gather( g_PrefsDlg, s_state );
    PP_CopyField( s_userIni,     sizeof( s_userIni ),     s_state.sUserIni.c_str() );
    PP_CopyField( s_userFilters, sizeof( s_userFilters ), s_state.sUserFilters.c_str() );
}

// OnOK's commit pass (prefs.cpp:612-659) minus UpdateData: staging buffers → state, then the
// one core call (which persists through Prefs_SavePrefs itself). The trailing re-Gather is the
// panel-flow addition that surfaces SavePrefs' in-place clamps.
static void PP_Apply()
{
    s_state.sUserIni     = s_userIni;
    s_state.sUserFilters = s_userFilters;
    Prefs_ApplyFromDialogState( g_PrefsDlg, s_state );
    PP_Refresh();
}

// Panel toggle, drawn inside the shell window (imgui_shell.cpp).
void ImGuiPanel_Prefs_MenuItem()
{
    ImGui::Checkbox( "Preferences", &s_showPrefs );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 32784, where the MFC handler called Prefs_ShowDialog( this ) — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_Prefs_Toggle()
{
    s_showPrefs = !s_showPrefs;
}

void ImGuiPanel_Prefs_Draw()
{
    if ( !s_showPrefs )
    {
        s_opened = false;      // a re-open re-Gathers (the dialog re-runs OnInitDialog too)
        return;
    }

    if ( !s_opened )
    {
        s_opened = true;
        PP_Refresh();
    }

    // Caption from the template (res/radiant.rc:594). Unlike the sibling panels this one is
    // NOT AlwaysAutoResize: 45 fields is taller than the dialog's own 359 DLU, so it gets a
    // resizable, scrolling window seeded at roughly the template's proportions instead.
    ImGui::SetNextWindowSize( ImVec2( 460.0f, 680.0f ), ImGuiCond_FirstUseEver );
    if ( ImGui::Begin( "CoD4Radiant Preferences", &s_showPrefs ) )
    {
        // ── mouse + wheel: the two radios and the checkbox that sit below the group boxes
        //    (rc 1003/1005, 1538). Gather derives rMouse from m_nMouseButtons==3
        //    (prefs.cpp:371) and Apply maps it back to 3 / 2 (prefs.cpp:422).
        ImGui::SeparatorText( "Mouse" );
        ImGui::RadioButton( "2 button", &s_state.rMouse, 0 );
        ImGui::SameLine();
        ImGui::RadioButton( "3 button", &s_state.rMouse, 1 );
        {
            const prefCheck_t items[] =
            {
                { "Use mouse wheel in camera window", &s_state.bUseWheel },   // 1538
            };
            PP_CheckGrid( "##prefsmouse", items, (int)( sizeof( items ) / sizeof( items[0] ) ) );
        }

        // ── "Views / Rendering" (rc group @7,4) ───────────────────────────────────
        // The three view radios (1006/1009/1014) are BITMAP radios with picture statics for
        // captions, so the template carries no text for them — labels below are descriptive,
        // keyed off the registry name ("QE4StyleWindows", prefs.h:14) and the port note that
        // index 1 is the QE4 layout (prefs.cpp:696-698).
        ImGui::SeparatorText( "Views / Rendering" );
        ImGui::TextUnformatted( "Window layout (\"QE4StyleWindows\"):" );
        ImGui::RadioButton( "Layout 0 (single-window)", &s_state.rView, 0 );
        ImGui::RadioButton( "Layout 1 (QE4 style / detached-capable)", &s_state.rView, 1 );
        ImGui::RadioButton( "Layout 2", &s_state.rView, 2 );
        {
            const prefCheck_t items[] =
            {
                { "Ents use '_color' value", &s_state.bColoredEnts },   // 1420
                { "Thick selection lines",   &s_state.bThickLines  },   // 1486
                { "Fast 2d view dragging",   &s_state.bFast2dDrag  },   // 1672
            };
            PP_CheckGrid( "##prefsviews", items, (int)( sizeof( items ) / sizeof( items[0] ) ) );
        }
        // OnSetGamePrefs (prefs.cpp:699-705): these two are enabled only for the QE4 layout.
        // Same gate, expressed as BeginDisabled instead of EnableWindow — the fields still
        // round-trip when greyed, exactly as the DDX-bound controls did.
        ImGui::BeginDisabled( s_state.rView != 1 );
        {
            const prefCheck_t items[] =
            {
                { "Detached Windows",       &s_state.bDetachWin },   // 1674
                { "Transparent background", &s_state.bTransBg   },   // 1687
            };
            PP_CheckGrid( "##prefsviewsqe4", items, (int)( sizeof( items ) / sizeof( items[0] ) ) );
        }
        ImGui::EndDisabled();

        // ── "Camera " (rc group @126,13) ──────────────────────────────────────────
        // The group's MoveSpeed trackbar (1219) is NOT part of prefsDlgState_t — it is not
        // DDX-bound in the port either (see the suspicious-item note in the unit report), so
        // there is no field for it here.
        ImGui::SeparatorText( "Camera" );
        {
            const prefCheck_t items[] =
            {
                { "Update XY on drag", &s_state.bCamXYUpdate },   // 1223
                { "Cull sky on clip",  &s_state.bCullSky     },   // 1517
            };
            PP_CheckGrid( "##prefscam", items, (int)( sizeof( items ) / sizeof( items[0] ) ) );
        }
        PP_Int  ( "Farplane", &s_state.nFarplane );   // 1027
        PP_Float( "FOV",      &s_state.fFov );        // 1700 — SavePrefs clamps to 2..160

        // ── "Texturing" (rc group @210,13) ────────────────────────────────────────
        ImGui::SeparatorText( "Texturing" );
        {
            const prefCheck_t items[] =
            {
                { "Texture toolbar",      &s_state.bTexToolbar   },   // 1047
                { "Texture scrollbar",    &s_state.bTexScrollbar },   // 1054
                { "Texture subset",       &s_state.bTexSubset    },   // 1045
                { "Texture brushes in 2d", &s_state.bTexBrush2d  },   // 1423
                { "Texture meshes in 2d", &s_state.bTexMesh2d    },   // 1424
            };
            PP_CheckGrid( "##prefstex", items, (int)( sizeof( items ) / sizeof( items[0] ) ) );
        }

        // ── "New functionality:" (rc group @7,108) ────────────────────────────────
        ImGui::SeparatorText( "New functionality" );
        {
            const prefCheck_t items[] =
            {
                { "Right click to drop entities", &s_state.bRightClick    },   // 1042
                { "Face selection",               &s_state.bFace          },   // 1040
                { "Linking keeps selection",      &s_state.bLinkKeepSel   },   // 1085
                { "ALT always move",              &s_state.bAltAlwaysMove },   // 1246
                { "Snap T to Grid",               &s_state.bSnapTGrid     },   // 1051
                { "Mouse chaser",                 &s_state.bChaseMouse    },   // 1249
                { "Paint sizing info",            &s_state.bPaintSizing   },   // 1084
            };
            PP_CheckGrid( "##prefsnew", items, (int)( sizeof( items ) / sizeof( items[0] ) ) );
        }
        PP_Int  ( "Rotation inc",       &s_state.nRotation );        // 1204
        PP_Int  ( "Vehicle Arrow Time", &s_state.nVehArrowTime );    // 1480
        PP_Int  ( "Vehicle Arrow Size", &s_state.nVehArrowSize );    // 1481
        PP_Float( "Model Origin Size",  &s_state.fModelOrg );        // 1559
        PP_Float( "Prefab Origin Size", &s_state.fPrefabOrg );       // 1560
        PP_Int  ( "Tolerant Weld",      &s_state.nTolerantWeld );    // 1456
        PP_Int  ( "Splay Distance",     &s_state.nSplay );           // 1457
        PP_Int  ( "Drop Height",        &s_state.nDropHeight );      // 1459
        PP_Int  ( "Scale Base",         &s_state.nScaleBase );       // 1461 — clamped on save
        PP_Int  ( "Scale Range",        &s_state.nScaleRange );      // 1463 — clamped on save

        // ── "Maps / Undo / INI" (rc group @7,234) ─────────────────────────────────
        ImGui::SeparatorText( "Maps / Undo / INI" );
        {
            const prefCheck_t items[] =
            {
                { "Don't clamp plane points", &s_state.bDontClamp   },   // 1255
                { "Snapshots",                &s_state.bSnapshots   },   // 1094
                // 1021 "Load last project on open" REMOVED — the registry LastProject
                // startup tier is gone (mainfrm.cpp Radiant_LoadProjectAtStartup); the
                // editor always uses the cod4.prj + game data beside the exe.
                { "Load last map on open",    &s_state.bLoadLastMap },   // 1024
                { "Auto save every",          &s_state.bAutoSave    },   // 1023
                { "Lose changes?",            &s_state.bLoseChanges },   // 1095 (DDX only;
                                                                         //  its handler is a
                                                                         //  nullsub, prefs.cpp:709)
            };
            PP_CheckGrid( "##prefsmaps", items, (int)( sizeof( items ) / sizeof( items[0] ) ) );
        }
        PP_Int( "Auto save minutes", &s_state.nAutoSaveMin );   // 1065
        PP_Int( "Status point size", &s_state.nStatusSize );    // 1201
        PP_Int( "Undo Levels",       &s_state.nUndoLevels );    // 1208

        // The two path strings. Typed here through the staging buffers; the "..." browse
        // buttons (1029 / 1673) remain MFC-side — they are CFileDialog handlers on the dialog
        // (prefs.cpp:670-694) and no HWND-free picker exists yet, so a panel-side Browse is an
        // orchestrator to-do rather than something to invent here.
        ImGui::SetNextItemWidth( 320.0f );
        ImGui::InputText( "User INI path", s_userIni, sizeof( s_userIni ) );        // 1026
        ImGui::SetNextItemWidth( 320.0f );
        ImGui::InputText( "User Filters",  s_userFilters, sizeof( s_userFilters ) ); // 1685
        ImGui::TextDisabled( "(\"...\" browse buttons stay on the MFC dialog for now)" );

        ImGui::Separator();
        // OK equivalent: commit + persist. Refresh is the Cancel equivalent (re-read
        // g_PrefsDlg, dropping staged edits) — nothing above has touched prefData_t yet.
        if ( ImGui::Button( "Apply" ) )
            PP_Apply();
        ImGui::SameLine();
        if ( ImGui::Button( "Refresh" ) )
            PP_Refresh();
    }
    ImGuiShell_CloseOnFocusLoss( &s_showPrefs );
    ImGui::End();
}
