#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_selext.h — RADIANT_UX_DESIGN §25's "smarter selection (coplanar / parallel
// / connected / similar-orientation, double-click = connected)".
//
// Four INSTANT commands in the reserved range plus one input hook:
//     KIWI_CMD_SELECT_COPLANAR   34023   "Select Coplanar Faces"
//     KIWI_CMD_SELECT_TOUCHING   34024   "Select Touching"
//     KIWI_CMD_SELECT_MATERIAL   34025   "Select Same Material"
//     KIWI_CMD_SELECT_CONNECTED  34026   "Select Connected (touching)"
//     + double-click in the camera image = pick a brush and run the last one.
//
// All four are pure SELECTION changes.  They mutate no geometry, so per
// kiwi_command.h they open NO undo bracket — an empty record would make one
// Ctrl+Z do nothing.  (Classic Radiant does not undo selection changes either.)
//
// ── WHAT THE PORT ALREADY HAS, AND WHY IT IS NOT WHAT §25 WANTS ─────────────
// The Selection menu's region-select cluster is fully ported and live:
//     select.cpp:1616  Select_CompleteTall   0x490170   id 32984
//     select.cpp:1638  Select_PartialTall    0x4903D0   id 32983
//     select.cpp:1667  Select_Touching_R     0x490520   id 32986
//     select.cpp:1701  Select_Inside_R       0x490650   id 33008
// (they are NOT residue — each has a real body and a real dispatch case).  BUT
// all four share the CLASSIC semantics, which are a different operation from the
// one §25 asks for: `Region_BeginFromSingleBrush` (select.cpp:1585) requires
// EXACTLY ONE selected brush, takes its bounds as a marquee BOX, **deletes that
// brush** (Select_Delete), and then selects everything matching the box.  It is
// "use this brush as a selection volume", not "grow the selection to what it
// touches", and it destroys the seed.
//
// So "Select Touching" here is written fresh as a bounds-overlap expansion.  It
// deliberately reuses the ported test's shape — overlap on ALL THREE axes with a
// +1 unit epsilon, so brushes that merely abut still count (select.cpp:1678-1679)
// — and the ported candidate filter (`Pick_BrushPickable`, kiwi_pick.h, which is
// the exported form of the walker sub_48D460's admission rules), so it never
// selects something a click could not.
//
// `Select_Connected` (select.cpp:2727, 0x490EC0, id 33134) also exists but is a
// completely different notion of connected: it walks target/targetname and
// script_linkTo/script_linkName ENTITY links.  §25's "connected" is geometric.
// Both are kept; this file's row is named "Select Connected (touching)" so the
// palette never confuses them.
//
// ── THE OPERATIONS ─────────────────────────────────────────────────────────
// COPLANAR — the ACTIVE item must name a face.  Every face of every pickable
//   brush whose plane matches it within KSELX_PLANE_DOT / KSELX_PLANE_DIST is
//   added as a SEL_FACE item.  The thresholds are the §19 duplicate-plane
//   thresholds (kiwi_validity.h KVALID_PLANE_DOT / _DIST), because "the same
//   plane" should mean the same thing to the selector and to the validator.
//   Note the sign convention: matching requires the normals to point the SAME
//   way, so the two facing walls of a corridor are NOT coplanar with each other.
//
// TOUCHING — one expansion step.  Every pickable brush whose bounds overlap
//   ANY brush the current typed selection names joins the selection as a whole
//   object.  The seed comes from KiwiSel() and not from the legacy list, because
//   a face-selected brush is NOT on selected_brushes (kiwi_selection.h DESIGN
//   NOTE 2).  The existing selection is KEPT — this expands, it does not replace.
//
//   DELIBERATE REFINEMENT of the brief's "intersects the selection's BOUNDS":
//   the test is per selected brush, not against the selection's union box.  A
//   union box over a scattered selection spans everything between its members
//   and would take the whole map; per-brush is what "touching" means to a
//   modeller, and it is the only version whose recursive form (CONNECTED, below)
//   terminates anywhere useful.
//
// SAME MATERIAL — matches on the CURRENT EDIT LAYER's material
//   (`face.mtldef[g_qeglobals.current_edit_layer].radMtl`, the registered
//   `qtexture_s`).  Identity is the registry POINTER first (materials are
//   singletons through Texture_GetHandle), falling back to a case-insensitive
//   name compare so a re-registered handle still matches.
//   Granularity follows the ACTIVE item, which is the rule that makes it
//   predictable: an active FACE selects faces, an active OBJECT selects whole
//   brushes (any of whose faces carries the material) and takes its reference
//   material from that brush's face 0.
//
// CONNECTED — TOUCHING, iterated to a fixed point, capped at KSELX_MAX_CONNECTED
//   brushes and KSELX_MAX_PASSES sweeps.  The cap is a hard stop with a console
//   message, not a silent truncation: on a dense map "everything touching
//   everything" really can be the whole world, and a modeller who asked for a
//   double-click should not lose their selection to it.
//
// ── THE DOUBLE-CLICK ───────────────────────────────────────────────────────
// §25: "double-click = connected".  The hook lives in kiwi_viewport.cpp's camera
// button-down path — INSIDE the existing modern-input gate and BELOW the active-
// modal-command arm, so with the master toggle off nothing changes and a live
// gesture still outranks it.  Detection is `ImGui::IsMouseDoubleClicked` read in
// the same tick the click edge is dispatched (imgui_shell.cpp's pump comment:
// "io mouse-click/release edges are still valid here").  The first click of the
// pair has already run the ordinary marquee/click select, so the double-click
// simply re-picks under the cursor and expands from there.
// ─────────────────────────────────────────────────────────────────────────────

#define KSELX_PLANE_DOT        0.999f    // == KVALID_PLANE_DOT
#define KSELX_PLANE_DIST       0.01f     // == KVALID_PLANE_DIST
#define KSELX_TOUCH_EPS        1.0f      // == the ported Select_Touching_R epsilon
#define KSELX_MAX_FACES        4096      // v1 cap on faces one expansion may select
#define KSELX_MAX_CONNECTED    1000      // §25's cap, stated in the spec brief
#define KSELX_MAX_PASSES       64        // fixed-point sweep cap

// Instant-command dispatch tail for the four ids (called by KiwiCmd_Dispatch).
bool KiwiSelExt_DispatchInstant( unsigned int commandId );
void KiwiSelExt_RegisterCommands();

// §3 canExecute predicates for the palette metadata rows.
bool KiwiSelExt_CanCoplanar();   // the active item names a face
bool KiwiSelExt_CanTouching();   // anything is selected
bool KiwiSelExt_CanMaterial();   // the active item resolves to a material
bool KiwiSelExt_CanConnected();  // anything is selected

// The camera double-click hook (kiwi_viewport.cpp).  Picks a brush under the
// cursor and expands to everything touching it, recursively.  Returns true when
// it consumed the click.
bool KiwiSelExt_CameraDoubleClick( int imgX, int imgY );

// The "Selection" block in the shell panel.
void KiwiSelExt_MenuItems();
