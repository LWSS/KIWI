#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_skybox.h — KIWI-UX (ROUND AZ, ITEM 3): THE SKY TAB.
// ═════════════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: *"We need a skybox feature.  Invent a nice way to show a
// skybox and allow the camera to ignore it's there so we can fly inside of the skybox
// and see the sky.  Make this a separate tab like the entity tab."*
//
// ── WHAT A SKYBOX IS IN THIS ENGINE, BEFORE ANY UI IS DESIGNED ──────────────────────
// A CoD4 skybox is not a map object, a key, or a special entity.  It is ORDINARY BRUSHES
// carrying a SKY MATERIAL, enclosing the playable space; the game draws the sky through
// them.  That fact sets the whole scope of this file:
//
//   * .map OUTPUT IS UNCHANGED.  A sky brush is a brush with a material on it, so
//     everything here writes stock geometry and nothing here invents map syntax.  A map
//     built with this tab loads in stock Radiant and compiles with the stock tools.
//   * THERE IS NOTHING TO "SET".  There is no map-level skybox slot to point at a
//     material, so this tab's verbs are about GEOMETRY: tag some brushes as sky, or
//     build the enclosing shell.
//   * THE EDITOR ALREADY DRAWS SKY MATERIALS.  camwnd.cpp:2925-2928 deliberately exempts
//     sky from the `d_white` tool-volume substitution, *"whose colormap is real content
//     the editor should show"*.  So a sky brush is already textured with the real sky
//     colormap in the 3D view; this round does not add that, it adds the ability to turn
//     it OFF when the shell is in the way.
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  D-AZ-A — WHAT MARKS A MATERIAL AS SKY.  ONE SPELLING, AND IT IS THE DATA'S.
// ═════════════════════════════════════════════════════════════════════════════════════
// The tree carried THREE different answers before this round and they disagree:
//
//   1. `SURF_SKY` (surfaceFlags bit 2, value 4) — the DATA's own answer.
//      `universal/surfaceflags.h:57` is the infoParms row (`{ "sky", 0, 4, 0x800, 0 }`),
//      `filters.cpp:130` is the same bit in the filter name table.  `MaterialDef_09`
//      folded over the layers ANDs
//      `qtexture_s::color_or_surfacetype_filter` (@0x1C, qe3.h:65).  That field IS the
//      material's surfaceFlags, written at BOTH registration paths
//      (texwnd.cpp:305 and texwnd.cpp:421) straight from the material header.
//   2. leaf-name substring `"sky"` — camwnd.cpp:2803 (the sky-arm predicate) and
//      camwnd.cpp:627 (the editor colour table's `{ "sky", ... }` row).
//   3. base-name substring `"sky_"` — select.cpp:708-727, the `SkyBrushOff` pick skip.
//
// THIS FILE MAKES (1) CANONICAL AND KEEPS (2) AS A FALLBACK, in one function —
// `KiwiSky_IsSkyMaterial`.  The reasoning:
//
//   * (1) IS THE ONLY ONE THAT IS NOT A GUESS.  It is the flag the compiler and the game
//     act on.  A material named `desert_backdrop` with `SURF_SKY`
//     set is sky; a material named `sky_scraper_wall` without it is not, and (2) and (3)
//     get both of those wrong.
//   * THE FALLBACK IS NOT SUPERSTITION, it covers a real state.  `Load_Materials`
//     (texwnd.cpp:431) reads each 0x28 material header directly, so the flags are
//     normally real without registering a render material — but a material reached
//     through some other path can arrive with a zero filter word, and for those the name
//     is the only evidence there is.  The fallback is therefore gated on the flags being
//     ZERO, not OR'd unconditionally: where the data has spoken, the data wins.
//   * (3) IS LEFT ALONE WHERE IT IS, and merely WIDENED.  `select.cpp:710-727` is the
//     binary's own `SkyBrushOff` behaviour; round AZ ORs this predicate into it rather
//     than replacing it, so the ported test still does exactly what it did and the pref
//     now also works for a sky material that is not called `sky_*`.
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  D-AZ-B — "THE CAMERA IGNORES IT": WHAT THE ASK ACTUALLY IS
// ═════════════════════════════════════════════════════════════════════════════════════
// *"allow the camera to ignore it's there so we can fly inside of the skybox and see the
// sky"* reads like a COLLISION request.  It is not one, and that was checked rather than
// assumed: **KIWI's camera has no collision at all.**  `KiwiCam_FlyTick` and
// `KiwiCam_Translate` (kiwi_camera.cpp) write `camera_s::origin` directly with no trace,
// and the only thing in the dolly that even looks at geometry is the zoom-to-cursor
// surface reference, which stops the eye SHORT of a wall it is flying TOWARD and does
// nothing to a wall it is flying THROUGH.  So "fly inside" already works today.
//
// WHAT DOES NOT WORK IS SEEING.  The two things a mapper needs are in tension:
//
//   FROM OUTSIDE the shell — which is where you are whenever you frame the whole map,
//   press `/`, or orbit out to look at the layout — six sky-textured walls stand between
//   the camera and everything you are trying to look at.  The map is inside an opaque
//   box.  This is the complaint.
//
//   FROM INSIDE the shell — standing in the level — those same faces ARE the sky, and
//   you want them textured, because that is the whole point of choosing a sky material.
//
// ── THE RULING: THE TREATMENT IS VIEW-DEPENDENT ─────────────────────────────────────
// `KiwiSky_SeeThroughFace()` answers "draw THIS sky face as a translucent tool volume
// right now", and it is TRUE only when the toggle is on AND the face is in the way.
// Round AZ read "in the way" as THE CAMERA IS OUTSIDE the union bounding box of every
// sky brush in the map; **D-BC-A below is the shipped rule** and reads it per FACE.
// Either way: work inside the shell and the sky is textured, look at the map from
// outside and what stands in front of it turns to film.
//
// WHY THE UNION AABB AND NOT A PER-FACE TEST.  A per-face "is the eye in front of this
// plane" test cannot answer the question: standing outside the shell and standing inside
// it both put the eye in front of SOME faces, so the two cases are indistinguishable
// face by face.  The question "am I in the map or looking at it" is a question about the
// SHELL, and the shell's union box is the cheapest honest form of it.  It is also
// correct for the degenerate cases by construction: a map with no sky brushes has an
// inverted box that contains nothing, so `SeeThroughFace` is false and nothing changes
// for a map that never used this feature.
//
// WHY IT IS TRANSLUCENT AND NOT HIDDEN.  Hiding the shell would be simpler and is wrong:
// a mapper needs to SEE where the sky boundary is while they build against it, and a
// hidden brush you can still select and still compile is exactly the kind of invisible
// state this editor has been removing round after round.  Translucent-tinted says "this
// is here, it is sky, and it is not in your way".
//
// WHAT IT COSTS TO RENDER.  No new material and no second pass — it is the same
// `g_qeglobals.d_white` handle at `TECHNIQUE_UNLIT` every other see-through tool volume
// already draws through (clip / trigger / hint / skip / portal / origin /
// lightgrid_volume, camwnd.cpp:2935-2940: *"all 0x08128965 like white_tools, i.e. already
// blended and already depth-write-free"*).  It is NOT one term in the substitution
// expression, though: the shipped arm is its own `if` AHEAD of it (camwnd.cpp:2989-2997),
// because the film needs a different DRAW rather than a different material —
// `Cam_DrawFaceTinted` with `MATERIAL_COLOR {0,0,0,0}` and a packed per-vertex RGBA whose
// alpha is the opacity (0.30, the sky-blue of the editor colour table at camwnd.cpp:627).
// Routing to `d_white` alone only drops the depth write; the per-vertex alpha is what
// makes it translucent.
//
// ── THE HONEST LIMIT ON "SEE THE SKY" ───────────────────────────────────────────────
// From inside, what you see is the sky COLORMAP laid flat on the brush faces.  That is
// what stock Radiant shows and it is what the editor's renderer can show: there is no
// sky dome, no `TECHNIQUE_SKY` in this tree at all (the engine's only sky symbol is
// `TEXTURE_SRC_CODE_SKY = 0xE`, r_material.h:44, a runtime code-image slot the editor
// never binds).  The tab does not pretend otherwise.
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  ROUND BA AMENDS D-AZ-B ON TWO POINTS, AND THE PARAGRAPH ABOVE ON ONE
// ═════════════════════════════════════════════════════════════════════════════════════
// USER REPORT after round AZ shipped: *"sky's are all blue, make it so they work
// properly.  It works in the real radiant and we're using the same rendering code
// basically, so it should work here."*  The user is right on both halves.
//
//  1. THERE IS A SKY TECHNIQUE AFTER ALL, and the paragraph above is wrong to say the
//     editor can only lay a colormap flat.  Decoded from the shipped assets:
//     main/techsets/wc_sky.techset maps "unlit" — and every lit/fakelight slot — to the
//     technique `sky`; main/techniques/sky.tech is `stateMap "color_only"`,
//     `pixelShader 2.0 "sky.hlsl" { skyMapSampler = material.colorMap; }`,
//     `vertex.position = code.position`.  It samples the CUBEMAP by a direction it
//     computes itself.  That is the binary's editor sky path, it was already reachable
//     from the ported draw, and KIWI never reached it — see 2.
//  2. THE FLAT BLUE WAS THE EDITOR COLOUR TABLE, NOT THE SKY.  `Cam_EditorMaterialColor`
//     (camwnd.cpp:618-641) leaves `out[3] = 1.0f`, and w == 1 is a FLAT-COLOUR OVERRIDE
//     in the editor shader family — so the `{ "sky", 0.45f, 0.62f, 0.92f }` row painted
//     every sky face solid (115,158,235) before the technique could sample anything.
//     Round BA emits w = 0 for a textured sky face.
//  3. SEE-THROUGH WAS NEVER TRANSLUCENT.  Round AZ routed it to `d_white`, which removes
//     the DEPTH WRITE and nothing else — `Cam_DrawFace` pins the per-vertex colour to
//     0xFFFFFFFF and white_tools blends SrcAlpha/InvSrcAlpha, so at alpha 255 the blend
//     is arithmetically OPAQUE (camwnd.cpp:667-670 says exactly this for caulk).  It is
//     drawn `Cam_DrawFaceTinted` with a packed per-vertex RGBA at alpha 0.30 now — the
//     binary's own translucent-fill recipe (Cam_DrawSelectedFaceFill at colors[16].a =
//     0.25, kiwi_hover.cpp:156-158 at 0.25-0.35), MATERIAL_COLOR {0,0,0,0} and all.
//  4. THE UNION BOX ASSUMED A SHELL.  A sky CEILING (one flat slab) has a union box
//     nothing can be inside on one axis, so `SeeThroughFace` was true from every camera
//     position and that map's sky was film for ever.  `FlattenRescue` (kiwi_skybox.cpp)
//     widens any axis whose sky extent is under half the MAP's extent to the map's own
//     range.  A real six-brush shell is unaffected, bit for bit.
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  D-AZ-C — THE THUMBNAILS ADD NO DEFAULT-POOL OBJECT.  TWO ARMS, ONE RULE.
// ═════════════════════════════════════════════════════════════════════════════════════
// A tile shows the material's REAL colormap, and it does it with NO render target and no
// DEFAULT-pool D3D resource — which matters, because round AX's lesson is that every
// default-pool object has to be walked into `RTT_ReleaseForReset` by hand and one that
// is not is a device-reset failure waiting to happen.  There are two arms and they do
// not cost the same: the MAPTYPE_2D arm reads the engine's own texture pointer and
// stores nothing; the MAPTYPE_CUBE arm — which is EVERY real sky material, see ROUND BA
// below — owns a D3DPOOL_MANAGED copy of one cube face, cached by material name and
// released from `KiwiSky_ReleaseForReset`.
//
// The route is a straight read down the material the engine already loaded:
//     qtexture_s::next (the registered Material*, qe3.h:50-53)
//       -> Material::textureTable[i] where semantic == 2 (TS_COLOR_MAP, r_image.h:51)
//          -- the same walk Radiant_MaterialTexScale already does at texwnd.cpp:149-160,
//             and for the same reason: the table is hash-sorted, so [0] is often the
//             normalMap
//       -> MaterialTextureDef::u.image (GfxImage*, r_material.h:412)
//       -> GfxImage::texture.map (IDirect3DTexture9*, r_gfx.h:202-209)
// which is handed to ImGui as `(ImTextureID)(intptr_t)tex`, exactly the cast the four
// viewport images already use (imgui_shell.cpp:939).
//
// ── ROUND BA: THE COLORMAP IS A CUBEMAP, SO THE TILE IS A COPY OF ONE FACE ──────────
// Every tile came up "no map" because the last guard below rejects a cubemap — and that
// is not an edge case, it is EVERY sky material.  main/materials/sky_aftermath decodes to
// techSet "sky", textureCount 1, one entry "colorMap"/semantic 2/image "aftermath_ft";
// main/images/aftermath_ft.iwi has IWI flags 0xC6, and 0x04 is IMG_FLAG_CUBEMAP (six
// 1024-square DXT1 faces).  The engine REQUIRES it: R_SetSkyImage Com_Errors with
// *"colorMap '%s' for sky material '%s' is not a cubemap"* (r_bsp_load_obj.cpp:1175).
// textureCount is 1, so there is nothing else in the table to fall back to either.
// One face is therefore copied into a 128-square D3DPOOL_MANAGED 2D texture with
// D3DXLoadSurfaceFromSurface, cached per material name, and released from
// KiwiSky_ReleaseForReset.  MANAGED survives a device reset by contract, so round AX's
// rule — every DEFAULT-pool object must be walked into the reset list by hand — is not
// bent: no DEFAULT-pool object is created here at all.  Which face is a UI setting,
// because the sky shader's own direction swizzle is not readable from this tree.
//
// THE ENGINE'S POINTER IS RE-READ EVERY FRAME AND NEVER STORED — that is the MAPTYPE_2D
// arm, and only that arm.  `GfxTexture` is a UNION whose other arm is `GfxImageLoadDef
// *loadDef` (r_gfx.h:202-209): before upload that field is a load descriptor, not a
// texture, so the read is gated on `!delayLoadPixels` **and** a non-null pointer **and**
// `mapType == MAPTYPE_2D` (r_gfx.h:166 — a cubemap is not an ImGui texture).  Re-reading
// rather than caching is what makes THAT arm device-reset-proof for free: the engine owns
// these images, and if a reset recreates them the next frame simply reads the new pointer.
// The MAPTYPE_CUBE arm cannot do that — it owns its copy — so it carries the lifetime
// instead, in `KiwiSky_ReleaseForReset`.
//
// REGISTRATION IS BUDGETED, at KSKY_REG_PER_FRAME per frame and only for tiles that are
// actually on screen.  `Load_Materials` leaves `qtexture_s::next` null so startup stays
// cheap (texwnd.cpp:359-361), so a tile's first draw has to register.  That is round AX's
// finding applied to a second browser: a visible tile may enqueue, a scrolled-out one may
// not, or the queue order becomes list order instead of reading order.
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  D-AZ-D — THE TWO VERBS, AND WHY BOTH
// ═════════════════════════════════════════════════════════════════════════════════════
// **APPLY TO SELECTION** retextures the selected brushes with the chosen sky material.
// It is not new code: it calls `TexWnd_ApplyMaterialAtIndex`, the SAME funnel a texture
// thumbnail click goes through (texwnd.cpp:1252), so it inherits the ported
// `Brush_SetTexture` apply with its one undo record covering both `selected_brushes` and
// `g_SelectedFaces` (select.cpp:1815-1879), AND round AK's patch re-naturalize fence
// (texwnd.cpp:1188).  Writing a second apply here would have been a second undo shape and
// a second place for the fence to be forgotten.
//
// **CREATE SKYBOX SHELL** builds the enclosing box in one undo record.  Six brushes, not
// one hollow solid, because a brush in this editor is convex — a hollow box IS six
// brushes, and this is the same thing the CSG hollow verb produces.  The math is stated
// on KiwiSky_CreateShell in the .cpp.
//
// BOTH SHIP, and that is a decision rather than indecision.  They answer different
// questions: "I already built my sky geometry, make it sky" and "I have a map and no sky
// at all".  A mapper with an irregular map (a canyon, a interior with a hole in the roof)
// needs the first; a mapper starting out needs the second and would otherwise be drawing
// six brushes by hand at coordinates they have to read off the map bounds themselves.
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  D-AZ-E — PICKING: THE PREF ALREADY EXISTS.  EXPOSE IT, DO NOT REINVENT IT.
// ═════════════════════════════════════════════════════════════════════════════════════
// "Sky brushes should be click-through" is `g_PrefsDlg->sky_brush_off`, which has existed
// since the binary: prefs.h:89, defaulted, loaded and saved (prefs.cpp:93/:202/:307),
// toggled by menu command 33169 (mainfrm.cpp:5902), forwarded into the pick contents mask
// (kiwi_pick.cpp:529) and acted on inside the ray walker itself (select.cpp:708-727).
//
// THE WALKER IS THE ONLY PLACE IT COULD LIVE, and that is worth saying because the
// obvious alternative is wrong: `Pick`'s area arm keeps ONE nearest hit
// (kiwi_pick.cpp:606-607), so rejecting the hit AFTER `Test_Ray` returns means "nothing
// is under the cursor", not "select what is behind it".  Only the walker's `continue`
// moves on to the next brush.  So the tab exposes the existing pref as a checkbox
// ("Sky pickable", inverted for readability) and the ONLY code change is widening the
// walker's material test from the `"sky_"` name substring to this file's predicate.
// ═════════════════════════════════════════════════════════════════════════════════════

struct qtexture_s;
struct brush_t;

// ── the dock window (§9 flag KIWI_WIN_SKY) ──────────────────────────────────────────
// Begins and Ends itself and early-outs when the flag is clear, exactly like
// KiwiEntBrowser_Draw / KiwiOutliner_Draw.
void KiwiSky_Draw();

// ── §15 palette registration + instant dispatch ─────────────────────────────────────
// The WINDOW TOGGLE is registered here (unbound, searchable); the two verbs are
// registered too, because unlike the entity browser's internal drop id they are things a
// mapper can mean on purpose.  Dispatch owns only the verbs — KiwiWindows_DispatchInstant
// owns every §9 window flag, which is the split round W and round AU both made.
void KiwiSky_RegisterCommands();
bool KiwiSky_DispatchInstant( unsigned int cmdId );

// ── THE predicate (D-AZ-A) ──────────────────────────────────────────────────────────
// True when `q` is a sky material: `SURF_SKY` in its surfaceFlags word, or — only when
// that word is zero, i.e. only when the data said nothing — a leaf-name substring "sky",
// which is the rule camwnd.cpp:2803 already uses.  Null-safe (null is not sky).
// This is the one spelling; do not add a fourth.
bool KiwiSky_IsSkyMaterial( const qtexture_s *q );

// ── KIWI-UX (ROUND BB): the same question, asked about a BRUSH ──────────────────────
// True when the brush's FACE 0 carries a sky material at the CURRENT EDIT LAYER — the
// test KiwiSky_SeeThroughFace's union-box walk was already doing longhand.  Exported because
// kiwi_focus.cpp needs it: frame-all must NOT frame the sky shell, or the camera opens
// every map at the distance that encloses the shell and the pivot can never be inside it
// (the whole of round BB's item 1a).  Null-safe.
bool KiwiSky_IsSkyBrush( const brush_t *def );

// ── the render treatment (D-AZ-B, D-BC-A) ───────────────────────────────────────────
// "Draw THIS sky face as a translucent tool volume."  Called PER FACE from the camera
// fill loop, so it is O(1) — the union box is cached and refreshed on a timer
// (KSKY_BOUNDS_MS), never rebuilt per face.  `facePoint` is any world point on the face
// (the winding centroid at the call site); pass nullptr to ask the frame-level question
// instead, which falls back to round BB's containment answer.
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  D-BC-A — THE TEST IS DEPTH, NOT CONTAINMENT, AND THAT IS WHY THE GRID VANISHED
// ═════════════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"yeah the grid broke totally in the last update"* / *"doesn't
// render at all"*.  It is this predicate, and round BB (e1b9ae4) is the commit.
//
// BB replaced round AZ's eye test with `pivot INSIDE the shell OR eye INSIDE the shell`,
// on the correct observation that KIWI's orbit rig leashes the eye s_dist behind the
// pivot so the eye is outside the shell at every working zoom.  What that OR then means,
// under the ORTHO default, is: the pivot is inside the shell whenever the user is working
// (that is where you point the camera), so `inside` is TRUE for ever, `SeeThroughFace` is
// FALSE for ever, and the six sky walls are drawn OPAQUE — while the pseudo-eye is still
// outside them.  You are looking THROUGH the shell's near wall at the map, and that wall
// now fills the frame.
//
// WHY THE GRID SPECIFICALLY, AND WHY THE MAP SURVIVED.  `main/statemaps/color_only.sm`
// (the sky technique's state map) is depthTest LessEqual + depthWrite DISABLE, and the
// ground lattice is submitted BEFORE the world since round S (camwnd.cpp:2691, and the
// long comment there says why).  So: the near sky wall is nearer than the Z=0 lattice,
// passes LESSEQUAL, and paints every grid pixel away — but writes no depth, so the world
// faces flushed after it are not rejected and the map draws over it normally.  Grid gone,
// map fine, which is the report to the letter.
//
// THE RULE THAT ANSWERS BOTH REPORTS AT ONCE.  A sky face is in the way iff it is between
// the viewer and WHAT THE VIEWER IS LOOKING AT — i.e. its view-space depth is less than
// the orbit pivot's.  Then:
//   * framing a map from "outside" (the ortho default): the near wall is filmed, so the
//     map, the grid and the axes are all visible; the FAR walls are beyond the pivot and
//     stay fully textured, so you see real sky behind the map.  That is strictly better
//     than round AZ, which filmed all six.
//   * standing inside the level: every wall is beyond the pivot, so all six are textured.
//     That is round BB's ask, met without making the near wall opaque.
// D-AZ-B's "why not a per-face test" paragraph rejected a per-face PLANE-SIDE test, and
// it was right to; this is not that test.  Plane-side asks "which side of this wall am
// I on" and cannot separate inside from outside.  Depth-versus-pivot asks "is this wall
// in front of my subject", which has the same answer from either side of the shell.
bool KiwiSky_SeeThroughFace( const float *facePoint );

// Drop the cached union box, so the next KiwiSky_SeeThroughFace rebuilds it.  Called by
// this file's own verbs; also safe to call from anywhere that bulk-changes geometry.
void KiwiSky_InvalidateBounds();

// ── KIWI-UX (ROUND BA): the cube-face tile cache ────────────────────────────────────
// A sky material's colorMap is a CUBEMAP (proved from the shipped assets and from
// R_SetSkyImage's own Com_Error, r_bsp_load_obj.cpp:1175), so a flat tile has to be a
// COPY of one face — D-AZ-C's CUBE arm.  Those copies are D3DPOOL_MANAGED, so round AX's
// rule is not bent (it is about DEFAULT-pool objects, and none are created here), and
// this drops them all.
// Called from RTT_ReleaseForReset beside KiwiEntThumb_ReleaseForReset.  Idempotent.
void KiwiSky_ReleaseForReset();
