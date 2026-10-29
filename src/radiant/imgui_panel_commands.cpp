// imgui_panel_commands.cpp — Help > Command List (32790): every command, every binding, and
// the keybind editor.
//
// KIWI (2026-10-27, user: "I want you to completely revamp the help->Command List window. This
// window should show all commands and all keybinds exhaustively. It should also allow
// assignment of new keybinds. Also allow searching for the keybind in addition to the command
// name ... Also add a Reset to Default button that will restore default state for everything.
// Keybind cfg is saved as you go").  Replaces the read-only P-8 port of CCommandsDlg (one
// "name / binding" line per table row, name filter only).
//
//   * Rows: one per command id in g_radiantCommands (the classic table, every KIWI_CMD_* and
//     every other registered row; duplicate-id alias rows fold into their command), plus the
//     FIXED keys that never go through the table (KFIXED below: key funnel contexts, modal tool
//     keys, typed entry, terrain / decal / grass / image tools, camera fly and mouse gestures,
//     selection modifiers).  Fixed rows are read-only and carry a "Fixed:" category.
//   * Search matches the command text AND the keys: "Ctrl+R", "ctrl r", "ctrl-r" find chords
//     with exactly those modifiers; a bare key ("R", "F5", "Delete") finds it with any
//     modifiers; modifiers alone ("alt") find every chord holding them.  A one-character
//     query is a key search only (every name contains an "r").
//   * Click a key to rebind it, "+" to add one (KIWI_KEYBIND_MAX per command), right-click a
//     key to remove it, right-click a command to reset it.  The next key press is captured
//     at the pump (KiwiKeybind_CaptureMessage), so it never runs anything.  A chord another
//     command holds asks once (Reassign / Cancel); a chord that can never reach the table
//     (system chords, F10, the fixed accelerators) is refused with the reason.
//   * Every edit saves at once (kiwi_keymap.cpp SaveUser -> kiwi_radiant.ini
//     [KiwiKeybindsModern|Classic]); boot loads it (KiwiKeymap_ApplyBoot).  The palette, the
//     hint strip, the add menu and the Win32 menu all read the live table, so they follow.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include "radiant_ui_actions.h"     // ImGuiShell_CloseOnFocusLoss
#include "radiant_frame.h"          // RadiantCommand + the table API
#include "kiwi_command.h"           // KiwiCmd_Info / KiwiCmd_IsKiwiId
#include "kiwi_keymap.h"            // the keybind model, persistence and capture

namespace
{
    // ── Fixed keys: hard-wired outside the command table ──────────────────────────────
    // Keys separated by spaces so the search can match each one ("W A S D" finds "w").
    // Keep in step with the code named in `where` (inventory 2026-10-27).
    struct fixedKey_t
    {
        const char *context;
        const char *keys;
        const char *action;
        const char *where;
    };

    const fixedKey_t KFIXED[] =
    {
        // Global (the message pump)
        { "Global",        "Alt+F4  Alt+Space  Alt+Tab  Alt+Enter  Alt+Esc  F10", "Windows system keys; never bound",                     "radiant_main.cpp Radiant_PreTranslateMessage" },
        { "Global",        "Ctrl+Shift+Z",                       "Undo (the frame-wide Ctrl+Z ignores Shift)",                            "radiant_main.cpp Radiant_PreTranslateMessage" },
        { "Global",        "any key while typing in a field",    "Goes to the text field only, never to a binding",                        "radiant_main.cpp Radiant_PreTranslateMessage" },

        // Key funnel, idle
        { "Idle",          "Esc",                                "Clear region / sun / construction selection, leave patch-vertex and section modes, then the bound Deselect", "kiwi_command.cpp KiwiUX_KeyFunnel" },
        { "Idle",          "Delete  Backspace",                  "Context delete: construction lines, regions, reference images; one bevelled face restores its edge; brush sub-items just deselect", "kiwi_command.cpp KiwiUX_KeyFunnel" },
        { "Idle",          "Shift+Delete  Shift+Backspace",      "Region selected: trim the lines outside it",                             "kiwi_command.cpp KiwiUX_KeyFunnel" },
        { "Idle",          "H",                                  "Hide selected construction / reference images first, then the bound Hide", "kiwi_command.cpp KiwiUX_KeyFunnel" },
        { "Idle",          "2 2",                                "Double-press in Edge mode (400 ms): lines-only edges",                   "kiwi_command.cpp KiwiCmd_Dispatch" },
        { "Idle",          "4 4",                                "Double-press in Object mode (400 ms): models only",                      "kiwi_command.cpp KiwiCmd_Dispatch" },
        { "Idle",          "V",                                  "Drag Vertices with a patch selected: patch-vertex mode",                 "mainfrm.cpp" },
        { "Idle",          "B",                                  "Fillet Curve on brush edges becomes Fillet Edge (patch)",               "kiwi_patchfillet.cpp" },
        { "Idle",          "E",                                  "Extrude Face with lines selected becomes Extend",                        "kiwi_conselect.cpp" },
        { "Add menu / palette", "Esc  Up  Down  PageUp  PageDown  Enter", "Close / move / run; every other key is held by the open list", "kiwi_addmenu.cpp kiwi_palette.cpp" },

        // Any live tool
        { "Live tool",     "Esc",                                "Clear typed text, then leave pivot placement, then cancel the tool",     "kiwi_command.cpp KiwiCmd_KeyDown" },
        { "Live tool",     "Enter  RMB-click",                   "Commit, or the next stage",                                              "kiwi_command.cpp KiwiCmd_KeyDown" },
        { "Live tool",     "Ctrl+Z",                             "Cancel the gesture exactly (no undo entry)",                             "kiwi_command.cpp KiwiCmd_KeyDown" },
        { "Live tool",     "Ctrl+Y",                             "Held (no redo mid-gesture)",                                             "kiwi_command.cpp KiwiCmd_KeyDown" },
        { "Live tool",     "Tab  Shift+Tab",                     "Open typed entry / step the numeric fields",                            "kiwi_command.cpp kiwi_cmdoptions.cpp" },
        { "Live tool",     "Ctrl (held)",                        "Snapping off / absolute value (draw, free rotate angle, push depth, heights, offset and fillet distance)", "kiwi_command.cpp kiwi_transform.cpp" },
        { "Live tool",     "Move Rotate Scale keys",             "Apply the running transform and start the other one",                   "kiwi_command.cpp KiwiUX_KeyFunnel" },
        { "Live tool",     "Paste and Clone keys",               "End the running gesture first, then paste / clone",                      "kiwi_command.cpp KiwiUX_KeyFunnel" },
        { "Live tool",     "Selection-mode keys",                "Switch mode without ending the gesture (typed entry closed)",            "kiwi_command.cpp KiwiUX_KeyFunnel" },

        // Typed numbers
        { "Typed entry",   "0 1 2 3 4 5 6 7 8 9 . - = * / ( ) Space Backspace", "Numbers, maths and the feet/inches separator",       "kiwi_numeric.cpp KiwiNum_Key" },
        { "Typed entry",   "Y D I N F T",                        "Unit letters (yd, in, ft) while a length field has text",               "kiwi_numeric.cpp KiwiNum_Key" },

        // Transforms
        { "Move / Rotate / Scale", "X Y Z",                      "Lock to that axis; the same key again frees it",                        "kiwi_transform.cpp" },
        { "Move / Scale",  "Shift+X  Shift+Y  Shift+Z",          "Lock to the plane across that axis",                                    "kiwi_transform.cpp" },
        { "Move / Rotate / Scale", "V",                          "Place the pivot (Esc stops placing)",                                   "kiwi_transform.cpp" },
        { "Move",          "G",                                  "Hand an active model drop to the gizmo",                                 "kiwi_transform.cpp" },

        // Drawing and primitives
        { "Draw tools",    "Z",                                  "Vertical constraint (non-planar tools)",                                 "kiwi_construct.cpp" },
        { "Draw tools",    "Esc",                                "Line / Polyline: remove the last point; others: clear the chain",        "kiwi_construct.cpp" },
        { "Line / Polyline", "S",                                "Spline spans on / off",                                                  "kiwi_construct.cpp" },
        { "Spline",        "Enter",                              "Finish",                                                                  "kiwi_construct.cpp" },
        { "Polygon",       "[ ]",                                "Fewer / more sides",                                                      "kiwi_construct.cpp" },
        { "Primitives",    "Z",                                  "Cycle the working plane before the base is placed",                      "kiwi_primitive.cpp" },
        { "Primitives",    "[ ]",                                "Cylinder / Sphere / Cone: sides, or density as a patch",                  "kiwi_primitive.cpp" },
        { "Primitives",    "P",                                  "Cylinder: patch or brush",                                                "kiwi_primitive.cpp" },
        { "Extrude",       "P  [ ]",                             "Circle profile: patch or brush / sides",                                  "kiwi_extrude.cpp" },

        // Modelling tools
        { "Split Face / Region", "Tab",                          "Flip the U / V axis",                                                     "kiwi_split.cpp" },
        { "Cut",           "Esc",                                "Back to picking",                                                         "kiwi_split.cpp" },
        { "Array",         "[ ]  P",                             "Count / pick again",                                                      "kiwi_dupe.cpp" },
        { "Linear Array",  "F",                                  "Reverse",                                                                 "kiwi_dupe.cpp" },
        { "Radial Array",  "A",                                  "Cycle the axis",                                                          "kiwi_dupe.cpp" },
        { "Mirror",        "X Y Z",                              "Mirror across that axis; the same key again takes it back",              "kiwi_dupe.cpp" },
        { "Loft",          "D  [ ]  Esc",                        "Bridge mode / twist / drop face B",                                       "kiwi_loft.cpp" },
        { "Patch Fillet",  "A  D",                               "Angle drag / chamfer or fillet",                                          "kiwi_patchfillet.cpp" },
        { "Physics Drop",  "N",                                  "New random seed",                                                         "kiwi_physdrop.cpp" },
        { "Pipe",          "[ ]",                                "Sides",                                                                   "kiwi_pipe.cpp" },
        { "Boolean",       "Q  Esc",                             "Difference or union / drop the last tool",                               "kiwi_boolean.cpp" },
        { "Extend",        "F",                                  "Stop at lines / pass lines",                                              "kiwi_conselect.cpp" },
        { "Texture shift / scale", "X Y",                        "Lock S / T; the same key again frees it",                                "kiwi_uv.cpp" },

        // Armed paint and placement tools
        { "Terrain",       "Esc",                                "Disarm",                                                                  "kiwi_terrain.cpp KiwiTerrain_HandleKey" },
        { "Terrain",       "Ctrl+Tab  Ctrl+Shift+Tab",           "Next / previous tool",                                                    "kiwi_terrain.cpp KiwiTerrain_HandleKey" },
        { "Terrain",       "Tab",                                "Hide / show the brush wireframe",                                         "kiwi_terrain.cpp KiwiTerrain_HandleKey" },
        { "Terrain",       "I",                                  "Texture tool: eyedropper",                                                "kiwi_terrain.cpp KiwiTerrain_HandleKey" },
        { "Terrain",       "V",                                  "Pick the height under the pointer",                                       "kiwi_terrain.cpp KiwiTerrain_HandleKey" },
        { "Terrain",       "[ ]  - =  Subtract Plus",            "Brush radius down / up",                                                  "kiwi_terrain.cpp KiwiTerrain_HandleKey" },
        { "Terrain",       "LMB  Shift+LMB  Ctrl+LMB",           "The tool's stroke and its two variants",                                 "kiwi_terrain.cpp" },
        { "Terrain",       "Ctrl+Wheel  Shift+Wheel  Alt+Wheel", "Radius / strength / height target",                                       "kiwi_terrain.cpp" },
        { "Decals",        "Esc  LMB",                           "Disarm / place",                                                          "kiwi_decal.cpp" },
        { "Decals",        "Ctrl+Wheel  Shift+Wheel",            "Size / rotation",                                                         "kiwi_decal.cpp" },
        { "Grass",         "Esc  Alt+LMB",                       "Disarm / stamp or stroke",                                                "kiwi_grass.cpp" },
        { "Reference images", "Esc  Delete",                     "Disarm / delete the selected images",                                     "kiwi_refimage.cpp" },
        { "Reference images", "LMB-drag  Shift+LMB-drag  Alt (held)", "Move (corner scales) / rotate / no snapping",                         "kiwi_refimage.cpp" },

        // Camera
        { "Camera",        "MMB-drag",                           "Orbit",                                                                   "kiwi_viewport.cpp" },
        { "Camera",        "Shift+MMB-drag  RMB-drag",           "Pan",                                                                     "kiwi_viewport.cpp" },
        { "Camera",        "Alt+RMB-drag",                       "Mouselook",                                                               "kiwi_viewport.cpp" },
        { "Camera",        "Wheel",                              "Zoom toward the cursor",                                                  "kiwi_viewport.cpp" },
        { "Camera",        "Up Down Left Right",                 "Hovered and idle: fly (perspective) / pan (ortho)",                      "kiwi_camera.cpp" },
        { "Camera",        "LMB-double-click",                   "Select connected",                                                        "kiwi_viewport.cpp" },
        { "Camera",        "LMB-drag",                           "On a selected model: place it by dragging",                               "kiwi_viewport.cpp" },
        { "Camera fly",    "Hold Mouse-Forward",                 "Fly: pinned cursor, mouse look",                                          "imgui_shell.cpp kiwi_viewport.cpp" },
        { "Camera fly",    "W A S D",                            "Forward / back / strafe",                                                 "kiwi_camera.cpp" },
        { "Camera fly",    "E Q",                                "Up / down",                                                               "kiwi_camera.cpp" },
        { "Camera fly",    "Shift (held)  Ctrl (held)",          "3x / 0.25x speed (also RMB look and pan)",                                "kiwi_camera.cpp" },
        { "Camera fly",    "Wheel",                              "Fly speed x1.25 per notch",                                               "kiwi_camera.cpp" },

        // Selection
        { "Selection",     "LMB  Shift+LMB  Ctrl+LMB",           "Replace / add / remove",                                                  "kiwi_boxselect.cpp" },
        { "Selection",     "LMB-drag",                           "Box: left-to-right takes contained items, right-to-left crossing",        "kiwi_boxselect.cpp" },
        { "Selection",     "Shift+LMB-drag  Ctrl+LMB-drag",      "Box add / remove",                                                        "kiwi_boxselect.cpp" },
        { "Selection",     "Ctrl+LMB",                           "On an unmoved paused tool: remove, then resume",                          "kiwi_viewport.cpp" },

        // Panels
        { "2D view",       "LMB  RMB  MMB  Wheel  Ctrl+RMB",     "Stock Radiant 2D handlers (select / drag / pan / zoom / clip points)",   "xywnd.cpp" },
        { "Outliner",      "Shift+LMB  Ctrl+LMB  LMB-double-click", "Range / toggle / rename",                                              "kiwi_outliner.cpp" },
        { "Outliner",      "Shift+LMB-drag  MMB-drag",           "Paint-select rows / image opacity",                                       "kiwi_outliner.cpp" },
        { "UV editor",     "Shift+LMB  Ctrl+LMB  Ctrl+LMB-drag", "Add / remove / snap-move",                                                "kiwi_uveditor.cpp" },
        { "UV editor",     "RMB-drag  MMB-drag  Wheel  Esc",     "Pan / pan / zoom / cancel",                                               "kiwi_uveditor.cpp" },
    };

    struct row_t
    {
        int               id;               // 0 = fixed row
        const char       *name;             // first table name (the cfg key)
        const fixedKey_t *fixed;
        std::string       display;
        std::string       category;
        std::string       hay;              // lower-case search text
        kiwiChord_t       chords[KIWI_KEYBIND_MAX * 2];
        int               nChords;
        kiwiChord_t       fixedChords[4];
        int               nFixed;
        bool              custom;
    };

    struct query_t
    {
        std::vector<std::string> words;     // lower case
        bool                     single;    // one character: key search only
        bool                     chordOk;
        kiwiChord_t              chord;
    };

    bool                         s_show       = false;
    bool                         s_focusNext  = false;   // Help > Command list: front + focus once
    bool                         s_built      = false;
    bool                         s_showFixed  = true;
    bool                         s_dirty      = true;     // refilter / resort
    char                         s_filter[96] = { 0 };
    std::vector<row_t>           s_rows;
    std::vector<int>             s_visible;
    std::unordered_map<int, int> s_idRow;
    unsigned                     s_tableHash  = 0;
    int                          s_customCount = 0;
    int                          s_sortCol    = 1;        // 0 command, 1 category
    bool                         s_sortAsc    = true;

    int                          s_capId      = 0;        // capturing for this command
    int                          s_capSlot    = -1;       // -1 = add
    int                          s_pendId     = 0;        // conflict waiting for Reassign
    int                          s_pendSlot   = -1;
    kiwiChord_t                  s_pendChord  = { 0, 0 };
    bool                         s_resetArmed = false;
    std::string                  s_status;
    bool                         s_statusErr  = false;

    std::string Lower( const std::string &s )
    {
        std::string out( s );
        for ( char &c : out )
            c = (char)tolower( (unsigned char)c );
        return out;
    }

    // "ToggleOutlineDraw" -> "Toggle Outline Draw", "KiwiClipCut" -> "Clip Cut".
    std::string SplitCamel( const char *name )
    {
        if ( !strncmp( name, "Kiwi", 4 ) && isupper( (unsigned char)name[4] ) )
            name += 4;
        std::string out;
        for ( const char *p = name; *p; ++p )
        {
            const unsigned char c = (unsigned char)*p;
            if ( p != name && isupper( c ) && out.back() != ' ' )
            {
                const unsigned char prev = (unsigned char)p[-1];
                const unsigned char next = (unsigned char)p[1];
                if ( islower( prev ) || ( isupper( prev ) && next && islower( next ) ) )
                    out += ' ';
            }
            out += (char)c;
        }
        return out;
    }

    // Classic rows carry no metadata; derive a category from the name (first match wins).
    const char *ClassicCategory( const char *n )
    {
        static const struct { const char *pat; const char *cat; } K[] =
        {
            { "Lightmap", "Texture" },   { "LightPreview", "Lighting" }, { "Light", "Lighting" },
            { "OverBright", "Lighting" }, { "Center2DOnCamera", "View" }, { "Camera", "Camera" },
            { "Tex", "Texture" },        { "Surface", "Texture" },      { "Naturalize", "Texture" },
            { "LayeredMaterial", "Texture" },
            { "Patch", "Patch" },        { "Curve", "Patch" },          { "Terrain", "Patch" },
            { "Redisperse", "Patch" },   { "Thicken", "Patch" },        { "Vert", "Patch" },
            { "DragEdges", "Patch" },    { "CyclinderHeight", "Patch" }, { "TolerantWeld", "Patch" },
            { "AutoEdgeTurn", "Patch" },
            { "Leak", "File" },          { "File", "File" },            { "Quit", "File" },
            { "MapInfo", "File" },       { "Preferences", "File" },
            { "Undo", "Edit" },          { "Redo", "Edit" },            { "Copy", "Edit" },
            { "Paste", "Edit" },
            { "Hide", "Visibility" },    { "ShowHidden", "Visibility" }, { "ShowLast", "Visibility" },
            { "Entit", "Entity" },       { "Target", "Entity" },        { "Associate", "Entity" },
            { "VehicleGroup", "Entity" }, { "Link", "Entity" },         { "ConnectSelection", "Entity" },
            { "Prefab", "Entity" },      { "ColorNode", "Entity" },
            { "CloneSelection", "Brush" }, { "DeleteSelection", "Brush" }, { "SplaySelection", "Brush" },
            { "MoveSelection", "Brush" }, { "Make", "Brush" },          { "CSG", "Brush" },
            { "Clip", "Brush" },         { "Split", "Brush" },          { "Rotate", "Brush" },
            { "AutoCaulk", "Brush" },    { "FitBrush", "Brush" },       { "GetDistance", "Brush" },
            { "Select", "Selection" },   { "InvertSelection", "Selection" },
            { "Zoom", "View" },          { "View", "View" },            { "Toggle", "View" },
            { "Center", "View" },        { "Floor", "View" },           { "Grid", "View" },
        };
        for ( const auto &k : K )
            if ( strstr( n, k.pat ) )
                return k.cat;
        return "Classic";
    }

    void SetStatus( bool err, const char *fmt, ... )
    {
        char buf[256];
        va_list ap;
        va_start( ap, fmt );
        _vsnprintf( buf, sizeof( buf ), fmt, ap );
        va_end( ap );
        buf[sizeof( buf ) - 1] = '\0';
        s_status    = buf;
        s_statusErr = err;
    }

    const char *DisplayOf( int id )
    {
        auto it = s_idRow.find( id );
        if ( it != s_idRow.end() )
            return s_rows[it->second].display.c_str();
        const char *n = KiwiKeybind_PrimaryName( id );
        return n ? n : "?";
    }

    // One row per command id (alias rows fold in; the user-binding pool rows are not commands),
    // then the fixed keys.  The command set is fixed after boot, so this runs once per open.
    void Build()
    {
        s_rows.clear();
        s_idRow.clear();
        const RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTable( &table );
        for ( int i = 0; i < count; ++i )
        {
            const RadiantCommand &c = table[i];
            if ( !c.name || !c.commandId || !strcmp( c.name, "KiwiUserBinding" ) )
                continue;
            if ( s_idRow.count( c.commandId ) )
                continue;
            row_t r;
            r.id    = c.commandId;
            r.name  = c.name;
            r.fixed = nullptr;
            const kiwiCommandInfo_t *info = KiwiCmd_Info( c.commandId );
            r.display  = info ? info->displayName : SplitCamel( c.name );
            r.category = info ? info->category
                              : ( KiwiCmd_IsKiwiId( c.commandId ) ? "KIWI" : ClassicCategory( c.name ) );
            r.hay      = Lower( r.display + " " + c.name + " " + r.category );
            r.nChords  = 0;
            r.nFixed   = 0;
            r.custom   = false;
            s_idRow[c.commandId] = (int)s_rows.size();
            s_rows.push_back( r );
        }
        for ( const fixedKey_t &f : KFIXED )
        {
            row_t r;
            r.id       = 0;
            r.name     = nullptr;
            r.fixed    = &f;
            r.display  = f.action;
            r.category = std::string( "Fixed: " ) + f.context;
            r.hay      = Lower( r.display + " " + r.category + " " + f.keys );
            r.nChords  = 0;
            r.nFixed   = 0;
            r.custom   = false;
            s_rows.push_back( r );
        }
        s_built     = true;
        s_tableHash = 0;
        s_dirty     = true;
    }

    // Live chords for every command in one pass over the table; the custom flags and the
    // fixed accelerator chips only when the table changed.
    void Gather()
    {
        const RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTable( &table );
        for ( row_t &r : s_rows )
            r.nChords = 0;
        unsigned h = 2166136261u ^ (unsigned)count;
        for ( int i = 0; i < count; ++i )
        {
            const RadiantCommand &c = table[i];
            h = ( h ^ ( (unsigned)c.vk | ( (unsigned)c.mods << 8 ) | ( (unsigned)c.commandId << 12 ) ) ) * 16777619u;
            if ( !c.vk )
                continue;
            auto it = s_idRow.find( c.commandId );
            if ( it == s_idRow.end() )
                continue;
            row_t &r = s_rows[it->second];
            if ( r.nChords < (int)( sizeof( r.chords ) / sizeof( r.chords[0] ) ) )
            {
                r.chords[r.nChords].vk   = c.vk;
                r.chords[r.nChords].mods = c.mods;
                ++r.nChords;
            }
        }
        if ( h == s_tableHash )
            return;
        s_tableHash   = h;
        s_customCount = 0;
        for ( row_t &r : s_rows )
        {
            if ( !r.id )
                continue;
            r.custom = KiwiKeybind_IsCustom( r.id );
            r.nFixed = KiwiKeybind_Fixed( r.id, r.fixedChords, 4 );
            if ( r.custom )
                ++s_customCount;
        }
        s_dirty = true;                     // a key search depends on the chords
    }

    query_t ParseQuery()
    {
        query_t q;
        q.single  = false;
        q.chordOk = false;
        q.chord.vk = q.chord.mods = 0;
        std::string cur;
        for ( const char *p = s_filter; ; ++p )
        {
            if ( !*p || isspace( (unsigned char)*p ) )
            {
                if ( !cur.empty() )
                    q.words.push_back( Lower( cur ) );
                cur.clear();
                if ( !*p )
                    break;
                continue;
            }
            cur += *p;
        }
        q.single  = ( q.words.size() == 1 && q.words[0].size() == 1 );
        q.chordOk = KiwiKeybind_Parse( s_filter, &q.chord ) && ( q.chord.vk || q.chord.mods );
        return q;
    }

    bool ChordMatches( kiwiChord_t c, const query_t &q )
    {
        if ( q.chord.vk )
            return c.vk == q.chord.vk && ( !q.chord.mods || c.mods == q.chord.mods );
        return ( c.mods & q.chord.mods ) == q.chord.mods;
    }

    // A fixed row matches a key query when one of its key tokens is that key and the
    // modifier tokens cover the query's modifiers.
    bool FixedKeysMatch( const char *keys, const query_t &q )
    {
        bool keyFound = ( q.chord.vk == 0 );
        byte modsSeen = 0;
        std::string tok;
        for ( const char *p = keys; ; ++p )
        {
            const char ch = *p;
            if ( !ch || ch == ' ' || ch == '+' )
            {
                if ( !tok.empty() )
                {
                    kiwiChord_t t;
                    if ( KiwiKeybind_Parse( tok.c_str(), &t ) )
                    {
                        modsSeen |= t.mods;
                        if ( t.vk && t.vk == q.chord.vk )
                            keyFound = true;
                    }
                }
                tok.clear();
                if ( !ch )
                    break;
                continue;
            }
            tok += ch;
        }
        return keyFound && ( modsSeen & q.chord.mods ) == q.chord.mods;
    }

    bool RowMatches( const row_t &r, const query_t &q )
    {
        if ( q.words.empty() )
            return true;
        // A full chord ("ctrl r") is a key search only: the words would also hit fixed rows
        // whose key text merely holds "ctrl" and an "r" (Ctrl+Wheel).
        const bool keyOnly = q.single || ( q.chordOk && q.chord.vk && q.chord.mods );
        if ( !keyOnly )
        {
            bool all = true;
            for ( const std::string &w : q.words )
                if ( r.hay.find( w ) == std::string::npos ) { all = false; break; }
            if ( all )
                return true;
        }
        if ( !q.chordOk )
            return false;
        if ( r.fixed )
            return FixedKeysMatch( r.fixed->keys, q );
        for ( int k = 0; k < r.nChords; ++k )
            if ( ChordMatches( r.chords[k], q ) )
                return true;
        for ( int k = 0; k < r.nFixed; ++k )
            if ( ChordMatches( r.fixedChords[k], q ) )
                return true;
        return false;
    }

    void Refilter()
    {
        const query_t q = ParseQuery();
        s_visible.clear();
        for ( int i = 0; i < (int)s_rows.size(); ++i )
        {
            if ( s_rows[i].fixed && !s_showFixed )
                continue;
            if ( RowMatches( s_rows[i], q ) )
                s_visible.push_back( i );
        }
        std::stable_sort( s_visible.begin(), s_visible.end(), []( int a, int b )
        {
            const row_t &ra = s_rows[a];
            const row_t &rb = s_rows[b];
            const std::string &pa = s_sortCol == 0 ? ra.display : ra.category;
            const std::string &pb = s_sortCol == 0 ? rb.display : rb.category;
            int c = _stricmp( pa.c_str(), pb.c_str() );
            if ( !c )
            {
                const std::string &sa = s_sortCol == 0 ? ra.category : ra.display;
                const std::string &sb = s_sortCol == 0 ? rb.category : rb.display;
                c = _stricmp( sa.c_str(), sb.c_str() );
                return c < 0;               // the secondary key always ascends
            }
            return s_sortAsc ? c < 0 : c > 0;
        } );
        s_dirty = false;
    }

    void Apply( int id, int slot, kiwiChord_t c )
    {
        char text[64];
        KiwiKeybind_Format( c, text, sizeof( text ) );
        if ( KiwiKeybind_Assign( id, slot, c ) )
            SetStatus( false, "%s now runs %s (saved)", text, DisplayOf( id ) );
        else
            SetStatus( true, "Could not bind %s to %s (%d keys at most, or the command table is full)",
                       text, DisplayOf( id ), KIWI_KEYBIND_MAX );
    }

    // A captured chord: refuse it, ask about a conflict, or apply it.
    void OfferChord( int id, int slot, kiwiChord_t c )
    {
        char text[64];
        KiwiKeybind_Format( c, text, sizeof( text ) );
        if ( const char *why = KiwiKeybind_Reserved( c, id ) )
        {
            SetStatus( true, "%s cannot be bound: %s", text[0] ? text : "That key", why );
            return;
        }
        int owners[16];
        const int n = KiwiKeybind_Owners( c, owners, 16 );
        for ( int i = 0; i < n; ++i )
            if ( owners[i] != id )
            {
                s_pendId    = id;
                s_pendSlot  = slot;
                s_pendChord = c;
                return;
            }
        Apply( id, slot, c );
    }

    void BeginCapture( int id, int slot )
    {
        s_capId      = id;
        s_capSlot    = slot;
        s_pendId     = 0;
        s_resetArmed = false;
        s_status.clear();
        KiwiKeybind_BeginCapture();
    }

    void PollCapture()
    {
        if ( !s_capId || KiwiKeybind_Capturing() )
            return;
        kiwiChord_t c;
        const int id   = s_capId;
        const int slot = s_capSlot;
        s_capId = 0;
        if ( KiwiKeybind_TakeCaptured( &c ) )
            OfferChord( id, slot, c );
        else
            SetStatus( false, "Cancelled" );
    }

    void DrawStatusLine()
    {
        if ( s_capId )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.85f, 0.35f, 1.0f ),
                                "Press the new key for %s  (Esc cancels)", DisplayOf( s_capId ) );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Cancel##cap" ) )
            {
                KiwiKeybind_CancelCapture();
                s_capId = 0;
            }
            return;
        }
        if ( s_pendId )
        {
            char text[64];
            KiwiKeybind_Format( s_pendChord, text, sizeof( text ) );
            int owners[16];
            const int n = KiwiKeybind_Owners( s_pendChord, owners, 16 );
            std::string who;
            for ( int i = 0; i < n; ++i )
            {
                if ( owners[i] == s_pendId )
                    continue;
                if ( !who.empty() )
                    who += ", ";
                who += DisplayOf( owners[i] );
            }
            ImGui::TextColored( ImVec4( 1.0f, 0.6f, 0.3f, 1.0f ), "%s is used by %s.", text, who.c_str() );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Reassign" ) )
            {
                const int id = s_pendId;
                s_pendId = 0;
                Apply( id, s_pendSlot, s_pendChord );
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Cancel##pend" ) )
                s_pendId = 0;
            return;
        }
        if ( !s_status.empty() )
        {
            if ( s_statusErr )
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", s_status.c_str() );
            else
                ImGui::TextDisabled( "%s", s_status.c_str() );
            return;
        }
        ImGui::TextDisabled( "Click a key to change it, + to add one, right-click to remove. Saved as you go." );
    }

    void DrawCommandKeys( row_t &r )
    {
        char label[96];
        for ( int k = 0; k < r.nChords; ++k )
        {
            const kiwiChord_t c = r.chords[k];
            char text[64];
            KiwiKeybind_Format( c, text, sizeof( text ) );
            const bool capturing = ( s_capId == r.id && s_capSlot == k );
            // Shadowed: an earlier row runs first, or a fixed key takes the chord.
            const int         winner = KiwiKeybind_Winner( c );
            const char       *why    = KiwiKeybind_Reserved( c, r.id );
            const bool        shadow = ( winner && winner != r.id ) || why;
            if ( k )
                ImGui::SameLine();
            _snprintf( label, sizeof( label ), "%s##k%d_%d", capturing ? "press a key..." : text, r.id, k );
            label[sizeof( label ) - 1] = '\0';
            if ( shadow )
                ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 0.6f, 0.3f, 1.0f ) );
            if ( ImGui::SmallButton( label ) )
                BeginCapture( r.id, k );
            if ( shadow )
                ImGui::PopStyleColor();
            if ( ImGui::IsItemClicked( ImGuiMouseButton_Right ) )
            {
                KiwiKeybind_Remove( r.id, k );
                SetStatus( false, "Removed %s from %s (saved)", text, r.display.c_str() );
            }
            if ( ImGui::IsItemHovered() && shadow )
            {
                if ( why )
                    ImGui::SetTooltip( "Never fires here: %s", why );
                else
                    ImGui::SetTooltip( "Never fires here: %s holds it earlier in the table", DisplayOf( winner ) );
            }
        }
        for ( int k = 0; k < r.nFixed; ++k )
        {
            char text[64];
            KiwiKeybind_Format( r.fixedChords[k], text, sizeof( text ) );
            if ( r.nChords || k )
                ImGui::SameLine();
            ImGui::TextDisabled( "%s (fixed)", text );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Hard-wired accelerator: always runs this command" );
        }
        if ( r.nChords < KIWI_KEYBIND_MAX )
        {
            if ( r.nChords || r.nFixed )
                ImGui::SameLine();
            const bool capturing = ( s_capId == r.id && s_capSlot < 0 );
            _snprintf( label, sizeof( label ), "%s##add%d", capturing ? "press a key..." : "+", r.id );
            label[sizeof( label ) - 1] = '\0';
            if ( ImGui::SmallButton( label ) )
                BeginCapture( r.id, -1 );
        }
    }

    void DrawRow( row_t &r )
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex( 0 );
        if ( r.fixed )
        {
            ImGui::TextUnformatted( r.display.c_str() );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s\n%s\nHard-wired in %s; not rebindable", r.display.c_str(),
                                   r.fixed->keys, r.fixed->where );
            ImGui::TableSetColumnIndex( 1 );
            ImGui::TextDisabled( "%s", r.category.c_str() );
            ImGui::TableSetColumnIndex( 2 );
            ImGui::TextDisabled( "%s", r.fixed->keys );
            return;
        }

        ImGui::PushID( r.id );
        if ( r.custom )
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.55f, 0.8f, 1.0f, 1.0f ) );
        ImGui::Selectable( r.display.c_str(), false );
        if ( r.custom )
            ImGui::PopStyleColor();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s  (id %d)%s", r.name, r.id, r.custom ? "\nCustomized" : "" );
        if ( ImGui::BeginPopupContextItem( "##cmdctx" ) )
        {
            if ( ImGui::MenuItem( "Reset to default", nullptr, false, r.custom ) )
            {
                KiwiKeybind_ResetCommand( r.id );
                SetStatus( false, "%s reset to its default keys (saved)", r.display.c_str() );
            }
            if ( ImGui::MenuItem( "Remove all keys", nullptr, false, r.nChords > 0 ) )
            {
                while ( KiwiKeybind_Get( r.id, r.chords, 1 ) )
                    KiwiKeybind_Remove( r.id, 0 );
                SetStatus( false, "%s has no keys now (saved)", r.display.c_str() );
            }
            ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex( 1 );
        ImGui::TextDisabled( "%s", r.category.c_str() );
        ImGui::TableSetColumnIndex( 2 );
        DrawCommandKeys( r );
        ImGui::PopID();
    }
}

// Panel toggle, drawn inside the shell window (imgui_panels.cpp).
void ImGuiPanel_Commands_MenuItem()
{
    ImGui::Checkbox( "Command list", &s_show );
}

// Help > Command list... (32790, Radiant_DispatchCommandDirect) sets the same flag.
// KIWI (2026-10-27): it OPENS the window and brings it to the front (a menu command never
// closes the window it names); the window's ✕ and the panels checkbox still close it.
void ImGuiPanel_Commands_Toggle()
{
    s_show      = true;
    s_focusNext = true;
}

void ImGuiPanel_Commands_Draw()
{
    if ( !s_show )
    {
        if ( s_capId )
            KiwiKeybind_CancelCapture();
        s_capId      = 0;
        s_pendId     = 0;
        s_resetArmed = false;
        s_built      = false;               // a re-open rebuilds
        return;
    }
    if ( !s_built )
        Build();

    ImGui::SetNextWindowSize( ImVec2( 900.0f, 600.0f ), ImGuiCond_FirstUseEver );
    if ( s_focusNext )
    {
        ImGui::SetNextWindowFocus();        // also selects its tab if it is docked
        s_focusNext = false;
    }
    if ( ImGui::Begin( "Command List", &s_show ) )
    {
        Gather();
        PollCapture();

        // Search + fixed-key toggle.
        ImGui::SetNextItemWidth( 300.0f );
        if ( ImGui::InputTextWithHint( "##cmdfind", "Search commands or keys (Ctrl+R, ctrl r, R)",
                                       s_filter, sizeof( s_filter ) ) )
            s_dirty = true;
        ImGui::SameLine();
        if ( ImGui::Button( "Clear" ) )
        {
            s_filter[0] = '\0';
            s_dirty     = true;
        }
        ImGui::SameLine();
        if ( ImGui::Checkbox( "Fixed keys", &s_showFixed ) )
            s_dirty = true;

        // Profile + Reset to Default (two clicks: it drops every customization).
        KiwiKeymap_DrawSettings();
        ImGui::SameLine();
        if ( !s_resetArmed )
        {
            if ( ImGui::Button( "Reset to Default" ) )
                s_resetArmed = true;
        }
        else
        {
            if ( ImGui::Button( "Reset everything" ) )
            {
                if ( s_capId )
                    KiwiKeybind_CancelCapture();
                s_capId      = 0;
                s_pendId     = 0;
                s_resetArmed = false;
                KiwiKeybind_ResetAll();
                SetStatus( false, "Every key restored to the Modern defaults (saved)" );
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel##reset" ) )
                s_resetArmed = false;
        }

        DrawStatusLine();

        if ( s_dirty )
            Refilter();

        const float footer = ImGui::GetTextLineHeightWithSpacing();
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                                    | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                                    | ImGuiTableFlags_Sortable
                                    // A reload of the saved sort-only entry reordered the columns
                                    // (Category first); the layout is fixed, so nothing is saved.
                                    | ImGuiTableFlags_NoSavedSettings;
        if ( ImGui::BeginTable( "##commandlist", 3, flags, ImVec2( 0.0f, -footer ) ) )
        {
            ImGui::TableSetupColumn( "Command", ImGuiTableColumnFlags_WidthStretch, 1.0f );
            ImGui::TableSetupColumn( "Category", ImGuiTableColumnFlags_WidthFixed
                                   | ImGuiTableColumnFlags_DefaultSort, 130.0f );
            ImGui::TableSetupColumn( "Keys", ImGuiTableColumnFlags_WidthStretch
                                   | ImGuiTableColumnFlags_NoSort, 1.2f );
            ImGui::TableSetupScrollFreeze( 0, 1 );
            ImGui::TableHeadersRow();

            if ( ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs() )
            {
                if ( specs->SpecsDirty && specs->SpecsCount > 0 )
                {
                    s_sortCol = specs->Specs[0].ColumnIndex == 0 ? 0 : 1;
                    s_sortAsc = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                    specs->SpecsDirty = false;
                    Refilter();
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin( (int)s_visible.size() );
            while ( clipper.Step() )
                for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
                    DrawRow( s_rows[s_visible[i]] );
            ImGui::EndTable();
        }

        int commands = 0;
        for ( const row_t &r : s_rows )
            if ( r.id )
                ++commands;
        ImGui::TextDisabled( "%d shown - %d commands, %d customized, %d fixed keys",
                             (int)s_visible.size(), commands, s_customCount,
                             (int)( sizeof( KFIXED ) / sizeof( KFIXED[0] ) ) );
    }
    // Auto-close on click-off, except mid-capture or while a conflict waits for an answer.
    if ( !s_capId && !s_pendId )
        ImGuiShell_CloseOnFocusLoss( &s_show );
    ImGui::End();
}
