// imgui_panel_entity.cpp — UI-rework Phase 3: the ENTITY INSPECTOR panel over the
// Phase-1 action/read functions in win_ent.cpp (declared in radiant_ui_actions.h and
// as externs below). New KISAK code; visible only under -imgui.
//
// Panel semantics vs the MFC CEntityWnd entity pane: the SAME core calls in the SAME
// order as the MFC handlers —
//   * eclass list  : LBN_SELCHANGE → EclassSelect_Apply( index, eclass )   (OnEclassSelChange)
//                    LBN_DBLCLK    → EclassCreate_Apply( name )            (OnEclassDblClk /
//                                                                          CreateEntity)
//   * key/values   : LBN_SELCHANGE → row into the key/value fields         (OnKVSelChange /
//                                                                          EditProp)
//                    Enter/commit  → EntSetKey_Apply( key, value )         (AddProp)
//                    "Delete Key"  → EntDeleteKey_Apply( key )             (OnDeleteKey / DelProp)
//   * spawnflags   : any box toggled → OR the 12 states → SpawnFlags_Apply  (OnSpawnFlagCheck /
//                                                                          SetSpawnFlags_2)
//   * angle grid   : button i → EntAngle_Apply( i )                        (OnAngleButton)
// The panel-flow differences (the panel stays open, the lists are re-gathered every
// frame instead of being push-populated by UpdateSelection, and the eclass filter box)
// are the sanctioned Phase-3 divergences noted in RADIANT_UI_REWORK_PLAN.md.
//
// NO HWND/MFC anywhere in this file: every read is a *_Gather and every write a *_Apply.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>
#include <string>
#include <vector>

// ── win_ent.cpp bindings ──────────────────────────────────────────────────────
// The entity-window globals the inspector edits (win_ent.cpp:83-84).  mainfrm.cpp
// declares edit_entity the same way — they are not in a shared header yet.
extern entity_s_def *edit_entity;              // win_ent.cpp (0x240A108)
extern int           multiple_edit_entities;   // win_ent.cpp (0x240A10C)

// One row of the eclass list: the name the row is labelled with and the eclass_t* the row
// carries as its item data (read back by the selchange / create paths).
// MUST MATCH win_ent.cpp verbatim (shared-header consolidation pending)
struct eclassRow_t
{
    const char *name;
    eclass_t   *eclass;
};

// One key/value row of the inspector's key/value list: the epair's key + value.  The
// synthesised "origin" row carries the def's origin vec3, pre-formatted, as its value.
// MUST MATCH win_ent.cpp verbatim (shared-header consolidation pending)
struct entKvRow_t
{
    std::string key;
    std::string value;
};

// The eclass-driven inspector fields: the description-box text and the first 8 spawnflag
// checkbox labels (an empty name means that box is blanked + disabled).
// MUST MATCH win_ent.cpp verbatim (shared-header consolidation pending)
struct entEclassInfo_t
{
    const char *comment;
    const char *flagname[8];
};

extern void EclassList_Gather( std::vector<eclassRow_t> &rows );                      // win_ent.cpp
extern void EntityKeyValues_Gather( entity_s_def *def, std::vector<entKvRow_t> &rows );// win_ent.cpp
extern int  SpawnFlags_Gather();                                                       // win_ent.cpp
extern void EclassInfo_Gather( eclass_t *cls, entEclassInfo_t &out );                  // win_ent.cpp
extern void EclassCreate_Apply( const char *name );                                    // win_ent.cpp
extern void EclassSelect_Apply( int listIndex, eclass_t *pec );                        // win_ent.cpp
// KIWI-UX (ROUND AU): the selection -> edit_entity refresh, as mainfrm.cpp:56
// declares it.  win_ent.cpp:800  void Entity_UpdateSelection()  ( = UpdateSelection( -1, NULL ) ).
extern void Entity_UpdateSelection();                                                  // win_ent.cpp:800
// KIWI-UX (ROUND AU): the dock-tab raise N uses, declared at FILE scope exactly as
// mainfrm.cpp:3054 declares it.  imgui_shell.cpp:133  void ImGuiShell_FocusTab( const char * ).
extern void ImGuiShell_FocusTab( const char *title );                                  // imgui_shell.cpp:201
// EntSetKey_Apply / EntDeleteKey_Apply / SpawnFlags_Apply / EntAngle_Apply come from
// radiant_ui_actions.h.

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showEntity = false;

// KIWI-UX (ROUND AU): "was this window the focused surface on the frame it was
// last drawn".  Recorded in Draw, read by Toggle — see ImGuiPanel_Entity_Toggle
// for why N cannot be a blind boolean flip once the panel is a DOCK TAB.
static bool s_entityFocused = false;

// The eclass list cursor, as an index into the FULL EclassList_Gather order (that is
// what EclassSelect_Apply forwards to UpdateSelection as the listbox index), plus a
// name filter over the list (a panel-only convenience — the eclass list is long).
static int  s_selEclass  = -1;
static char s_eclassFilter[64] = { 0 };

// The key/value list cursor + the two edit fields.  4096 bytes each: the MFC fields are
// read with WM_GETTEXT(0xFFF) into 4096-byte buffers (win_ent.cpp:319-321).
static int  s_selKv      = -1;
static char s_key[4096]  = { 0 };
static char s_value[4096]= { 0 };

// The last-4 spawnflag labels the eclass never names (win_ent.cpp:1186-1187's
// kCheckDefault tail — the difficulty/gametype flags).
static const char *const kCheckDefaultTail[4] = { "Easy", "Medium", "Hard", "Deathmatch" };

// The 10 angle/direction button labels, in ENTITY_DEFINES order (win_ent.cpp:1194-1195);
// the index IS the EntAngle_Apply switch case (win_ent.cpp:1603-1631).
static const char *const kDirLabel[10] =
    { "E", "NE", "N", "NW", "W", "SW", "S", "SE", "Up", "Dn" };

static void Panel_CopyField( char *dst, size_t dstSz, const char *src )
{
    dst[0] = '\0';
    if ( src )
    {
        strncpy( dst, src, dstSz - 1 );
        dst[dstSz - 1] = '\0';
    }
}

// Case-insensitive substring test for the eclass filter box (panel-only).  ASCII fold
// done by hand so this needs no header beyond the contract's include list.
static char Panel_LowerAscii( char c )
{
    return ( c >= 'A' && c <= 'Z' ) ? (char)( c + ( 'a' - 'A' ) ) : c;
}

static bool Panel_ContainsNoCase( const char *hay, const char *needle )
{
    if ( !needle || !needle[0] )
        return true;
    if ( !hay )
        return false;
    for ( const char *h = hay; *h; ++h )
    {
        const char *a = h, *b = needle;
        while ( *a && *b && Panel_LowerAscii( *a ) == Panel_LowerAscii( *b ) )
            ++a, ++b;
        if ( !*b )
            return true;
    }
    return false;
}

// ── the eclass list ───────────────────────────────────────────────────────────
// Single click = the MFC LBN_SELCHANGE arm, double click = the LBN_DBLCLK arm.  A
// double click fires both (the click, then the double) — exactly the MFC notification
// order (SELCHANGE then DBLCLK).
static void Panel_DrawEclassList( std::vector<eclassRow_t> &rows )
{
    ImGui::SeparatorText( "Entity class" );

    ImGui::SetNextItemWidth( -1.0f );
    ImGui::InputTextWithHint( "##eclassfilter", "filter...", s_eclassFilter, sizeof( s_eclassFilter ) );

    if ( ImGui::BeginChild( "##eclasslist", ImVec2( 0.0f, 160.0f ), ImGuiChildFlags_Borders ) )
    {
        for ( size_t i = 0; i < rows.size(); ++i )
        {
            if ( !Panel_ContainsNoCase( rows[i].name, s_eclassFilter ) )
                continue;

            ImGui::PushID( (int)i );
            if ( ImGui::Selectable( rows[i].name ? rows[i].name : "(unnamed)",
                                    s_selEclass == (int)i,
                                    ImGuiSelectableFlags_AllowDoubleClick ) )
            {
                s_selEclass = (int)i;
                EclassSelect_Apply( (int)i, rows[i].eclass );   // = OnEclassSelChange
                if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                    EclassCreate_Apply( rows[i].name );         // = OnEclassDblClk → CreateEntity
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled( "single click: select class   double click: create entity" );
}

// The description box + the eclass flag names (UpdateSelection's comments + flag walk).
// Returns true when `out` was filled.  EclassInfo_Gather feeds cls->comments straight
// into TranslateString, which asserts on NULL and returns a SHARED STATIC buffer, so the
// comment is guarded here and copied out before anything else can call it.
static bool Panel_GatherEclassInfo( eclass_t *cls, entEclassInfo_t &out, std::string &commentOut )
{
    commentOut.clear();
    if ( !cls || !cls->comments )
        return false;

    EclassInfo_Gather( cls, out );
    if ( out.comment )
        commentOut = out.comment;
    return true;
}

// ── the key/value grid ────────────────────────────────────────────────────────
static void Panel_DrawKeyValues()
{
    ImGui::SeparatorText( "Key / value" );

    std::vector<entKvRow_t> rows;
    EntityKeyValues_Gather( edit_entity, rows );
    if ( s_selKv >= (int)rows.size() )       // the pair list shrank (delete / new selection)
        s_selKv = -1;

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if ( ImGui::BeginTable( "##entkv", 2, flags, ImVec2( 0.0f, 160.0f ) ) )
    {
        ImGui::TableSetupColumn( "Key" );
        ImGui::TableSetupColumn( "Value" );
        ImGui::TableSetupScrollFreeze( 0, 1 );
        ImGui::TableHeadersRow();

        for ( size_t i = 0; i < rows.size(); ++i )
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            ImGui::PushID( (int)i );
            // = OnKVSelChange / EditProp: the picked pair lands in the two edit fields.
            if ( ImGui::Selectable( rows[i].key.c_str(), s_selKv == (int)i,
                                    ImGuiSelectableFlags_SpanAllColumns ) )
            {
                s_selKv = (int)i;
                Panel_CopyField( s_key,   sizeof( s_key ),   rows[i].key.c_str()   );
                Panel_CopyField( s_value, sizeof( s_value ), rows[i].value.c_str() );
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex( 1 );
            ImGui::TextUnformatted( rows[i].value.c_str() );
        }
        ImGui::EndTable();
    }

    ImGui::SetNextItemWidth( -1.0f );
    bool commit = ImGui::InputText( "Key",   s_key,   sizeof( s_key ),
                                    ImGuiInputTextFlags_EnterReturnsTrue );
    ImGui::SetNextItemWidth( -1.0f );
    commit |= ImGui::InputText( "Value", s_value, sizeof( s_value ),
                                ImGuiInputTextFlags_EnterReturnsTrue );

    // Enter in either field = the MFC FieldWndProc's Enter→AddProp path.
    if ( ImGui::Button( "Set" ) || commit )
        EntSetKey_Apply( s_key, s_value );          // = AddProp
    ImGui::SameLine();
    if ( ImGui::Button( "Delete Key" ) )
        EntDeleteKey_Apply( s_key );                // = OnDeleteKey → DelProp
}

// ── spawnflags: the 12 checkboxes ─────────────────────────────────────────────
// Seeded from SpawnFlags_Gather every frame (the panel has no push-refresh hook), then
// any toggle rebuilds all 12 bits and writes them — SetSpawnFlags_2's whole-int rebuild.
// SpawnFlags_Apply has NO !edit_entity guard (faithful; see radiant_ui_actions.h:53), so
// the call is gated here.
static void Panel_DrawSpawnFlags( const entEclassInfo_t &info, bool haveInfo )
{
    ImGui::SeparatorText( "Spawnflags" );

    const int flags0 = SpawnFlags_Gather();
    bool      box[12];
    for ( int i = 0; i < 12; ++i )
        box[i] = ( flags0 & ( 1 << i ) ) != 0;

    // Two columns of six, the MFC pane's flag grid (win_ent.cpp:1303-1307).  The column
    // offset is taken BEFORE any item is submitted (GetContentRegionAvail shrinks as the
    // row fills).
    const float halfW  = ImGui::GetContentRegionAvail().x * 0.5f;
    bool        changed = false;
    for ( int slot = 0; slot < 12; ++slot )
    {
        // MFC places box i at column i/6, row i%6; ImGui submits left-to-right, so the
        // slot order interleaves the two columns (0,6,1,7,...) to land the same grid.
        const int i = ( slot & 1 ) ? ( slot / 2 + 6 ) : ( slot / 2 );

        // Label from the eclass flag names for the first 8 (UpdateSelection's labelling);
        // an unnamed box is DISABLED there, so it is disabled here too.  The last 4 keep
        // the static difficulty/gametype labels the eclass never names.
        char        generic[32];
        const char *label   = nullptr;
        bool        disable = false;
        if ( i < 8 )
        {
            const char *flagname = haveInfo ? info.flagname[i] : nullptr;
            if ( flagname && *flagname )
            {
                label = flagname;
            }
            else
            {
                sprintf( generic, "flag %i", i );
                label   = generic;
                disable = haveInfo;
            }
        }
        else
        {
            label = kCheckDefaultTail[i - 8];
        }

        if ( slot & 1 )
            ImGui::SameLine( halfW );

        ImGui::PushID( i );
        ImGui::BeginDisabled( disable );
        if ( ImGui::Checkbox( label, &box[i] ) )
            changed = true;
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if ( changed && edit_entity )
    {
        int flags = 0;
        for ( int i = 0; i < 12; ++i )
            flags |= (int)box[i] << i;              // = SetSpawnFlags_2's OR of the 12 boxes
        SpawnFlags_Apply( flags );
    }
}

// ── the angle / direction grid ────────────────────────────────────────────────
// The compass cell layout is the MFC one (win_ent.cpp:1310-1315): N up, E right —
//   NW(3) N(2) NE(1) / W(4) · E(0) / SW(5) S(6) SE(7), with Up(8)/Dn(9) in a 4th column.
static void Panel_AngleButton( int idx, const ImVec2 &size )
{
    ImGui::PushID( idx );
    if ( ImGui::Button( kDirLabel[idx], size ) )
        EntAngle_Apply( idx );                      // = OnAngleButton
    ImGui::PopID();
}

static void Panel_DrawAngles()
{
    ImGui::SeparatorText( "Angle" );

    const float   side = ImGui::GetFrameHeight() * 1.4f;
    const ImVec2  sz( side, side );

    // Cases 8/9 read edit_entity's "angles" key directly (win_ent.cpp:1618/1625) and the
    // shared tail refreshes the key/value list from it — the grid is inert without an
    // edited entity, so it is disabled rather than left to deref null.
    ImGui::BeginDisabled( edit_entity == nullptr );
    Panel_AngleButton( 3, sz ); ImGui::SameLine(); Panel_AngleButton( 2, sz ); ImGui::SameLine();
    Panel_AngleButton( 1, sz ); ImGui::SameLine(); ImGui::Dummy( ImVec2( side * 0.4f, side ) );
    ImGui::SameLine(); Panel_AngleButton( 8, sz );

    Panel_AngleButton( 4, sz ); ImGui::SameLine(); ImGui::Dummy( sz ); ImGui::SameLine();
    Panel_AngleButton( 0, sz );

    Panel_AngleButton( 5, sz ); ImGui::SameLine(); Panel_AngleButton( 6, sz ); ImGui::SameLine();
    Panel_AngleButton( 7, sz ); ImGui::SameLine(); ImGui::Dummy( ImVec2( side * 0.4f, side ) );
    ImGui::SameLine(); Panel_AngleButton( 9, sz );
    ImGui::EndDisabled();
}

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_Entity_MenuItem()
{
    ImGui::Checkbox( "Entity inspector", &s_showEntity );
}

// U-RIP seam: Edit→Entity Info (cmd 32787, was the MFC entity-list browser) routes here
// until the entity-list dock tab lands — same show/hide flip the sibling toggles use.
//
// ── KIWI-UX (ROUND AU): N MUST BRING IT TO THE FRONT, NOT HIDE IT ────────────
// USER DIRECTIVE: "make sure the legacy entity inspector (N) bind works so we can
// change their properties."  N is `{ "ViewEntityInfo", 0x4E, 0, 33017 }`
// (mainfrm.cpp:1140) in the DEFAULT table and kiwi_keymap.cpp's modern profile
// never touches vk 0x4E, so the BINDING was always live — it reached
// Cmd_OnViewEntity (mainfrm.cpp:3055) and flipped this bool.  What a blind flip
// cannot express is the state this panel is now in: round AU docks it as a TAB
// beside Textures / Entities (ImGuiShell_BuildDefaultDockLayout), so "open" and
// "visible" are different things — pressing N while it sat BEHIND the Textures tab
// closed a window the user could not see.
//
// The rule is the one every editor uses for a panel key: N brings it up and
// focuses it; N again, while it IS the focused surface, puts it away.  The focus
// question is answered by the panel's own last draw (s_entityFocused) rather than
// guessed, and the raise goes through ImGuiShell_FocusTab, which is the same
// SetWindowFocus the O / texture-view keys already use (imgui_shell.cpp:133).
void ImGuiPanel_Entity_Toggle()
{
    if ( s_showEntity && s_entityFocused )
    {
        s_showEntity = false;
        return;
    }
    s_showEntity = true;
    ImGuiShell_FocusTab( "Entity inspector" );
}

void ImGuiPanel_Entity_Draw()
{
    // KIWI-UX (ROUND AU): the focus latch N reads.  Cleared BEFORE every early-out
    // and before Begin, so "closed" and "collapsed / behind another tab" both read
    // as unfocused and N raises rather than hides.
    const bool wasShown = s_showEntity;
    s_entityFocused = false;
    if ( !wasShown )
        return;

    if ( ImGui::Begin( "Entity inspector", &s_showEntity ) )
    {
        // RootAndChildWindows so a click in one of the fields still counts as
        // "this panel is the surface".
        s_entityFocused = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );

        // ── KIWI-UX (ROUND AU): RE-DERIVE, DO NOT TRUST THE CACHE ───────────
        // Round AU made UpdateSelection publish edit_entity in this shell (see
        // win_ent.cpp), so the two selection funnels — Brush_Select_Helper's
        // splice (brush.cpp:947) and Brush_RemoveFromList (:980) — keep it live
        // through every ordinary select / deselect.  A MAP LOAD is the one path
        // that frees entity defs WITHOUT going through them (Map_Free ->
        // Map_New, entity.cpp:1824), and a pointer into a freed def is a crash
        // rather than a wrong readout.  One call per drawn frame re-derives it
        // from selected_brushes + world_entity, both of which the load rebuilds
        // — and it is nearly free: with no entity listbox in this shell
        // UpdateSelection returns immediately after publishing those two globals.
        Entity_UpdateSelection();     // win_ent.cpp:800 — UpdateSelection( -1, NULL )

        std::vector<eclassRow_t> rows;
        EclassList_Gather( rows );
        if ( s_selEclass >= (int)rows.size() )      // the list was refilled (map load)
            s_selEclass = -1;

        Panel_DrawEclassList( rows );

        entEclassInfo_t info      = { nullptr, { nullptr } };
        std::string     comment;
        const bool      haveInfo  = ( s_selEclass >= 0 )
                                  ? Panel_GatherEclassInfo( rows[s_selEclass].eclass, info, comment )
                                  : false;
        if ( !comment.empty() )
        {
            ImGui::SeparatorText( "Description" );
            ImGui::TextWrapped( "%s", comment.c_str() );
        }

        if ( !edit_entity )
        {
            ImGui::SeparatorText( "Entity" );
            ImGui::TextUnformatted( "(no entity selected)" );
        }
        else
        {
            // ── KIWI-UX (ROUND AU): SAY WHAT IS BEING EDITED ────────────────
            // With edit_entity finally tracking the selection (win_ent.cpp
            // UpdateSelection), the binary's own "nothing selected == worldspawn"
            // rule becomes visible for the first time in this shell — and an
            // unlabelled key grid that is silently worldspawn's is how a mapper
            // puts a key on the map instead of on their entity.  The header names
            // the class and the empty-selection case says so outright.
            ImGui::SeparatorText( "Entity" );
            const char *cls = ( edit_entity->eclass && edit_entity->eclass->name )
                            ? edit_entity->eclass->name : "(unknown class)";
            const bool empty = ( selected_brushes.next == &selected_brushes );
            if ( empty )
                ImGui::TextDisabled( "nothing selected — editing %s", cls );
            else
                ImGui::Text( "%s", cls );
            if ( multiple_edit_entities )
                ImGui::TextDisabled( "multiple entities selected: edits apply to all of them" );

            Panel_DrawKeyValues();
            Panel_DrawSpawnFlags( info, haveInfo );
            Panel_DrawAngles();
        }
    }
    // ── KIWI-UX (ROUND AU): NO CLICK-OFF AUTO-CLOSE ─────────────────────────
    // ImGuiShell_CloseOnFocusLoss (imgui_shell.cpp:91) closed this panel ~130 ms
    // after focus left it, and the ONE thing a mapper does with an entity
    // inspector open is click the entity — in the 3D view, which takes the focus.
    // Round AU's dock placement already exempts it in practice (the helper skips
    // DockIsActive windows), but a user who tears the tab off must not lose the
    // panel either.  An inspector is not a transient pop-out; N and the ✕ box are
    // its close.
    ImGui::End();
}
