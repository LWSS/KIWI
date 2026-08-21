#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_sun.h — the SUN HELPER: place the worldspawn sun keys, show where the sun
// is, show what it projects, and let it be dragged around the map.
//
// USER REQUEST, verbatim: "Make me a sun helper that places a sun for me.  I want
// it to show a projected frustum whenever the sun is selected, and allow it to
// rotate around the extents of the biggest brush."
//
// ── THE SUN IS NOT AN ENTITY ────────────────────────────────────────────────
// It is a set of WORLDSPAWN KEY/VALUES that both map compilers parse.  There is
// no classname to place, no brush to select and no origin to move; the whole of
// the sun's position is one angles string.  So the helper is a VIRTUAL SELECTABLE
// OBJECT — this file owns a `selected` flag and a glyph, exactly the way
// kiwi_region.h and kiwi_conselect.h own their own parallel selections, and
// nothing here ever enters selection_t (kiwi_selection.h DESIGN NOTE 1 rules it
// out: a sel_item_t is (selbrush_t*, index) by construction, and the sun has no
// brush).
//
// ── `sundirection` IS AN ANGLES STRING, AND IT POINTS *AT* THE SUN ──────────
// Both compilers read it identically — the value is "pitch yaw roll" and it is
// consumed through AngleVectors (cod4rad/com_math.c:105):
//     cod4rad   mapio.c:511-512            AngleVectors(sunDir, worldSunDir, 0, 0)
//     cod4map   tris_sunshadow.cpp:311     AngleVectors(sunAngles, sunDirection, ...)
//               primarylights.cpp:1448     AngleVectors(sunAngles, light->dir, ...)
//
// WHICH WAY THE RESULT POINTS is not a matter of taste; cod4map settles it twice:
//   * primarylights.cpp:493  VectorMA(point, 262144.0f, light->dir, endpoint) —
//     the sun-shadow ray starts at a surface point and marches ALONG light->dir
//     expecting to leave the map, so light->dir goes FROM the geometry TOWARD the
//     sun.
//   * primarylights.cpp:674  DotProduct(light->dir, surface->props->plane) > 0.0f
//     is "this surface is lit", i.e. a face whose normal agrees with light->dir
//     faces the sun.
// The compiler default when the key is absent agrees: (0.4418350, 0.5680700,
// 0.6943130) (cod4rad/cmdline.c:28-30) has a POSITIVE z — a vector aimed upward,
// which is where a sun is.  So:
//
//     sundirection's AngleVectors forward = the unit vector FROM THE SCENE
//     TOWARD THE SUN.  Light TRAVELS along its negation.
//
// Everything in this file is written in those terms: `sunDir` always points at
// the sun, the glyph sits at targetCentre + sunDir * radius, and the frustum is
// extruded along -sunDir.
//
// ── THE ANGLE ROUND TRIP ────────────────────────────────────────────────────
// AngleVectors (cod4rad/com_math.c:116-118) with roll ignored:
//     forward = ( cos p * cos y,  cos p * sin y,  -sin p )        (p, y degrees)
// so, inverting for a unit `d`:
//     p = -asin( d.z ) ,  y = atan2( d.y, d.x )
// and substituting back:
//     -sin p   = -sin( -asin d.z )   = d.z                                   ✓
//     cos p    = cos( asin d.z )     = sqrt( 1 - d.z^2 ) = |d.xy|            (>= 0)
//     cos p*cos y = |d.xy| * d.x / |d.xy| = d.x                              ✓
//     cos p*sin y = |d.xy| * d.y / |d.xy| = d.y                              ✓
// The identity holds for every d with |d.xy| > 0, and `cos p >= 0` is what makes
// it exact rather than merely consistent: asin returns p in [-90, 90], where
// cosine is non-negative, which is the same half that |d.xy| lives in.
//
// CHECKED AGAINST THE COMPILER DEFAULT, by hand:
//     d   = (0.4418350, 0.5680700, 0.6943130)
//     p   = -asin(0.6943130)            = -43.9726 deg
//     y   = atan2(0.5680700, 0.4418350) =  52.1259 deg
//     back: cos(-43.9726) = 0.719656
//           x = 0.719656 * cos(52.1259) = 0.719656 * 0.613955 = 0.441834      ✓
//           y = 0.719656 * sin(52.1259) = 0.719656 * 0.789341 = 0.568069      ✓
//           z = -sin(-43.9726)          = 0.694313                            ✓
// KiwiSun_Place writes that pair rather than a hard-coded string: the default
// direction is stated as the compiler's VECTOR and run through the same
// derivation the drag commits with, so the two can never disagree.
//
// ── THE HORIZON CLAMP ───────────────────────────────────────────────────────
// The sun must stay above the horizon, i.e. d.z > 0, i.e. sin p < 0, i.e. p < 0.
// The drag clamps pitch to [-89, -1] degrees; -89 is nearly overhead and -1 is
// nearly grazing.  The clamp REWINDS its own accumulator on a refused frame so
// travel that was declined is never banked (the same rule and the same reason as
// KiwiCam_OrbitDrag's, kiwi_camera.cpp:781-799: an absolute-from-latch rig that
// keeps refused pixels reads as "stuck").
//
// ── WHAT A CHANGE COSTS THE USER ────────────────────────────────────────────
// `sundirection` is consumed at BSP/light time — sun-shadow classification
// (cod4map/tris_sunshadow.cpp:310-311) and primary-light assignment
// (cod4map/primarylights.cpp:1447-1448) both happen in the compiler.  Moving the
// sun in the editor changes the helper and the editor's own preview; the baked
// lighting does not follow until the map is compiled again, and this file says so
// out loud every time it commits.
// ─────────────────────────────────────────────────────────────────────────────

// ── THE DOCK TAB IS THE DISCOVERY SURFACE ───────────────────────────────────
// USER REPORT, verbatim: *"where is the add menu?  I can't see it.  You need to
// do a tab like the skybox helper."*
//
// So the helper gets a "Sun" tab in the SAME dock node as Textures / Entities /
// Sky / UV editor — the fifth tab of that node, by the one-line mechanism
// ImGuiShell_BuildDefaultDockLayout states (imgui_shell.cpp:1317 is the Sky
// line): docking a second window into the same node id makes it a TAB, not a
// split.
//
// AND THAT IS ONLY HALF OF IT.  A DockBuilder line alone runs on a FRESH layout
// and nothing else, so an install that already has a dock ini would never see the
// tab — which is exactly the report above.  The mechanism that makes a new tab
// appear for an EXISTING install is KIWI_LAYOUT_VERSION (kiwi_windows.h:70), and
// it is ONE number with TWO consumers:
//   * imgui_shell.cpp's DockIniPath() builds "kiwi_dock<N>.ini", so a bump means
//     the current ini does not exist, so s_dockLayoutPending is set and
//     ImGuiShell_BuildDefaultDockLayout runs again and PLACES the tab;
//   * kiwi_windows.cpp's Load() re-seeds the [KiwiWindows] section from `s_def`
//     when the stored DefaultsVersion is older, so the new window is actually
//     OPEN when the rebuilt layout places it.
// Half a bump gives a placed-but-closed window or an open-but-floating one, with
// no diagnostic — which is why the two share the number.  This round takes it
// 11 -> 12, the same one-time layout reseed every default change since shakeout I
// has made.
//
// The Add-menu row and the palette command are UNCHANGED; the tab is simply the
// surface a user finds without knowing the feature exists.
void KiwiSun_Draw();

// ── the commands ────────────────────────────────────────────────────────────
void KiwiSun_RegisterCommands();
bool KiwiSun_DispatchInstant( unsigned int cmdId );

// Write the ABSENT worldspawn sun keys and select the helper.  Keys the map
// already carries are left exactly as they are — placing a sun on a map that
// has one is a select, not an overwrite.  One undo record, and none at all when
// nothing was missing.
void KiwiSun_Place();

// Palette predicate: there has to be a worldspawn to write onto.
bool KiwiSun_CanPlace();

// ── state ───────────────────────────────────────────────────────────────────
// True when the worldspawn carries a parseable "sundirection".  The glyph draws
// only then, so a map with no sun costs this file nothing.
bool KiwiSun_Exists();

bool KiwiSun_Selected();
void KiwiSun_Select();              // the click grammar's "take it" (kiwi_boxselect.cpp)
void KiwiSun_ClearSelection();

// The LIVE angles in degrees — the dragged pair while a drag is running, the
// committed worldspawn pair otherwise.  False when there is no sun.
bool KiwiSun_Angles( float *outPitch, float *outYaw );

// Drop every derived flag.  Called from the new-map path: a sun helper is state
// about a document and must not survive one.
void KiwiSun_ResetForNewMap();

// ── input, from kiwi_viewport.cpp / kiwi_boxselect.cpp ──────────────────────
// All pixel coordinates are camera-RTT-image relative, TOP-LEFT origin
// (kiwi_pick.h's convention).

// Is the cursor over the sun glyph?  The click grammar asks this before it looks
// at brushes — the glyph is an aimed-at screen target, the same class of thing as
// the section ball, and it sits far outside the map where nothing competes.
bool KiwiSun_GlyphHit( int imgX, int imgY );

// The LMB press.  True = the glyph was taken and the caller owns the gesture
// until the release.  Self-gated: nothing happens unless the helper is already
// SELECTED, so the click that selects it can never also orbit it.
bool KiwiSun_HandleDown( int imgX, int imgY );
void KiwiSun_HandleDrag( int imgX, int imgY );
void KiwiSun_HandleUp();              // commit: ONE undo record for the whole drag
// Lost capture.  Nothing was ever written while the drag ran, so dropping the
// grab IS the restore — the next KiwiSun_Angles reads the worldspawn again, which
// still holds the pair the press latched.
void KiwiSun_HandleAbort();

// Cursor tracking for the glyph's hover highlight.  `over` false clears.
void KiwiSun_Hover( int imgX, int imgY, bool over );

// True while the glyph is being dragged — the viewport's gesture predicate.
bool KiwiSun_Grabbed();

// ── render ──────────────────────────────────────────────────────────────────
// Camera-draw overlay tail.  Emits the glyph whenever a sun exists, and the
// projected frustum only while the helper is selected or being dragged.
// Self-budgeted (KSUN_MAX_SEGMENTS); emits nothing when there is no sun.
//
// CAMERA VIEW ONLY, and deliberately so: this layer's whole overlay set draws
// from Cam_Draw's tail through kiwi_lines.h, which bottoms out on
// R_AddCmd_Line3D — there is no kiwi overlay in the XY/Z windows at all, and
// adding a second 2D emitter for one helper would be the parallel renderer
// RADIANT_UX_DESIGN §18 forbids.
void KiwiSun_DrawWorld();

// The hard per-frame segment budget (kiwi_lines.h TRAP 1) is KSUN_MAX_SEGMENTS,
// file-local in kiwi_sun.cpp beside the emitters it bounds.  The tally:
//   glyph    ring KSUN_DISC_SEGS (20) + KSUN_SPOKES (8)
//            + the arrow (1 shaft + 4 head)                                  = 33
//   frustum  2 end rectangles * 4 + 4 long edges
//            + KSUN_RAY_GRID^2 (9) interior rays                             = 21
//   worst case 54, and the budget is 64 so a wider ray lattice has headroom.
