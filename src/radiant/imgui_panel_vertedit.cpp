// imgui_panel_vertedit.cpp — UI-rework Phase 3: ImGui "Vertex edit" panel over the
// verteditdlg.cpp core action. New KISAK code; visible only under -imgui.
//
// Panel semantics vs CVertEditDlg: the SAME single core call its one command handler
// makes, with the same widget reads —
//   * the four R/G/B/A values, 0..255, the dialog's IDC_VED_*_SLIDER track bars
//     (verteditdlg.cpp:214-233, TBM_SETRANGE 0..255) held in the dialog's stored colour
//     (verteditdlg.cpp:153-157, seeded 255/255/0/0 like the binary's ctor).
//   * the two enable checkboxes, label text verbatim from OnCreate
//     (verteditdlg.cpp:239-240): "Apply Color" → st.doColour (IDC_VED_CHK_COLOR, binary
//     CButton @this+600) and "Apply Alpha" → st.doAlpha (IDC_VED_CHK_ALPHA, @this+516).
//     "Apply Color" starts CHECKED, as OnCreate's BM_SETCHECK does (verteditdlg.cpp:241);
//     "Apply Alpha" starts clear.
//   * [Apply] → fill vertEditState_t and call VertEditDlg_Apply, then `g_nUpdateBits |= 1`
//     — the whole of OnApply (verteditdlg.cpp:274-286).  The |= 1 is DIALOG-side there, not
//     part of the action, so it is reproduced here rather than assumed.  (The action's own
//     tail already stamps g_nUpdateBits = -1 (verteditdlg.cpp:120), which makes the |= 1
//     a no-op in practice; it is bound anyway because that is what the handler does.)
//
// KNOWN CORE ISSUE, bound AS-IS: the action's undo bracket is one-shot per patch —
// VED_BracketPatch sets `patch->xx22b` and nothing on the vert-edit path ever clears it
// (only Patch_Paint / PMESH_18 do), so a SECOND [Apply] on the same patch silently skips
// the undo bracket (RADIANT_KNOWN_ISSUES.md, verteditdlg.cpp ~34).  The panel must not
// paper over it: no extra Undo_* calls here, and no repeat-apply guard.  The fix belongs in
// the core, against IDB 0x461210, and lands separately.
//
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md):
//   * the panel stays open; hiding it is the OnClose ShowWindow( SW_HIDE ) equivalent, so
//     the colour + checkbox state PERSISTS across a hide/show exactly like the modeless MFC
//     popup (whose stored colour lives in file statics, verteditdlg.cpp:153-157).
//   * OnColorButton's CColorDialog and the owner-drawn swatch static collapse into
//     ColorEdit3's own swatch/picker (see the panel-UX note below) — no CColorDialog, and
//     therefore none of its GetRValue/DXSDK macro workaround (verteditdlg.cpp:265-269).
//   * OnHScroll → VED_SyncFromSliders has no analogue: the widgets ARE the stored colour
//     here, so there is nothing to pull out of a track bar before [Apply].
//
// The dialog is WRITE-ONLY: it never reads the picked control points' current colour back,
// so nothing here pre-fills or reflects map state (no read-side *_Gather exists, and
// inventing one would be new behaviour).
//
// NO HWND/MFC anywhere in this file: the only bind is the one UI-independent action below.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>
#include "radiant_ui_actions.h"

// ── verteditdlg.cpp bindings ──────────────────────────────────────────────────
extern int g_nUpdateBits;                    // 0x25D5A74 (engine_stubs.cpp), as verteditdlg.cpp:20

// One [Apply] pass snapshot: the four R/G/B/A slider values and the two enable check
// boxes the paint consults.
// MUST MATCH verteditdlg.cpp verbatim (shared-header consolidation pending)
struct vertEditState_t
{
    byte r;               // IDC_VED_R_SLIDER   (binary this+128)
    byte g;               // IDC_VED_G_SLIDER   (binary this+124)
    byte b;               // IDC_VED_B_SLIDER   (binary this+120)
    byte a;               // IDC_VED_A_SLIDER   (binary this+116)
    bool doColour;        // IDC_VED_CHK_COLOR  (binary CButton @this+600)
    bool doAlpha;         // IDC_VED_CHK_ALPHA  (binary CButton @this+516)
};

extern void VertEditDlg_Apply( const vertEditState_t &st );   // 0x461210 (verteditdlg.cpp:66)

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showVertEdit = false;

// The dialog's stored colour, kept in the SAME 0..255 byte form the sliders and
// vertEditState_t use (verteditdlg.cpp:153-157; the binary's ctor seeds 255/255/0/0).
static byte s_vedColR = 255;
static byte s_vedColG = 255;
static byte s_vedColB = 0;
static byte s_vedColA = 0;

// The two enable checkboxes.  Colour on by default = OnCreate's BM_SETCHECK
// (verteditdlg.cpp:241); alpha unchecked, as OnCreate leaves it.
static bool s_doColour = true;
static bool s_doAlpha  = false;

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_VertEdit_MenuItem()
{
    ImGui::Checkbox( "Vertex edit", &s_showVertEdit );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 33199, where the MFC handler called CVertEditDlg::Toggle() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_VertEdit_Toggle()
{
    s_showVertEdit = !s_showVertEdit;
}

void ImGuiPanel_VertEdit_Draw()
{
    if ( !s_showVertEdit )
        return;

    // "Vertex Edit" = the popup's caption (verteditdlg.cpp:330).
    if ( ImGui::Begin( "Vertex Edit", &s_showVertEdit, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // VertEditDlg_Apply only touches control points that are in g_qeglobals.d_move_points
        // (verteditdlg.cpp:95-97), i.e. the points picked in vertex-edit mode, and only on
        // selected patches — with nothing picked it is simply a no-op.  The MFC [Apply] is
        // not disabled in that case either, so nothing is gated here.
        ImGui::TextDisabled( "acts on the picked control points of the selected patches" );

        // ── R/G/B ─────────────────────────────────────────────────────────────
        // PANEL UX: the three colour sliders + the swatch static + [Color...] become ONE
        // ColorEdit3.  Uint8|DisplayRGB keeps the three editable fields reading 0..255 like
        // the track bars, and the widget's own swatch/picker popup is the CColorDialog
        // equivalent.  Alpha deliberately stays OUT of it (a ColorEdit4 would fold it in):
        // the MFC picker never touches alpha (verteditdlg.cpp:258), so A keeps its own
        // 0..255 slider below, exactly the IDC_VED_A_SLIDER shape.
        //
        // Byte ↔ float round-trips through /255 and *255+0.5, which is exact for all 256
        // values, so dragging RGB never drifts a channel.
        float rgb[3] = { s_vedColR / 255.0f, s_vedColG / 255.0f, s_vedColB / 255.0f };
        if ( ImGui::ColorEdit3( "Color", rgb,
                                ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB ) )
        {
            s_vedColR = (byte)( rgb[0] * 255.0f + 0.5f );
            s_vedColG = (byte)( rgb[1] * 255.0f + 0.5f );
            s_vedColB = (byte)( rgb[2] * 255.0f + 0.5f );
        }

        // ── A ─────────────────────────────────────────────────────────────────
        // The one row kept as a literal 0..255 slider: same range as the track bar's
        // TBM_SETRANGE( 0, 255 ) (verteditdlg.cpp:228).
        int alpha = s_vedColA;
        if ( ImGui::SliderInt( "A", &alpha, 0, 255 ) )
            s_vedColA = (byte)alpha;

        // ── the two enable checkboxes ─────────────────────────────────────────
        // Labels verbatim from OnCreate (verteditdlg.cpp:239-240).  They gate the two write
        // arms INSIDE the action (verteditdlg.cpp:100-113), so both clear = a no-op apply,
        // like the dialog with both boxes unchecked.
        ImGui::Checkbox( "Apply Color", &s_doColour );
        ImGui::SameLine();
        ImGui::Checkbox( "Apply Alpha", &s_doAlpha );

        // ── [Apply] ───────────────────────────────────────────────────────────
        // OnApply, verbatim: snapshot → action → the dialog-side g_nUpdateBits |= 1.
        if ( ImGui::Button( "Apply" ) )
        {
            vertEditState_t st;
            st.r        = s_vedColR;
            st.g        = s_vedColG;
            st.b        = s_vedColB;
            st.a        = s_vedColA;
            st.doColour = s_doColour;
            st.doAlpha  = s_doAlpha;
            VertEditDlg_Apply( st );          // rebuilds each touched patch, under Undo
            g_nUpdateBits |= 1;               // verteditdlg.cpp:285
        }

        ImGui::TextDisabled( "Write-only, like the dialog: it never reads the points back" );
    }
    ImGuiShell_CloseOnFocusLoss( &s_showVertEdit );
    ImGui::End();
}
