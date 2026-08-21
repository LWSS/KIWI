#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_primitive.h — RADIANT_UX_DESIGN §16b: the SOLID half of the creation
// suite.  Four modal commands that land REAL BRUSHES straight out of the 3D
// view: Box (34051), Cylinder (34052), Sphere (34053), Cone (34054).
//
// USER DIRECTIVE (shakeout C): "How am I supposed to create a new brush in the
// 3dcam window?"  Region-extrude (§23) could already do it, but only after
// drawing a closed construction loop first — three commands deep for a cube.
// These are the one-gesture answer.
//
// ── THE GESTURE (transliterated from Plasticity, §16b.1) ────────────────────
// Two or three stages, all on the ACTIVE CONSTRUCTION PLANE (§16), all snapped:
//   Box       corner  →  opposite corner  →  pull height   (CornerBoxCommand)
//   Cylinder  centre  →  radius           →  pull height   (CylinderCommand)
//   Cone      centre  →  radius           →  pull height   (no Plasticity twin;
//                                                            Radiant has the
//                                                            primitive, so it ships)
//   Sphere    centre  →  radius                            (SphereCommand)
// The height stage uses the SAME closest-point-on-the-normal-line mapping face
// push/pull and region-extrude use (kiwi_extrude.cpp RayAxis).  A typed number
// means the stage's own scalar — half-size / radius / height; the HUD says which
// on every frame.
//
// ── HOW EACH ONE ACTUALLY BECOMES A BRUSH ───────────────────────────────────
// BOX is a convex prism over a 4-gon, which is exactly what §23 already writes,
// so it calls the audited writer directly (KiwiExtrude_BuildPrismDef →
// KiwiValid_Rebuild → KiwiValid_CheckBrush → KiwiExtrude_LandDef).  No new
// winding math exists in this file.
//
// CYLINDER / SPHERE / CONE are the PORTED primitives.  Brush_MakeSided
// (brush.cpp:3381, 0x4731E0), Brush_MakeSidedCone (brush.cpp:3625, 0x47BC10) and
// Brush_MakeSidedSphere (brush.cpp:3704, 0x47BE90) all RESHAPE AN EXISTING BRUSH
// IN PLACE: they read `def->mins` / `def->maxs`, free the face array, re-allocate
// it at the new count and write the ring.  So the sequence is: build a BOX def
// over the placement's AABB, rebuild it so the bounds exist, then let the ported
// primitive cut it.  Nothing here re-implements a ring.
//
// ── TWO LANDING ORDERS, AND WHY THEY DIFFER ─────────────────────────────────
// Brush_MakeSided takes the def POINTER, so the cylinder can be built, cut,
// validated and only THEN linked — the §23 "rejection is free" property, kept.
//
// Brush_MakeSidedCone and Brush_MakeSidedSphere do NOT: both open with
// QE_SingleBrush() (qe3.cpp:362) and then read `selected_brushes.next->def`
// (brush.cpp:3640 / 3719).  They are only reachable through the LIVE SELECTION.
// Cone and sphere therefore land first and validate after, and a failed gate is
// rolled back with KiwiCmd_UndoCancel() — which is exactly what that helper is
// for (undo.cpp's Undo_EndBrushList stamps the new brush; Undo_Undo removes
// every brush carrying the stamp, i.e. "undo a creation").  Documented
// divergence, forced by the ported cores, not chosen.
//
// ── WHAT THE PORTED CORES IMPOSE (accepted, logged) ─────────────────────────
//   * All three finish with Brush_BuildWindings(def, 1) — bFull 1, i.e.
//     Brush_SnapPlanepts, the LEGACY power-of-two grid.  A primitive therefore
//     lands on the classic grid even though its placement was snapped to the
//     modern one (§17).  Changing that means editing ported logic, which this
//     layer does not do.  RADIANT_KNOWN_ISSUES records it.
//   * Brush_MakeSidedCone's apex is FIXED at +Z (maxs[2], brush.cpp:3681-3683)
//     and its base ring at mins[2].  A cone is therefore only meaningful on the
//     XY construction plane, and the command refuses any other.
//   * Brush_MakeSided's `axis` is a WORLD axis (0/1/2), so the cylinder likewise
//     needs a world-axis-aligned plane — XY / XZ / YZ, i.e. the three §16
//     presets.  "From face" / "from view" planes are refused with a message
//     rather than silently producing a cylinder pointing somewhere else.
//   * Brush_MakeSidedSphere is orientation-free in effect (it fills a CUBE),
//     so the sphere accepts any plane.
//
// ── SIDE COUNTS ─────────────────────────────────────────────────────────────
// The classic route is the sides prompt (was the IDD_ARBITRARY_SIDES modal
// SidesDlgProc, now imgui_panel_sides.cpp) which ships NO default at all — it
// `atol`s whatever text is in the edit field, so an empty field yields 0 and the
// ported core rejects it ("Bad sides number").  KIWI picks its own, and both
// numbers are taken from the binary rather than invented:
//   * cylinder / cone = 16, the count the ported Brush_MakePhysCylinder itself
//     hardcodes (brush.cpp:3598, `Brush_MakeSided_Prolog( 0x10u, 0 )`);
//   * sphere = 8, because Brush_MakeSidedSphere builds sides×sides FACES
//     (brush.cpp:3736) — 8 is 64 faces, and 16 would be 256 on one brush.
// Adjusted live with `[` / `]`, the same non-digit input the polygon tool uses
// and for the same reason (kiwi_construct.cpp's KiwiPolygonTool note): every
// digit belongs to the numeric entry before a command ever sees it.
// ─────────────────────────────────────────────────────────────────────────────

class KiwiEditorCommand;

#define KPRIM_MIN_EXTENT     1.0f   // world units — below this the gesture is a no-op

// ═══════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BK, ITEM 5) — THE AXIS-LOCKED CREATION HEIGHT
// ═══════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: *"When a cylinder (or similar 3D object) is created
// from a snapped camera position that cannot possibly portray any Z movement (or
// X/Y depending on axis/height), make it so the 3D object is just automatically
// created with a 5ft height, but the lollipop extrusion for the top side (the one
// that was facing the locked camera) is active and ready to be pulled on whenever
// the camera is turned.  (This mimics Plasticity.)"*
//
// WHAT IT REPLACES.  Round AI, ITEM 2 gave the height stage a VIEW GATE
// (KiwiCam_AxisPortrayable, kiwi_camera.h): looking straight down the height axis,
// the cursor mapping is degenerate, so the height is HELD at 0 and the commit
// click is REFUSED with a console line telling the user to orbit.  That was the
// right answer to "the height was already -140 yd" and it is a dead end to the
// user standing in a top view who wants a box: the tool tells them to go away and
// come back.  This round completes the gesture instead — 5 ft of height, landed,
// with the face push already armed on the cap they are looking at.
//
// 5 ft = 60 UNITS.  A Radiant/CoD4 world unit is one inch (kiwi_units.h carries
// the whole ladder and KiwiUnits_Format prints in feet), so five feet is 5 * 12.
// It is written as a product rather than as `60.0f` so the intent survives a
// future units change.
#define KPRIM_AUTO_HEIGHT   ( 5.0f * 12.0f )   // 5 ft, in world units (inches)
#define KPRIM_CYL_SIDES_DEF  16     // brush.cpp:3598 (Brush_MakePhysCylinder)
#define KPRIM_CYL_SIDES_MIN  3      // brush.cpp:3383 rejects < 3
// -- KIWI-UX (ROUND AF, ITEM 7): 32 -> 64 --------------------------------
// USER DIRECTIVE, verbatim: "Sometimes you want more sides on the
// circles/cylinders."  A cylinder is sides + 2 faces, so 64 sides is a 66-face
// brush - large, but nowhere near the BINARY's own ceiling, which refuses only
// at 1020 ("too many sides", brush.cpp:3383-3391, MAX_POINTS_ON_WINDING - 4).
// 64 is chosen to match KCON_SEGS_MAX and KEXT_MAX_PROFILE: a cylinder coarser
// than the construction circle it would otherwise be traced from is a seam
// waiting to happen, and 64 is the number the rest of this layer already caps
// profiles at.
#define KPRIM_CYL_SIDES_MAX  64
// ===========================================================================
//  ROUND AG, ITEM 8 - THE EXPERIMENTAL PATCH MODE (cylinder)
// ===========================================================================
// USER PROPOSAL, verbatim: "In normal cod4, they use patches for curves.  Maybe
// you could add an experimental patch hybrid option for the circle/cylinder (any
// round) tools?  What do you think?"  -- prompted by a boolean'd cylinder arch
// coming out as "a fan of sliver faces" where "texturing becomes hell".
//
// The answer is yes, and it is what stock Radiant already does: Curve > Cylinder
// (mainfrm.cpp:3831) is Patch_BrushToMesh( 0, 0, 0, 0 ), which discards the box
// and leaves a 9x3 PATCH_CYLINDER.  A CoD4 mapper's round geometry IS patches.
//
// THE CONTROL GRID IS FIXED, and this is the part worth stating out loud because
// it looks like a limitation and is not: a patch's smoothness comes from its
// TESSELLATION, not from how many control points it has.  Four quarter-arc spans
// (a 9-wide grid) describe a circle EXACTLY -- the quadratic bezier's handle
// column is pushed out to r / cos(45 deg) = r * sqrt(2), which is precisely the
// corner construction Patch_BrushToMesh uses.  More columns would not make the
// curve rounder; they would only make it harder to edit.  So the `sides` count
// and the [ ] keys are simply not the knob in patch mode, and the HUD says so.
//
// The format's own ceiling is 15 usable columns (Patch_GenericMesh refuses a
// width outside 3..15, pmesh.cpp:1550), i.e. 7 spans, so 4 is comfortably inside
// it and leaves room for a future user-facing span count.
//
// WHAT A PATCH IS NOT: it is a RENDER surface with no collision and no caps.
// That is stock CoD4 practice (the mapper puts a caulk brush inside), it is
// stated on every creation, and it is why the mode is EXPERIMENTAL rather than
// the default.
#define KPRIM_PATCH_SPANS    4      // quarter arcs -> width 9
#define KPRIM_PATCH_ROWS     3      // bottom / middle / top, the stock layout

// The remembered preference ("RoundToolPatch" in the KiwiUX profile section),
// lazily loaded on first read exactly as KiwiCon_ToolSides is.
bool KiwiPrim_PatchMode();
void KiwiPrim_SetPatchMode( bool on );

#define KPRIM_SPH_SIDES_DEF  8      // 64 faces; see the note above
#define KPRIM_SPH_SIDES_MIN  4      // brush.cpp:3706 rejects < 4
// ROUND AF, ITEM 7: 12 -> 16, and DELIBERATELY NOT 64.  A sphere is sides x
// sides faces (Brush_MakeSidedSphere, brush.cpp:0x47BE90 - bands x segments),
// so the cylinder's 64 would be a FOUR THOUSAND AND NINETY SIX face brush,
// which no compiler and no editor wants to see.  16 is 256 faces: twice the
// smoothness at four times the cost, and the last rung where the number is
// still of the same order as a detailed brush.  The asymmetry with the
// cylinder is the geometry's, not a policy.
#define KPRIM_SPH_SIDES_MAX  16     // 256 faces - already a lot for one brush

void KiwiPrim_RegisterCommands();
KiwiEditorCommand *KiwiPrim_CommandForId( int commandId );

// The "Solids" block inside the shell's Construct panel (same shape as
// KiwiCon_MenuItems — buttons, because that panel is a window, not a menu bar).
void KiwiPrim_MenuItems();
