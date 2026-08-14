#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_grid.h — RADIANT_UX_DESIGN §17: world axes + the grey ground grid.
//
// Both are PURE ADDITIONS (nothing legacy drew them), so they are not gated by
// the modern-input master toggle — each has its own switch in kiwi_ux.h.
//
// ═════════════════════════════════════════════════════════════════════════════
//  ROUND M — GRID v3.  THE EMISSION STRATEGY IS REPLACED WHOLESALE.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "The grid is still buggy. at shallow camera angles it
// doesn't render.  The density also changes with angle, it shouldn't do that.
// Look how ugly the grid is.  You need to fix it.  Could use anti aliasing too."
//
// ── WHY v2 FAILED AT SHALLOW ANGLES — THE EXACT MATH ────────────────────────
// v2 (shakeout C) derived its FOOTPRINT from the view frustum: four corner rays
// through the ported CameraCalcRayDir, each intersected with Z=0, and a ray that
// missed the plane was walked KGRID_MAX_EXTENT (131072) along itself and its XY
// taken.  Both arms of that collapse as the pitch flattens:
//
//   * MISS ARM.  Pitch near 0 means the upper corner rays point at or above the
//     horizon, so `t` stays at 131072 and the corner lands ~131072 units away in
//     XY (the ray is nearly horizontal, so almost all of that length is
//     horizontal displacement).
//   * HIT ARM is no better: a ray that dips one degree below the horizon from a
//     camera 192 units up hits the ground at t = 192/sin(1°) ≈ 11000 units.
//
//   Either way the footprint SPAN blows up with 1/sin(pitch).  The auto-coarsen
//   loop then ran
//        while ( span / spacing > KGRID_LINES_PER_AXIS (200) )  spacing *= 2;
//   so a span of ~131072 forced spacing >= 655 — i.e. seven doublings of the
//   default 10-unit grid, landing on 640 or 1280 world units per cell.  At that
//   spacing the nearest line can be 1280 units from the camera and the majors are
//   12800 apart: the near field is EMPTY.  That is the "doesn't render at shallow
//   angles" report and the "density changes with angle" report — one bug, because
//   `span` (and therefore the coarsen count) was a function of PITCH.
//
// ── v3: A WORLD LATTICE, SIZED BY ALTITUDE, NEVER BY ANGLE ──────────────────
// The grid is conceptually infinite; each frame we draw the WINDOW of a fixed
// world lattice that is worth drawing, and the window is derived from quantities
// that do not involve pitch at all:
//
//   LOD          spacing = baseSpacing * 2^k, k chosen from the camera's HEIGHT
//                above Z=0 alone, with a Schmitt band so it cannot flicker
//                (KGRID_LOD_* below).  ONE spacing for the whole frame — no
//                bands-by-distance, no per-line screen-space cull, no
//                per-line hysteresis.  Density is therefore a function of
//                ALTITUDE ONLY, which is exactly the directive.
//   RANGE        R = spacing * KGRID_HALF_CELLS.  Tied to the LOD, so the line
//                count is bounded BY CONSTRUCTION (see the budget below) and the
//                reach grows as you climb.
//   CENTRE       the camera's ground projection, pushed KGRID_FWD_BIAS * R along
//                camera.forward — the YAW-PLANE forward CamWnd_BuildMatrix
//                already computes (camwnd.cpp:169-171), which is pitch-free by
//                construction.  That spends ~72% of the window in front of the
//                viewer instead of 50%, which is the only "reach" trick left once
//                the frustum math is gone, and it cannot vary with pitch.
//
// No ray is cast, nothing is intersected with the ground plane, and no branch in
// the emitter reads camera.angles[0].  A camera at 192 units draws 10-unit cells
// out to 1200 units whether it is looking straight down or straight ahead.
//
// ── THE LINE BUDGET (bounded by construction) ───────────────────────────────
//   window on one axis      2R = 2 * KGRID_HALF_CELLS * spacing
//   indices in it           floor(hi/s) - ceil(lo/s) + 1  <=  2*KGRID_HALF_CELLS + 1
//                           = 241 with KGRID_HALF_CELLS = 120
//   both axes               482
//   axes overlay            6 (each world axis split at the origin — shakeout A)
//   MAIN BATCH worst case   488  <=  KGRID_MAX_SEGMENTS (500)
//   majors (every 10th)     ceil(241/10) = 25 per axis -> 50
//   HALO BATCH worst case   50   <=  KGRID_HALO_SEGMENTS (56)
//   TOTAL                   538 segments, two RC_DRAW_LINES commands.
// The `axesOn` skip of index 0 and the behind-the-eye cull only ever REMOVE
// lines, so 538 stands after them too.
//
// ── "ANTI ALIASING", HONESTLY ───────────────────────────────────────────────
// Real AA is not available on this path and that was VERIFIED, not assumed:
//   * kiwi_lines.h TRAP 2 — the editor $line technique consumes the run colour as
//     MATERIAL_COLOR and LERPS toward it, so a sub-1 alpha means "blend less of
//     my colour", not "be transparent".  No per-vertex alpha, no per-vertex fade.
//   * MSAA is not ours to switch on from here: the editor renders into the shared
//     D3D9 device/swap chain set up by r_init.cpp, and turning on multisampling
//     would mean re-creating the device and every render target the RTT viewport
//     path owns.  That is a renderer change, not a grid change, and is out of
//     scope for this round.
// So the softening is FAKED, the way DCC line overlays have always faked it: each
// MAJOR line is drawn TWICE — a width-2 pass at KGRID_HALO_MUL of its brightness
// first, then the width-1 bright core on top.  $line is depthTest LESSEQUAL and
// depthWrite ON (main/materials/$line: loadBits[1] = 0x0000000d), so the coplanar
// core passes the depth the halo just wrote and lands over it.  Cost: one extra
// RC_DRAW_LINES, one extra SetMaterialColor per distance band, <= 50 segments.
// MINOR lines are single-pass — haloing them would fill the gaps between them.
// ─────────────────────────────────────────────────────────────────────────────

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AG — THE FAR RING.  v3 WAS RIGHT ABOUT DENSITY AND WRONG ABOUT REACH.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "the grid still shrinks and scales inappropriately at
// narrow camera angles.  Needs to be fixed, dont understand why this is
// happening."  The screenshots show a near-horizontal camera with the ground
// lattice covering a patch around the viewer and ENDING, in mid-air, well short
// of the geometry beyond it.
//
// ── THE DIAGNOSIS, AND IT IS NOT A REGRESSION OF v2 ─────────────────────────
// Nothing is shrinking.  The lattice window is 2R wide with R = spacing *
// KGRID_HALF_CELLS, and R depends on the camera's HEIGHT alone — by design, since
// that is what removed v2's pitch coupling.  At 192 units up on a 10-unit grid
// that is R = 1200, so the emitted window is 2400 units across and the grid stops
// 1740 units in front of the eye.  Look STRAIGHT DOWN and that window covers the
// whole screen, so nobody notices.  Tip toward the horizon and the VISIBLE ground
// plane runs to tens of thousands of units while the window does not move —
// so the user sees the window's own edge, and reads it (reasonably) as the grid
// shrinking with the angle.
//
// So: v3 made DENSITY independent of pitch, which was the directive, and left
// REACH independent of pitch too — which was never the directive and is what the
// report is about.  Reach and density are different questions.
//
// ── WHY THE FIX IS A COARSER RING AND NOT A BIGGER WINDOW ───────────────────
// Covering a ground distance D at spacing S costs 2D/S lines per axis.  The reach
// cannot grow at fixed spacing without the line count growing with it — that is
// arithmetic, not a budget preference.  But R = 120 * spacing is not an arbitrary
// number either; it is very nearly the SCREEN-SPACE limit already:
//
//     a cell of size S at ground distance d subtends about  (S/d) * f  pixels
//     across, where f = (height/2) / (tan(fov/2)*0.75) is the pixel focal length
//     (CameraCalcRayDir's own scale, camwnd.cpp:3090).  For a 65-degree fov at
//     900 px that is f ~ 940, so a 4-pixel floor puts d_max ~ 235 * S — the same
//     order as the 120 * S the window already reaches.
//
// i.e. the FINE tier is drawn to about where it stops being legible, and pushing
// it further would spend hundreds of segments on sub-pixel moire.  The honest
// extension is therefore a SECOND, COARSER LATTICE for the far field, which is
// what every DCC ground plane does:
//
//   FAR RING     spacing  = near spacing << KGRID_FAR_STEP   (8x)
//                reach    = near reach   << KGRID_FAR_STEP   (8x)
//                lines    = MAJORS ONLY (ROUND AH — see below)
//   and it is emitted only when there is visible ground beyond the near window.
//
// The near field is COMPLETELY UNCHANGED — the part that matters for the original
// report.  Density at the camera still comes from altitude alone; the far ring
// only adds lines the near window was never going to draw.  It is emitted BEFORE
// the near pass so the bright near lines land on top of the coincident far ones
// (8 * spacing is a multiple of spacing, so every far line is also a near line
// inside the near window; $line is depthTest LESSEQUAL so the later, brighter
// draw wins).
//
// ── ROUND AH: THE FAR RING IS MAJORS ONLY (the black-slab correction) ───────
// Round AG shipped the ring with BOTH runs and produced a solid black ground
// slab ("grid problem is worse").  The refutation is structural, not a tuning
// matter: farOn requires (visible ground) > 1.30R, and R tracks height ~9-10x,
// so the ring only ever exists below ~5 deg of pitch — the regime where the
// round-S edge fade already establishes that a 241-line tier compressed into a
// few pixel rows merges into a slab (that is WHY minors fade out of the NEAR
// lattice there).  The far window is 8x that span through the same pixels, at
// band2 * KGRID_FAR_MUL * fade ~= 0.02 grey — denser than pixels AND darker
// than the viewport clear: a black slab by construction, plus 482 segments of
// the shared batch spent before the near lattice drew.  Minors at these angles
// cannot resolve as lines at any brightness, so the ring draws the one thing
// that can: its majors — <=25 per axis, ~50 segments.
//
// ── HOW "IS THERE VISIBLE GROUND OUT THERE" IS ANSWERED ─────────────────────
// By the frustum, and yes, that is the machinery v2 died of.  The difference is
// what it is allowed to decide.  v2 fed the frustum footprint into the SPACING
// (`while (span/spacing > 200) spacing *= 2`), so a shallow angle coarsened the
// grid under the viewer's feet — the near field emptied out.  Here the frustum
// answers ONE boolean, "is there ground past R", and the far ring it turns on has
// a FIXED spacing and a FIXED reach.  A pitch change can add or remove the far
// ring; it can never change a single line of the near lattice.
//
// The probe is four rays through Ed_CameraCalcRayDir (camwnd.cpp:3144, the same
// forwarder the picker uses, so ortho is handled for free): the three along the
// TOP edge (the shallowest the frustum has) and one at the bottom centre.
//   * bottom ray points AWAY from the plane   -> no ground in view at all -> off
//   * top rays point away, bottom toward      -> the HORIZON is on screen ->
//                                                unbounded -> clamp to the cap
//   * all toward                              -> d = h * |horiz| / |dz|, max of
//                                                the three top rays
// The result is clamped to KGRID_FAR_MAX_MUL * R and passed through a Schmitt
// latch (KGRID_FAR_ON / KGRID_FAR_OFF) so a camera parked at the threshold cannot
// blink the ring on and off.
//
// ── THE BUDGET, RESTATED (ROUND AH) ─────────────────────────────────────────
//   near window, both axes    482        (unchanged, 2*120+1 per axis)
//   axes overlay                6
//   FAR ring, MAJORS ONLY      50        (<=25 per axis: i % 10 == 0 in 241)
//   MAIN BATCH worst case     538  <=  KGRID_MAX_SEGMENTS (1024)
//   halo: NEAR MAJORS ONLY     50  <=  KGRID_HALO_SEGMENTS (56)   (unchanged)
// The far ring gets no halo: it is a dim, distant tier and doubling its cost to
// soften it would be the wrong trade.  The width-1 cap stays at 1024 — the slack
// is headroom, not a plan.
// ─────────────────────────────────────────────────────────────────────────────

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AK — THE ORTHOGRAPHIC GRID IS DERIVED FROM WORLD-PER-PIXEL, FULL STOP.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "There is still (after like 10 attempts) bugs in the
// grid where it snaps and morphs and transforms into an ugly mess.  You really
// gotta fix this."  The screenshots are ORTHO views (the viewcube pill reads "P",
// i.e. "switch to perspective", so perspective is NOT what is on screen) showing
// banded density messes, dense near-black far fields, and the whole lattice
// re-scaling as the camera merely ORBITS.
//
// ── THE DIAGNOSIS: THE LOD INPUT IS UNRELATED TO THE ORTHO IMAGE ────────────
// v3's LOD is `PickLod( base, fabsf(c->origin[2]) )` — camera ALTITUDE.  Under a
// PERSPECTIVE projection that is a reasonable proxy for "how big is a cell on
// screen", because the eye height IS the distance to the ground under your feet.
// Under an ORTHOGRAPHIC projection it is not a proxy for anything:
//
//   * ORTHO ZOOM IS `KiwiCam_OrthoHalfHeight()`, NOT ALTITUDE.  The camera origin
//     is `s_lookAt - forward * s_dist` (kiwi_camera.cpp:812) and the image scale
//     is `s_dist * tan(fov/2) * 0.75`.  In a TOP view forward[2] = -1 so altitude
//     happens to track s_dist; in ANY OTHER view forward[2] is smaller, and the
//     altitude is dominated by `s_lookAt[2]` — the pivot's height, which the zoom
//     does not touch at all.  Focus on a brush 2000 units up and zoom all the way
//     in: the altitude stays ~2000, the LOD stays coarse, and the cells blow up
//     to fill the screen.  Zoom all the way out: the altitude is still ~2000, the
//     LOD is still the same tier, and the cells shrink to sub-pixel — 241 lines
//     per axis inside a few hundred pixels, which is the "dense black field".
//   * AND ORBITING CHANGES IT.  `origin[2] = lookAt[2] - forward[2]*s_dist`, so
//     tipping the camera moves the altitude by up to s_dist with NOTHING on screen
//     changing scale.  Cross a Schmitt threshold mid-orbit and the entire lattice
//     doubles or halves.  That is "it morphs and transforms" — literally, the LOD
//     stepping on an input the user cannot see.
//   * THE FAR RING'S PROBE IS ALSO ALTITUDE MATH.  `FarGroundDistance` returns
//     `h * horiz / dz` off camera rays — but in ortho every ray is PARALLEL, so
//     all three top-edge rays return the same number and that number is "where the
//     eye's own ray meets the ground", which has nothing to do with how much
//     ground is in the ORTHO view rect.  A high pivot at a shallow angle latched
//     the ring ON over a tightly zoomed image: an 8x-spaced lattice at
//     band2 * 0.62 * fade grey, i.e. the dense dark field again.
//
// ── THE FIX: IN ORTHO, WORLD-PER-PIXEL IS EXACT AND CONSTANT ────────────────
// `KiwiCam_WorldPerPixel` (kiwi_camera.cpp:662) returns `2*H/height` for the ortho
// arm and does not read its `world` argument at all — a parallel projection has
// ONE scale for the whole image, by definition.  So the ortho grid is derived from
// it and from NOTHING else:
//
//   SPACING   the smallest tier base*2^k whose projected cell is at least
//             KGRID_PX_MIN pixels across — on the axis the TILT COMPRESSES:
//                 px(k) = base*2^k * |vpn·Z| / wpp        (see THE FORESHORTENED
//                                                          FLOOR, below)
//             Schmitt-banded exactly as the altitude latch is (KGRID_PX_REFINE),
//             so a zoom parked on a boundary cannot flicker.
//             ROUND AK wrote `px(k) = base*2^k / wpp` here and called it "NO pitch
//             term".  That was the bug round AL fixed: the un-foreshortened cell
//             is the size of the axis that does NOT collapse, so the floor was
//             being applied to the wrong number and the compressed axis dived
//             under it at every tilt.  The |vpn·Z| factor is a FRAME-LEVEL scalar
//             (constant across the image — parallel projection, no divide), not
//             the per-line pitch coupling round M banned; see below.
//   REACH     the EXACT XY footprint of the visible ground, per axis.  In ortho
//             this is a bounded parallelogram and it is closed-form:
//                 halfW = wpp*width/2,  halfH = wpp*height/2
//                 screen-right on the ground = c->vright        (horizontal, |.|=1)
//                 screen-up   on the ground = g = vup - vpn*(vup[2]/vpn[2])
//                 need[a] = halfW*|vright[a]| + halfH*|g[a]|,   a = X, Y
//             centred on the SCREEN-CENTRE GROUND HIT, not on the eye's ground
//             projection — in ortho those differ by the whole pitch offset.
//   CENTRE    that hit, pulled proportionally back toward the eye's own ground
//             point by whatever the clamp below took away (see the budget).
//
// ── THE 1/sin QUESTION, ANSWERED HONESTLY RATHER THAN WAVED AT ──────────────
// `g` carries a factor of `vup[2]/vpn[2]`, i.e. 1/sin(pitch), so `need` DOES grow
// without bound as the view goes edge-on — the line COUNT is `2*need/spacing` and
// spacing does not grow with it, because spacing is now pinned to a PIXEL size.
// Both available answers were considered:
//   (a) coarsen the spacing by the same 1/sin factor for the far span.  REJECTED:
//       that is v2's `while (span/spacing > 200) spacing *= 2` wearing a different
//       hat — it puts pitch back into the density under the viewer's feet, which
//       is the one thing round M exists to prevent.
//   (b) CLAMP THE REACH at KGRID_HALF_CELLS cells and let round S's edge fade
//       cover the rest.  TAKEN.  `vpn[2]` is floored at KGRID_ORTHO_MIN_SIN, which
//       is deliberately KGRID_EDGE_FULL (0.08, ~4.6 deg) — the angle below which
//       the edge ramp is ALREADY dropping minors and dimming the lattice toward
//       nothing, because 241 lines compressed into a few pixel rows cannot resolve
//       at any brightness.  Clamping a reach the viewer cannot resolve anyway
//       costs nothing real; coarsening the near field to buy it costs the whole
//       directive.
// The FAR RING survives in ortho for exactly the case where the clamp bit, and it
// needs NO frustum probe there: `need[a] > reach[a]` is the exact statement of
// "there is visible ground the near window does not cover".  Same 1.30/1.05
// Schmitt latch, same majors-only rule round AH established.
//
// ── KIWI-UX (ROUND AL, ITEM 3): THE FORESHORTENED FLOOR ────────────────────
// USER REPORT, verbatim: "Grid ugliness still there. not fixed."  The screenshot
// is a TILTED ortho view at 6 in spacing showing anisotropic dense line bands in
// the middle distance — the classic under-a-pixel moire, on ONE axis only.
//
// THE DERIVATION.  Under a parallel projection a world displacement d lands on
// screen at (d·vright, d·vup)/wpp — no divide, so this is exact everywhere in the
// image.  CamWnd_BuildMatrix builds the basis with no roll, so at pitch phi and
// yaw psi
//     vpn    = ( cos phi cos psi,  cos phi sin psi, -sin phi )
//     vright = (        -sin psi,          cos psi,        0 )
//     vup    = ( sin phi cos psi,  sin phi sin psi,  cos phi )
// Take any GROUND displacement d (d_z = 0) and split it as d = a*vright + b*h with
// h = (cos psi, sin psi, 0) the ground-forward direction.  Then
//     d · vright = a                (vright is horizontal — this axis is UNTOUCHED)
//     d · vup    = b * sin phi      (h · vup = sin phi — this axis is COMPRESSED)
// The ground plane is therefore compressed by EXACTLY |sin phi| = |vpn·Z| along
// the screen's vertical axis and not at all along vright, uniformly over the whole
// view.  The two line families' screen pitches are
//     lines parallel to h        :  spacing / wpp                (full)
//     lines parallel to vright   :  spacing * |vpn·Z| / wpp      (compressed)
// and the second family is what stacks into the bands.  A 9 px floor on the first
// is a 9*|vpn·Z| px floor on the second: 4.5 px at 30 deg, 2.3 px at 15 deg, i.e.
// straight under the resolvable limit — which is the report.  The ladder now takes
// wppEff = wpp / |vpn·Z| and everything else is unchanged.
//
// WHY THIS IS NOT THE PITCH COUPLING ROUND M BANNED.  That disease was SPATIAL —
// a per-line or per-row factor under a PERSPECTIVE divide, so density varied
// across one image and crawled within one frame.  |vpn·Z| in ortho is one scalar
// per frame, identical at every pixel, and it feeds the SAME Schmitt-latched
// integer s_lod that zoom already feeds.  Orbiting can now move the tier — but
// unlike round AK's altitude ladder, which moved it while nothing on screen
// changed scale, the cells genuinely ARE compressing when it does.  KGRID_PX_REFINE
// / KGRID_PX_MIN = 22/9 = 2.44 > 2, so the latch still cannot oscillate.
//
// THE FLOOR AND THE EDGE FADE HAND OFF AT ONE CONSTANT.  |vpn·Z| is clamped at
// KGRID_ORTHO_MIN_SIN, which is KGRID_EDGE_FULL — the exact angle below which
// EdgeFade starts ramping the lattice out and OrthoFootprint already clamps.  So
// the ladder stops coarsening precisely where the fade takes over, rather than the
// two chasing each other through the grazing regime.  Bounded: 1/0.08 = 12.5x,
// under four tiers, and KGRID_LOD_MAX still caps it.
//
// THE FAR RING AND THE MAJORS NEED NO SEPARATE GATE, and this was checked rather
// than assumed: the far ring's spacing is the near spacing << KGRID_FAR_STEP (8x)
// and the majors are a coarser multiple again, so both are 8x or more ABOVE
// whatever floor the near lattice just satisfied.  Coarser can only be safer —
// the direction of the inequality is the whole argument.  Nothing else in the
// ortho arm reads a pixel size.
//
// ── DISTANCE BANDS ARE OFF IN ORTHO, AND THAT IS THE "BANDING" REPORT ───────
// The three brightness bands model PERSPECTIVE attenuation — lines further from
// the eye are dimmer because they are further away.  A parallel projection has no
// such attenuation: every line is at the same image scale, so banding by world
// distance from the camera's ground point paints a literal bullseye centred on the
// viewer, which is what "banded density messes" describes.  Ortho draws ONE flat
// tier (band 0, the brightest), majors then minors.  This also halves the colour
// runs and therefore the RC_DRAW_LINES count.
//
// ── THE ORTHO BUDGET, DECLARED ─────────────────────────────────────────────
//   spacing floor       ROUND AL: px(k) = spacing*|vpn·Z|/wpp >= KGRID_PX_MIN in
//                       steady state.  The COMPRESSED axis is now the one that is
//                       bounded, so the cell count a W x H px viewport can show is
//                       bounded on BOTH axes and not just one:
//                         compressed axis   <= H / KGRID_PX_MIN
//                         free axis         <= W / (KGRID_PX_MIN / |vpn·Z|)
//                                            <= W / KGRID_PX_MIN     (|vpn·Z| <= 1)
//                       At 1920x1080 and 9 px that is still 213 x 120 cells, and
//                       every tilt from here on lands STRICTLY UNDER that — the
//                       ladder only ever coarsens relative to round AK, never
//                       refines, so no budget line in this table can grow.
//   reach clamp         reach[a] <= KGRID_HALF_CELLS * spacing, per axis, so
//                       indices per axis <= 2*KGRID_HALF_CELLS + 1 = 241
//                       — the SAME bound v3 declared, reached a different way.
//                       ROUND AL: coarser spacing makes `cap` LARGER, so the clamp
//                       bites less often and the far ring latches on less often.
//                       Both directions are away from the budget, not toward it.
//   near window         241 * 2 axes                     = 482
//   axes overlay                                         =   6
//   FAR ring, majors only (<=25/axis, round AH)          =  50
//   MAIN BATCH worst case                                = 538 <= 1024
//   halo: near majors only                               =  50 <=   56
// Unchanged from round AH by construction: the clamp is the same clamp.
//
// ── AND THE SNAP IS NOT THE DISPLAY TIER (checked, not assumed) ────────────
// `KiwiGrid_Snap` quantises with `KiwiUnits_GridSpacingWorld()` (kiwi_grid.cpp,
// below) and `KiwiSnap_LightGridAxis` with the same call (kiwi_snap.cpp:1111).
// NEITHER reads `s_lod`, which is a file-static of this file's anonymous
// namespace and is never exported.  The display tier coarsening as you zoom out
// therefore cannot move a vertex: you still land on the spacing the units pill
// says.  That is a REQUIREMENT of this design (spacing now changes with zoom,
// which it never did before) and it holds without any change.
// ─────────────────────────────────────────────────────────────────────────────

// ── ROUND AK: the ortho LOD, in PIXELS ──────────────────────────────────────
// The smallest legible cell, and the size at which the NEXT FINER tier is taken.
// KGRID_PX_REFINE must exceed 2*KGRID_PX_MIN or the ladder can oscillate (a
// coarsen doubles px, a refine halves it); 22 / 9 is a 2.44x dead band, the same
// shape as the altitude latch's 1.67x.  Steady state is therefore a cell between
// 9 and 22 px — legible without being a wall.
#define KGRID_PX_MIN         9.0f
#define KGRID_PX_REFINE      22.0f
// The floor on |vpn[2]| used when projecting the view rect onto Z=0.  DEFINED AS
// KGRID_EDGE_FULL rather than as its value (see the 1/sin discussion above):
// below that angle round S's ramp already owns the frame, and the two numbers
// must never drift apart.  Expanded at the use site, so it does not matter that
// KGRID_EDGE_FULL is spelled further down this header.
#define KGRID_ORTHO_MIN_SIN  KGRID_EDGE_FULL

// ── budget ──────────────────────────────────────────────────────────────────
// Two batches, because KiwiLines_Begin fixes ONE width for a batch and the halo
// needs a different one.  Both are hard caps; the math above says neither binds.
// (Round AG: the width-1 cap went 500 -> 1024 to hold the far ring.  KiwiLines'
// own 512-vertex staging array auto-flushes, so a bigger batch costs extra
// RC_DRAW_LINES commands and nothing else — kiwi_lines.cpp KLINES_VERTS.)
#define KGRID_MAX_SEGMENTS   1024       // width-1 batch: near grid + far ring + axes
#define KGRID_HALO_SEGMENTS  56         // width-2 batch: the major-line halo

// ── ROUND AG: the far ring ──────────────────────────────────────────────────
// Spacing/reach multiplier, as a shift.  3 = 8x: one ring buys 8x the reach for
// the same line count, which covers a 9600-unit ground span from the default
// 192-unit eye height — past the far side of any CoD4 map.
#define KGRID_FAR_STEP       3
// The reach cap, in units of the NEAR reach R.  Equal to 1 << KGRID_FAR_STEP by
// construction: the ring cannot usefully claim more ground than it draws.
#define KGRID_FAR_MAX_MUL    8.0f
// Schmitt latch on (visible ground distance / R).  1.30 / 1.05 leaves a 1.24x
// dead band — the same shape as the LOD's, for the same reason.
#define KGRID_FAR_ON         1.30f
#define KGRID_FAR_OFF        1.05f
// The far ring's share of the FAR band's colour.  Dim on purpose: it is context,
// not a measuring tool, and it must never compete with the near lattice.
#define KGRID_FAR_MUL        0.62f

// ── KIWI-UX (ROUND AM, ITEM 3): WHEN THE FAR RING STOPS BEING A GRID ────────
// USER REPORT, verbatim: "Grid ugliness still there. not fixed." — with a
// near-horizon ORTHO screenshot showing a handful of stray long dark diagonals
// crossing an otherwise empty view.
//
// WHICH PASS THEY ARE, identified rather than guessed.  They are the FAR RING's
// majors, and four properties of that pass pin it:
//   * DARK.  The ring paints KGRID_BANDS[2][1] — the FAR band's major colour,
//     the dimmest tier there is — times KGRID_FAR_MUL (0.62).  Nothing else in
//     the ortho arm draws that dim; DrawGround uses band 0 (:767-780) and the
//     axes are saturated colours.
//   * LONG, and stopping in mid-air.  Its reach is ClampAxis-clamped exactly as
//     the near window's is (:494), so at grazing angles each line ends at the
//     window edge rather than at the screen edge — the round-AK/AL reach-clamp
//     regime, on the pass with the longest lines.
//   * DIAGONAL.  They are world-axis-aligned and the view is yawed.
//   * A HANDFUL.  Majors only since round AH, at 8x the near spacing, so the
//     count is `<= 25 per axis` at the top of its range and single digits at the
//     bottom (the count arithmetic is at KiwiGrid_Draw).
//
// AND WHY THE EXISTING FADE DOES NOT CATCH THEM — the hole is exact.  EdgeFade
// (:678-686) returns 1.0 for every |vpn.Z| >= KGRID_EDGE_FULL (0.08), so the
// ring's `KGRID_FAR_MUL * fade` is at FULL strength everywhere above about 4.6
// degrees.  Between there and roughly 9 degrees the near lattice is reach-clamped
// into a small patch, and the ring is the only thing left drawing across the rest
// of the view.  The fade was doing its job; the band it protects simply ends
// below where this starts.
//
// TWO GATES, both on the RING only, neither touching the near lattice or the
// axes (the axes are exempt from every fade BY DESIGN — three lines that do not
// alias and that say which way is which when everything else has gone):
//   * an ANGLE floor at 2x KGRID_EDGE_FULL.  Twice the constant the fade hands
//     off at, so the two are stacked rather than overlapping and there is no
//     angle at which both are half-applying.
//   * a COUNT floor.  Fewer than this many lines in the whole ring is not a grid
//     — it is clutter that happens to be straight, which is the report word for
//     word.  6 is two per axis plus a margin; at 5 or fewer no reading of the
//     picture recovers a lattice from it.
#define KGRID_FAR_MIN_SIN    ( 2.0f * KGRID_EDGE_FULL )
#define KGRID_FAR_MIN_LINES  6

// ── the window ──────────────────────────────────────────────────────────────
// Half-width of the lattice window, in CELLS.  This is the one number that sets
// the budget: 2*120 + 1 = 241 lines per axis.
#define KGRID_HALF_CELLS     120
// How far the window centre is pushed along the yaw-plane forward, as a fraction
// of R.  0.45 puts 1.45R in front and 0.55R behind.
#define KGRID_FWD_BIAS       0.45f

// ── the LOD, from ALTITUDE ONLY ─────────────────────────────────────────────
// Nominal relation: a cell should be about 1/KGRID_CELLS_PER_HEIGHT of the
// camera's height above the ground plane, so the grid reads the same at 100 units
// up and at 10000.  The Schmitt band is asymmetric on purpose:
//     coarsen (k -> k+1)  when  h >  base*2^k     * C * KGRID_LOD_UP   (2.0)
//     refine  (k -> k-1)  when  h <  base*2^(k-1) * C * KGRID_LOD_DOWN (1.2)
// The two thresholds are measured against DIFFERENT levels, and 2.0/1.2 leaves a
// 1.67x dead band, so a camera parked on a boundary cannot oscillate.  This is
// hysteresis on ONE scalar (height) for the WHOLE grid — v2's per-line Schmitt
// table, with its slot-ownership problem, is gone entirely.
#define KGRID_CELLS_PER_HEIGHT 16.0f
#define KGRID_LOD_UP           2.0f
#define KGRID_LOD_DOWN         1.2f
#define KGRID_LOD_MAX          20       // 2^20 * base — far past any real map

// ── brightness ──────────────────────────────────────────────────────────────
// THREE distance bands, and they are BRIGHTNESS ONLY: every band draws both its
// minors and its majors, so a line never appears or disappears with camera
// motion.  (v2's outer bands dropped minors outright and culled on projected
// pixel gap; both were sources of the twinkle they were meant to cure.)
#define KGRID_BAND_COUNT     3
#define KGRID_BAND_NEAR      0.40f      // fraction of R
#define KGRID_BAND_MID       0.75f
// The halo pass's share of the major colour it sits under.
#define KGRID_HALO_MUL       0.35f

// ── ROUND S: THE EDGE-ON FADE ───────────────────────────────────────────────
// USER REPORT: an axis view down the grid PLANE (the BOT / TOP-side views, where
// the eye lies in or near Z = 0) collapsed the whole lattice into a dense moiré
// slab a couple of pixels tall.
//
// WHY.  The lattice is a set of world lines ON Z = 0.  Their projected separation
// is proportional to the sine of the angle between the view direction and the
// plane — i.e. to |vpn·Z| — so as the view goes edge-on, 241 lines per axis land
// inside a handful of pixels.  No amount of LOD helps: the LOD is driven by the
// camera's HEIGHT (deliberately, kiwi_grid.h THE LOD), and at an edge-on view the
// height is exactly what has gone to zero.  v3 simply had no term for it.
//
// THE RAMP, on |vpn·Z| (0 = exactly in the plane, 1 = straight down):
//     |vpn·Z| <  KGRID_EDGE_OFF  (0.03, ~1.7 deg)   the ground lattice is SKIPPED
//                                                   entirely (the axes still draw)
//     KGRID_EDGE_OFF .. KGRID_EDGE_FULL (0.08, ~4.6 deg)
//                                                   linear brightness ramp
//                                                   t = (|vpn·Z| - OFF)/(FULL - OFF)
//     |vpn·Z| >= KGRID_EDGE_FULL                    unchanged, t = 1
// and, inside the ramp, MINOR lines are dropped below KGRID_EDGE_MINOR (t < 0.5).
// They are 10x denser than the majors and are what actually aliases, so removing
// them first keeps a readable major lattice for twice as long as fading alone.
//
// BRIGHTNESS, NOT ALPHA — kiwi_lines.h TRAP 2: on the $line path an alpha below 1
// means "blend less of my colour in", not "be transparent".  The ramp therefore
// scales the band RGB toward zero, exactly as the distance bands do.
#define KGRID_EDGE_OFF       0.03f
#define KGRID_EDGE_FULL      0.08f
#define KGRID_EDGE_MINOR     0.5f

// Axis half-length.  Finite on purpose (spec §17 "long but finite"): the engine's
// own world bound — Brush_BuildWindings seeds its AABB at ±131072
// (brush.cpp:1459) — so nothing that can ever be built lies outside it.
#define KGRID_MAX_EXTENT     131072.0f

// Cam_Draw tail hook (// KIWI-UX in camwnd.cpp).  Emits nothing when both
// toggles are off.
void KiwiGrid_Draw();

// Snap a world point to the modern grid (kiwi_units.h spacing, raw world units).
// Exported for the SnapManager (SNAP_GRID).  Returns false (and copies `in`
// through) when the spacing is unusable — or when grid snapping is switched OFF
// (below), which every consumer already handles because it is the same answer an
// unusable spacing gives.
bool KiwiGrid_Snap( const float in[3], float out[3] );

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AJ, ITEM 5 — THE GRID-SNAP MASTER SWITCH
//
// USER DIRECTIVE, verbatim: "Add a checkbox by the grid density setting to
// enable/disable snapping to grid."
//
// ONE BOOLEAN, AND IT IS ENFORCED AT THE TWO PLACES THE LATTICE CAN REACH A
// NUMBER — not at each of the six call sites, which is how a toggle acquires an
// exception:
//   * KiwiGrid_Snap itself, the FULL quantiser (kiwi_snap.cpp arm 9 / SNAP_GRID,
//     kiwi_transform.cpp's move quantise, kiwi_split.cpp, kiwi_dupe.cpp);
//   * KiwiSnap_LightGridAxis, the round-AG magnet BAND (kiwi_extrude.cpp x2,
//     kiwi_transform.cpp's push/pull).
// Both already have a documented "leave the value alone" answer for a degenerate
// spacing, so switching off reuses a path that was always there.
//
// IT IS NOT WHAT CTRL DOES.  Ctrl is the per-gesture, hold-to-suppress override
// (spec §6, deliberately inverted vs other apps) and still suppresses everything
// including geometry snaps; this is a persistent preference about the GRID alone,
// and geometry / face / construction snapping is untouched by it.
//
// Persisted under KiwiUX/GridSnap, default ON — the editor's whole history is
// grid-snapped and a preference that silently changes on upgrade is worse than
// one the user has to find.
bool KiwiGrid_SnapEnabled();
void KiwiGrid_SetSnapEnabled( bool on );
