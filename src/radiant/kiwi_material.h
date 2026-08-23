#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_material.h — ROUND T: MATERIAL INHERITANCE FOR THE KIWI VERBS.
//
// USER DIRECTIVE, verbatim: "When operating (copying, splitting, etc) on
// brushes, I think the Z order bug (still happens) occurs because of the caulk
// texture you're applying.  Make it so the texture is just inherited from the
// parent brush that are being operated on.  The caulk texture is not usable."
//
// ═════════════════════════════════════════════════════════════════════════════
//  WHAT WAS HAPPENING, AND WHY IT IS THE SAME BUG AS THE INVISIBLE FILLETS
// ═════════════════════════════════════════════════════════════════════════════
// Every KIWI verb that creates a NEW SURFACE has, until this round, seeded that
// surface's material from ONE of two places, and both of them were wrong for a
// modelling tool:
//
//   * Cut (C), Split Face (Ctrl+R) and the Q boolean's carve all built their
//     template face with `Ed_BuildClipFaceMaterial_Kiwi` (xywnd.cpp:2233) — the
//     CLIPPER's synthesis, which stamps **caulk** (or nodraw_decal).  That is
//     correct for the clipper, whose whole job is to trim structural brushwork
//     against a plane the mapper drew, and it is what the binary does; it is
//     wrong for "split this thing in two", where the two halves are meant to
//     look exactly like the thing that was split.
//   * The §25 chamfer and the ROUND Q patch fillet inherited from
//     `kiwiBevelEdge_t::srcFace`, which was hardcoded to `adj[0]` — an ARBITRARY
//     one of the two faces meeting at the edge.  Feed that machine an edge whose
//     `adj[0]` happens to be a caulk face (and after ONE Cut, half the edges on
//     the map are exactly that) and the chamfer comes out caulk…
//   * …and then the fillet PATCH copied its material from the CHAMFER FACE
//     (`kiwi_patchfillet.cpp` LandPatches), i.e. from caulk, i.e. **the patch was
//     created carrying a tool material and drew nothing at all**.  That is the
//     "fillets are being created invisible" report: not a broken copy, a
//     faithfully copied INVISIBLE MATERIAL.  The two user reports are one defect
//     with two symptoms, which is why they are fixed in one file.
//
// The z-order half is the same story from round O: caulk and the tool family are
// the depth-writeless class, so a caulk-faced half-brush paints in submission
// order and "the copy always wins".  Stop stamping tool materials onto model
// surfaces and the depth ordering stops being a lottery.
//
// ═════════════════════════════════════════════════════════════════════════════
//  THE RULES, STATED
// ═════════════════════════════════════════════════════════════════════════════
// ── R1: WHAT COUNTS AS INHERITABLE ──────────────────────────────────────────
// A face is a valid material SOURCE when its channel-0 material name is not in
// the TOOL family (caulk / nodraw* / *clip / hint / skip / portal / origin /
// trigger / lightgrid / a "tools/" path — the list is `KMTL_TOOL_NAMES` in
// kiwi_material.cpp, and `KiwiMtl_FaceIsInheritable` is the test).  It is by NAME
// because that is the only classification this editor already trusts: the
// clipper's own decal test (`xywnd.cpp:2247`) and the texture filter
// (`MtlDef_IsFaceFiltered`, mayaexport.cpp:112) both classify by name, and the
// STATE bits are an asset-side fact that round O proved cannot be reasoned about
// from code.  `$default` is deliberately NOT a tool material — it is what an
// untextured brush legitimately wears, and it draws.
//
// ── R2: WHICH FACE A CUT INHERITS FROM ──────────────────────────────────────
// The source brush's largest inheritable face whose plane is MOST PERPENDICULAR
// to the cut plane, scored
//
//     score = area(f) * ( 1 - |n_f · n_cut| )
//
// and maximised.  Reading: a face perpendicular to the cut is a face the cut
// PASSES THROUGH, so the new wall is a continuation of that surface's
// neighbourhood; `area` breaks the ties toward the brush's dominant material
// rather than toward whatever happened to be face 0.  A cut through a
// stone-walled box therefore paints its new walls stone, not caulk.
//
// ── R3: WHEN THERE IS NOTHING TO INHERIT ────────────────────────────────────
// A brush with NO inheritable face is a tool brush (an all-caulk block, a clip
// volume).  It falls back to `Ed_BuildClipFaceMaterial_Kiwi` — the classic
// caulk/nodraw_decal synthesis — so cutting a caulk block still gives caulk and
// nothing invents a visible surface where the mapper wanted none.
//
// ── R4: THE BOOLEAN ─────────────────────────────────────────────────────────
// The difference carves the TARGET, and every carve plane runs through the same
// splitter, so the hole's walls inherit from the TARGET brush under R2 — never
// from the tool.  Stated positively: the tool is scaffolding and is consumed;
// the hole belongs to the solid it was cut into.
//
// ── R5: THE CHAMFER AND THE FILLET PATCH ────────────────────────────────────
// `KiwiMtl_PickSourceFace` is given the two adjacent faces as the preference
// pair; the first INHERITABLE one wins, else R2's brush-wide scan, else `adj[0]`
// unchanged.  The fillet patch then copies from THAT face rather than from the
// chamfer it created, so the patch cannot inherit a material the chamfer only
// has because the chamfer inherited it from somewhere worse.
//
// ── R6: REALIZATION ─────────────────────────────────────────────────────────
// A `MaterialDef` copied by value carries `{lyrMtl, radMtl}` as POINTERS, and one
// of those pointers can be a DEGENERATE layered handle (name at offset 0,
// `layerCount == 0`) minted by `MakeDegenerateLayerMtl` while the renderer was
// down or a prefab was loading (materialdef.cpp:72/103).  A patch whose
// `texture` channel is degenerate reports `MaterialDef_11 == 0`, so
// `Patch_BuildInstanceVisuals` sets `visCount = 0 / visArray = NULL`
// (pmesh.cpp:9903) and `DrawPatches` bails before it emits anything — the patch
// exists, is selectable, and draws NOTHING.  Every copy this file makes is
// therefore pushed through `Materialdef_Realize` (materialdef.cpp:125), which is
// the editor's own name→handle re-dispatch, so an inherited material is a REAL
// material by the time anything tries to draw it.
//
// NOTHING PORTED CHANGES.  `Ed_ProduceSplitLists` (the classic clipper) still
// calls `Ed_BuildClipFaceMaterial_Kiwi` and still produces caulk — that is the
// binary's behaviour and it stays.  This file is only ever reached from the KIWI
// verbs.
// ─────────────────────────────────────────────────────────────────────────────

struct brush_t;
struct face_t;
struct patchMesh_t;
struct MaterialDef;

// ── R1 ───────────────────────────────────────────────────────────────────────
// R1 applied to a face: it has a channel-0 material and that material is not a
// TOOL material (caulk / nodraw / clip / hint / skip / portal / origin / trigger
// / lightgrid / anything under a "tools" path).  A face with no channel-0
// material is not inheritable — an unnamed material is not something to inherit.
//
// This is the file's R1 entry point.  KIWI-UX (CLEANUP, C-69): the name test and
// the channel-0 name accessor it reads through used to be exported beside it and
// had no caller outside kiwi_material.cpp; they are file-local there now (the
// tool-name list is `KMTL_TOOL_NAMES` in that file's anonymous namespace).
bool KiwiMtl_FaceIsInheritable( const face_t *f );

// ── R2 / R5 ──────────────────────────────────────────────────────────────────
// Choose the face of `def` whose material a NEW surface should inherit.
//
//   cutNormal  the new surface's own normal, or NULL when there is none to score
//              against (then the scan degenerates to "largest inheritable face").
//   prefer0/1  optional preferred face indices, tried IN ORDER before the scan
//              (pass -1 for "none").  This is R5's adjacent-face pair.
//
// Returns a face index, or -1 when the brush has NO inheritable face at all
// (R3 — the caller falls back to the classic caulk synthesis).
int KiwiMtl_PickSourceFace( const brush_t *def, const float cutNormal[3],
                            int prefer0, int prefer1 );

// ── the copy ─────────────────────────────────────────────────────────────────
// Copy all four material channels of `def->faces[srcFace]` onto `out` and
// realize them (R6).  Leaves every other field of `out` alone — the caller owns
// the planepts, the contents and the toolflags.  False = bad arguments.
bool KiwiMtl_SeedFaceFrom( face_t *out, const brush_t *def, int srcFace );

// R6 on a face that already carries its materials (a Face_Alloc clone, say).
void KiwiMtl_RealizeFace( face_t *f );

// R6 on a patch's three material channels.
void KiwiMtl_RealizePatch( patchMesh_t *p );

// ── the cut/split entry point (R2 + R3) ──────────────────────────────────────
// Seed the template face a splitter is about to hand to Brush_SplitBrushByFace.
// Inherits under R2 when the brush has anything to inherit, and falls back to
// `Ed_BuildClipFaceMaterial_Kiwi` (the classic caulk/nodraw_decal synthesis)
// when it does not.  Returns true when it INHERITED, false when it fell back —
// callers use that only for the console line.
bool KiwiMtl_SeedClipFace( face_t *out, const brush_t *def, const float cutNormal[3] );

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND Y, ITEM 1 — "KiwiMatInfo": THE PERMANENT MATERIAL-STATE READOUT
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "z-order bug is still here.  All I did was split a cube
// in half.  All extrusions get this on top drawing bug too. (Maybe material
// related??)"
//
// It was material related, for the third round running, and each round guessed at
// which material and at what state it carried.  The rule this project already has
// — material state truth comes ONLY from the material x technique statemap in the
// shipped assets — is not enforceable from a bug report, so this is the readout
// that puts the answer in the console instead.
//
// KiwiMtl_InfoCommand() prints, for the FACE UNDER THE CURSOR and for the current
// material TEMPLATE (g_qeglobals.random_texture_stuff[layer], the thing every new
// brush face is stamped from):
//     material name, techniqueSet name (+ the base name with the
//     g_materialTypeInfo vertex-declaration prefix stripped), sortKey, the
//     technique the CAMERA actually binds, that technique's decoded depthTest /
//     depthWrite, the raw loadBits pair, and whether the camera substitutes a
//     depth-writing material for it (and which one).
//
// Reached from the command palette (F) as "Material info (under cursor)", id
// KIWI_CMD_MATINFO.  Small on purpose: the decode is camwnd.cpp's — the file that
// owns the substitution predicate — and this side only formats.
struct Material;

struct kiwiMatDiag_t
{
    const char  *name;            // Material.info.name
    const char  *techSet;         // MaterialTechniqueSet.name, as loaded ("wc_tools")
    const char  *techSetBase;     // ...prefix-stripped ("tools")
    int          sortKey;         // Material.info.sortKey (r_material.h:438)
    int          cameraTech;      // the MaterialTechniqueType the camera binds
    bool         techPresent;     // stateBitsEntry[cameraTech] != 0xFF
    bool         depthTest;       // decoded from the BOUND loadBits[1]
    bool         depthWrite;
    unsigned int loadBits0;       // GfxStateBits.loadBits[0] at that technique
    unsigned int loadBits1;       // ...[1]
    bool         substituted;     // the camera draws it with the substitute instead
    const char  *substituteName;  // ...this one
};

// camwnd.cpp — reads the same fields the backend binds.  False = no such material.
bool KiwiMtl_Diagnose( Material *handle, kiwiMatDiag_t *out );

// kiwi_material.cpp — the palette command's body (KIWI_CMD_MATINFO).
void KiwiMtl_InfoCommand();

// ═════════════════════════════════════════════════════════════════════════════
//  MATERIAL-LAYER INTEGRITY — "the three channels"
// ═════════════════════════════════════════════════════════════════════════════
// A CoD4 face carries THREE material channels and a patch carries the same
// triple:
//     mtldef[0] / patchMesh_t.texture    base colormap
//     mtldef[1] / patchMesh_t.lightmap   lightmap  ("lightmap_gray")
//     mtldef[2] / patchMesh_t.smoothing  smoothing ("smoothing_hard" on a face,
//                                        "smoothing_smooth" on a patch)
// Retail Radiant fills all three on every face it makes: Brush_Alloc memcpy's
// the current template into channel 0 (brush.cpp:496) and then
// Brush_SetDefaultMaterials -> Face_SetDefaultMaterials (brush.cpp:434) fills
// channels 1 and 2 through Face_InitMaterialChannel (brush.cpp:417).  MakeNewPatch
// (pmesh.cpp:136) does the patch equivalent with three SetMaterial calls.
//
// WHY IT MATTERS BEYOND THE VIEWPORT.  A surface whose LIGHTMAP channel is
// missing, unnamed, or carries a zero texture scale is not merely invisible in
// the editor's Shift+L (lightmap) render method — it compiles with no lightmap at
// all: the map compiler needs a non-zero lightmap scale to build the surface's
// lightmap vectors, and a surface without them receives no baked light and no
// baked sun-shadow mask.  Nothing in the editor says so, which is why this is an
// invariant with a repair rather than a warning.
//
// The repair always runs through the PORTED helpers named above, so a repaired
// channel is byte-for-byte what Brush_Create / MakeNewPatch would have produced.
// The one addition is a scale backstop: Init_MaterialLayer (materialdef.cpp:351)
// writes mat_texDef only for materials that resolved to at least one layer, so a
// channel whose material came back degenerate keeps its zero scale even after the
// ported init; that case is filled with the same width*sampleSize product
// Ed_BuildClipFaceMaterial_Kiwi uses (xywnd.cpp:2293).

// True when all three of a face's channels name a material AND carry a finite,
// non-zero texture scale.  A null face answers true (nothing to be wrong).
bool KiwiMtl_FaceLayersAreValid( face_t *f );

// Repair whichever of a face's three channels is missing, unnamed or zero-scaled.
// Returns true when something was repaired.  Touches no other field of the face.
bool KiwiMtl_EnsureFaceLayers( face_t *f );

// The patch equivalent for the three MATERIAL channels only, falling back to
// MakeNewPatch's own default triple.  This is what a CREATOR wants: it runs
// before the creator's own texCoord pass, which needs the channels to resolve.
bool KiwiMtl_EnsurePatchChannels( patchMesh_t *p );

// The channels AND the LIGHTMAP-layer control texCoords: re-lays them when the
// grid has no lightmap parametrisation left at all (every control point on the
// same S AND the same T, or an INF/NAN coordinate).  This is what a REPAIR of an
// existing patch wants; a creator would only make its own texCoord pass run
// twice.  Returns true when something was repaired.
bool KiwiMtl_EnsurePatchLayers( patchMesh_t *p );

// Dispatch on a brush DEF: its patch when it owns one, else all of its faces.
bool KiwiMtl_EnsureBrushLayers( brush_t *def );

// Sweep every brush the map would serialise (the per-entity def lists, walked as
// MapFile_WriteEntity walks them).  Returns the number of repaired brushes +
// patches; the two out-parameters split that total and may be NULL.
int  KiwiMtl_HealMapLayers( int *outBrushes, int *outPatches );

// The load-time funnel: sweep, and when anything was repaired report it and mark
// the map modified so a re-save persists the repair.
void KiwiMtl_HealMapLayersOnLoad();
