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

#include <string.h>                        // strcmp — BindName

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

        Bind( table, count, 35022, 0x00, 0 );   // SetGrid1        -> unbound
        Bind( table, count, 35023, 0x00, 0 );   // SetGrid2        -> unbound
        Bind( table, count, 35024, 0x00, 0 );   // SetGrid4        -> unbound
        Bind( table, count, 35025, 0x00, 0 );   // SetGrid8        -> unbound
        Bind( table, count, 35026, 0x00, 0 );   // SetGrid16       -> unbound
        Bind( table, count, 33092, 0x53, 3 );   // PatchInspector  -> Shift+Alt+S (vacate Shift+S)
        // Alt+S is the audited destination that frees both S and Shift+S.
        Bind( table, count, 33041, 0x53, 2 );   // SurfaceInspector-> Alt+S  (frees S AND Shift+S)
        Bind( table, count, 32835, 0x52, 3 );   // ToggleTexRotateLock -> Shift+Alt+R
        // Interim destination; the final Alt+R write below frees Shift+R for Repeat.
        Bind( table, count, 32810, 0x52, 1 );   // MouseRotate     -> Shift+R     (frees R)
        Bind( table, count, 33104, 0x46, 2 );   // ViewFilters     -> Alt+F       (frees F)
        Bind( table, count, 33084, 0xDB, 5 );   // GridDown        -> Shift+Ctrl+[
        Bind( table, count, 33083, 0xDD, 5 );   // GridUp          -> Shift+Ctrl+]
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

        // Ctrl+1..4 are free because the classic bare digit rows were unbound above.
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

        // End displaces CenterView to free Shift+End; classic remains untouched.
        Bind( table, count, 32953, 0x23, 1 );                  // CenterView -> Shift+End
        Bind( table, count, KIWI_CMD_CAULK_FACES, 0x23, 0 );   // End -> Caulk Selection

        // Space displaces Clone to free Shift+Space; modal numeric input handles Space
        // before global lookup, preserving its feet/inches separator.
        Bind( table, count, 33001, 0x20, 1 );                  // Clone -> Shift+Space
        Bind( table, count, KIWI_CMD_VIEW_FACE, 0x20, 0 );     // Space -> View Face

        // V is command-local for movable pivots and must not gain a global row;
        // modal dispatch precedes hotkey lookup, leaving DragVertices available when idle.

    }

    // Defaults -> radiant.ini -> modern patch keeps switching idempotent.
    void Rebuild()
    {
        Radiant_ResetCommandBindings();
        Radiant_LoadCommandMap();
        if ( KiwiKeymap_Get() == KEYMAP_MODERN )
            ApplyModern();
        Radiant_RefreshMenuKeyBindings();
    }
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

// Boot already reset the table and loaded radiant.ini; only the profile patch remains.
void KiwiKeymap_ApplyBoot()
{
    if ( KiwiKeymap_Get() == KEYMAP_MODERN )
        ApplyModern();
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
            "End caulks the selection (View->Center moves to Shift+End).\n"
            "The displaced commands move to Shift/Alt chords, never dropped.\n"
            "Classic: the stock table plus your radiant.ini remaps, unchanged.\n"
            "Every new feature stays reachable from the command palette in both." );
}
