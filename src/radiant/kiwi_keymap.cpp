#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_keymap.cpp — RADIANT_UX_DESIGN §11 implementation.  See kiwi_keymap.h for
// the full remap table, the collision audit behind every destination, and the
// reset/reload/patch ordering that makes profile switching idempotent.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_keymap.h"
#include "kiwi_command.h"
#include "radiant_registry.h"

#include <string.h>                        // strcmp — BindName

// MUST MATCH mainfrm.cpp's definition verbatim (same reasoning as
// imgui_panel_commands.cpp:46 / kiwi_palette.cpp).
struct RadiantCommand { const char *name; byte vk; byte mods; int commandId; };

// ── mainfrm.cpp bindings — the // KIWI-UX accessors added for this file ─────
extern int  Radiant_GetCommandTableMutable( RadiantCommand **out );
extern void Radiant_ResetCommandBindings();
extern void Radiant_LoadCommandMap();          // radiant_frame.h — the ported 0x421230
// radiant_main.cpp — re-annotate the menu bar with the current bindings.
extern void Radiant_RefreshMenuKeyBindings();

namespace
{
    const char *KKEY_SECTION = "KiwiUX";
    int         s_profile = -1;                // -1 = not loaded yet

    // One rebinding: give command `commandId` the binding (vk, mods).
    // vk 0 = leave the command UNBOUND (the same way the default table marks e.g.
    // ToggleLightmapLock).  Silently no-ops for an id that is not in the table.
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

    // KIWI-UX (shakeout B): the same, keyed by NAME.  Needed for the one id that has
    // TWO rows — Delete Selection 33003, whose classic Backspace row must stay put
    // while its alias row ("KiwiDeleteSelection", kiwi_command.cpp) takes VK_DELETE.
    // Bind() above would always find the base row first and rebind the wrong one.
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

    // THE modern profile.  Order matters only for readability — every destination
    // was audited against the default table for a (vk, mods) collision, and the
    // displaced commands are moved rather than dropped (kiwi_keymap.h).
    void ApplyModern()
    {
        RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTableMutable( &table );
        if ( !table || count <= 0 )
            return;

        // 1. Move the classic occupants OUT of the keys the modern profile claims.
        //    Done FIRST so the table never transiently has two rows on one key
        //    (first-match-wins would otherwise pick whichever came earlier).
        Bind( table, count, 35022, 0x00, 0 );   // SetGrid1        -> unbound
        Bind( table, count, 35023, 0x00, 0 );   // SetGrid2        -> unbound
        Bind( table, count, 35024, 0x00, 0 );   // SetGrid4        -> unbound
        Bind( table, count, 35025, 0x00, 0 );   // SetGrid8        -> unbound
        Bind( table, count, 35026, 0x00, 0 );   // SetGrid16       -> unbound
        Bind( table, count, 33092, 0x53, 3 );   // PatchInspector  -> Shift+Alt+S (vacate Shift+S)
        // SHAKEOUT F moved SurfaceInspector AGAIN: shakeout C parked it on Shift+S,
        // and Shift+S is Plasticity's `command:curve`.  vk 0x53's FULL occupancy in
        // the default table: mods 0 SurfaceInspector 33041 (mainfrm.cpp:1003) ·
        // mods 1 PatchInspector 33092 (:1004) · mods 4 FileSave 57603 (:1093) ·
        // mods 5 MakeStructural 33043 (:1163).  In the MODERN profile mods 0 is
        // Scale and mods 3 is PatchInspector, so mods 2 (Alt+S) is the only chord
        // left on S at all — every non-Alt one is spoken for.  Alt+S therefore, with
        // the Alt-chord limitation already logged in RADIANT_KNOWN_ISSUES
        // (WM_SYSKEYDOWN is not routed to the hotkey table in the ImGui shell);
        // the Surface Inspector stays reachable from the Textures menu and the
        // palette, exactly like the other Alt destinations in this profile.
        Bind( table, count, 33041, 0x53, 2 );   // SurfaceInspector-> Alt+S  (frees S AND Shift+S)
        Bind( table, count, 32835, 0x52, 3 );   // ToggleTexRotateLock -> Shift+Alt+R
        // ROUND J moves this ONE MORE STEP, to Alt+R, further down this function —
        // Shift+R becomes Repeat Last Command.  The row is left here so the
        // shakeout-3 chain still reads in order; Bind is last-write-wins for an id,
        // and the later call is the one that lands.
        Bind( table, count, 32810, 0x52, 1 );   // MouseRotate     -> Shift+R     (frees R)
        Bind( table, count, 33104, 0x46, 2 );   // ViewFilters     -> Alt+F       (frees F)
        Bind( table, count, 33084, 0xDB, 5 );   // GridDown        -> Shift+Ctrl+[
        Bind( table, count, 33083, 0xDD, 5 );   // GridUp          -> Shift+Ctrl+]
        // Phase 3, the G chain.  G's classic occupant is VertEdit (33199, "Vertex
        // Edit", the CVertEditDlg panel) — audited against the whole default table:
        // 0x47 appears with mods 0 (VertEdit), 1 (AssociateEntities), 4
        // (SelectSnapPointsToGrid), 5 (DisassociateEntities) and 6 (ToggleSnapToGrid),
        // so mods 3 is the only free G chord.  Displaced with the SAME two-step shape
        // the S and R chains above use — occupant to Shift+key, whatever held
        // Shift+key to Shift+Alt+key — so the profile stays one readable rule.
        Bind( table, count, 33150, 0x47, 3 );   // AssociateEntities -> Shift+Alt+G
        Bind( table, count, 33199, 0x47, 1 );   // VertEdit          -> Shift+G   (frees G)

        // Shakeout A — the ARROW KEYS.  Classic binds the bare arrows to the tank
        // controls (mainfrm.cpp:1023-1026 -> Cmd_OnCameraLeft/Right yaw the camera
        // 22.5 degrees a press, Forward/Back hop it 32 units), which is precisely
        // the feel this round replaces.  Unbinding them here hands the bare arrows
        // to the modern fly (kiwi_camera.cpp KiwiCam_FlyTick, polled with
        // GetAsyncKeyState so it is smooth and dt-scaled rather than a per-press
        // hop).  Four rows only: every MODIFIED arrow chord (TexShift*, TexRotate*,
        // TexScale*, SelectNudge*, Vertex Select Up/Down) keeps its classic binding
        // in BOTH profiles, and the fly deliberately ignores modified arrows so the
        // two can never both fire.  The four commands stay reachable from the
        // palette, the menus, and the whole classic profile.
        Bind( table, count, 33057, 0x00, 0 );   // CameraLeft    -> unbound (modern)
        Bind( table, count, 33058, 0x00, 0 );   // CameraRight   -> unbound
        Bind( table, count, 33059, 0x00, 0 );   // CameraForward -> unbound
        Bind( table, count, 33060, 0x00, 0 );   // CameraBack    -> unbound

        // Shakeout B — the DELETE key.  USER DIRECTIVE: "Change backspace to delete
        // and make it work with the new 3d cam view."  vk 0x2E is NOT free in stock
        // Radiant: mainfrm.cpp:1056 binds it to "ZoomIn" 32995 (View->Zoom->XY Zoom
        // In), and Radiant_TryHotkey is first-match-wins over ROW ORDER, so any later
        // row on 0x2E would be permanently shadowed — pressing Delete zoomed the (now
        // hidden by default) 2D view instead of deleting anything.  Displaced with the
        // same two-step shape as the S / R / G chains: audited against the whole
        // default table, vk 0x2E carries only mods 0 (ZoomIn) and mods 4 (ZZoomIn
        // 32999, which is ALSO the Ctrl+Delete accelerator, res/radiant.rc:499), so
        // Shift+Delete is the free chord.  Insert/ZoomOut is left completely alone.
        Bind( table, count, 32995, 0x2E, 1 );   // ZoomIn (XY)   -> Shift+Delete (frees Delete)

        // Shakeout C — SHIFT+A, the §16b add menu.  USER DIRECTIVE: "shift-a to
        // create a line".  That is also what Plasticity itself binds
        // (plasticity/src/startup/default-keymap.ts:283, `shift-a` ->
        // `command:line`); KIWI opens the menu instead of one tool, because nine
        // creators cannot each own a chord (RADIANT_UX_DESIGN §16b.4).
        //
        // AUDIT of vk 0x41 across the WHOLE default table (mainfrm.cpp), all five
        // occupants:  mods 0 CameraAngleUp (33061, :1031) · mods 1 SelectAllOfType
        // (33093, :1149) · mods 2 SelectAllOfTypeRecurse (33212, :1150) · mods 4
        // ShowAllTextures (32973, :1148) · mods 5 AddTerrainRow (33153, :1000).
        // Shift+A is therefore TAKEN, and mods 3 (Shift+Alt+A) is the one free A
        // chord — the same destination the S / R / G chains use for a displaced
        // occupant.  One step, not two: the key being claimed IS the Shift chord,
        // so there is no second row to cascade.
        Bind( table, count, 33093, 0x41, 3 );   // SelectAllOfType -> Shift+Alt+A (frees Shift+A)

        // ═══════════════════════════════════════════════════════════════════════
        //  SHAKEOUT F — THE PLASTICITY CREATION CHORDS
        //
        //  USER DIRECTIVE, verbatim: "the shift-A menu is unacceptable.  Shift-A is
        //  for LINES.  Start the line tool immediately and the other ones are on
        //  other keys (lookup how plasticity keybinds work)."
        //
        //  Looked up, not guessed: plasticity/src/startup/default-keymap.ts, the
        //  `body:not([gizmo])` block, lines 283-292 —
        //      shift-a line   · shift-s curve  · shift-q corner-rectangle
        //      shift-w center-circle · shift-z sphere · shift-x cylinder
        //      shift-c corner-box    · shift-v center-box
        //  All eight ship.  The add menu KEEPS EXISTING (palette row, KIWI panel
        //  block, KIWI_CMD_ADD_MENU 34027 still dispatches) and only loses its
        //  binding — the directive is about what Shift+A does, not about deleting
        //  a menu somebody may still want.
        //
        //  Five of the eight collide with a compiled-in row and each occupant is
        //  DISPLACED with the house two-step (occupant -> Shift+Alt+key, which is
        //  the free chord on every one of them).  Three are free outright.  Every
        //  destination below was checked against the WHOLE default table, and the
        //  resulting per-key occupancy is written out in kiwi_keymap.h.
        // ═══════════════════════════════════════════════════════════════════════

        // Shift+S — vk 0x53 audited above; SurfaceInspector has just left it.
        // (No further cascade: mods 3 is PatchInspector, mods 2 is where 33041
        // went, and neither is what this claims.)

        // Shift+Q — vk 0x51's ONLY occupants are RemoveTerrainRow 33154 at mods 5
        // (mainfrm.cpp:1000) and LinkSelected 33211 at mods 2 (:1146).  mods 1 is
        // FREE: nothing is displaced.

        // Shift+Z — vk 0x5A carries CameraAngleDown 33062 at mods 0 (:1032),
        // Undo 57643 at mods 4 (:1120) and ToggleZ 33070 at mods 5 (:1071).
        // mods 1 is FREE: nothing is displaced.  (Ctrl+Z is intercepted upstream in
        // Radiant_PreTranslateMessage before the table is consulted at all, so
        // claiming Shift+Z cannot disturb undo.)

        // Shift+W — TAKEN by TogglePatchWireframes 32857 (mainfrm.cpp:1019).
        // 0x57's occupancy: mods 0 ConnectSelection 33021 (:1078) · mods 1
        // TogglePatchWireframes · mods 2 SplaySelection 33157 (:1082) · mods 5
        // MakeWeaponClip 196 (:1087).  mods 3 is the free W chord.
        Bind( table, count, 32857, 0x57, 3 );   // TogglePatchWireframes -> Shift+Alt+W

        // Shift+X — TAKEN by ToggleCrosshairs 33100 (mainfrm.cpp:1063).
        // 0x58's occupancy: mods 0 ToggleClipper 32783 (:1062) · mods 1
        // ToggleCrosshairs · mods 2 Center2DOnCamera 33108 (:1157) · mods 4
        // SelectedAssociated 33152 (:1136) · mods 5 SplitPatch 33158 (:1002).
        // mods 3 is the free X chord.
        Bind( table, count, 33100, 0x58, 3 );   // ToggleCrosshairs -> Shift+Alt+X

        // ═══════════════════════════════════════════════════════════════════════
        //  ROUND S — Ctrl+X IS CUT (kiwi_command.h KIWI_CMD_CLIP_CUT)
        //
        //  USER DIRECTIVE, verbatim: "Ctrl-X should not quit the app!!!"  It did:
        //  res/radiant.rc's IDR_MAIN_ACCEL bound Ctrl+X to 32951 = ID_FILE_EXIT_RAD.
        //  That accelerator ENTRY is deleted (the .rc carries the // KIWI-UX fence),
        //  which is what lets Ctrl+X reach this table at all — TranslateAccelerator
        //  runs BEFORE Radiant_TryHotkey (radiant_main.cpp:651 then :675), so while
        //  the entry existed no binding here could ever have been seen.
        //
        //  AUDIT of vk 0x58 in the DEFAULT table, restated for this chord:
        //      mods 0  ToggleClipper       32783  (mainfrm.cpp:1067)
        //      mods 1  ToggleCrosshairs    33100  (:1068)  -> Shift+Alt+X above
        //      mods 2  Center2DOnCamera    33108  (:1158)
        //      mods 4  SelectedAssociated  33152  (:1135)  <- the chord Cut claims
        //      mods 5  SplitPatch          33158  (:1008)
        //  With mods 1 and 3 now spoken for, the free X chords are 6 and 7.
        //  SelectedAssociated moves to Ctrl+Alt+X (mods 6) — the same shape the S
        //  and R chains use, and with the same already-logged Alt-chord caveat
        //  (RADIANT_KNOWN_ISSUES: WM_SYSKEYDOWN is not routed to the hotkey table in
        //  the ImGui shell, so Alt destinations are reachable from the menus and the
        //  §15 palette rather than from the keyboard).  Moved FIRST so the table
        //  never transiently carries two rows on Ctrl+X.
        Bind( table, count, 33152, 0x58, 6 );   // SelectedAssociated -> Ctrl+Alt+X
        // ── KIWI-UX (ROUND U): Ctrl+X IS UNBOUND AGAIN ─────────────────────
        //  USER DIRECTIVE, verbatim: "Cut should only be on (C), not ctrl-X."
        //
        //  Round S read "Ctrl-X should not quit the app!!!" as a request for a
        //  clipboard cut, and it is not: with C already meaning the CUT TOOL (the
        //  cut-a-solid-along-a-line verb, KIWI_CMD_CUT, bound below), a second
        //  "Cut" on Ctrl+X made the same word mean two unrelated things one chord
        //  apart.  The user read the Ctrl+X result as the Cut TOOL misbehaving,
        //  which is exactly the confusion two same-named commands produce.
        //
        //  WHAT SURVIVES AND WHAT DOES NOT:
        //    * the .rc accelerator entry stays DELETED — Ctrl+X must never quit,
        //      and that is the whole of what the round-S directive asked for.
        //      With the entry gone AND no row here, Ctrl+X now does NOTHING;
        //    * KIWI_CMD_CLIP_CUT itself is untouched and still palette-reachable
        //      by name ("Cut (Ctrl+X)" -> renamed "Cut to Clipboard" in
        //      kiwi_command.cpp, since the chord is no longer part of its name);
        //    * SelectedAssociated stays on Ctrl+Alt+X.  It is NOT moved back to
        //      Ctrl+X: round S's displacement is already in shipped .ini files and
        //      un-displacing it would make the profile depend on which round a
        //      user's radiant.ini was written in.  The chord is free, and free is
        //      what "unbind Ctrl+X entirely" asks for.
        //  The CLASSIC profile never bound it either (kiwi_command.cpp's
        //  Radiant_RegisterCommand for KiwiClipCut registers vk 0), so after this
        //  the id carries no binding in either profile.
        Bind( table, count, KIWI_CMD_CLIP_CUT, 0x00, 0 );           // (unbound)

        // Shift+C — TAKEN by CapCurrentCurve 32885 (mainfrm.cpp:1161).
        // 0x43's occupancy: mods 0 CameraDown 33056 (:1030) · mods 1
        // CapCurrentCurve · mods 2 AutoCaulk 33220 (:984) · mods 4 Copy 33039
        // (:1117) · mods 5 ToggleCamera 33069 (:1069).  mods 3 is the free C chord.
        Bind( table, count, 32885, 0x43, 3 );   // CapCurrentCurve -> Shift+Alt+C

        // Shift+V — TAKEN by VehicleGroup 33221 (mainfrm.cpp:1131).
        // 0x56's occupancy: mods 0 DragVertices 33005 (:1045) · mods 1
        // VehicleGroup · mods 4 Paste 33040 (:1118) · mods 5 ToggleView 33071
        // (:1070).  mods 3 is the free V chord.
        Bind( table, count, 33221, 0x56, 3 );   // VehicleGroup -> Shift+Alt+V

        // 2. Claim the freed keys for the KIWI commands.
        Bind( table, count, KIWI_CMD_SELMODE_POINT,  0x31, 0 );   // 1
        Bind( table, count, KIWI_CMD_SELMODE_EDGE,   0x32, 0 );   // 2
        Bind( table, count, KIWI_CMD_SELMODE_FACE,   0x33, 0 );   // 3
        Bind( table, count, KIWI_CMD_SELMODE_OBJECT, 0x34, 0 );   // 4
        Bind( table, count, KIWI_CMD_SELMODE_ALL,    0x35, 0 );   // 5
        Bind( table, count, KIWI_CMD_PALETTE,        0x46, 0 );   // F
        Bind( table, count, KIWI_CMD_GRID_HALVE,     0xDB, 0 );   // [
        Bind( table, count, KIWI_CMD_GRID_DOUBLE,    0xDD, 0 );   // ]

        // §13's transforms (Phase 3).  These are the keys freed above: G by the
        // VertEdit displacement, R by MouseRotate -> Shift+R, S by SurfaceInspector
        // (which shakeout C moved to Shift+S and shakeout F moved on to Alt+S when
        // Shift+S was claimed for the spline tool).
        Bind( table, count, KIWI_CMD_MOVE,           0x47, 0 );   // G
        Bind( table, count, KIWI_CMD_ROTATE,         0x52, 0 );   // R
        Bind( table, count, KIWI_CMD_SCALE,          0x53, 0 );   // S

        // Shakeout B: Delete -> Delete Selection, through the ALIAS row so the base
        // 33003 row keeps its Backspace binding.  BOTH keys therefore delete in the
        // modern profile, which is what the directive asked for ("change backspace to
        // delete" without taking the muscle-memory key away).
        BindName( table, count, "KiwiDeleteSelection", 0x2E, 0 );   // Delete -> 33003

        // ── SHAKEOUT F: the eight Plasticity creation chords ────────────────
        // The audit for every one of these is in step 1 above; this is only the
        // claim.  Shift+A goes STRAIGHT TO THE LINE TOOL — the directive's whole
        // point — and the add menu is UNBOUND rather than deleted: it stays in the
        // palette ("Add Menu (create)") and in the KIWI panel's Construct block,
        // and radiant.ini [Commands] can still give it a key by name.
        Bind( table, count, KIWI_CMD_ADD_MENU,        0x00, 0 );    // (unbound)
        Bind( table, count, KIWI_CMD_DRAW_LINE,       0x41, 1 );    // Shift+A  line
        Bind( table, count, KIWI_CMD_DRAW_SPLINE,     0x53, 1 );    // Shift+S  curve
        Bind( table, count, KIWI_CMD_DRAW_RECT,       0x51, 1 );    // Shift+Q  corner rect
        Bind( table, count, KIWI_CMD_PRIM_SPHERE,     0x5A, 1 );    // Shift+Z  sphere
        Bind( table, count, KIWI_CMD_PRIM_CYLINDER,   0x58, 1 );    // Shift+X  cylinder
        // ── KIWI-UX (ROUND AG, ITEM 4): SHIFT+C IS THE CIRCLE ────────────────
        // USER DIRECTIVE, verbatim: "Shift-c should be circle, change the keybinds
        // so it is so."
        //
        // THIS OVERRIDES PLASTICITY, KNOWINGLY.  Their table is
        //     shift-c -> command:corner-box   (default-keymap.ts:288)
        //     shift-v -> command:center-box   (default-keymap.ts:289)
        // and shakeout F reproduced it key for key.  The user's own hand wins over
        // an upstream convention; §16b's job was to give KIWI a coherent creation
        // table, not to make it byte-identical to another editor's.
        //
        // IT IS A STRAIGHT SWAP, NOT A DISPLACEMENT: the circle vacates Shift+W and
        // the corner box takes it, so no third command has to move and no chord is
        // dropped.  vk 0x57 (W) occupancy, re-audited in full for this round:
        // mods 0 ConnectSelection 33021 (mainfrm.cpp) · mods 1 was the circle, now
        // the corner box · mods 2 SplaySelection 33157 · mods 3
        // TogglePatchWireframes 32857 (displaced there at :203) · mods 5
        // MakeWeaponClip 196.  vk 0x43 (C): mods 0 KIWI_CMD_CUT (:404) · mods 1 was
        // the corner box, now the circle · mods 2 AutoCaulk 33220 · mods 3
        // CapCurrentCurve 32885 (:267) · mods 4 Copy 33039 · mods 5 ToggleCamera
        // 33069 · mods 6 CameraDown 33056 (displaced there at :403).  NEITHER key
        // appears in res/radiant.rc's accelerator table (:506-516 is Ctrl+O/S/L/P/
        // K/M and Ctrl+Delete / Ctrl+Insert and nothing else), which matters
        // because TranslateAccelerator runs BEFORE the hotkey table
        // (radiant_main.cpp:651 then :675).
        //
        // The centre pairings are UNCHANGED and still read left-to-right: the
        // centre box keeps Shift+V and the centre rect keeps Alt+V, below.  What
        // the swap costs is the corner-box / centre-box adjacency (C/V) — the
        // corner box is now on W, one key from its own Shift+Q corner rect, which
        // is arguably the better grouping anyway (the two CORNER draws together).
        Bind( table, count, KIWI_CMD_DRAW_CIRCLE,     0x43, 1 );    // Shift+C  centre circle
        Bind( table, count, KIWI_CMD_PRIM_BOX,        0x57, 1 );    // Shift+W  corner box
        // ── KIWI-UX (ROUND AF, ITEM 8): THE DEVIATION IS OVER ───────────────
        // Shakeout F wrote, here: "Plasticity's shift-v is `command:center-box` and
        // KIWI HAS NO CENTRE-BOX primitive (§16b.3 lists it as LATER — it is a
        // two-line variant of Box, but it does not exist today).  Binding Rectangle
        // (CENTRE) here instead keeps the corner/centre PAIRING the neighbouring
        // keys teach… and means the chord will not have to move when the centre box
        // does ship."
        //
        // USER DIRECTIVE, verbatim: "Add an option to the Box command that allows it
        // to be a 'Center' box instead of the current 'Corner box' behavior."
        //
        // It has shipped (kiwi_primitive.cpp KPRIM_BOX_CENTER), so Shift+V is
        // claimed for it exactly as that note promised, and Plasticity's pairing is
        // now reproduced key for key:
        //     shift-c -> command:corner-box   (default-keymap.ts:288)
        //     shift-v -> command:center-box   (default-keymap.ts:289)
        //
        // THE CENTRE RECT MOVES TO ALT+V, and nothing else is disturbed.  vk 0x56's
        // occupancy, re-audited in full for this round: mods 0 DragVertices 33005
        // (mainfrm.cpp:1045) · mods 1 was the centre rect, now the centre box ·
        // mods 3 VehicleGroup 33221 (displaced there by shakeout F, above) ·
        // mods 4 Paste 33040 (:1118) · mods 5 ToggleView 33071 (:1070).  **mods 2
        // (Alt) is FREE**, and it is not in res/radiant.rc's accelerator table
        // either (:508-515 — Ctrl+O/S/L/P/K/M and Ctrl+Delete / Ctrl+Insert), which
        // matters because TranslateAccelerator runs BEFORE the hotkey table.  So the
        // centre rect is DISPLACED, not deleted, it keeps the V key its corner/centre
        // pairing was about, and it is still in the palette and the Shift+A add menu
        // by name.
        Bind( table, count, KIWI_CMD_DRAW_RECT_CENTER, 0x56, 2 );   // Alt+V    centre rect
        Bind( table, count, KIWI_CMD_PRIM_BOX_CENTER,  0x56, 1 );   // Shift+V  centre box

        // ── SHAKEOUT F: CTRL+J, "Join Lines" (kiwi_conselect.h) ─────────────
        // Plasticity binds JOIN to a BARE j (`"j": "command:join-curves"`,
        // default-keymap.ts:261).  KIWI does not, and this is the one place this
        // round deliberately parts company with it: vk 0x4A mods 0 is
        // ToggleOutlineDraw 33103 (mainfrm.cpp:982) — a display toggle a mapper
        // reaches for constantly — and displacing it to buy a chord that Ctrl+J
        // provides for free is a bad trade.
        // AUDIT of vk 0x4A across the WHOLE default table: mods 0 ToggleOutlineDraw
        // 33103 (:982) · mods 1 ToggleTintDraw 33172 (:983) · mods 5 TolerantWeld
        // 33155 (:1007).  mods 4 is FREE, and Ctrl+J is not in the resource
        // accelerator table either (res/radiant.rc:490-501 — Ctrl+X/O/S/L/P/K/M and
        // Ctrl+Delete / Ctrl+Insert, and TranslateAccelerator runs BEFORE the
        // hotkey table, so an accelerator collision would have been invisible).
        // NOTHING is displaced.
        Bind( table, count, KIWI_CMD_CONSTRUCT_JOIN, 0x4A, 4 );     // Ctrl+J

        // Shakeout D — CTRL+1..CTRL+4, the selection conversion (kiwi_selconv.h).
        // USER DIRECTIVE: "in plasticity […] you can use Ctrl+2 to convert the
        // selection to edges, Ctrl+3 convert it to faces, that doesn't work here."
        // Plasticity binds exactly these four (default-keymap.ts:247-250), which is
        // also why they sit next to the plain 1..4 mode keys claimed above.
        //
        // AUDIT of vk 0x31..0x34 across the WHOLE default table (mainfrm.cpp:1037-
        // 1041): the ONLY occupants are SetGrid1/2/4/8 at mods 0, which this profile
        // already unbinds at the top of this function to free the bare digits.  There
        // is NO mods-4 row on any digit, and no Ctrl+digit accelerator in the
        // resource table either (res/radiant.rc:490-501 — the nine accelerators are
        // Ctrl+X/O/S/L/P/K/M and Ctrl+Delete / Ctrl+Insert).  So Ctrl+1..4 are free
        // and NOTHING is displaced: no two-step is needed for the first time in this
        // profile, and 0x35 (5) keeps its bare mode-set binding with no Ctrl partner.
        Bind( table, count, KIWI_CMD_SELCONV_POINT,  0x31, 4 );     // Ctrl+1
        Bind( table, count, KIWI_CMD_SELCONV_EDGE,   0x32, 4 );     // Ctrl+2
        Bind( table, count, KIWI_CMD_SELCONV_FACE,   0x33, 4 );     // Ctrl+3
        Bind( table, count, KIWI_CMD_SELCONV_OBJECT, 0x34, 4 );     // Ctrl+4

        // ═══════════════════════════════════════════════════════════════════════
        //  SHAKEOUT G — THE PLASTICITY MODELLING VERBS: C · Z · J · E · Ctrl+R
        //
        //  Four of the five are BARE keys Plasticity itself binds bare
        //  (default-keymap.ts:257 `c` cut, :258 `j` join-curves, :264 `e` extrude);
        //  Z and Ctrl+R are KIWI's own (Plasticity has no match-face verb at all
        //  and its split factory is dead code from the UI — kiwi_matchface.h /
        //  kiwi_split.h).  Every one of them is TAKEN in stock Radiant, so every one
        //  gets the house two-step, and every destination below was checked against
        //  the WHOLE 187-row default table AND against the chords this profile has
        //  already claimed above.  The per-key occupancy tables are in kiwi_keymap.h.
        // ═══════════════════════════════════════════════════════════════════════

        // ── C (0x43) -> Cut ─────────────────────────────────────────────────
        // Occupancy: mods 0 CameraDown 33056 (mainfrm.cpp:1030) · 1 CapCurrentCurve
        // 32885 (:1151, moved to mods 3 above by shakeout F) · 2 AutoCaulk 33220
        // (:985) · 4 Copy 33039 (:1099) · 5 ToggleCamera 33069 (:1070).  After this
        // profile's earlier patches, mods 3 is CapCurrentCurve and mods 1 is the
        // Box primitive, so the free C chords are 6 and 7.
        // CameraDown goes to Ctrl+Alt+C (mods 6).  It is one of the classic TANK
        // CAMERA keys — the same family whose four bare arrows this profile already
        // UNBINDS in favour of the smooth fly (kiwi_camera.cpp), and whose modern
        // equivalent is the fly's own Q/E — so it is displaced rather than kept on a
        // prime key.  It stays on the Camera menu and in the palette.
        Bind( table, count, 33056, 0x43, 6 );   // CameraDown -> Ctrl+Alt+C (frees C)
        Bind( table, count, KIWI_CMD_CUT, 0x43, 0 );                // C

        // ── Z (0x5A) -> Match Face ──────────────────────────────────────────
        // Occupancy: mods 0 CameraAngleDown 33062 (:1032) · 4 Undo 57643 (:1101) ·
        // 5 ToggleZ 33070 (:1074); mods 1 is the Sphere primitive (shakeout F).
        // mods 2 and 3 are free.  CameraAngleDown is the other half of the tank
        // camera and goes to Shift+Alt+Z (mods 3), the same destination shape every
        // other displacement in this profile uses.
        // (Ctrl+Z never reaches this table at all — Radiant_PreTranslateMessage
        // handles undo upstream, radiant_main.cpp:661-665 — so claiming Z cannot
        // disturb it.)
        Bind( table, count, 33062, 0x5A, 3 );   // CameraAngleDown -> Shift+Alt+Z (frees Z)
        Bind( table, count, KIWI_CMD_MATCH_FACE, 0x5A, 0 );         // Z

        // ── J (0x4A) -> Join ────────────────────────────────────────────────
        // Occupancy: mods 0 ToggleOutlineDraw 33103 (:982) · 1 ToggleTintDraw 33172
        // (:983) · 5 TolerantWeld 33155 (:1007); mods 4 is Join Lines (shakeout F).
        // mods 2, 3, 6, 7 free.  ToggleOutlineDraw -> Shift+Alt+J (mods 3).
        //
        // SHAKEOUT F EXPLICITLY DECLINED THIS TRADE ("displacing ToggleOutlineDraw
        // to buy a chord Ctrl+J gives away free is a bad trade") and shakeout G
        // reverses it, because the directive names the bare key and because J is now
        // a CONTEXT verb rather than a lines-only one — it is the merge key as well
        // as the join key, so it earns a prime binding.  CTRL+J IS LEFT EXACTLY AS
        // IT WAS: KIWI_CMD_CONSTRUCT_JOIN keeps it as the lines-only alias, so
        // nothing shakeout F taught stops working and nothing becomes unreachable.
        Bind( table, count, 33103, 0x4A, 3 );   // ToggleOutlineDraw -> Shift+Alt+J (frees J)
        Bind( table, count, KIWI_CMD_JOIN, 0x4A, 0 );               // J

        // ── E (0x45) -> Extrude (face / region, resolved by context) ────────
        // Occupancy: mods 0 DragEdges 33006 (:1046) · 1 RedisperseRows 32888 (:1009)
        // · 4 SelectTargettedEntities 36110 (:1081) · 5 RedisperseCols 32889 (:1010)
        // · 6 SelectConnectedEntities 33134 (:1080).  mods 2, 3, 7 free.
        // DragEdges -> Shift+Alt+E (mods 3), the house destination.
        // NOTE: E is ALSO one of the six keys the RMB-mouselook fly swallows
        // (kiwi_camera.cpp KiwiCam_FlySwallowKey, W/A/S/D/Q/E).  That is not a
        // collision: the fly polls with GetAsyncKeyState and its swallow rung sits
        // BELOW the active-command arm in KiwiUX_KeyFunnel, so E extrudes when the
        // user is not flying and flies when they are — exactly as S already both
        // scales and strafes.
        Bind( table, count, 33006, 0x45, 3 );   // DragEdges -> Shift+Alt+E (frees E)
        Bind( table, count, KIWI_CMD_EXTRUDE_FACE, 0x45, 0 );       // E

        // ── Ctrl+R (0x52, mods 4) -> Split Face ─────────────────────────────
        // Occupancy of 0x52: mods 0 MouseRotate 32810 (:1098, moved to Shift+R
        // above) · 1 ToggleTexRotateLock 32835 (:1065, moved to Shift+Alt+R above) ·
        // 4 RemoveColorNode 10 (:1067).  After this profile: 0 = Rotate, 1 =
        // MouseRotate, 3 = ToggleTexRotateLock, 4 = RemoveColorNode.  Free: 2, 5,
        // 6, 7.  RemoveColorNode -> Shift+Ctrl+R (mods 5).
        // Ctrl+R is NOT in res/radiant.rc's accelerator table (:490-501 — Ctrl+X/O/
        // S/L/P/K/M and Ctrl+Delete / Ctrl+Insert), which matters because
        // TranslateAccelerator runs BEFORE the hotkey table and a collision there
        // would have been silent.
        Bind( table, count, 10, 0x52, 5 );      // RemoveColorNode -> Shift+Ctrl+R
        Bind( table, count, KIWI_CMD_SPLIT_FACE, 0x52, 4 );         // Ctrl+R

        // ═══════════════════════════════════════════════════════════════════════
        //  SHAKEOUT H — T -> TRIM (kiwi_trim.h)
        //
        //  USER DIRECTIVE: "Add a Trim tool (T) that only works on lines."  That is
        //  also the key Plasticity uses, bare: `"t": "command:trim"` in the
        //  `body:not([gizmo])` block of src/startup/default-keymap.ts:267.
        //
        //  AUDIT of vk 0x54 across the WHOLE default table:
        //      mods 0  ViewTextures       33018  (mainfrm.cpp:998)
        //      mods 1  ToggleTexMoveLock  32785  (mainfrm.cpp:1064)
        //      mods 5  ThickenPatch       32904  (mainfrm.cpp:999)
        //  mods 2, 3, 4, 6, 7 are free.  ViewTextures takes the house two-step to
        //  Shift+Alt+T (mods 3) — the same destination every other displacement in
        //  this profile uses — and stays reachable from the Textures menu, the
        //  Windows menu and the palette.  ToggleTexMoveLock (Shift+T) and
        //  ThickenPatch (Shift+Ctrl+T) are untouched.
        //
        //  T is NOT in res/radiant.rc's accelerator table (:490-501 — Ctrl+X/O/S/L/
        //  P/K/M and Ctrl+Delete / Ctrl+Insert), which matters because
        //  TranslateAccelerator runs BEFORE the hotkey table and a collision there
        //  would have been silent.
        //
        //  T is also not one of the six keys the RMB-mouselook fly swallows
        //  (W/A/S/D/Q/E — kiwi_camera.cpp KiwiCam_FlySwallowKey), so unlike E there
        //  is nothing to arbitrate.
        //
        //  FINAL OCCUPANCY on 0x54 after this profile: 0 = Trim, 1 =
        //  ToggleTexMoveLock, 3 = ViewTextures, 5 = ThickenPatch.  No (vk, mods)
        //  pair carries two rows.
        // ═══════════════════════════════════════════════════════════════════════
        Bind( table, count, 33018, 0x54, 3 );   // ViewTextures -> Shift+Alt+T (frees T)
        Bind( table, count, KIWI_CMD_TRIM, 0x54, 0 );              // T

        // ═══════════════════════════════════════════════════════════════════════
        //  ROUND J — THE LAST PLASTICITY VERBS
        //
        //  O · Shift+D · / · Shift+R and the H-FAMILY SWAP are all keys PLASTICITY
        //  ITSELF uses (default-keymap.ts:268 / :280 / :327 / :304 / :296-299, each
        //  cited again in the feature's own header).  B is KIWI's own — Plasticity's
        //  `b` is the SOLID fillet KIWI does not have, and its CURVE fillet lives
        //  inside modify-contour, which has no top-level chord (kiwi_fillet.h).
        //
        //  Every per-key occupancy table is in kiwi_keymap.h; the displacements
        //  below all take the house two-step to Shift+Alt+key, which is free on all
        //  three of them.
        // ═══════════════════════════════════════════════════════════════════════

        // ── O (0x4F) -> Offset Curve ────────────────────────────────────────
        // Occupancy: 0 ViewConsole 33016 · 1 LinkSelectionToggle 1085 ·
        // 2 ExtrudeTerrainRow 33192 · 4 FileOpen 57601.  mods 3/5/6/7 free.
        // Ctrl+O is NOT touched: FileOpen is also a resource ACCELERATOR
        // (res/radiant.rc:493), and TranslateAccelerator runs before the hotkey
        // table, so moving it here would have been silently overridden.
        // The console keeps the Inspector menu, the palette, and — in the modern
        // layout — its own ImGui window on KIWI_CMD_WINDOW_CONSOLE.
        Bind( table, count, 33016, 0x4F, 3 );   // ViewConsole -> Shift+Alt+O (frees O)
        Bind( table, count, KIWI_CMD_OFFSET_CURVE, 0x4F, 0 );      // O

        // ── B (0x42) -> Fillet Corners ──────────────────────────────────────
        // Occupancy: 0 SameTargetname 36121 · 1 FitBrush 33098 · 4 SameTarget
        // 36123.  mods 2/3/5/6/7 free.  SameTargetname is a select-by-key helper
        // on the Selection menu, not a per-second key.
        Bind( table, count, 36121, 0x42, 3 );   // SameTargetname -> Shift+Alt+B (frees B)
        Bind( table, count, KIWI_CMD_FILLET_CURVE, 0x42, 0 );      // B

        // ── Shift+D (0x44, mods 1) -> Duplicate ─────────────────────────────
        // Occupancy: 0 CameraUp 33055 · 1 RotateZ 32961 · 5 MakeDetail 33042 ·
        // 7 DropVertices 33213.  mods 2/3/4/6 free.  RotateZ keeps its Brush ->
        // Rotate menu item and its palette row.
        Bind( table, count, 32961, 0x44, 3 );   // RotateZ -> Shift+Alt+D (frees Shift+D)
        Bind( table, count, KIWI_CMD_DUPLICATE, 0x44, 1 );         // Shift+D

        // ── "/" (0xBF) -> Focus On Selection ────────────────────────────────
        // Occupancy: mods 4 ToggleTurnTerrainEdges 33141 and nothing else.
        // mods 0 is FREE — nothing is displaced.  The ported key-NAME table has no
        // 0xBF entry, so this row cannot be renamed from radiant.ini and the
        // command-list panel prints a raw glyph for it; both are logged in
        // RADIANT_KNOWN_ISSUES and neither affects the binding itself.
        Bind( table, count, KIWI_CMD_FOCUS_SELECTION, 0xBF, 0 );   // /

        // ── THE HIDE FAMILY (0x48) -> Plasticity's order ────────────────────
        // Classic Radiant's Shift+H is Show Hidden and its Alt+H is Hide
        // Unselected; Plasticity has them the other way round
        // (default-keymap.ts:297-298, Menu.ts:50-49).  The two ids simply TRADE
        // chords — nothing is displaced, nothing becomes unreachable, and Shift+H
        // then means ISOLATE, which is what a mapper reaches for.
        // Ctrl+H (mods 4) is free and takes the KIWI-owned Invert Hidden, the one
        // member of the family Radiant never had (kiwi_visibility.h).
        Bind( table, count, 32934, 0x48, 1 );   // HideUnSelected -> Shift+H (isolate)
        Bind( table, count, 32924, 0x48, 2 );   // ShowHidden     -> Alt+H
        Bind( table, count, KIWI_CMD_HIDE_INVERT, 0x48, 4 );       // Ctrl+H

        // ── Shift+R (0x52, mods 1) -> Repeat Last Command ───────────────────
        // THE ROUND'S ONE UNCOMFORTABLE TRADE, made with eyes open (kiwi_keymap.h):
        // MouseRotate was parked on Shift+R by this same profile when it took bare
        // R for the real Rotate command, and it now moves ONE more step to Alt+R
        // (mods 2 — the only non-Alt-free chord left on R after Rotate, Repeat,
        // ToggleTexRotateLock, Split Face and RemoveColorNode).  MouseRotate is the
        // CLASSIC 2D-view rotate MODE that bare R has already superseded, and it
        // stays on the Selection menu and in the palette; Alt+R inherits the
        // Alt-chord limitation already logged in RADIANT_KNOWN_ISSUES.
        Bind( table, count, 32810, 0x52, 2 );   // MouseRotate -> Alt+R (frees Shift+R)
        Bind( table, count, KIWI_CMD_REPEAT_LAST, 0x52, 1 );       // Shift+R

        // ═══════════════════════════════════════════════════════════════════════
        //  ROUND L — Q -> BOOLEAN (kiwi_boolean.h)
        //
        //  USER DIRECTIVE: "Add a new feature for solids(brush).  Boolean and
        //  difference. (Q)."  It is also PLASTICITY'S OWN key, bare:
        //  src/startup/default-keymap.ts:294 `"q": "command:boolean",` inside the
        //  `body:not([gizmo])` block.
        //
        //  AUDIT of vk 0x51 across BOTH tables and the accelerators:
        //    * g_radiantCommandsDefault (mainfrm.cpp, the binary's 187 rows):
        //        mods 2  LinkSelected      33211  (mainfrm.cpp:1143)
        //        mods 5  RemoveTerrainRow  33154  (mainfrm.cpp:1007)
        //      mods 0 IS FREE — the classic profile has no bare Q at all.
        //    * g_radiantCommandsKiwiDefault (the KIWI extension block's registered
        //      defaults, mainfrm.cpp:1208): every KIWI row registers vk 0 (unbound)
        //      and gets its chord from THIS function, so the only 0x51 claim in it
        //      is the one this profile makes below — Shift+Q (mods 1) for the
        //      corner Rectangle, taken in shakeout F (:257).
        //    * res/radiant.rc's IDR_MAIN_ACCEL (:490-501) is Ctrl+X/O/S/L/P/K/M and
        //      Ctrl+Delete / Ctrl+Insert.  No Q.  This matters because
        //      TranslateAccelerator runs BEFORE the hotkey table and a collision
        //      there would have been silent.
        //  NOTHING IS DISPLACED — the first key in three rounds that needs no
        //  two-step.  FINAL OCCUPANCY on 0x51: 0 = Boolean, 1 = Rectangle,
        //  2 = LinkSelected, 5 = RemoveTerrainRow.  No (vk, mods) pair carries two.
        //
        //  Q IS one of the six keys the RMB-mouselook fly swallows (W/A/S/D/Q/E —
        //  kiwi_camera.cpp KiwiCam_FlySwallowKey; Q/E are its up/down pair).  That
        //  is NOT a collision, and it is the same arbitration E already lives with
        //  (see the E block above): the fly polls with GetAsyncKeyState and its
        //  swallow rung sits BELOW the active-command arm in KiwiUX_KeyFunnel, so Q
        //  starts a boolean when the user is not flying and descends when they are.
        //  It is also why the LIVE toggle is Q and not Plasticity's q/w/e trio —
        //  kiwi_boolean.h deviation 1.
        // ═══════════════════════════════════════════════════════════════════════
        Bind( table, count, KIWI_CMD_BOOLEAN, 0x51, 0 );           // Q

        // ═══════════════════════════════════════════════════════════════════════
        //  ROUND AF, ITEM 6 — L -> LOFT
        //
        //  USER DIRECTIVE, verbatim: "You select 1 face then press L and select the
        //  other face."  Plasticity binds the same key, bare:
        //  `"l": "command:loft"` (default-keymap.ts:270).
        //
        //  vk 0x4C's occupancy, over the WHOLE default table:
        //      mods 0  ToggleLayers   33954  (mainfrm.cpp:1160)   <- displaced
        //      mods 1  TexLayerCycle  33238  (mainfrm.cpp:1207)
        //      mods 5  PrevLeakSpot   33025  (mainfrm.cpp:1182)
        //  and res/radiant.rc's accelerator table (:508-515) binds Ctrl+L to 33024,
        //  which matters because TranslateAccelerator runs BEFORE the hotkey table —
        //  so mods 4 is spoken for even though the command table is silent on it.
        //  FREE: mods 2 (Alt), mods 3 (Shift+Alt), mods 6 (Ctrl+Alt).
        //
        //  ToggleLayers goes to Shift+Alt+L, which is the same landing spot and the
        //  same reasoning shakeout F used for CapCurrentCurve (Shift+Alt+C) and
        //  VehicleGroup (Shift+Alt+V): a rarely-pressed display toggle keeps a chord
        //  nobody's fingers are on, and the modelling verb takes the bare key.  The
        //  Layers panel is also reachable from the Windows menu, so the binding is
        //  not its only route.
        Bind( table, count, 33954, 0x4C, 3 );   // ToggleLayers -> Shift+Alt+L (frees L)
        Bind( table, count, KIWI_CMD_LOFT, 0x4C, 0 );              // L

        // ═══════════════════════════════════════════════════════════════════════
        //  ROUND N — PAGE UP / PAGE DOWN -> GRID SPACING
        //
        //  USER DIRECTIVE, verbatim: "the gridsize needs to be changeable.  Put
        //  that in the top right somewhere and maybe put it on pageup/pagedown if
        //  they aren't taken."
        //
        //  THEY ARE TAKEN — the audit, over the WHOLE default table:
        //      vk 0x21 (PageUp)    mods 0  UpFloor      32954  (mainfrm.cpp:1064)
        //                          mods 2  LeavePrefab  33174  (mainfrm.cpp:1160)
        //      vk 0x22 (PageDown)  mods 0  DownFloor    32955  (mainfrm.cpp:1065)
        //                          mods 2  EnterPrefab  33173  (mainfrm.cpp:1159)
        //  and NOTHING else on either key in either table; the KIWI extension block
        //  registers every row unbound, so its only claims are the ones made here.
        //  Neither key is in res/radiant.rc's IDR_MAIN_ACCEL (:490-501 — Ctrl+X/O/
        //  S/L/P/K/M and Ctrl+Delete / Ctrl+Insert), which matters because
        //  TranslateAccelerator runs BEFORE the hotkey table and a collision there
        //  would have been silent.  Neither is one of the six keys the RMB fly
        //  swallows (W/A/S/D/Q/E), so there is nothing to arbitrate.
        //
        //  So the house two-step applies, exactly as it has for S/R/G/C/Z/J/E/T/O/B:
        //  the occupants move to Shift+Alt+key (mods 3, free on both), and they stay
        //  reachable from the menus and the palette.  UpFloor / DownFloor are the
        //  classic "jump the camera to the next floor" pair — a 2D-view-era
        //  convenience the modern fly and Focus (/) have superseded — which is why
        //  they are the ones that move rather than the ones that keep the prime key.
        //
        //  DIRECTION: PageUp is the BIGGER grid.  "Up" = coarser = fewer, larger
        //  cells, which matches the [ ] pair's own reading ("]" is double) and the
        //  top-right readout's [+] on the right.
        //
        //  FINAL OCCUPANCY — 0x21: 0 = Grid Double, 2 = LeavePrefab, 3 = UpFloor.
        //                    0x22: 0 = Grid Halve,  2 = EnterPrefab, 3 = DownFloor.
        //  No (vk, mods) pair carries two rows.  [ and ] keep their bindings above:
        //  the two pairs are aliases of one command, not a replacement.
        // ═══════════════════════════════════════════════════════════════════════
        //  THE ALIAS ROWS, and why they are needed: Bind() is keyed by ID and finds
        //  the FIRST row carrying it, which for these two is the [ / ] row claimed
        //  at the top of this function.  Binding by id here would MOVE the command
        //  off [ ] rather than add a second key, and the directive asks to keep
        //  both.  So kiwi_command.cpp registers "KiwiGridDoublePage" /
        //  "KiwiGridHalvePage" alias rows (the Delete-key precedent, same registrar,
        //  same reason) and these bind BY NAME.
        Bind( table, count, 32954, 0x21, 3 );   // UpFloor   -> Shift+Alt+PageUp
        Bind( table, count, 32955, 0x22, 3 );   // DownFloor -> Shift+Alt+PageDown
        BindName( table, count, "KiwiGridDoublePage", 0x21, 0 );   // PageUp   -> 34021
        BindName( table, count, "KiwiGridHalvePage",  0x22, 0 );   // PageDown -> 34020

        // ── V IS NOT BOUND HERE, AND MUST NOT BE ────────────────────────────
        // The movable pivot's V (kiwi_transform.h) is a COMMAND-LOCAL key, matching
        // Plasticity, which binds it per gizmo context and never globally
        // (default-keymap.ts:147 `[command="move"] v`, :179 rotate, :159 scale).
        // In KIWI it is reached because KiwiUX_KeyFunnel routes EVERY key to
        // KiwiCmd_KeyDown while a modal command is active, and rung 3 offers it to
        // the command before anything else can see it — so vk 0x56's global
        // occupancy (mods 0 DragVertices 33005 :1047 · 1 the centre-rect tool ·
        // 3 VehicleGroup · 4 Paste 33040 :1100 · 5 ToggleView 33071 :1071) is
        // COMPLETELY UNDISTURBED, and pressing V with no command running still
        // means Drag Vertices.  Nothing is displaced and nothing needs to be.
    }

    void Rebuild()
    {
        // The order kiwi_keymap.h documents: defaults, then the user's radiant.ini,
        // then the profile on top.
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
        // DEFAULT = modern (spec §11: "keymap_modern (default for the overhaul)").
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

void KiwiKeymap_ApplyBoot()
{
    // Boot already ran the reset (the table was just seeded) and LoadCommandMap, so
    // only the patch is due — re-running them here would be a wasted 187-key ini
    // sweep for no change.
    if ( KiwiKeymap_Get() == KEYMAP_MODERN )
        ApplyModern();
}

// ─── settings UI (drawn inside the KiwiUX block) ─────────────────────────────
void KiwiKeymap_DrawSettings()
{
    int cur = (int)KiwiKeymap_Get();
    const char *items[] = { "Classic (stock Radiant bindings)", "Modern (Plasticity-style)" };
    ImGui::SetNextItemWidth( 260.0f );
    if ( ImGui::Combo( "Keymap profile", &cur, items, 2 ) )
        KiwiKeymap_Set( (kiwiKeymap_t)cur );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip(
            "Modern: 1-5 selection modes, Ctrl+1-4 convert the selection,\n"
            "F command palette, [ ] or PageUp/PageDown grid spacing,\n"
            "G/R/S transforms (V moves the pivot while one is running),\n"
            "C cut, Z match face, J join, E extrude, Ctrl+R split face,\n"
            // KIWI-UX (ROUND AG, ITEM 4): re-stated from the live table.  This was
            // stale on TWO counts before this round — Shift+V had been the centre
            // BOX since round AF (the centre rect moved to Alt+V), and Shift+C/W
            // swapped this round.
            "and the creation chords: Shift+A line, Shift+S spline,\n"
            "Shift+Q rect, Shift+C circle, Shift+W box, Shift+V box(centre),\n"
            "Alt+V rect(centre), Shift+X cylinder, Shift+Z sphere.\n"
            "The displaced commands move to Shift/Alt chords, never dropped.\n"
            "Classic: the stock table plus your radiant.ini remaps, unchanged.\n"
            "Every new feature stays reachable from the command palette in both." );
}
