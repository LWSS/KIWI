#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_lines.h — the Phase-1b world-space overlay line batcher.
//
// A thin, BUDGETED wrapper over the editor's own line path (R_Add3DLine ->
// R_AddCmd_Line3D), i.e. exactly what the ported terrain-ring / vertex-handle /
// connection-line tail passes use.  It exists so every new overlay shares one
// place that (a) caps how many segments a frame may emit and (b) documents the
// two traps below.
//
// ── TRAP 1: the render-command buffer overflows on big maps ──────────────────
// Overflow now silently DROPS commands, so an unbounded overlay would not crash
// but would quietly eat other passes.  Every batch declares its segment budget up
// front and KiwiLines_Add refuses past it — callers coarsen instead of spilling.
//
// ── TRAP 2: there is no per-vertex alpha here ────────────────────────────────
// Under KISAK_RADIANT, R_AddCmd_Line3D routes through Ed_EmitLineBatch, which
// pushes the run's colour as MATERIAL_COLOR — and the editor's $line technique
// computes  rgb = lerp( sample * vertexColour, materialColor.rgb, materialColor.w ).
// An alpha below 1 therefore means "blend LESS of my colour in", NOT "be
// transparent".  Alpha is consequently pinned to 1 and distance fading is done
// with DIMMER COLOURS (the grid's distance bands), never with alpha.
//
// Colour runs: Ed_EmitLineBatch splits a batch at every colour change and emits
// one SetMaterialColor + DrawLines pair per run, so callers should still emit
// same-coloured segments contiguously to keep the command count down.
// ─────────────────────────────────────────────────────────────────────────────

// Open a batch.  `maxSegments` is this batch's hard budget; `width` is the line
// width handed to R_AddCmd_Line3D (the ported overlays use 1, handles use 2).
void KiwiLines_Begin( int maxSegments, int width );

// Colour for subsequently added segments.  Alpha is always opaque (see TRAP 2).
void KiwiLines_Color( float r, float g, float b );

// Append one segment.  Returns false when the batch budget is exhausted.
bool KiwiLines_Add( const float *a, const float *b );

// Emit whatever is pending and close the batch.
void KiwiLines_Flush();

// Segments still allowed in the open batch.
int  KiwiLines_Remaining();

// ─────────────────────────────────────────────────────────────────────────────
// KIWI-UX (ROUND AA, ITEM 2) — TRAP 3: EVERY FILL IN THIS LAYER IS BACKFACE-CULLED
//
// USER REPORT, verbatim: "the light blue construction lineface isn't rendering
// unless you're facing the other way (see pics).  Fix this."
//
// Round Y found this once, on the cut disc, and fixed it in place: the kiwi layer
// draws every translucent fill with g_qeglobals.d_white =
// Material_RegisterHandle( "white_tools" ) (gfxwrapper.cpp:77), and
// main/materials/white_tools carries refStateBits[0] = 0x08128965, whose cull
// field (& GFXS0_CULL_MASK 0xC000 == 0x8000) is GFXS0_CULL_BACK -> s_cullTable_30[2]
// = 3 = D3DCULL_CCW (r_state.cpp:28, :919), and main/statemaps/default.sm passes
// cullFace through.  So a fill emitted with a FIXED winding is visible from one
// side and gone from the other, and several places in this layer carried comments
// asserting the opposite (kiwi_extrude.cpp: "backface culling is not available on
// this path"; kiwi_region.cpp: "the fill is visible from both sides").  It is
// available, it is on, and those comments were wrong.
//
// This is the shared answer, so the rule lives ONCE rather than as five copies of
// a cross product.  It rewrites the index buffer IN PLACE, per triangle: a
// triangle whose geometric normal points away from `eye` has its last two indices
// swapped.  Per-TRIANGLE rather than per-polygon on purpose — it is then equally
// correct for a flat polygon (every triangle agrees, the whole thing flips), for a
// closed volume drawn in one batch (each face resolves independently, so the near
// half fills and the far half fills too instead of one of them vanishing), and for
// a fan whose vertices are not coplanar.
//
//   xyz      first vertex position; `stride` floats apart (4 for the xyzw arrays
//            these emitters build, 3 for a bare vec3 array)
//   idx      the triangle list, 3 entries per triangle, MODIFIED
//   idxCount 3 * triangle count
//   eye      the camera origin in world space (Ed_Camera()->origin)
//
// A degenerate triangle (zero-area, so no normal) is left exactly as it was.
//
// KIWI-UX (ROUND AL, ITEM 2): IN AN ORTHOGRAPHIC VIEW THERE IS NO EYE POINT.
// The rasteriser decides a triangle's winding after the PROJECTION, and under a
// parallel projection that sign is `n · vpn` — it does not involve the camera
// position at all.  `c->origin` in ortho is an arbitrary point on the view axis
// (KiwiCam: origin = lookAt - forward*s_dist, kiwi_camera.cpp:812), and the
// ortho half-height is only 0.75*s_dist (KiwiCam_OrthoHalfHeight,
// kiwi_camera.cpp:623-633), so a vertex at the edge of the screen subtends ~37
// degrees off the view axis from that point.  Testing against the POINT
// therefore disagrees with the rasteriser for every triangle within that cone of
// edge-on, and those triangles are flipped the wrong way and culled — the
// silhouette walls of the gizmo's cones, the rim of a large fan.  The function
// now tests the DIRECTION in ortho and the point in perspective, which is exactly
// what each projection's own winding rule is.
void KiwiTris_OrientToEye( const float *xyz, int stride,
                           unsigned short *idx, int idxCount, const float *eye );

// ─────────────────────────────────────────────────────────────────────────────
// KIWI-UX (ROUND AL, ITEM 1) — TRAP 4: A FILL'S NORMAL IS A LIGHTING INPUT,
//                                      SO IT MUST NOT BE A CAMERA QUANTITY.
//
// USER REPORTS, verbatim: round AK — "construction faces that can be extruded
// from still lack their light blue background on the face.  It worked a few days
// ago sometimes, but now never shows - not even from the backside."  Round AL —
// "the blue face only shows up at steep angles.  Fix this!" and "the gizmos are
// only solid at a high zoom level."
//
// THE STATE, read out of the shipped assets (the full decode is in camwnd.cpp
// around Cam_MaterialWritesDepth and in RADIANT_UX_DESIGN §55):
//   g_qeglobals.d_white = Material_RegisterHandle("white_tools")
//   -> techSet "tools" -> "unlit" = **vertcol_shaded_tools** -> statemap default
//   -> depth test LESSEQUAL, depth write OFF, cull BACK, SrcAlpha/InvSrcAlpha.
// The pixel shader of that family is
//     rgb = lerp( sample(colorMap) * vertexColour, materialColor.rgb, materialColor.w )
// (r_rendercmds.cpp:1947-1966, measured, not guessed), and its VERTEX stage is
// the "shaded" half of the name: r_shade.cpp:366-372 records that a zeroed def
// constant "NaNs the FAKELIGHT VERTEX COLOUR", i.e. the vertex colour this
// pixel shader receives has already been modulated by a term the vertex shader
// computes FROM THE NORMAL.  The shader itself is not in this tree; what the
// term is a function of was derived from two rounds of evidence instead:
//
//   round AK, normal = the region's own PLANE normal:  a sketch on a WALL
//     (horizontal normal) never showed FROM EITHER SIDE; a sketch on the ground
//     showed.  -> the term is ~0 for a horizontal normal, large for a vertical
//     one, and it is NOT a function of the view (a wall seen face-on was dark).
//   round AL, normal = -vpn:  the fill shows looking DOWN and vanishes as the
//     view levels out.  -> normal.z is what drives it, again.
//
// Only ONE model satisfies both: a FIXED-DIRECTION, roughly world-UP fake light.
// A view-relative term (dot(N, V)) is excluded by round AK's wall-seen-face-on
// case, and an abs()/two-sided term is excluded by the same case.
//
// THE RULE, therefore: a translucent overlay fill wants CONSTANT brightness, so
// its normal must be a CONSTANT — and the constant that maximises an up-lit term
// is world +Z.  This is also what the BINARY does: Face_AddWindingToTriBatch
// (brush.cpp:5924-5926, 0x47b86a), the ported selected-face fill's own batcher,
// writes a fixed WORLD normal per vertex (the face plane's) and never a camera
// vector.  The kiwi layer's `-vpn` was invented by this port.
//
// WHY NOT MATERIAL_COLOR WITH .w = 1 (the other way to be immune).  That is what
// the editor's LINE batches do — Ed_EmitLineBatch pushes a per-colour-run
// MATERIAL_COLOR with w == 1 (r_rendercmds.cpp:1960-1995), which lerps the whole
// vertex term away, and it is exactly why the OUTLINES in this layer never
// showed any of this and the FILLS did.  It is refused here because .w is the
// lerp weight and the fills need per-vertex ALPHA: whether this shader's alpha
// output is sample.a * vcol.a (fill survives) or materialColor.a (fill becomes
// an OPAQUE SLAB) cannot be read from this tree, and round AH already paid for
// one opaque-slab regression.  If a constant normal is ever shown to be
// insufficient, THAT is the next probe and this is the risk it carries.
// ── KIWI-UX (CLEANUP, BoxEdges / C-71): THE AABB, ONCE ──────────────────────
// The eight corners of [mins,maxs] in the canonical bit order — bit 0 = x,
// bit 1 = y, bit 2 = z, 0 = min and 1 = max — and the twelve edges that index
// them.  Both were written out verbatim in three places (kiwi_entbrowser.cpp's
// isometric tile AND its world drag ghost, 700 lines apart, plus kiwi_dupe.cpp's
// array preview DrawBox), and kiwi_entbrowser.cpp's own comment already said the
// three "are literally the same box".  kiwi_entthumb.cpp expands the same eight
// corners for its framing pass and uses this too.
//
// They live in kiwi_lines.h because the overlay batcher is what every world-space
// consumer already includes, and because a box outline IS twelve KiwiLines_Add
// calls; the ImGui tile consumer needs only the table, which costs it nothing.
#define KIWI_BOX_CORNERS 8
#define KIWI_BOX_EDGES   12

// z-min ring, z-max ring, then the four verticals.  The two rings are wound so a
// projection of either reads convex.
const int KIWI_BOX_EDGE[KIWI_BOX_EDGES][2] =
{
    { 0,1 },{ 1,3 },{ 3,2 },{ 2,0 },      // z min ring
    { 4,5 },{ 5,7 },{ 7,6 },{ 6,4 },      // z max ring
    { 0,4 },{ 1,5 },{ 2,6 },{ 3,7 },      // verticals
};

inline void KiwiBox_Corners( const float *mins, const float *maxs,
                             float out[KIWI_BOX_CORNERS][3] )
{
    for ( int i = 0; i < KIWI_BOX_CORNERS; ++i )
    {
        out[i][0] = ( i & 1 ) ? maxs[0] : mins[0];
        out[i][1] = ( i & 2 ) ? maxs[1] : mins[1];
        out[i][2] = ( i & 4 ) ? maxs[2] : mins[2];
    }
}

inline void KiwiTris_FillNormal( float *out )
{
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 1.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// KIWI-UX (ROUND AM, ITEMS 1/2/4) — TRAP 5: THE FLAT-COLOUR OVERRIDE, i.e. THE
//                                   IMMUNITY THE LINES HAVE HAD ALL ALONG.
//
// KIWI-UX (CLEANUP, A-2) — CORRECTION, READ THIS FIRST.  The translucent probe
// scheduled below WAS RUN (round AQ) and the ~0.32x neutral-bracket model below
// is FALSIFIED — the live explanation is at kiwi_region.cpp:1541+.  Everything
// after this paragraph is the superseded argument, kept for its measurements.
// KiwiTris_FillFlatColor survives only for the gizmo's opaque-by-design elements
// (kiwi_gizmo.cpp), which is its one remaining caller.
//
// THREE OF THIS ROUND'S SEVEN REPORTS ARE ONE BUG, and seeing that is what
// finally makes the diagnosis falsifiable:
//   item 1  "Still no light blue face on extrudable line clusters."
//   item 2  "Need hover highlighting when hovering a face in mode 3."  A FACE
//           EMITS NOTHING BUT A FILL — kiwi_hover.cpp's EmitItem has an explicit
//           empty `case SEL_FACE:` (shakeout G, so a face accent cannot look like
//           an edge one), and the mode-3 hover pick already resolves faces
//           (kiwi_hover.cpp:707 picks with the LIVE mode mask).  So "no hover
//           highlight on a face" is not missing wiring: it is the fill again.
//   item 4  "gizmo arrows are still not filled in."  Round AK gave the heads real
//           triangles; what is left on screen is round AJ's OUTLINE chevron.
// Every one of them is an R_AddRenderCmdDrawTris fill on `d_white` inside a
// NEUTRAL MATERIAL_COLOR bracket, and every LINE in the same layer — same
// material family, same frame, same pass — renders at full strength.
//
// THE MEASUREMENT THAT NAMES THE DIFFERENCE is already in this tree, in
// r_rendercmds.cpp's own words (the RADIANT_LINEVCOL experiment, :1913-1930):
// with MATERIAL_COLOR neutral, so that the draw is `sample(colorMap) * vcol`,
// "the XY brush wireframes came back at ~0.32x, so the neutral-matColor route
// needs the colorMap binding verified first".  THAT IS THE WHOLE STORY: under a
// neutral bracket this editor's tools shaders return about a THIRD of the colour
// asked for, because the editor draws outside a full scene render and the
// colorMap binding / fakelight term it multiplies by is not what a scene would
// have set up.  A line at 0.32x is dim but visible.  A translucent FILL at
// alpha 0.22, whose colour is then multiplied by 0.32 as well, is nothing.
//
// SO THE FIX IS THE ROUTE THE LINES TOOK.  Ed_EmitLineBatch pushes a per-colour-
// run MATERIAL_COLOR with .w == 1 (r_rendercmds.cpp:1960-1995), a FLAT COLOUR
// OVERRIDE that lerps the sampled term entirely away — which is exactly why the
// outlines in this layer never showed any of this and the fills did.  This
// helper is that push, spelled once.
//
// THE RISK, STATED PLAINLY AND SCOPED RATHER THAN HAND-WAVED.  `.w` is the lerp
// weight, and whether this shader's ALPHA output is `sample.a * vcol.a` (the fill
// keeps its translucency) or `matColor.a` (the fill becomes an OPAQUE SLAB)
// cannot be read from this tree.  Round AL refused the probe on that ground and
// cited "the round-AH failure mode" — but round AH's regression was the grid's
// FAR RING drawing minors (RADIANT_UX_DESIGN §61/§63, "the black-slab
// correction"), a LINE-DENSITY bug with no alpha in it at all.  There is
// therefore no evidence in this tree that a flat override turns a fill opaque,
// only an absence of evidence that it does not.  This round takes it where the
// downside is zero or bounded and leaves the rest alone:
//
//   * GIZMO HEADS and the ORIGIN DISC — alpha 1.00 and 0.85, opaque BY DESIGN
//     (kiwi_gizmo.cpp's per-element opacity note).  Nothing to lose.
//   * THE REGION FILL — the one translucent probe, which is what
//     RADIANT_KNOWN_ISSUES round AL asked for in as many words ("Try it on ONE
//     fill first").  If it comes back as a solid light-blue slab, the model is
//     CONFIRMED and one constant reverts it; if it comes back translucent, the
//     model is confirmed and the rest of the layer follows next round.
//
// The caller still owns the surrounding neutral bracket; this replaces it for the
// duration of ONE element and KiwiTris_FillNeutral puts it back.
void KiwiTris_FillFlatColor( const float rgba[4] );
void KiwiTris_FillNeutral();
