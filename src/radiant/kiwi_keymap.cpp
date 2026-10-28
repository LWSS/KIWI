#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Modern-profile implementation; the binding summary and lifecycle invariants live in kiwi_keymap.h.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_keymap.h"
#include "radiant_frame.h"          // RadiantCommand and command-table API
#include "kiwi_command.h"
#include "radiant_registry.h"
#include "kiwi_ported.h"                   // Sys_Printf

#include <string.h>                        // strcmp — BindName
#include <algorithm>                       // std::sort — user alias rows
#include <string>
#include <vector>

// Re-annotate menu accelerator text after a profile change.
extern void Radiant_RefreshMenuKeyBindings();

namespace
{
    const char *KKEY_SECTION = "KiwiUX";
    int         s_profile = -1;                // -1 = not loaded yet

    // Rebind the first matching id; vk 0 unbinds. A missing row currently fails silently.
    void Bind( RadiantCommand *table, int count, int commandId, byte vk, byte mods )
    {
        for ( int i = 0; i < count; ++i )
        {
            if ( table[i].commandId != commandId )
                continue;
            table[i].vk   = vk;
            table[i].mods = mods;
            return;
        }
    }

    // Duplicate-id aliases bind by name so their classic sibling rows stay intact.
    void BindName( RadiantCommand *table, int count, const char *name, byte vk, byte mods )
    {
        for ( int i = 0; i < count; ++i )
        {
            if ( !table[i].name || strcmp( table[i].name, name ) != 0 )
                continue;
            table[i].vk   = vk;
            table[i].mods = mods;
            return;
        }
    }

    // Move classic occupants before claims because hotkey lookup is first-match-wins.
    void ApplyModern()
    {
        RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTableMutable( &table );
        if ( !table || count <= 0 )
            return;

        // KIWI (2026-09-27): the classic SetGrid rows that sat on 1-9 and ` are gone with the
        // Grid menu (mainfrm.cpp), so the digits need no unbinding.
        Bind( table, count, 33092, 0x53, 3 );   // PatchInspector  -> Shift+Alt+S (vacate Shift+S)
        // Alt+S is the audited destination that frees both S and Shift+S.
        Bind( table, count, 33041, 0x53, 2 );   // SurfaceInspector-> Alt+S  (frees S AND Shift+S)
        Bind( table, count, 32835, 0x52, 3 );   // ToggleTexRotateLock -> Shift+Alt+R
        // Interim destination; the final Alt+R write below frees Shift+R for Repeat.
        Bind( table, count, 32810, 0x52, 1 );   // MouseRotate     -> Shift+R     (frees R)
        Bind( table, count, 33104, 0x46, 2 );   // ViewFilters     -> Alt+F       (frees F)
        // Shift+Alt+G is the only free G chord after VertEdit moves to Shift+G.
        Bind( table, count, 33150, 0x47, 3 );   // AssociateEntities -> Shift+Alt+G
        Bind( table, count, 33199, 0x47, 1 );   // VertEdit          -> Shift+G   (frees G)

        // Only bare arrows move to the polled modern fly; modified arrow chords stay classic.
        Bind( table, count, 33057, 0x00, 0 );   // CameraLeft    -> unbound (modern)
        Bind( table, count, 33058, 0x00, 0 );   // CameraRight   -> unbound
        Bind( table, count, 33059, 0x00, 0 );   // CameraForward -> unbound
        Bind( table, count, 33060, 0x00, 0 );   // CameraBack    -> unbound

        // Delete is occupied by ZoomIn, so move it before binding the later alias row.
        Bind( table, count, 32995, 0x2E, 1 );   // ZoomIn (XY)   -> Shift+Delete (frees Delete)

        // Shift+A is occupied; Shift+Alt+A is the free displacement chord.
        Bind( table, count, 33093, 0x41, 3 );   // SelectAllOfType -> Shift+Alt+A (frees Shift+A)

        // Free occupied Shift creation chords on W and X.
        Bind( table, count, 32857, 0x57, 3 );   // TogglePatchWireframes -> Shift+Alt+W

        Bind( table, count, 33100, 0x58, 3 );   // ToggleCrosshairs -> Shift+Alt+X

        // Ctrl+X stays empty after its File->Exit accelerator was removed; keep the
        // displaced selection command on the audited Ctrl+Alt+X chord.
        Bind( table, count, 33152, 0x58, 6 );   // SelectedAssociated -> Ctrl+Alt+X
        Bind( table, count, KIWI_CMD_CLIP_CUT, 0x00, 0 );           // (unbound)

        // Free the remaining occupied Shift creation chords on C and V.
        Bind( table, count, 32885, 0x43, 3 );   // CapCurrentCurve -> Shift+Alt+C

        Bind( table, count, 33221, 0x56, 3 );   // VehicleGroup -> Shift+Alt+V

        // Claim primary modern keys only after their classic occupants have moved.
        Bind( table, count, KIWI_CMD_SELMODE_POINT,  0x31, 0 );   // 1
        Bind( table, count, KIWI_CMD_SELMODE_EDGE,   0x32, 0 );   // 2
        Bind( table, count, KIWI_CMD_SELMODE_FACE,   0x33, 0 );   // 3
        Bind( table, count, KIWI_CMD_SELMODE_OBJECT, 0x34, 0 );   // 4
        Bind( table, count, KIWI_CMD_SELMODE_ALL,    0x35, 0 );   // 5
        Bind( table, count, KIWI_CMD_PALETTE,        0x46, 0 );   // F
        Bind( table, count, KIWI_CMD_GRID_HALVE,     0xDB, 0 );   // [
        Bind( table, count, KIWI_CMD_GRID_DOUBLE,    0xDD, 0 );   // ]

        // G/R/S use the keys freed above.
        Bind( table, count, KIWI_CMD_MOVE,           0x47, 0 );   // G
        Bind( table, count, KIWI_CMD_ROTATE,         0x52, 0 );   // R
        Bind( table, count, KIWI_CMD_SCALE,          0x53, 0 );   // S

        // Universal groups (2026-09-22).  KIWI (2026-10-24, user: "what does ctrl-G currently
        // do? Can we make that a new hotkey that's (Add to new group)"): Group Selection takes
        // Ctrl+G and the classic SelectSnapPointsToGrid (patch points to grid / Go-to panel)
        // moves to the Alt+G chord the group command held.
        Bind( table, count, 33091, 0x47, 2 );                     // SelectSnapPointsToGrid -> Alt+G
        Bind( table, count, KIWI_CMD_GROUP_CREATE,   0x47, 4 );   // Ctrl+G  group selection

        // Bind the duplicate-id alias so the base Backspace row remains unchanged.
        BindName( table, count, "KiwiDeleteSelection", 0x2E, 0 );   // Delete -> 33003

        // Shift+A starts Line; the add menu stays registered but unbound.
        Bind( table, count, KIWI_CMD_ADD_MENU,        0x00, 0 );    // (unbound)
        Bind( table, count, KIWI_CMD_DRAW_LINE,       0x41, 1 );    // Shift+A  line
        Bind( table, count, KIWI_CMD_DRAW_SPLINE,     0x53, 1 );    // Shift+S  curve
        Bind( table, count, KIWI_CMD_DRAW_RECT,       0x51, 1 );    // Shift+Q  corner rect
        Bind( table, count, KIWI_CMD_PRIM_SPHERE,     0x5A, 1 );    // Shift+Z  sphere
        Bind( table, count, KIWI_CMD_PRIM_CYLINDER,   0x58, 1 );    // Shift+X  cylinder
        // Deliberate Plasticity divergence: Shift+C is Circle, so Box moves to Shift+W.
        Bind( table, count, KIWI_CMD_DRAW_CIRCLE,     0x43, 1 );    // Shift+C  centre circle
        Bind( table, count, KIWI_CMD_PRIM_BOX,        0x57, 1 );    // Shift+W  corner box
        // Shift+V is Centre Box; Centre Rectangle moves to the free Alt+V chord.
        Bind( table, count, KIWI_CMD_DRAW_RECT_CENTER, 0x56, 2 );   // Alt+V    centre rect
        Bind( table, count, KIWI_CMD_PRIM_BOX_CENTER,  0x56, 1 );   // Shift+V  centre box

        // Ctrl+J remains the lines-only alias; bare J below is context-sensitive Join.
        Bind( table, count, KIWI_CMD_CONSTRUCT_JOIN, 0x4A, 4 );     // Ctrl+J

        // Ctrl+1..4 are free: the classic digit rows (SetGrid) no longer exist.
        Bind( table, count, KIWI_CMD_SELCONV_POINT,  0x31, 4 );     // Ctrl+1
        Bind( table, count, KIWI_CMD_SELCONV_EDGE,   0x32, 4 );     // Ctrl+2
        Bind( table, count, KIWI_CMD_SELCONV_FACE,   0x33, 4 );     // Ctrl+3
        Bind( table, count, KIWI_CMD_SELCONV_OBJECT, 0x34, 4 );     // Ctrl+4

        // Bare modelling verbs displace their classic occupants to audited free chords.
        Bind( table, count, 33056, 0x43, 6 );   // CameraDown -> Ctrl+Alt+C (frees C)
        Bind( table, count, KIWI_CMD_CUT, 0x43, 0 );                // C

        Bind( table, count, 33062, 0x5A, 3 );   // CameraAngleDown -> Shift+Alt+Z (frees Z)
        Bind( table, count, KIWI_CMD_MATCH_FACE, 0x5A, 0 );         // Z

        // Bare J is the context verb; Ctrl+J above remains the lines-only command.
        Bind( table, count, 33103, 0x4A, 3 );   // ToggleOutlineDraw -> Shift+Alt+J (frees J)
        Bind( table, count, KIWI_CMD_JOIN, 0x4A, 0 );               // J

        // The modern fly consumes arrows only, so bare E remains globally available.
        Bind( table, count, 33006, 0x45, 3 );   // DragEdges -> Shift+Alt+E (frees E)
        Bind( table, count, KIWI_CMD_EXTRUDE_FACE, 0x45, 0 );       // E

        // Ctrl+R is not a resource accelerator; move its stock occupant first.
        Bind( table, count, 10, 0x52, 5 );      // RemoveColorNode -> Shift+Ctrl+R
        Bind( table, count, KIWI_CMD_SPLIT_FACE, 0x52, 4 );         // Ctrl+R

        // T matches Plasticity's Trim binding; ViewTextures moves to a free chord.
        Bind( table, count, 33018, 0x54, 3 );   // ViewTextures -> Shift+Alt+T (frees T)
        Bind( table, count, KIWI_CMD_TRIM, 0x54, 0 );              // T

        // Ctrl+O remains FileOpen; only bare O is claimed.
        Bind( table, count, 33016, 0x4F, 3 );   // ViewConsole -> Shift+Alt+O (frees O)
        Bind( table, count, KIWI_CMD_OFFSET_CURVE, 0x4F, 0 );      // O

        Bind( table, count, 36121, 0x42, 3 );   // SameTargetname -> Shift+Alt+B (frees B)
        Bind( table, count, KIWI_CMD_FILLET_CURVE, 0x42, 0 );      // B

        Bind( table, count, 32961, 0x44, 3 );   // RotateZ -> Shift+Alt+D (frees Shift+D)
        Bind( table, count, KIWI_CMD_DUPLICATE, 0x44, 1 );         // Shift+D
        // Ctrl+Shift+V is the classic ToggleView chord (mainfrm.cpp command table, 33071 -
        // show / hide the 2D view).  The first version of this line missed that and bound a
        // second command to the same chord; the occupant moves first, like every other claim.
        Bind( table, count, 33071, 0x56, 6 );   // ToggleView -> Ctrl+Alt+V (frees Ctrl+Shift+V)
        Bind( table, count, KIWI_CMD_PASTE_IN_PLACE, 0x56, 5 );    // Ctrl+Shift+V

        // The ported key-name table lacks 0xBF, so radiant.ini cannot name this chord.
        Bind( table, count, KIWI_CMD_FOCUS_SELECTION, 0xBF, 0 );   // /

        // Swap Shift/Alt hide chords to Plasticity's order; Ctrl+H was free.
        Bind( table, count, 32934, 0x48, 1 );   // HideUnSelected -> Shift+H (isolate)
        Bind( table, count, 32924, 0x48, 2 );   // ShowHidden     -> Alt+H
        Bind( table, count, KIWI_CMD_HIDE_INVERT, 0x48, 4 );       // Ctrl+H

        // Repeat takes Shift+R; legacy MouseRotate lands on the audited Alt+R chord.
        Bind( table, count, 32810, 0x52, 2 );   // MouseRotate -> Alt+R (frees Shift+R)
        Bind( table, count, KIWI_CMD_REPEAT_LAST, 0x52, 1 );       // Shift+R

        // Bare Q was free; Shift+Q remains Rectangle.
        Bind( table, count, KIWI_CMD_BOOLEAN, 0x51, 0 );           // Q

        // L displaces ToggleLayers; Ctrl+L remains the resource accelerator.
        Bind( table, count, 33954, 0x4C, 3 );   // ToggleLayers -> Shift+Alt+L (frees L)
        Bind( table, count, KIWI_CMD_LOFT, 0x4C, 0 );              // L

        // PageUp is coarser/double and PageDown finer/halve, matching ] and [.
        // Name-bound aliases preserve both bracket bindings because Bind() stops at the first id.
        Bind( table, count, 32954, 0x21, 3 );   // UpFloor   -> Shift+Alt+PageUp
        Bind( table, count, 32955, 0x22, 3 );   // DownFloor -> Shift+Alt+PageDown
        BindName( table, count, "KiwiGridDoublePage", 0x21, 0 );   // PageUp   -> 34021
        BindName( table, count, "KiwiGridHalvePage",  0x22, 0 );   // PageDown -> 34020

        // End caulks and Shift+End uncaulks, displacing CenterView to Ctrl+End; classic
        // remains untouched.
        Bind( table, count, 32953, 0x23, 4 );                  // CenterView -> Ctrl+End
        Bind( table, count, KIWI_CMD_CAULK_FACES, 0x23, 0 );   // End -> Caulk Selection
        Bind( table, count, KIWI_CMD_UNCAULK_FACES, 0x23, 1 ); // Shift+End -> Uncaulk Selection

        // Space displaces Clone to free Shift+Space; modal numeric input handles Space
        // before global lookup, preserving its feet/inches separator.
        Bind( table, count, 33001, 0x20, 1 );                  // Clone -> Shift+Space
        Bind( table, count, KIWI_CMD_VIEW_FACE, 0x20, 0 );     // Space -> View Face

        // KIWI (2026-09-28, user: "pipe tool needs to be on 'P' key"): Preferences moves to
        // the free Alt+P (Shift+P is ApplyPatchCap, Ctrl+Shift+P the Plasticity push).
        Bind( table, count, 32784, 0x50, 2 );                  // Preferences -> Alt+P (frees P)
        Bind( table, count, KIWI_CMD_CONSTRUCT_PIPE, 0x50, 0 ); // P -> Pipe

        // V is command-local for movable pivots and must not gain a global row;
        // modal dispatch precedes hotkey lookup, leaving DragVertices available when idle.

    }

    // ── User layer (KIWI 2026-10-27, user: "the keybinds will save to disk and be loaded
    // upon start") ── see kiwi_keymap.h.  s_base is every row's binding after the profile
    // patch and before the user layer: the "default" each command is diffed against.
    const char *KUSER_ALIAS = "KiwiUserBinding";
    std::vector<kiwiChord_t> s_base;

    const char *UserSection( kiwiKeymap_t profile )
    {
        return profile == KEYMAP_CLASSIC ? "KiwiKeybindsClassic" : "KiwiKeybindsModern";
    }

    bool IsModifierVk( unsigned vk )
    {
        return vk == 0x10 || vk == 0x11 || vk == 0x12 || ( vk >= 0xA0 && vk <= 0xA5 )
            || vk == 0x5B || vk == 0x5C;
    }

    bool SameChord( kiwiChord_t a, kiwiChord_t b ) { return a.vk == b.vk && a.mods == b.mods; }

    void SnapshotBase()
    {
        const RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTable( &table );
        s_base.resize( count > 0 ? (size_t)count : 0 );
        for ( int i = 0; i < count; ++i )
        {
            s_base[i].vk   = table[i].vk;
            s_base[i].mods = table[i].mods;
        }
    }

    // A row registered after the snapshot (a fresh user alias) defaults to its registered
    // binding, which is what it holds until the user layer touches it.
    kiwiChord_t BaseOf( const RadiantCommand *table, int i )
    {
        if ( i < (int)s_base.size() )
            return s_base[i];
        kiwiChord_t c = { table[i].vk, table[i].mods };
        return c;
    }

    // Rewrite one command's rows so they hold exactly `chords`, first chord in the first row
    // (the palette / hint strip show the first bound row).  Extra chords take a free user
    // alias row or register one.  False when the table is full.
    bool SetRows( int commandId, const kiwiChord_t *chords, int n )
    {
        RadiantCommand *table = nullptr;
        int count = Radiant_GetCommandTableMutable( &table );
        if ( !table || commandId <= 0 )
            return false;

        std::vector<int> rows;
        for ( int i = 0; i < count; ++i )
            if ( table[i].commandId == commandId )
                rows.push_back( i );
        if ( rows.empty() )
            return false;                       // only existing commands are rebound

        while ( (int)rows.size() < n )
        {
            int freeRow = -1;
            for ( int i = 0; i < count; ++i )
                if ( table[i].vk == 0 && table[i].commandId != commandId
                  && table[i].name && strcmp( table[i].name, KUSER_ALIAS ) == 0 )
                {
                    freeRow = i;
                    break;
                }
            if ( freeRow < 0 )
            {
                if ( !Radiant_RegisterCommandAlias( KUSER_ALIAS, 0, 0, commandId ) )
                    return false;               // mainfrm already printed the FULL line
                count = Radiant_GetCommandTableMutable( &table );
                freeRow = count - 1;
                if ( (int)s_base.size() < count )
                {
                    kiwiChord_t none = { 0, 0 };
                    s_base.resize( (size_t)count, none );
                }
            }
            table[freeRow].commandId = commandId;
            rows.push_back( freeRow );
            std::sort( rows.begin(), rows.end() );
        }

        for ( size_t k = 0; k < rows.size(); ++k )
        {
            const bool has = (int)k < n;
            table[rows[k]].vk   = has ? chords[k].vk   : 0;
            table[rows[k]].mods = has ? chords[k].mods : 0;
        }
        return true;
    }

    // Apply the selected profile's saved section.  Unknown names (a command removed since)
    // are skipped and dropped by the next save.
    void ApplyUser()
    {
        char buf[32768];
        const DWORD n = ::GetPrivateProfileSectionA( UserSection( KiwiKeymap_Get() ), buf,
                                                     sizeof( buf ), Radiant_IniPath() );
        if ( !n )
            return;
        for ( const char *line = buf; *line; line += strlen( line ) + 1 )
        {
            const char *eq = strchr( line, '=' );
            if ( !eq || eq == line )
                continue;
            const std::string name( line, eq - line );
            const int id = KiwiKeybind_FindCommand( name.c_str() );
            if ( !id )
                continue;

            kiwiChord_t chords[KIWI_KEYBIND_MAX];
            int count = 0;
            std::string list( eq + 1 );
            size_t pos = 0;
            while ( pos <= list.size() && count < KIWI_KEYBIND_MAX )
            {
                size_t comma = list.find( ',', pos );
                if ( comma == std::string::npos )
                    comma = list.size();
                const std::string tok = list.substr( pos, comma - pos );
                kiwiChord_t c;
                if ( KiwiKeybind_Parse( tok.c_str(), &c ) && c.vk )
                    chords[count++] = c;
                pos = comma + 1;
            }
            SetRows( id, chords, count );
        }
    }

    // The diff against s_base, written as one section (an empty diff deletes it).
    void SaveUser()
    {
        const RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTable( &table );
        std::string blob;
        for ( int i = 0; i < count; ++i )
        {
            const int id = table[i].commandId;
            if ( !id || !table[i].name || strcmp( table[i].name, KUSER_ALIAS ) == 0 )
                continue;
            bool first = true;                  // one line per command, at its first row
            for ( int j = 0; j < i; ++j )
                if ( table[j].commandId == id ) { first = false; break; }
            if ( !first || !KiwiKeybind_IsCustom( id ) )
                continue;

            kiwiChord_t chords[KIWI_KEYBIND_MAX];
            const int n = KiwiKeybind_Get( id, chords, KIWI_KEYBIND_MAX );
            blob += table[i].name;
            blob += '=';
            if ( !n )
                blob += "none";
            for ( int k = 0; k < n; ++k )
            {
                char text[64];
                KiwiKeybind_Format( chords[k], text, sizeof( text ) );
                if ( k )
                    blob += ", ";
                blob += text;
            }
            blob += '\0';
        }

        const char *section = UserSection( KiwiKeymap_Get() );
        if ( blob.empty() )
        {
            ::WritePrivateProfileStringA( section, nullptr, nullptr, Radiant_IniPath() );
            return;
        }
        blob += '\0';                           // the section buffer ends in a double NUL
        if ( !::WritePrivateProfileSectionA( section, blob.c_str(), Radiant_IniPath() ) )
            Sys_Printf( "KiwiKeybind: could not write [%s] to %s\n", section, Radiant_IniPath() );
    }

    // Everything after Reset + radiant.ini: the profile patch, the default snapshot, the user layer.
    void ApplyLayers()
    {
        if ( KiwiKeymap_Get() == KEYMAP_MODERN )
            ApplyModern();
        SnapshotBase();
        ApplyUser();
    }

    // Defaults -> radiant.ini -> modern patch -> user layer keeps switching idempotent.
    void Rebuild()
    {
        Radiant_ResetCommandBindings();
        Radiant_LoadCommandMap();
        ApplyLayers();
        Radiant_RefreshMenuKeyBindings();
    }

    // Capture state (KiwiKeybind_CaptureMessage).
    bool        s_capArmed  = false;
    bool        s_capDone   = false;
    bool        s_eatAltUp  = false;
    kiwiChord_t s_capChord  = { 0, 0 };
}

kiwiKeymap_t KiwiKeymap_Get()
{
    if ( s_profile < 0 )
    {
        // The overhaul defaults to the modern profile.
        s_profile = Radiant_ProfileGetInt( KKEY_SECTION, "Keymap", (int)KEYMAP_MODERN );
        if ( s_profile != (int)KEYMAP_CLASSIC && s_profile != (int)KEYMAP_MODERN )
            s_profile = (int)KEYMAP_MODERN;
    }
    return (kiwiKeymap_t)s_profile;
}

void KiwiKeymap_Set( kiwiKeymap_t profile )
{
    if ( profile != KEYMAP_CLASSIC && profile != KEYMAP_MODERN )
        return;
    if ( KiwiKeymap_Get() == profile )
        return;
    s_profile = (int)profile;
    Radiant_ProfileSetInt( KKEY_SECTION, "Keymap", s_profile );
    Rebuild();
}

// Boot already reset the table and loaded radiant.ini; the profile patch and (KIWI
// 2026-10-27) the saved user keybinds remain.
void KiwiKeymap_ApplyBoot()
{
    ApplyLayers();
}

// Profile switcher inside the KiwiUX settings block.
void KiwiKeymap_DrawSettings()
{
    int cur = (int)KiwiKeymap_Get();
    const char *items[] = { "Classic (stock Radiant bindings)", "Modern (Plasticity-style)" };
    ImGui::SetNextItemWidth( 260.0f );
    if ( ImGui::Combo( "Keymap profile", &cur, items, 2 ) )
        KiwiKeymap_Set( (kiwiKeymap_t)cur );
    if ( ImGui::IsItemHovered() )
        // Keep this summary synchronized with ApplyModern().
        ImGui::SetTooltip(
            "Modern: 1-5 selection modes, Ctrl+1-4 convert the selection,\n"
            "F command palette, [ ] or PageUp/PageDown grid spacing,\n"
            "G/R/S transforms (V moves the pivot while one is running),\n"
            "C cut, Z match face, J join, E extrude, Ctrl+R split face,\n"
            "and the creation chords: Shift+A line, Shift+S spline,\n"
            "Shift+Q rect, Shift+C circle, Shift+W box, Shift+V box(centre),\n"
            "Alt+V rect(centre), Shift+X cylinder, Shift+Z sphere.\n"
            "End caulks the selection, Shift+End uncaulks it\n"
            "(View->Center moves to Ctrl+End).\n"
            "The displaced commands move to Shift/Alt chords, never dropped.\n"
            "Classic: the stock table plus your radiant.ini remaps, unchanged.\n"
            "Every new feature stays reachable from the command palette in both." );
}

// ═════════════════════════════════════════════════════════════════════════════
//  User keybinds — KIWI (2026-10-27, user: "This window should show all commands and all
//  keybinds exhaustively. It should also allow assignment of new keybinds ... Also add a
//  Reset to Default button").  Model + persistence; the window is imgui_panel_commands.cpp.
// ═════════════════════════════════════════════════════════════════════════════
extern void ImGuiShell_ForwardSwallowedKey( UINT msg, WPARAM wParam, LPARAM lParam );   // imgui_shell.cpp:171

namespace
{
    // Chords that never reach the table for anyone but their own command: IDR_MAIN_ACCEL
    // (radiant.rc, TranslateAccelerator runs first) and the pump's Ctrl+Z / Ctrl+Y
    // (radiant_main.cpp Radiant_PreTranslateMessage).  All are Ctrl chords.
    const struct { byte vk; int commandId; const char *why; } KFIXED_CTRL[] =
    {
        { 'Z',  57643, "Ctrl+Z is wired to Undo ahead of the command table" },
        { 'Y',  57644, "Ctrl+Y is wired to Redo ahead of the command table" },
        { 'O',  57601, "Ctrl+O is the fixed File > Open accelerator" },
        { 'S',  57603, "Ctrl+S is the fixed File > Save accelerator" },
        { 'L',  33024, "Ctrl+L is the fixed Next Leak Spot accelerator" },
        { 'P',  33025, "Ctrl+P is the fixed Previous Leak Spot accelerator" },
        { 'K',  33021, "Ctrl+K is the fixed Connect Entities accelerator" },
        { 'M',  33042, "Ctrl+M is the fixed Make Detail accelerator" },
        { 0x2E, 32999, "Ctrl+Delete is the fixed Z-view Zoom In accelerator" },
        { 0x2D, 33000, "Ctrl+Insert is the fixed Z-view Zoom Out accelerator" },
    };

    // Display name of a VK; false when it has none (CommandList_KeyName's raw-char arm).
    bool KeyText( unsigned vk, char *out, int outSize )
    {
        out[0] = '\0';
        if ( !vk )
            return false;
        if ( ( vk >= '0' && vk <= '9' ) || ( vk >= 'A' && vk <= 'Z' ) )
        {
            _snprintf( out, outSize, "%c", (char)vk );
            out[outSize - 1] = '\0';
            return true;
        }
        RadiantCommand rc = { nullptr, (byte)vk, 0, 0 };
        char keybuf[8];
        const char *name = CommandList_KeyName( rc, keybuf );
        if ( !name || name == keybuf )
            return false;
        _snprintf( out, outSize, "%s", name );
        out[outSize - 1] = '\0';
        return true;
    }

    // One token of a chord: a modifier bit (*mod) or a key (*vk).  False = unknown.
    bool ParseToken( const char *tok, byte *mod, byte *vk )
    {
        *mod = 0;
        *vk  = 0;
        if ( !tok[0] )
            return false;
        if ( !_stricmp( tok, "ctrl" ) || !_stricmp( tok, "control" ) ) { *mod = 4; return true; }
        if ( !_stricmp( tok, "shift" ) )                               { *mod = 1; return true; }
        if ( !_stricmp( tok, "alt" ) )                                 { *mod = 2; return true; }
        if ( !_stricmp( tok, "win" ) || !_stricmp( tok, "lwin" ) )     { *mod = 8; return true; }

        static const struct { const char *alias; byte vk; } KALIAS[] =
        {
            { "esc", 0x1B }, { "del", 0x2E }, { "enter", 0x0D }, { "ins", 0x2D },
            { "pgup", 0x21 }, { "pgdn", 0x22 }, { "bksp", 0x08 }, { "slash", 0xBF },
            { "-", 0xBD }, { ",", 0xBC }, { ".", 0xBE }, { "`", 0xC0 },
        };
        for ( const auto &a : KALIAS )
            if ( !_stricmp( tok, a.alias ) ) { *vk = a.vk; return true; }

        if ( !tok[1] && isalnum( (unsigned char)tok[0] ) )
        {
            *vk = (byte)toupper( (unsigned char)tok[0] );
            return true;
        }
        if ( !_strnicmp( tok, "VK_", 3 ) && tok[3] )
        {
            const unsigned long v = strtoul( tok + 3, nullptr, 16 );
            if ( v > 0 && v < 0xFF ) { *vk = (byte)v; return true; }
        }
        char name[32];
        for ( unsigned v = 1; v < 0xFF; ++v )
        {
            if ( IsModifierVk( v ) || !KeyText( v, name, sizeof( name ) ) )
                continue;
            if ( !_stricmp( tok, name ) ) { *vk = (byte)v; return true; }
        }
        return false;
    }

    void FinishEdit()
    {
        SaveUser();
        Radiant_RefreshMenuKeyBindings();
    }

    // Drop chord `c` from every command except `keepId`.
    void StripOthers( kiwiChord_t c, int keepId )
    {
        int owners[64];
        const int n = KiwiKeybind_Owners( c, owners, 64 );
        for ( int i = 0; i < n; ++i )
        {
            if ( owners[i] == keepId )
                continue;
            kiwiChord_t list[KIWI_KEYBIND_MAX * 4];
            const int count = KiwiKeybind_Get( owners[i], list, KIWI_KEYBIND_MAX * 4 );
            kiwiChord_t kept[KIWI_KEYBIND_MAX * 4];
            int k = 0;
            for ( int j = 0; j < count; ++j )
                if ( !SameChord( list[j], c ) )
                    kept[k++] = list[j];
            SetRows( owners[i], kept, k );
        }
    }
}

const char *KiwiKeymap_ExtraKeyName( unsigned vk )
{
    switch ( vk )
    {
    case 0x0C: return "Clear";
    case 0x13: return "Pause";
    case 0x14: return "CapsLock";
    case 0x24: return "Home";
    case 0x2C: return "PrintScreen";
    case 0x5D: return "Apps";
    case 0x6C: return "Separator";
    case 0x6E: return "Decimal";
    case 0x6F: return "Divide";
    case 0x90: return "NumLock";
    case 0x91: return "ScrollLock";
    case 0xBA: return ";";
    case 0xBB: return "=";
    case 0xBF: return "/";
    case 0xDE: return "'";
    case 0xE2: return "OEM102";
    default:   break;
    }
    static const char *const KF13[] =
        { "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24" };
    if ( vk >= 0x7C && vk <= 0x87 )
        return KF13[vk - 0x7C];
    return nullptr;
}

void KiwiKeybind_Format( kiwiChord_t c, char *out, int outSize )
{
    if ( !out || outSize < 1 )
        return;
    out[0] = '\0';
    if ( !c.vk )
        return;
    char key[32];
    if ( !KeyText( c.vk, key, sizeof( key ) ) )
        _snprintf( key, sizeof( key ), "VK_%02X", c.vk );
    key[sizeof( key ) - 1] = '\0';
    _snprintf( out, outSize, "%s%s%s%s%s",
               ( c.mods & 4 ) ? "Ctrl+"  : "",
               ( c.mods & 1 ) ? "Shift+" : "",
               ( c.mods & 2 ) ? "Alt+"   : "",
               ( c.mods & 8 ) ? "Win+"   : "", key );
    out[outSize - 1] = '\0';
}

bool KiwiKeybind_Parse( const char *text, kiwiChord_t *out )
{
    out->vk   = 0;
    out->mods = 0;
    if ( !text )
        return false;

    std::vector<std::string> toks;
    std::string cur;
    for ( const char *p = text; ; ++p )
    {
        const char ch = *p;
        // "+" alone is no key name here (numpad + is "Plus"), so it is always a separator.
        if ( !ch || ch == ' ' || ch == '\t' || ch == '+' )
        {
            if ( !cur.empty() )
                toks.push_back( cur );
            cur.clear();
            if ( !ch )
                break;
            continue;
        }
        cur += ch;
    }
    if ( toks.empty() )
        return false;

    bool haveKey = false;
    for ( size_t i = 0; i < toks.size(); ++i )
    {
        byte mod, vk;
        if ( !ParseToken( toks[i].c_str(), &mod, &vk ) )
        {
            // The Win32 menus spell chords "Shift-Alt-R": split on '-' and retry.
            const std::string &t = toks[i];
            if ( t.size() < 3 || t.find( '-' ) == std::string::npos )
                return false;
            std::vector<std::string> parts;
            size_t pos = 0;
            for ( ;; )
            {
                const size_t dash = t.find( '-', pos );
                parts.push_back( t.substr( pos, dash == std::string::npos ? std::string::npos
                                                                         : dash - pos ) );
                if ( dash == std::string::npos )
                    break;
                pos = dash + 1;
            }
            // "Ctrl-" + "" = the Minus key.
            if ( parts.back().empty() )
                parts.back() = "-";
            toks.erase( toks.begin() + i );
            toks.insert( toks.begin() + i, parts.begin(), parts.end() );
            --i;
            continue;
        }
        if ( vk )
        {
            if ( haveKey )
                return false;               // two keys are not a chord
            haveKey  = true;
            out->vk  = vk;
        }
        out->mods |= mod;
    }
    return true;
}

int KiwiKeybind_Get( int commandId, kiwiChord_t *out, int maxOut )
{
    const RadiantCommand *table = nullptr;
    const int count = Radiant_GetCommandTable( &table );
    int n = 0;
    for ( int i = 0; i < count && n < maxOut; ++i )
        if ( table[i].commandId == commandId && table[i].vk )
        {
            out[n].vk   = table[i].vk;
            out[n].mods = table[i].mods;
            ++n;
        }
    return n;
}

int KiwiKeybind_GetDefault( int commandId, kiwiChord_t *out, int maxOut )
{
    const RadiantCommand *table = nullptr;
    const int count = Radiant_GetCommandTable( &table );
    int n = 0;
    for ( int i = 0; i < count && n < maxOut; ++i )
    {
        if ( table[i].commandId != commandId )
            continue;
        const kiwiChord_t b = BaseOf( table, i );
        if ( b.vk )
            out[n++] = b;
    }
    return n;
}

bool KiwiKeybind_IsCustom( int commandId )
{
    kiwiChord_t live[KIWI_KEYBIND_MAX * 4], def[KIWI_KEYBIND_MAX * 4];
    const int nl = KiwiKeybind_Get( commandId, live, KIWI_KEYBIND_MAX * 4 );
    const int nd = KiwiKeybind_GetDefault( commandId, def, KIWI_KEYBIND_MAX * 4 );
    // Same SET of chords = not custom; row order and duplicates do not count.
    for ( int i = 0; i < nl; ++i )
    {
        bool found = false;
        for ( int j = 0; j < nd && !found; ++j )
            found = SameChord( live[i], def[j] );
        if ( !found )
            return true;
    }
    for ( int j = 0; j < nd; ++j )
    {
        bool found = false;
        for ( int i = 0; i < nl && !found; ++i )
            found = SameChord( live[i], def[j] );
        if ( !found )
            return true;
    }
    return false;
}

int KiwiKeybind_Winner( kiwiChord_t c )
{
    if ( !c.vk )
        return 0;
    const RadiantCommand *table = nullptr;
    const int count = Radiant_GetCommandTable( &table );
    for ( int i = 0; i < count; ++i )
        if ( table[i].vk == c.vk && table[i].mods == c.mods )
            return table[i].commandId;
    return 0;
}

int KiwiKeybind_Owners( kiwiChord_t c, int *ids, int maxIds )
{
    if ( !c.vk )
        return 0;
    const RadiantCommand *table = nullptr;
    const int count = Radiant_GetCommandTable( &table );
    int n = 0;
    for ( int i = 0; i < count && n < maxIds; ++i )
    {
        if ( table[i].vk != c.vk || table[i].mods != c.mods )
            continue;
        bool seen = false;
        for ( int k = 0; k < n && !seen; ++k )
            seen = ( ids[k] == table[i].commandId );
        if ( !seen )
            ids[n++] = table[i].commandId;
    }
    return n;
}

const char *KiwiKeybind_Reserved( kiwiChord_t c, int commandId )
{
    if ( !c.vk )
        return "no key";
    if ( IsModifierVk( c.vk ) )
        return "a modifier alone cannot be a binding";
    char name[32];
    if ( !KeyText( c.vk, name, sizeof( name ) ) )
        return "this key has no name in the binding table";
    if ( c.vk == 0x79 )                                         // VK_F10
        return "F10 belongs to the Windows menu bar";
    if ( c.mods & 2 )
    {
        switch ( c.vk )
        {
        case 0x73: case 0x20: case 0x09: case 0x0D: case 0x1B:  // F4 Space Tab Enter Esc
            return "Alt+F4 / Space / Tab / Enter / Esc are Windows system chords";
        default:
            break;
        }
    }
    if ( c.mods == 4 )
        for ( const auto &f : KFIXED_CTRL )
            if ( f.vk == c.vk && f.commandId != commandId )
                return f.why;
    return nullptr;
}

int KiwiKeybind_Fixed( int commandId, kiwiChord_t *out, int maxOut )
{
    kiwiChord_t live[KIWI_KEYBIND_MAX * 4];
    const int nl = KiwiKeybind_Get( commandId, live, KIWI_KEYBIND_MAX * 4 );
    int n = 0;
    for ( const auto &f : KFIXED_CTRL )
    {
        if ( f.commandId != commandId || n >= maxOut )
            continue;
        const kiwiChord_t c = { f.vk, 4 };
        bool listed = false;
        for ( int i = 0; i < nl && !listed; ++i )
            listed = SameChord( live[i], c );
        if ( !listed )
            out[n++] = c;
    }
    return n;
}

bool KiwiKeybind_Assign( int commandId, int slot, kiwiChord_t c )
{
    if ( !commandId || !c.vk || KiwiKeybind_Reserved( c, commandId ) )
        return false;

    kiwiChord_t list[KIWI_KEYBIND_MAX * 4];
    const int n = KiwiKeybind_Get( commandId, list, KIWI_KEYBIND_MAX * 4 );
    kiwiChord_t next[KIWI_KEYBIND_MAX * 4];
    int m = 0;
    bool placed = false;
    for ( int k = 0; k < n; ++k )
    {
        if ( k == slot )
        {
            next[m++] = c;
            placed = true;
        }
        else if ( !SameChord( list[k], c ) )
        {
            next[m++] = list[k];
        }
        else if ( slot < 0 || slot >= n )
        {
            next[m++] = list[k];             // adding a chord it already has: keep it once
            placed = true;
        }
    }
    if ( !placed )
    {
        if ( m >= KIWI_KEYBIND_MAX )
            return false;
        next[m++] = c;
    }

    StripOthers( c, commandId );
    const bool ok = SetRows( commandId, next, m );
    FinishEdit();
    return ok;
}

void KiwiKeybind_Remove( int commandId, int slot )
{
    kiwiChord_t list[KIWI_KEYBIND_MAX * 4];
    const int n = KiwiKeybind_Get( commandId, list, KIWI_KEYBIND_MAX * 4 );
    if ( slot < 0 || slot >= n )
        return;
    for ( int k = slot; k + 1 < n; ++k )
        list[k] = list[k + 1];
    SetRows( commandId, list, n - 1 );
    FinishEdit();
}

void KiwiKeybind_ResetCommand( int commandId )
{
    kiwiChord_t def[KIWI_KEYBIND_MAX * 4];
    const int n = KiwiKeybind_GetDefault( commandId, def, KIWI_KEYBIND_MAX * 4 );
    for ( int k = 0; k < n; ++k )
        StripOthers( def[k], commandId );
    SetRows( commandId, def, n );
    FinishEdit();
}

void KiwiKeybind_ResetAll()
{
    ::WritePrivateProfileStringA( UserSection( KEYMAP_MODERN ),  nullptr, nullptr, Radiant_IniPath() );
    ::WritePrivateProfileStringA( UserSection( KEYMAP_CLASSIC ), nullptr, nullptr, Radiant_IniPath() );
    if ( KiwiKeymap_Get() != KEYMAP_MODERN )
        KiwiKeymap_Set( KEYMAP_MODERN );     // persists the profile and rebuilds
    else
        Rebuild();
    Sys_Printf( "Keybinds reset to the Modern defaults\n" );
}

void KiwiKeybind_Reload()
{
    s_profile = -1;                          // re-read [KiwiUX] Keymap as well
    Rebuild();
}

int KiwiKeybind_FindCommand( const char *name )
{
    if ( !name || !name[0] || !_stricmp( name, KUSER_ALIAS ) )
        return 0;
    const RadiantCommand *table = nullptr;
    const int count = Radiant_GetCommandTable( &table );
    for ( int i = 0; i < count; ++i )
        if ( table[i].name && !_stricmp( table[i].name, name ) )
            return table[i].commandId;
    return 0;
}

const char *KiwiKeybind_PrimaryName( int commandId )
{
    const RadiantCommand *table = nullptr;
    const int count = Radiant_GetCommandTable( &table );
    for ( int i = 0; i < count; ++i )
        if ( table[i].commandId == commandId && table[i].name )
            return table[i].name;
    return nullptr;
}

void KiwiKeybind_BeginCapture()
{
    s_capArmed = true;
    s_capDone  = false;
}

void KiwiKeybind_CancelCapture()
{
    s_capArmed = false;
    s_capDone  = false;
}

bool KiwiKeybind_Capturing()
{
    return s_capArmed;
}

bool KiwiKeybind_TakeCaptured( kiwiChord_t *out )
{
    if ( !s_capDone )
        return false;
    s_capDone = false;
    *out = s_capChord;
    return true;
}

// The pump offers every message here first (radiant_main.cpp Radiant_PreTranslateMessage).
// While armed, the next non-modifier key down is the chord and never reaches ImGui, the key
// funnel, the accelerators or the table.  Modifiers pass so ImGui's modifier state stays
// true; an Alt chord also eats its Alt release, or the menu bar would take the focus.
bool KiwiKeybind_CaptureMessage( const MSG *msg )
{
    const UINT m = msg->message;
    if ( s_eatAltUp && ( m == WM_KEYUP || m == WM_SYSKEYUP ) )
    {
        const unsigned up = (unsigned)msg->wParam;
        if ( up == VK_MENU || up == VK_LMENU || up == VK_RMENU )
        {
            s_eatAltUp = false;
            ImGuiShell_ForwardSwallowedKey( m, msg->wParam, msg->lParam );
            return true;
        }
    }
    if ( !s_capArmed )
        return false;
    if ( m == WM_CHAR || m == WM_SYSCHAR )
        return true;
    if ( m != WM_KEYDOWN && m != WM_SYSKEYDOWN )
        return false;

    const unsigned vk = (unsigned)msg->wParam;
    if ( IsModifierVk( vk ) )
        return false;

    byte mods = 0;
    if ( ::GetKeyState( VK_MENU )    < 0 ) mods |= 2;
    if ( ::GetKeyState( VK_CONTROL ) < 0 ) mods |= 4;
    if ( ::GetKeyState( VK_SHIFT )   < 0 ) mods |= 1;
    if ( ::GetKeyState( VK_LWIN )    < 0 ) mods |= 8;

    s_capArmed = false;
    if ( vk == VK_ESCAPE && !mods )
        return true;                         // cancel: nothing captured
    s_capChord.vk   = (byte)vk;
    s_capChord.mods = mods;
    s_capDone       = true;
    if ( mods & 2 )
        s_eatAltUp = true;
    return true;
}
