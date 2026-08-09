// imgui_panel_model.cpp — UI-rework Phase 3: the MODEL REPLACE panel over the
// modeldlg.cpp core action (ModelReplace_Apply → ModelDlg_DoReplace). New KISAK code;
// visible only under -imgui.
//
// Panel semantics vs the MFC CModelDlg popup: the SAME core call its [Replace] handler
// makes, with the same widget reads —
//   * two model SETS, the FROM ("Replace these models:") and TO ("...with one of these
//     (random):") list boxes, label text verbatim from OnCreate (modeldlg.cpp:305-306).
//     Each column carries [Add file...] and [Remove], the same two buttons in the same
//     order (modeldlg.cpp:315-318).
//   * the three class checkboxes, captions verbatim from OnCreate (modeldlg.cpp:322-324)
//     and bits exactly as CModelDlg::OnReplace ORs them (modeldlg.cpp:345-347):
//       "misc_model"  → bit0 (1)
//       "dyn_model"   → bit1 (2)
//       "misc_prefab" → bit2 (4)
//     misc_model starts CHECKED, the BM_SETCHECK OnCreate seeds (modeldlg.cpp:325).
//   * [Replace] → snapshot both sets + the checkbox bits into modelReplaceState_t, then
//     ModelReplace_Apply — the OnReplace order, unchanged (modeldlg.cpp:338-359).  No undo
//     bracket here because the MFC handler opens none either.
//
// !!! WARNING — THE BOUND CORE IS SUSPECTED BROKEN !!!
// ModelDlg_DoReplace (modeldlg.cpp:44-46) pre-saves `selbrush_t *next = b->next->prev;`,
// which on a well-formed list is `b` itself (prev@0x00 / next@0x04, qe3.h:431) while the
// body never unlinks anything — so `b = next` never advances and [Replace] should spin
// forever on the first brush.  This is the top REVIEW entry in RADIANT_KNOWN_ISSUES.md.
// The panel BINDS IT ANYWAY: parity first — the panel must call what the MFC button calls,
// and the fix belongs in the core after IDA verification (0x434EC0), not in a shell file.
// The panel surfaces the risk to the user instead (see the warning line in the Draw).
//
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md):
//   * the panel stays open; hiding it is OnClose's ShowWindow( SW_HIDE ) equivalent
//     (modeldlg.cpp:361-365), so both sets and the checkbox state PERSIST across a
//     hide/show exactly like the modeless MFC popup — no first-open reset latch.
//   * CModelDlg::Toggle's create/show/hide dance (modeldlg.cpp:376-410) collapses into the
//     menu checkbox plus the window's own close box.
//   * the ctrl-id plumbing (IDC_MDL_*, the message map) has no analogue: a button calls its
//     action directly instead of routing an id through ON_BN_CLICKED.
//
// The two SETS live panel-side as std::vector<std::string>: the MFC list boxes are
// user-authored SCRATCH lists, not map data — nothing reads them back out of the map and
// there is no *_Gather for them — so the listbox HWNDs are the storage in the MFC build and
// the vectors are the storage here.  Everything MFC-side that touched them is file-static
// and cannot be bound (MDL_AddFromFile :183, MDL_RemoveSel :238, MDL_SnapshotList :249,
// MDL_FreeSet :269), so their logic is re-expressed over the vectors below — INCLUDING the
// picker's quirks, which are reproduced rather than fixed (see Panel_ModelAddFromFile).
//
// The ONE HWND-adjacent thing in this file is that picker: native common dialogs are
// sanctioned, so it is a raw GetOpenFileNameA (the modeldlg.cpp MDL_AddFromFile pattern,
// as imgui_panel_dynent.cpp does for Browse).  The only core bind is ModelReplace_Apply.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>
#include <cstring>
#include <string>
#include <vector>

// ── modeldlg.cpp bindings ─────────────────────────────────────────────────────
// One [Replace] pass snapshot: the two model-set list boxes and the three class check
// boxes the replace consults.
// MUST MATCH modeldlg.cpp verbatim (shared-header consolidation pending)
struct modelReplaceState_t
{
    const char *const *fromSet;    // IDC_MDL_FROM_LIST contents (binary ctrl 1566)
    int                fromCount;
    const char *const *toSet;      // IDC_MDL_TO_LIST contents   (binary ctrl 1564)
    int                toCount;
    int                classFlags; // IDC_MDL_CHK_MODEL / _DYN / _PREFAB → bit0 / bit1 / bit2
};

// The UI-independent action behind CModelDlg's [Replace] button (modeldlg.cpp:152) — it
// runs ModelDlg_DoReplace (0x434EC0) and ORs the redraw bit.  Not in
// radiant_ui_actions.h yet: it is struct-coupled, like SurfaceDlg_Apply.
extern void ModelReplace_Apply( const modelReplaceState_t &st );        // modeldlg.cpp

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showModel = false;

// The two model sets.  Entries are the bare, lowercased model names the picker produces —
// the same strings the MFC list boxes held (and the same strings MDL_SnapshotList would
// have handed the replace).
static std::vector<std::string> s_fromSet;
static std::vector<std::string> s_toSet;

// Each column's list cursor, -1 = no selection (LB_ERR).  An INDEX is right here, unlike
// the layers panel: these lists are panel-owned and only ever change under the two buttons
// beneath them, so a row cannot move out from under the cursor between frames.
static int s_fromSel = -1;
static int s_toSel   = -1;

// The three class checkboxes.  misc_model starts on, the OnCreate BM_SETCHECK default
// (modeldlg.cpp:325); the other two start off, as CreateWindowEx leaves them.
static bool s_chkModel  = true;
static bool s_chkDyn    = false;
static bool s_chkPrefab = false;

// The picker's filter, as the MFC filter string translates to the raw-Win32 form.  A plain
// string literal would truncate at the first embedded NUL, so it is an explicit char array
// and the literal's own terminator supplies the double NUL (modeldlg.cpp:203).
static const char kModelFilter[] = "Model files (*)\0*\0";

// ── [Add file...] ─────────────────────────────────────────────────────────────
// MDL_AddFromFile (modeldlg.cpp:183-234) re-expressed over the vector: open the picker at
// <project "basepath" value>\main_shared\xmodel\, lowercase + forward-slash the pick,
// relativise it to the xmodel root, then de-dup and append.
//
// TWO QUIRKS ARE REPRODUCED, NOT FIXED:
//   1. the FALLBACK-TO-FULL-PATH: `rel` only advances past "xmodel/" when that substring is
//      actually present (modeldlg.cpp:226-228).  Pick a model from anywhere else and the
//      whole lowercased ABSOLUTE path is what lands in the set — and an entity's "model"
//      key never holds an absolute path, so such an entry can never match anything.
//   2. when the project entity has no "basepath" epair (or there is no project entity)
//      initDir stays EMPTY and lpstrInitialDir goes null, so the picker opens wherever the
//      process CWD happens to be — which is exactly how quirk 1 gets triggered.
static void Panel_ModelAddFromFile( std::vector<std::string> &set )
{
    // Resolve <project basepath>\main_shared\xmodel\ for the initial dir (the binary reads
    // the project entity's "basepath" key; if unavailable the dialog just opens at the CWD).
    char initDir[1024] = { 0 };
    const char *basepath = nullptr;
    if ( g_qeglobals.d_project_entity )
    {
        for ( epair_t *e = g_qeglobals.d_project_entity->epairs; e; e = e->next )
            if ( _stricmp( e->key, "basepath" ) == 0 ) { basepath = e->value; break; }
    }
    if ( basepath && basepath[0] )
        _snprintf( initDir, sizeof( initDir ), "%s\\main_shared\\xmodel\\", basepath );

    char fileBuf[1024] = { 0 };
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize     = sizeof( ofn );
    ofn.hwndOwner       = ::GetActiveWindow();
    ofn.lpstrFilter     = kModelFilter;
    ofn.lpstrFile       = fileBuf;
    ofn.nMaxFile        = sizeof( fileBuf );
    ofn.lpstrInitialDir = ( initDir[0] ? initDir : nullptr );
    ofn.Flags           = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if ( !::GetOpenFileNameA( &ofn ) )
        return;

    // Lowercase + forward-slash, then relativise to the xmodel root (matching
    // CString_MakeLower 0x435390 + the picker's path normalisation).  For the replace match
    // we want the bare model name the entity stores in its "model" key.
    char name[1024];
    strncpy( name, fileBuf, sizeof( name ) - 1 );
    name[sizeof( name ) - 1] = 0;
    for ( char *p = name; *p; ++p )
    {
        if ( *p >= 'A' && *p <= 'Z' ) *p += 32;
        if ( *p == '\\' ) *p = '/';
    }
    const char *rel = name;
    if ( const char *x = strstr( name, "xmodel/" ) )
        rel = x + 7;                                   // strip up to and including "xmodel/"

    // De-dup then add.  LB_FINDSTRINGEXACT (modeldlg.cpp:232) compares case-INSENSITIVELY,
    // so _stricmp is the faithful test — every entry is already lowercased above, which is
    // why the distinction never actually shows.
    for ( size_t i = 0; i < set.size(); ++i )
        if ( _stricmp( set[i].c_str(), rel ) == 0 )
            return;
    set.push_back( rel );
}

// ── [Remove] ──────────────────────────────────────────────────────────────────
// MDL_RemoveSel (modeldlg.cpp:238-245): LB_GETCURSEL → LB_DELETESTRING, a no-op with no
// selection.  A listbox holds no selection after a delete, so the cursor drops to -1 here
// rather than sliding onto the neighbouring row.
static void Panel_ModelRemoveSel( std::vector<std::string> &set, int &sel )
{
    if ( sel < 0 || sel >= (int)set.size() )           // = LB_ERR
        return;
    set.erase( set.begin() + sel );
    sel = -1;
}

// ── one model-set column ──────────────────────────────────────────────────────
// caption + the list + [Add file...] [Remove], the OnCreate column layout
// (modeldlg.cpp:305-318): colW 220 and listH 200, the two buttons on the row below.
static void Panel_ModelSetColumn( const char *id, const char *caption,
                                  std::vector<std::string> &set, int &sel )
{
    ImGui::PushID( id );
    ImGui::BeginGroup();

    ImGui::TextUnformatted( caption );

    if ( ImGui::BeginChild( "##list", ImVec2( 220.0f, 200.0f ), ImGuiChildFlags_Borders ) )
    {
        for ( size_t i = 0; i < set.size(); ++i )
        {
            ImGui::PushID( (int)i );
            if ( ImGui::Selectable( set[i].c_str(), sel == (int)i ) )
                sel = (int)i;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if ( ImGui::Button( "Add file...", ImVec2( 90.0f, 0.0f ) ) )     // = btnW
        Panel_ModelAddFromFile( set );
    ImGui::SameLine();
    if ( ImGui::Button( "Remove", ImVec2( 90.0f, 0.0f ) ) )
        Panel_ModelRemoveSel( set, sel );

    ImGui::EndGroup();
    ImGui::PopID();
}

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_Model_MenuItem()
{
    ImGui::Checkbox( "Model replace", &s_showModel );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 33240, where the MFC handler called CModelDlg::Toggle() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_Model_Toggle()
{
    s_showModel = !s_showModel;
}

void ImGuiPanel_Model_Draw()
{
    if ( !s_showModel )
        return;

    // "Replace Models" = the popup's caption (modeldlg.cpp:403).
    if ( ImGui::Begin( "Replace Models", &s_showModel, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // The replace walks ACTIVE_BRUSHES, not the selection (modeldlg.cpp:44) — it is a
        // map-wide bulk edit with no undo bracket on either shell, so say so.
        ImGui::TextDisabled( "map-wide bulk edit: every matching entity, selected or not" );

        Panel_ModelSetColumn( "from", "Replace these models:", s_fromSet, s_fromSel );
        ImGui::SameLine( 0.0f, 30.0f );                 // = OnCreate's tx gap
        Panel_ModelSetColumn( "to", "...with one of these (random):", s_toSet, s_toSel );

        // The three class checkboxes, laid out on one row as OnCreate does
        // (modeldlg.cpp:322-324).  These ARE checkboxes rather than the vehicle panel's
        // button pairs because the dialog owns their state — nothing is read back out of
        // the map, so the panel can honestly show what the next [Replace] will use.
        ImGui::Checkbox( "misc_model", &s_chkModel );     // bit0
        ImGui::SameLine( 160.0f );
        ImGui::Checkbox( "dyn_model", &s_chkDyn );        // bit1
        ImGui::SameLine( 320.0f );
        ImGui::Checkbox( "misc_prefab", &s_chkPrefab );   // bit2

        // See the file header: the bound core is the suspected infinite loop in
        // RADIANT_KNOWN_ISSUES.md.  Parity says bind it; honesty says warn about it.
        ImGui::TextDisabled( "WARNING: Replace may hang (known issue in ModelDlg_DoReplace)" );

        if ( ImGui::Button( "Replace", ImVec2( 120.0f, 0.0f ) ) )
        {
            // OnReplace's order, unchanged (modeldlg.cpp:338-359): snapshot both lists,
            // then the three BM_GETCHECK bits, then run the action.  The const char*
            // arrays stand in for MDL_SnapshotList's malloc'd copies — the strings outlive
            // the call, so there is no MDL_FreeSet tail to mirror.
            std::vector<const char *> fromPtrs;
            std::vector<const char *> toPtrs;
            fromPtrs.reserve( s_fromSet.size() );
            toPtrs.reserve( s_toSet.size() );
            for ( size_t i = 0; i < s_fromSet.size(); ++i )
                fromPtrs.push_back( s_fromSet[i].c_str() );
            for ( size_t i = 0; i < s_toSet.size(); ++i )
                toPtrs.push_back( s_toSet[i].c_str() );

            int flags = 0;
            if ( s_chkModel )  flags |= 1;
            if ( s_chkDyn )    flags |= 2;
            if ( s_chkPrefab ) flags |= 4;

            // An empty set gives count 0 and a null pointer, exactly what MDL_SnapshotList
            // returns for an empty listbox — ModelDlg_DoReplace bails on either count <= 0
            // before touching the arrays (modeldlg.cpp:34).
            modelReplaceState_t st;
            st.fromSet    = fromPtrs.empty() ? nullptr : fromPtrs.data();
            st.fromCount  = (int)fromPtrs.size();
            st.toSet      = toPtrs.empty() ? nullptr : toPtrs.data();
            st.toCount    = (int)toPtrs.size();
            st.classFlags = flags;
            ModelReplace_Apply( st );
        }
    }
    ImGuiShell_CloseOnFocusLoss( &s_showModel );
    ImGui::End();
}
