#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Material inheritance for surfaces created by KIWI modeling verbs.
//
// KIWI-created surfaces inherit visible parent materials instead of clipper
// caulk, preventing invisible fillets and tool-material depth-order artifacts.
//
// Inheritance rules:
// R1 — what counts as inheritable.
// A named channel-0 material is inheritable unless its name matches the tool
// family. This mirrors existing name-based filters; `$default` remains visible.
//
// R2 — which face a cut inherits from.
// Maximize `area(f) * (1 - |n_f · n_cut|)` over inheritable faces, favoring a
// dominant surface that the cut passes through rather than arbitrary face 0.
//
// R3 — when there is nothing to inherit.
// An all-tool brush falls back to Ed_BuildClipFaceMaterial_Kiwi, preserving its
// caulk/nodraw_decal intent.
//
// R4 — boolean cuts.
// Boolean carve walls inherit from the target; the consumed tool is scaffolding.
//
// R5 — chamfers and fillet patches.
// Adjacent faces are preferred in order, then R2's scan. Fillet patches copy the
// selected source directly rather than inheriting indirectly through the chamfer.
//
// R6 — realization.
// By-value copies can carry a zero-layer handle from headless or prefab loading.
// Materialdef_Realize (materialdef.cpp:125) must re-dispatch it before pmesh.cpp's
// visual build, or a selectable patch can have no visuals.
// The classic Ed_ProduceSplitLists clipper remains unchanged and still emits caulk.

// ImGui drag payload from the Textures tab: the material NAME (NUL-terminated).
#define KMTL_PAYLOAD "KIWI_MATERIAL"

struct brush_t;
struct face_t;
struct patchMesh_t;
struct MaterialDef;

// True when channel 0 is named and is not in the tool-material family.
bool KiwiMtl_FaceIsInheritable( const face_t *f );

// Choose the face of `def` whose material a NEW surface should inherit.
// `cutNormal` may be NULL for area-only scoring; `prefer0/1` are tried in order
// before the scan (-1 means none). Returns -1 when no face is inheritable.
int KiwiMtl_PickSourceFace( const brush_t *def, const float cutNormal[3],
                            int prefer0, int prefer1 );

// Copy all four material channels of `def->faces[srcFace]` onto `out` and
// realize them. Other face fields remain caller-owned. False = bad arguments.
bool KiwiMtl_SeedFaceFrom( face_t *out, const brush_t *def, int srcFace );

// Realize a face that already carries copied materials.
void KiwiMtl_RealizeFace( face_t *f );

// Realize a patch's three material channels.
void KiwiMtl_RealizePatch( patchMesh_t *p );

// Seed the template face a splitter is about to hand to Brush_SplitBrushByFace.
// Returns true on inheritance; false means the classic caulk/nodraw fallback ran.
bool KiwiMtl_SeedClipFace( face_t *out, const brush_t *def, const float cutNormal[3] );

// Material-state diagnostic.
// KIWI_CMD_MATINFO prints the current template and face-under-cursor material,
// technique set, sort key, bound camera state, and any substitution. camwnd.cpp
// owns the asset-derived state decode.
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

// Material-layer integrity.
// Faces and patches carry base, lightmap, and smoothing channels. Face smoothing
// defaults to smoothing_hard; patch smoothing defaults to smoothing_smooth.
// A missing/unnamed or zero-scale lightmap prevents the compiler from building
// lightmap vectors, losing baked light and sun-shadow data.
// Repairs use the ported creation helpers. If a zero-layer handle makes
// Init_MaterialLayer skip its scale, use Ed_BuildClipFaceMaterial_Kiwi's
// width*sampleSize fallback (materialdef.cpp:351; xywnd.cpp:2293).

// True when all three face channels are named with finite, non-zero scales.
// A null face is vacuously valid.
bool KiwiMtl_FaceLayersAreValid( face_t *f );

// Repair missing, unnamed, or zero-scaled face channels. Returns true on repair.
bool KiwiMtl_EnsureFaceLayers( face_t *f );

// Repair patch material channels with MakeNewPatch's defaults, before texcoords.
bool KiwiMtl_EnsurePatchChannels( patchMesh_t *p );

// Also repair lightmap texcoords when both axes are constant or non-finite.
// Intended for existing patches; creators already run their texcoord pass.
bool KiwiMtl_EnsurePatchLayers( patchMesh_t *p );

// Dispatch on a brush DEF: its patch when it owns one, else all of its faces.
bool KiwiMtl_EnsureBrushLayers( brush_t *def );

// Sweep per-entity def lists. Returns repaired brushes + patches; optional
// out-parameters split the total.
int  KiwiMtl_HealMapLayers( int *outBrushes, int *outPatches );

// Load-time sweep; repaired maps are reported and marked modified for re-save.
void KiwiMtl_HealMapLayersOnLoad();
