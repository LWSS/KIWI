// imgui_panel_advpatch.cpp — UI-rework Phase 3: ImGui "Advanced patch edit" panel over the
// patchdialog.cpp CAdvPatchEditDlg core actions. New KISAK code; visible only under -imgui.
//
// Panel semantics vs CAdvPatchEditDlg (patchdialog.cpp:72-219): the SAME core calls its
// handlers make, in the same order —
//   * OnInitDialog     (patchdialog.cpp:96-116): CurveEdit_BindData for the 3 param slots with
//     the binary's baked trackbar/edit ids, defaults, ranges and steps (patchdialog.cpp:101-103),
//     then the two slider-max range edits seeded to 1024 (patchdialog.cpp:112).  Run once, from
//     the first-open latch below.  The TBM_SETRANGE / TBM_SETPOS / SetHwnds half of that loop
//     (patchdialog.cpp:106-110) has no analogue: there are no HWNDs here, and the slider's
//     range and thumb are READ from the slot every frame instead of being pushed into a control.
//   * RefreshSlot      (patchdialog.cpp:79-86): the buddy edit shows the
//     CurveEdit_DisplayValue and the thumb sits at CurveEdit_StepIndex.  Both are re-read every
//     frame here, so every action's effect shows up on the next frame with no explicit refresh
//     call — that IS the RefreshSlot equivalent.
//   * SyncCtrl         (patchdialog.cpp:88-94, EN_KILLFOCUS on 1428/1429/1430): the typed value
//     goes straight into AdvPatchEdit_ApplySlotValue, which transforms it
//     (CurveEdit_InputToValue: amplitude types the AMPLITUDE and stores log2+8) and then
//     clamps+snaps it to the slot's grid (CurveEdit_SnapStore).
//   * SyncRange        (patchdialog.cpp:123-133, EN_KILLFOCUS on 1482/1483): the typed max goes
//     into AdvPatchEdit_ApplySlotRange, which sets max and re-grids step = max/64
//     (pmesh.cpp:5006-5013).  The `mx == 0 -> 1024` fallback AND the write-back of "1024" into
//     the edit box (patchdialog.cpp:128) live in the HANDLER, not the action, so they are
//     replicated panel-side below.  The TBM_SETRANGE re-send that follows it
//     (patchdialog.cpp:131) needs no analogue for the same reason as above — the slider's max
//     is CurveEdit_StepCount, read fresh every frame.
//   * OnHScroll        (patchdialog.cpp:137-152): thumb pos -> AdvPatchEdit_ApplySlotStep for
//     the slot whose trackbar id matched.  The panel already knows the slot, so the id lookup
//     (patchdialog.cpp:142-143) collapses away.
//   * OnSetColor       (patchdialog.cpp:158-163): CColorDialog seeded with the current paint
//     colour, and on OK the RAW COLORREF into AdvPatchEdit_ApplyPaintColor.
//   * OnSetAlpha       (patchdialog.cpp:166-172): CColorDialog seeded with the current alpha as
//     a grey, and on OK the picked colour into AdvPatchEdit_ApplyPaintAlpha, which stores
//     (R+G+B)/3 (patchdialog.cpp:67-70).
//   * OnDrawItem       (patchdialog.cpp:174-191): the two owner-draw swatches (1462 colour,
//     1466 alpha-as-grey) become the two read-only ImGui::ColorButtons.  Both mirror
//     CreateSolidBrush( raw ), i.e. they interpret the stored value as a COLORREF (low byte =
//     R) — see the BYTE ORDER note below.
//
// COLORREF BYTE ORDER — the one thing this panel must not "fix": CColorDialog::GetColor returns
// RGB(r,g,b) = r | g<<8 | b<<16, and AdvPatchEdit_ApplyPaintColor stores that value VERBATIM
// (patchdialog.cpp:61-64, the binary's this[323] = m_cc.rgbResult).  The paint consumer then
// reads it as {B,G,R} — low byte = B (patchdialog.cpp:432-434) — and the case-5 eyedropper
// writes it in that {B,G,R} order (patchdialog.cpp:480).  So the picker path and the
// eyedropper path disagree by a channel swap.  That is a PRE-EXISTING inconsistency in the
// original, already called out at patchdialog.cpp:155-157 and preserved for fidelity: this
// panel packs exactly what GetColor would have returned (r | g<<8 | b<<16) and nothing more,
// so painting with a panel-picked colour swaps R/B exactly as the MFC dialog does.  The alpha
// action is order-independent — it sums all three bytes.
//
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md):
//   * the panel stays open; hiding it is the OnCancel/OnClose ShowWindow( SW_HIDE ) equivalent
//     (patchdialog.cpp:202-203), so there is no [Close] button and the param values PERSIST
//     across a hide/show — they live in the shared g_curveEditCtrls table (pmesh.cpp:4565),
//     exactly like the modeless MFC popup's.
//   * the first-open latch is ONE-SHOT PER PROCESS, not per re-open (unlike the P-7 panel's):
//     OnInitDialog runs once per dialog CREATE and AdvPatchEdit_Show creates once
//     (patchdialog.cpp:260-268), so re-running the bind on every re-open would reset the
//     user's radii every time the panel is toggled — a behaviour the MFC dialog does not have.
//     Consequence worth knowing: if both UIs are used in one session, the MFC Create and this
//     latch each run the same seeding once with the same baked defaults, so the only visible
//     effect is that whichever runs SECOND resets the three values to 16 / 64 / 8.
//   * EN_KILLFOCUS becomes an explicit [Set] (or Enter): a live-on-every-keystroke apply would
//     push half-typed numbers through CurveEdit_SnapStore, and the ImGui equivalent of "lost
//     focus" would fire on every window change.  [Set] is the commit point.
//   * CColorDialog + the [Color...] / [Alpha...] buttons collapse into ColorEdit3 / one grey
//     slider, and each accepted change IS that modal's OK — so there is no cancel arm, the
//     same shape as the P-7 panel's inlined [Set] (imgui_panel_patch.cpp:36-38).
//   * only the two slots that HAVE a range edit get a max field: 1482 -> slot 0 and 1483 ->
//     slot 1 (patchdialog.cpp:134-135).  The amplitude slot has no range edit in the template
//     (res/radiant.rc:742-743 are the only two), and giving it one would invent a control that
//     can re-grid the amplitude exponent — new behaviour, so it is not offered.
//   * the slider's range is CurveEdit_StepCount( slot ), which is what TBM_SETRANGE is sent
//     (patchdialog.cpp:108/131): 0..64 for the two radii (1024/16, and max/64 after any
//     ApplySlotRange) but 0..16 for the amplitude exponent (16/1).  A hardcoded 0..64 would
//     let the amplitude slider address 48 positions the trackbar cannot reach.
//
// NOT BOUND HERE — no UI-independent action exists yet (orchestrator to-do): the 7 mode radios
// (1435/1439/1437/1436/1438/1468/1440), the 6 channel checkboxes (1469..1474) and the two
// soft-selection checkboxes (1431/1432) are read LIVE off the MFC dialog's HWND through
// AdvDlg_IsChecked (patchdialog.cpp:362-370, consumed by sub_401DB0 at :372 and sub_43E6F0 at
// :407-415) and IsDlgButtonChecked (CurvEditDlg_OnSomeSetting, patchdialog.cpp:239-243); the
// picked-height readout (edit 1467) is likewise dialog-side (OnHeightText, patchdialog.cpp:
// 193-199, reached through the file-static AdvPatchEdit_ShowHeight at :230).  All of those need
// a UI-independent store before a panel can drive them, which is a core change, not a panel
// one.  Until then the mode still comes from the MFC dialog — and with no MFC dialog created,
// sub_401DB0 returns 6 ("Disabled"), so terrain paint is OFF no matter what this panel sets.
// That is stated in the panel itself rather than hidden.
//
// NO HWND/MFC anywhere in this file: every bind is a UI-independent action or a plain getter,
// and the panel re-reads the slot table instead of going through SetDlgItemText.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>
#include "kiwi_fmt.h"
#include "radiant_ui_actions.h"

// ── patchdialog.cpp bindings (the five wave-3 actions; not in radiant_ui_actions.h —
//    this panel is their first non-MFC consumer) ────────────────────────────────
extern void AdvPatchEdit_ApplySlotValue( int slot, float typed );    // patchdialog.cpp:42  (SyncCtrl core)
extern void AdvPatchEdit_ApplySlotRange( int slot, float maxVal );   // patchdialog.cpp:49  (sub_4015C0)
extern void AdvPatchEdit_ApplySlotStep( int slot, int pos );         // patchdialog.cpp:55  (OnHScroll core)
extern void AdvPatchEdit_ApplyPaintColor( COLORREF color );          // patchdialog.cpp:61  (sub_4021E0 core)
extern void AdvPatchEdit_ApplyPaintAlpha( COLORREF c );              // patchdialog.cpp:67  (sub_4022A0 core)

// UI-rework: the mode + channel store that replaces the MFC dialog's live BM_GETCHECK reads
// (patchdialog.cpp). Without driving these the paint mode stays 6 ("Disabled") and no stroke
// ever fires — this is what made the Y-hotkey terrain edit do nothing.
extern int  AdvPatchEdit_GetMode();
extern void AdvPatchEdit_SetMode( int mode );
extern bool AdvPatchEdit_GetChannel( int idx );
extern void AdvPatchEdit_SetChannel( int idx, bool on );

// The paint target, for the two owner-draw swatch equivalents + the pickers' seed values
// (the CColorDialog ctor arguments, patchdialog.cpp:160/169).  Both are file-scope globals,
// not statics (patchdialog.cpp:358-359 = IDB dword_25D65A4 / byte_25D65A8).
extern unsigned int g_paintColorBGR;                                 // patchdialog.cpp:195
extern byte         g_paintColorA;                                   // patchdialog.cpp:196

// ── pmesh.cpp bindings (the CurvEditDlg control table; all non-static) ──────────
extern void  CurveEdit_BindData( int slot, int trackbarId, int editId, float defVal,
                                 float mn, float mx, float step );   // pmesh.cpp:4942 (sub_4010D0 bind half)
extern int   CurveEdit_Slots();                                      // pmesh.cpp:5240 (== 3)
extern int   CurveEdit_StepCount( int slot );                        // pmesh.cpp:5246 (TBM_SETRANGE max)
extern int   CurveEdit_StepIndex( int slot );                        // pmesh.cpp:5252 (TBM_SETPOS pos)
extern float CurveEdit_DisplayValue( int slot );                     // pmesh.cpp:5270 (sub_401000 text)

// The 3 param slots exactly as OnInitDialog binds them (patchdialog.cpp:101-103), plus the
// group labels from the template (res/radiant.rc:723/725/727, ampersands dropped) and the
// slider-max edit that drives each one (res/radiant.rc:742-743; 0 = no range edit exists).
struct advPatchSlot_t
{
    int         trackbarId;      // 1424 / 1425 / 1426
    int         editId;          // buddy edit 1428 / 1429 / 1430
    float       defVal, mn, mx, step;
    const char *label;
    int         rangeEditId;     // 1482 / 1483, or 0
};

// KIWI-UX: defaults raised from the dialog's 16/64.  CoD4 terrain control points sit
// hundreds of units apart, so a 64-unit outer radius usually contained NO control point
// and a stroke silently did nothing.  64/256 reaches typical terrain immediately.
static const advPatchSlot_t s_slotDef[3] =
{
    { 1424, 1428,  64.0f, 0.0f, 1024.0f, 16.0f, "Inner Radius", 1482 },
    { 1425, 1429, 256.0f, 0.0f, 1024.0f, 16.0f, "Outer Radius", 1483 },
    { 1426, 1430,  11.0f, 0.0f,   16.0f,  1.0f, "Amplitude",       0 },
};

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showAdvPatch = false;
static bool s_bound        = false;   // OnInitDialog latch — one-shot per PROCESS (see header)

// The typed-value fields.  In the MFC dialog one control is both the display and the input
// (the buddy edit); here they are split, so these hold only what the user typed and are
// seeded once from the slot's display value.
static float s_typed[3] = { 0.0f, 0.0f, 0.0f };

// The slider-max edits 1482 / 1483, seeded to 1024 as OnInitDialog's SetDlgItemInt does
// (patchdialog.cpp:112).  The third entry is unused (no range edit for the amplitude slot).
static float s_range[3] = { 1024.0f, 1024.0f, 0.0f };

// The picker state, seeded once from the live paint target (the CColorDialog ctor seeds,
// patchdialog.cpp:160/169).  Kept in ImGui's 0..1 floats; the COLORREF is packed on commit.
static float s_pickRGB[3] = { 1.0f, 1.0f, 1.0f };
static int   s_pickAlpha  = 255;

// Pack a COLORREF exactly as CColorDialog::GetColor would have returned it: r | g<<8 | b<<16
// (== the RGB macro).  Written out by hand because the matching GetRValue/GetGValue/GetBValue
// macros clash with a DXSDK redefinition under this target (verteditdlg.cpp:267-269), so the
// unpack below cannot use them either — and a hand-packed literal is the one place where the
// byte order has to be visible.  Byte <-> float round-trips through /255 and *255+0.5, exact
// for all 256 values.
static COLORREF AP_PackColorRef( const float rgb[3] )
{
    const unsigned int r = (unsigned int)( rgb[0] * 255.0f + 0.5f );
    const unsigned int g = (unsigned int)( rgb[1] * 255.0f + 0.5f );
    const unsigned int b = (unsigned int)( rgb[2] * 255.0f + 0.5f );
    return (COLORREF)( r | ( g << 8 ) | ( b << 16 ) );
}

// The inverse, mirroring what OnDrawItem's CreateSolidBrush does with the stored value
// (patchdialog.cpp:178): low byte = R.
static void AP_UnpackColorRef( unsigned int c, float rgb[3] )
{
    rgb[0] = (float)( c & 0xFF ) / 255.0f;
    rgb[1] = (float)( ( c >> 8 ) & 0xFF ) / 255.0f;
    rgb[2] = (float)( ( c >> 16 ) & 0xFF ) / 255.0f;
}

// One param slot: the RefreshSlot readout, the trackbar, the buddy-edit commit and (for the
// two radii) the slider-max edit.
static void AP_SlotRow( int slot )
{
    const advPatchSlot_t &def = s_slotDef[slot];
    ImGui::PushID( def.trackbarId );
    ImGui::SeparatorText( def.label );

    // RefreshSlot's buddy-edit half: format CurveEdit_DisplayValue( slot ) for display.
    // (patchdialog.cpp:83-85).  Amplitude displays 2^(value-8), the others the raw value
    // (pmesh.cpp:4990-4994).
    char display[32];
    ImGui::Text( "value: %s", KiwiFmt_Num( display, sizeof( display ),
                                           CurveEdit_DisplayValue( slot ) ) );

    // RefreshSlot's TBM_SETPOS half + OnHScroll, fused: the thumb IS CurveEdit_StepIndex and
    // the range IS CurveEdit_StepCount, both re-read every frame, so a drag applies and the
    // next frame shows the snapped result.  Nothing about the position is cached panel-side.
    int pos = CurveEdit_StepIndex( slot );
    ImGui::SetNextItemWidth( 200.0f );
    if ( ImGui::SliderInt( "##slider", &pos, 0, CurveEdit_StepCount( slot ) ) )
        AdvPatchEdit_ApplySlotStep( slot, pos );

    // The buddy edit (EN_KILLFOCUS -> SyncCtrl): typed value -> transform -> clamp+snap.
    // Enter or [Set] is the commit (see the header note on EN_KILLFOCUS).
    ImGui::SetNextItemWidth( 90.0f );
    bool setValue = ImGui::InputFloat( "##typed", &s_typed[slot], 0.0f, 0.0f, KIWI_FMT_FLOAT,
                                      ImGuiInputTextFlags_EnterReturnsTrue );
    ImGui::SameLine();
    setValue |= ImGui::Button( "Set value" );
    ImGui::SameLine();
    ImGui::TextDisabled( "edit %d", def.editId );
    if ( setValue )
        AdvPatchEdit_ApplySlotValue( slot, s_typed[slot] );

    // The slider-max edit (EN_KILLFOCUS -> SyncRange), for the two slots that have one.
    if ( def.rangeEditId )
    {
        ImGui::SetNextItemWidth( 90.0f );
        bool setRange = ImGui::InputFloat( "##range", &s_range[slot], 0.0f, 0.0f, KIWI_FMT_FLOAT,
                                          ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::SameLine();
        setRange |= ImGui::Button( "Set max" );
        ImGui::SameLine();
        ImGui::TextDisabled( "slider max (edit %d)", def.rangeEditId );
        if ( setRange )
        {
            // HANDLER-side, not action-side: the mx == 0 -> 1024 fallback and the write-back
            // of "1024" into the edit box (patchdialog.cpp:128).
            if ( s_range[slot] == 0.0f )
                s_range[slot] = 1024.0f;
            AdvPatchEdit_ApplySlotRange( slot, s_range[slot] );   // max + step = max/64 + re-snap
        }
    }

    ImGui::PopID();
}

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_AdvPatch_MenuItem()
{
    ImGui::Checkbox( "Advanced patch edit", &s_showAdvPatch );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 33130, where the MFC handler called AdvPatchEdit_Toggle( this ) — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_AdvPatch_Toggle()
{
    s_showAdvPatch = !s_showAdvPatch;
}

void ImGuiPanel_AdvPatch_Draw()
{
    // Bind the paint-param slots (inner/outer radius, amplitude) at STARTUP — BEFORE the
    // panel-visibility early-out below.  sub_401D50 gates terrain paint on outer(1425) >
    // inner(1424); if the panel was never opened this session the getters returned 0 > 0 =
    // false, so Alt+LMB terrain paint silently did nothing.  Binding here (first Draw frame,
    // panel open or not) makes the radius gate valid from the first frame.
    if ( !s_bound )
    {
        // OnInitDialog (patchdialog.cpp:96-116), minus its HWND half.  One-shot per process:
        // the latch is deliberately never cleared (see the header).
        s_bound = true;
        for ( int slot = 0; slot < 3; ++slot )
        {
            const advPatchSlot_t &def = s_slotDef[slot];
            CurveEdit_BindData( slot, def.trackbarId, def.editId, def.defVal, def.mn, def.mx, def.step );
            s_typed[slot] = CurveEdit_DisplayValue( slot );   // the text RefreshSlot would have written
        }
        s_range[0] = 1024.0f;                                 // SetDlgItemInt( 1482, 1024 )
        s_range[1] = 1024.0f;                                 // SetDlgItemInt( 1483, 1024 )
        AP_UnpackColorRef( g_paintColorBGR, s_pickRGB );      // the CColorDialog seeds
        s_pickAlpha = g_paintColorA;
    }

    if ( !s_showAdvPatch )
        return;

    // "Advanced Patch Editing Options" = the dialog's CAPTION (res/radiant.rc:709).
    if ( ImGui::Begin( "Advanced Patch Editing Options", &s_showAdvPatch,
                       ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::TextDisabled( "Select the patch(es), then ALT+LEFT-DRAG in the 3D view to paint." );

        // ── KIWI-UX: LIVE READINESS — every gate a stroke passes through, verified
        // here every frame, so "nothing happened" is never a mystery.  These are the
        // same tests sub_401D50 (arm), sub_43E6F0 (mask), and PMESH_16 (strength)
        // apply; a stroke that reaches the patch with all rows green WILL paint.
        {
            extern float sub_401BB0();               // inner radius     (pmesh.cpp)
            extern float sub_401C00();               // outer radius
            extern float sub_401C50();               // strength = 2^(amp-8)
            extern int   OnlyPatchesSelected();      // engine_stubs.cpp 0x447860
            extern int   CurvEditDlg_OnSomeSetting();// "apply to active" (patchdialog.cpp)
            const ImVec4 bad( 1.0f, 0.35f, 0.3f, 1.0f );
            const int  liveMode = AdvPatchEdit_GetMode();
            bool anyChan = false;
            for ( int c = 0; c < 6 && !anyChan; ++c )
                anyChan = AdvPatchEdit_GetChannel( c );
            bool ok = true;
            if ( liveMode == 6 )
            { ok = false; ImGui::TextColored( bad, "BLOCKED: mode is Disabled - pick one below." ); }
            if ( liveMode == 1 )
            { ok = false; ImGui::TextColored( bad, "NOTE: Drag up/down is not an Alt+LMB paint - it"
                                                   " weights vertex drags instead." ); }
            if ( !( sub_401C00() > sub_401BB0() ) )
            { ok = false; ImGui::TextColored( bad, "BLOCKED: outer radius must be > inner radius." ); }
            if ( sub_401C50() == 0.0f )
            { ok = false; ImGui::TextColored( bad, "BLOCKED: amplitude 0 - strokes have no effect." ); }
            if ( !anyChan )
            { ok = false; ImGui::TextColored( bad, "BLOCKED: no channel ticked - strokes touch nothing." ); }
            if ( !OnlyPatchesSelected() && !CurvEditDlg_OnSomeSetting() )
            { ok = false; ImGui::TextColored( bad, "BLOCKED: select ONLY patch(es) (or tick apply-to-"
                                                   "unselected)." ); }
            if ( ok )
            {
                static const char *s_verb[6] =
                    { "RAISE height", "(vertex-drag weighting)", "flatten toward the grabbed value",
                      "SMOOTH (average) - flat areas will not visibly change", "add noise",
                      "grab height/colour (eyedropper)" };
                extern bool AdvPatchEdit_GetLower();   // patchdialog.cpp
                const char *verb = ( liveMode >= 0 && liveMode < 6 ) ? s_verb[liveMode] : "?";
                if ( liveMode == 0 && AdvPatchEdit_GetLower() )
                    verb = "LOWER height";
                ImGui::TextColored( ImVec4( 0.4f, 1.0f, 0.5f, 1.0f ),
                                    "READY - Alt+LMB drag will %s.", verb );
            }
        }

        // ── paint mode radios (1435..1440) — drives sub_401DB0 via the store ───
        // Labels follow sub_43E6F0's switch behaviour (patchdialog.cpp): the operation the
        // stroke performs on the ticked channels.  Mode 1 is labelled for what it really
        // is — the soft-select weighting for ordinary vertex drags, NOT an Alt+LMB paint.
        ImGui::SeparatorText( "Mode" );
        static const struct { int mode; const char *label; } s_modeUi[] = {
            { 0, "Raise / Lower height" },
            { 3, "Smooth (average)" },
            { 2, "Flatten (toward grabbed value)" },
            { 5, "Grab value (eyedropper)" },
            { 4, "Add noise" },
            { 1, "Drag up/down (soft-select vertex drags)" },
            { 6, "Disabled" },
        };
        int mode = AdvPatchEdit_GetMode();
        for ( int i = 0; i < 7; ++i )
        {
            if ( ImGui::RadioButton( s_modeUi[i].label, mode == s_modeUi[i].mode ) )
                AdvPatchEdit_SetMode( s_modeUi[i].mode );
        }
        // KIWI-UX: the Lower toggle replaces the binary's unreachable Alt+RMB lower stroke
        // (Alt+RMB is the mouselook in this shell).  Only mode 0 reads it.
        {
            extern bool AdvPatchEdit_GetLower();          // patchdialog.cpp
            extern void AdvPatchEdit_SetLower( bool );
            ImGui::BeginDisabled( mode != 0 );
            bool lower = AdvPatchEdit_GetLower();
            if ( ImGui::Checkbox( "Lower instead of raise", &lower ) )
                AdvPatchEdit_SetLower( lower );
            ImGui::EndDisabled();
        }

        // ── channel checkboxes (1469..1474) — drives sub_43E6F0's mask via the store ──
        // Panel-order index: 0 Height, 1 Color(all), 2 Blue, 3 Green, 4 Red, 5 Alpha.
        ImGui::SeparatorText( "Channels" );
        static const char *s_chanLbl[6] = { "Height", "Color (all)", "Blue", "Green", "Red", "Alpha" };
        for ( int i = 0; i < 6; ++i )
        {
            bool on = AdvPatchEdit_GetChannel( i );
            if ( ImGui::Checkbox( s_chanLbl[i], &on ) )
                AdvPatchEdit_SetChannel( i, on );
        }

        // ── the 3 param slots ─────────────────────────────────────────────────
        // CurveEdit_Slots() is the table size the getters guard against (pmesh.cpp:4960).
        for ( int slot = 0; slot < CurveEdit_Slots() && slot < 3; ++slot )
            AP_SlotRow( slot );

        // ── the paint target ──────────────────────────────────────────────────
        ImGui::SeparatorText( "Paint target" );

        // The two owner-draw swatches (1462 / 1466), read-only: OnDrawItem
        // (patchdialog.cpp:174-191) fills each from the stored value, the alpha one as a grey.
        float liveRGB[3];
        AP_UnpackColorRef( g_paintColorBGR, liveRGB );
        ImGui::ColorButton( "##swatchColor", ImVec4( liveRGB[0], liveRGB[1], liveRGB[2], 1.0f ),
                            ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip );
        ImGui::SameLine();
        const float liveA = g_paintColorA / 255.0f;
        ImGui::ColorButton( "##swatchAlpha", ImVec4( liveA, liveA, liveA, 1.0f ),
                            ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip );
        ImGui::SameLine();
        ImGui::TextDisabled( "current colour / alpha (statics 1462 / 1466)" );

        // "Color..." (button 1460) — ColorEdit3's own picker replaces CColorDialog, and a
        // committed change IS its OK.  Uint8|DisplayRGB keeps the three fields reading 0..255
        // like the picker's.  The packed value goes in RAW, byte order and all (header note).
        if ( ImGui::ColorEdit3( "Color", s_pickRGB,
                                ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB ) )
            AdvPatchEdit_ApplyPaintColor( AP_PackColorRef( s_pickRGB ) );

        // "Alpha..." (button 1464) — ONE grey slider rather than a colour picker.
        // AdvPatchEdit_ApplyPaintAlpha stores (R+G+B)/3 of whatever COLORREF it is handed
        // (patchdialog.cpp:67-70), so a grey v|v<<8|v<<16 stores (3v)/3 == v exactly, for all
        // 256 values: the slider reaches every alpha the MFC picker could produce, and unlike
        // an RGB picker it never lies about which value will land.  It is also the value the
        // MFC dialog SEEDS its picker with (patchdialog.cpp:169), so the round-trip matches.
        if ( ImGui::SliderInt( "Alpha", &s_pickAlpha, 0, 255 ) )
        {
            const unsigned int a = (unsigned int)s_pickAlpha;
            AdvPatchEdit_ApplyPaintAlpha( (COLORREF)( a | ( a << 8 ) | ( a << 16 ) ) );
        }
    }
    // NO auto-close: this is a modeless TOOL PALETTE (the header's "the panel stays open"
    // divergence). Terrain sculpting happens by Alt+dragging in a VIEW while this palette is
    // up — CloseOnFocusLoss would slam it shut the instant the view took focus, ending the
    // edit mode. It closes only via its own [x] (the OnClose SW_HIDE equivalent).
    ImGui::End();
}
