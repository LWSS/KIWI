// imgui_panel_dynent.cpp — UI-rework Phase 3: the DYNAMIC ENTITY panel over the Phase-1
// action functions in dynentitydlg.cpp (declared in radiant_ui_actions.h). New KISAK
// code; visible only under -imgui.
//
// Panel semantics vs the MFC CDynEntityDlg popup: the SAME core calls the MFC handlers
// make, with the same widget reads —
//   * "type" row   : the combo seeded with dyn_model's two presets ("clutter"/"destruct",
//                    OnCreate — dynentitydlg.cpp:269-270) + [Set] → DynEntSetType_Apply
//                    (OnSetType) and [Clear] → DynEntClearKey_Apply( "type" ) (OnClearType).
//                    NOTE the deliberate asymmetry documented at dynentitydlg.cpp:144-149:
//                    DynEntSetType_Apply ALWAYS SetPairs — it has no empty→RemovePair
//                    branch like DynEntSetKey_Apply does — which is why OnSetType refuses
//                    an empty type instead of letting it through as a clear.  Same refusal
//                    here (message inline, see below).
//   * the four edit rows, with the exact keys the MFC Set/Clear handlers use
//     (dynentitydlg.cpp:348-357): "health", "physPreset", "destroyEfx", "destroyPieces".
//     [Set] → DynEntSetKey_Apply( value, key ) — the empty-value→RemovePair decision lives
//     INSIDE the action (dynentitydlg.cpp:128-135), so the button needs no guard of its
//     own, exactly like DE_Commit.  [Clear] → DynEntClearKey_Apply( key ).
//   * [Browse] on the three file-backed keys → the relative path into the row's field,
//     then that row's Set — the OnBrowsePhys/Efx/Pieces order (dynentitydlg.cpp:408-425).
//   * [Help] → DynEntHelp_Gather() (OnHelp).
//
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md):
//   * the panel stays open; hiding it is the OnClose ShowWindow( SW_HIDE ) equivalent, so
//     the field contents PERSIST across a hide/show exactly like the modeless MFC popup
//     (only the transient help/message block is dropped on re-open).
//   * OnHelp's modal MessageBoxA becomes a collapsible text block in the panel — the
//     comments pointer is the eclass's QUAKED doc text and can be long.  DynEntHelp_Gather
//     returns NULL when the selection has no dyn_ entity with eclass comments; that case
//     gets its own "no help" line rather than OnHelp's silent do-nothing.
//   * OnSetType's "Type is not selected" MessageBoxA and DynEntityDlg_OpenDialog's "File
//     must be under [...]" MessageBoxA become the same message text shown inline.  The
//     TEXT and the reject DECISION are unchanged — only the presentation.
//
// The ONE HWND-adjacent thing in this file is the Browse picker: native common dialogs
// are sanctioned, so it is a raw GetOpenFileNameA (the modeldlg.cpp MDL_AddFromFile
// pattern) instead of the MFC CFileDialog.  Everything else is a radiant_ui_actions.h
// call.  DynEntityDlg_OpenDialog (dynentitydlg.cpp:365) is file-static and cannot be
// bound, so its basepath/prefix logic is re-expressed here — INCLUDING its quirks, which
// are reproduced rather than fixed; see Panel_BrowseRelative.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>
#include <cstring>
#include <string>

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showDynEnt = false;

// The dialog's five input controls.  The type combo's buffer is 256 bytes like OnSetType's
// CB_GETLBTEXT/GetWindowTextA buffer (dynentitydlg.cpp:326); the four edits are 1024 bytes
// like DE_Commit's (dynentitydlg.cpp:316) — ImGui reserves the NUL, so the same usable
// capacity as the ::GetWindowTextA( ..., sizeof( buf ) - 1 ) reads.
static char s_type[256]     = { 0 };
static char s_health[1024]  = { 0 };
static char s_phys[1024]    = { 0 };
static char s_efx[1024]     = { 0 };
static char s_pieces[1024]  = { 0 };

// The two message-box texts, shown inline instead (see the header comment).  Cleared at
// the start of every Set/Clear/Browse so a stale refusal never outlives its click.
static char s_message[1024] = { 0 };

// The [Help] result: whether Help has been pressed (a press with no result is the "no
// help" case, which must be distinguishable from "not pressed yet") and the comments text,
// COPIED out — the eclass owns the pointer and the panel re-draws every frame.
static bool        s_helpAsked = false;
static std::string s_help;

static bool s_opened = false;            // first-open latch

// dyn_model's "type" presets, verbatim in OnCreate's CB_ADDSTRING order
// (dynentitydlg.cpp:269-270).  The MFC control is a CBS_DROPDOWN, so a typed value is
// allowed too (dynentitydlg.cpp:331-333) — hence an edit field plus this preset list,
// not a closed ImGui::Combo.
static const char *const kTypePreset[2] = { "clutter", "destruct" };

// The Browse filters, as the MFC filter strings translate to the raw-Win32 form.  A plain
// string literal would truncate at the first embedded NUL, so they are explicit char
// arrays and the literal's own terminator supplies the double NUL (modeldlg.cpp:203).
static const char kFilterAll[] = "All files (*.*)\0*.*\0";      // "All files (*.*)|*.*||"
static const char kFilterEfx[] = "EFX files (*.efx)\0*.efx\0";  // "EFX files (*.efx)|*.efx||"

// Row label column, so the five rows line up like the MFC lblW/inW grid
// (dynentitydlg.cpp:254).
static const float kLabelW = 96.0f;
static const float kInputW = 220.0f;

static void Panel_RowLabel( const char *text )
{
    ImGui::TextUnformatted( text );
    ImGui::SameLine( kLabelW );
}

// ── the Browse picker ─────────────────────────────────────────────────────────
// DynEntityDlg_OpenDialog (dynentitydlg.cpp:365-406) re-expressed over GetOpenFileNameA:
// pick a file under <project "basepath" value>\<subdir>; if the pick is NOT under that
// directory refuse it with "File must be under [<dir>]"; else strip the prefix and put the
// relative path into `field`.  Returns true on a kept pick.
//
// TWO QUIRKS ARE REPRODUCED, NOT FIXED:
//   1. when the project entity has no "basepath" epair (or there is no project entity at
//      all) baseDir stays EMPTY, so the prefix degrades to the bare RELATIVE subdir —
//      lpstrInitialDir gets a relative path, and the _strnicmp test against an absolute
//      picked path can then never match, so Browse rejects every pick.
//   2. the MFC version skips its MessageBoxA entirely when `parent` is null
//      (dynentitydlg.cpp:396), i.e. the refusal is silent.  The panel has no parent CWnd
//      at all, so its refusal is reported through s_message — the DECISION is the binary's
//      either way; only the reporting differs.
static bool Panel_BrowseRelative( const char *subdir, const char *filter,
                                  char *field, size_t fieldSz )
{
    // <project basepath> with a trailing '\' appended when it lacks one — the
    // baseDir.IsEmpty() guard means an absent basepath appends nothing (quirk 1).
    char baseDir[1024] = { 0 };
    const char *basepath = nullptr;
    if ( g_qeglobals.d_project_entity )
    {
        for ( epair_t *ep = g_qeglobals.d_project_entity->epairs; ep; ep = ep->next )
            if ( _stricmp( ep->key, "basepath" ) == 0 ) { basepath = ep->value; break; }
    }
    if ( basepath && basepath[0] )
    {
        strncpy( baseDir, basepath, sizeof( baseDir ) - 2 );
        baseDir[sizeof( baseDir ) - 2] = 0;
        size_t n = strlen( baseDir );
        if ( n && baseDir[n - 1] != '\\' )
        {
            baseDir[n]     = '\\';
            baseDir[n + 1] = 0;
        }
    }

    char initialDir[1024];
    _snprintf( initialDir, sizeof( initialDir ), "%s%s", baseDir, subdir );
    initialDir[sizeof( initialDir ) - 1] = 0;

    char fileBuf[1024] = { 0 };
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize     = sizeof( ofn );
    ofn.hwndOwner       = ::GetActiveWindow();
    ofn.lpstrFilter     = filter;
    ofn.lpstrFile       = fileBuf;
    ofn.nMaxFile        = sizeof( fileBuf );
    ofn.lpstrInitialDir = initialDir;                 // relative when basepath is missing
    ofn.Flags           = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST;
    if ( !::GetOpenFileNameA( &ofn ) )                // = dlg.DoModal() != IDOK
        return false;

    // Case-insensitive prefix test over the WHOLE initial dir (the binary's
    // _strnicmp( picked, base + sub, strlen( base + sub ) )).
    const size_t prefixLen = strlen( initialDir );
    if ( _strnicmp( fileBuf, initialDir, prefixLen ) != 0 )
    {
        _snprintf( s_message, sizeof( s_message ),
                   "Could not complete operation.\n\nFile must be under [%s]", initialDir );
        s_message[sizeof( s_message ) - 1] = 0;
        return false;
    }

    // Strip the prefix → the relative path the key stores (the binary's Mid +
    // SetWindowText into the row's edit control).
    strncpy( field, fileBuf + prefixLen, fieldSz - 1 );
    field[fieldSz - 1] = 0;
    return true;
}

// ── one edit row: label + edit + [Set] [Clear] (+ [Browse]) ───────────────────
// `browseSubdir` null = a row with no Browse button (health).  Enter in the field is the
// IDOK-ish commit, matching the other panels' EnterReturnsTrue rows.
static void Panel_KeyRow( const char *label, const char *key, char *buf, size_t bufSz,
                          const char *browseSubdir, const char *browseFilter )
{
    ImGui::PushID( key );
    Panel_RowLabel( label );

    ImGui::SetNextItemWidth( kInputW );
    bool set = ImGui::InputText( "##value", buf, bufSz, ImGuiInputTextFlags_EnterReturnsTrue );

    ImGui::SameLine();
    set |= ImGui::Button( "Set" );
    if ( set )
    {
        s_message[0] = 0;
        DynEntSetKey_Apply( buf, key );          // = DE_Commit's tail (empty → RemovePair inside)
    }

    ImGui::SameLine();
    if ( ImGui::Button( "Clear" ) )
    {
        s_message[0] = 0;
        DynEntClearKey_Apply( key );             // = OnClear<Row>
    }

    if ( browseSubdir )
    {
        ImGui::SameLine();
        if ( ImGui::Button( "Browse" ) )
        {
            s_message[0] = 0;
            // OnBrowse<Row>: a kept pick lands in the field AND is committed straight
            // away (the handlers call OnSet<Row> on true).
            if ( Panel_BrowseRelative( browseSubdir, browseFilter, buf, bufSz ) )
                DynEntSetKey_Apply( buf, key );
        }
    }
    ImGui::PopID();
}

// ── the "type" row ────────────────────────────────────────────────────────────
static void Panel_DrawTypeRow()
{
    ImGui::PushID( "type" );
    Panel_RowLabel( "type:" );

    // CBS_DROPDOWN = an edit plus a preset list, so both the typed and the picked value
    // reach DynEntSetType_Apply (OnSetType's CB_GETLBTEXT arm and its GetWindowTextA
    // fallback arm respectively).
    ImGui::SetNextItemWidth( kInputW - ImGui::GetFrameHeight() );
    bool set = ImGui::InputText( "##type", s_type, sizeof( s_type ),
                                 ImGuiInputTextFlags_EnterReturnsTrue );
    ImGui::SameLine( 0.0f, 0.0f );
    if ( ImGui::BeginCombo( "##typepreset", "", ImGuiComboFlags_NoPreview ) )
    {
        for ( int i = 0; i < 2; ++i )
        {
            if ( ImGui::Selectable( kTypePreset[i], strcmp( s_type, kTypePreset[i] ) == 0 ) )
            {
                strncpy( s_type, kTypePreset[i], sizeof( s_type ) - 1 );
                s_type[sizeof( s_type ) - 1] = 0;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    set |= ImGui::Button( "Set" );
    if ( set )
    {
        s_message[0] = 0;
        if ( !s_type[0] )
        {
            // OnSetType's refusal, verbatim text.  DynEntSetType_Apply has no
            // empty→RemovePair branch, so without this an empty field would SET type=""
            // instead of clearing it — the asymmetry the dialog's header comments call out.
            strncpy( s_message, "Could not complete operation.\n\nType is not selected",
                     sizeof( s_message ) - 1 );
            s_message[sizeof( s_message ) - 1] = 0;
        }
        else
        {
            DynEntSetType_Apply( s_type );
        }
    }

    ImGui::SameLine();
    if ( ImGui::Button( "Clear" ) )
    {
        s_message[0] = 0;
        DynEntClearKey_Apply( "type" );          // = OnClearType
    }
    ImGui::PopID();
}

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_DynEnt_MenuItem()
{
    ImGui::Checkbox( "Dynamic entity", &s_showDynEnt );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 36106, where the MFC handler called CDynEntityDlg::Toggle() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_DynEnt_Toggle()
{
    s_showDynEnt = !s_showDynEnt;
}

void ImGuiPanel_DynEnt_Draw()
{
    if ( !s_showDynEnt )
    {
        s_opened = false;
        return;
    }

    if ( !s_opened )
    {
        // OnCreate has no field pre-fill (it only builds the controls and seeds the combo
        // with the two presets, which kTypePreset does statically), and OnClose HIDES
        // rather than destroys — so the five fields deliberately keep their contents here.
        // Only the transient help/message block is dropped.
        s_opened    = true;
        s_helpAsked = false;
        s_help.clear();
        s_message[0] = 0;
    }

    if ( ImGui::Begin( "Dyn Entities", &s_showDynEnt, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // Every action walks selected_brushes and touches only the non-world owners whose
        // eclass name starts with "dyn_" (DynEnt_SelectedDef), so an unrelated selection is
        // a silent no-op INSIDE the action — no gating here, just the hint.
        ImGui::TextDisabled( "acts on the selected dyn_ entities" );

        Panel_DrawTypeRow();
        Panel_KeyRow( "health:",        "health",        s_health, sizeof( s_health ),
                      nullptr, nullptr );
        Panel_KeyRow( "physPreset:",    "physPreset",    s_phys,   sizeof( s_phys ),
                      "main_shared\\physic\\",      kFilterAll );
        Panel_KeyRow( "destroyEfx:",    "destroyEfx",    s_efx,    sizeof( s_efx ),
                      "raw_shared\\fx\\",           kFilterEfx );
        Panel_KeyRow( "destroyPieces:", "destroyPieces", s_pieces, sizeof( s_pieces ),
                      "main_shared\\xmodelpieces\\", kFilterAll );

        if ( ImGui::Button( "Help" ) )
        {
            // OnHelp: the first selected dyn_ entity's eclass comments, or NULL.
            s_message[0] = 0;
            s_helpAsked  = true;
            const char *comments = DynEntHelp_Gather();
            s_help = comments ? comments : "";
        }

        if ( s_message[0] )
        {
            ImGui::Separator();
            ImGui::TextWrapped( "%s", s_message );   // the two MessageBoxA texts, inline
            if ( ImGui::SmallButton( "OK" ) )
                s_message[0] = 0;
        }

        if ( s_helpAsked )
        {
            if ( ImGui::CollapsingHeader( "Help", ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                if ( s_help.empty() )
                    ImGui::TextDisabled( "(no help: nothing selected carries a dyn_ eclass"
                                         " with comments)" );
                else
                    ImGui::TextWrapped( "%s", s_help.c_str() );
            }
        }
    }
    ImGuiShell_CloseOnFocusLoss( &s_showDynEnt );
    ImGui::End();
}
