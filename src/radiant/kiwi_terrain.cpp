#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Terrain Sculpt — see kiwi_terrain.h.
//
// Sculpts the control points of the selected patches (optionally every visible patch)
// with a circular or square brush: raise/dig, set height, smooth, noise, texture
// layers, vertex colour, grass scatter, trim, plus chunk splitting and terrain
// creation ("Allow terrain creation": the Raise brush lays new chunks in the empty
// lattice cells it covers, next to existing terrain or over nothing at all).
//
// TEXTURE LAYERS live on ONE patch.  patchMesh_t.kiwiLayer[4] names up to four extra
// materials; the weight of layer k is the control point's vert_color byte k (r,g,b,a).
// The .map carries them as "kiwilayer <slot> <material>" lines and cod4map expands
// them into the stock duplicate-patch layered surface at parse time, so the game gets
// exactly what a hand-duplicated CoD4 terrain produces while the editor keeps one
// patch: one pick, one set of triangles, no stacks.  The camera previews each used
// slot as one extra alpha-blended run over the base (KiwiTerrain_LayerUpload).

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                 // camera_s: the re-tint wave starts at the camera

#include <imgui/imgui.h>

#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>   // TECHNIQUE_UNLIT, R_AddCmdSetMaterialColor - the weight overlay
#include <gfx_d3d/r_state.h>        // GFXS1_POLYGON_OFFSET_MASK - the blend twins' decal offset is cleared
#include "kiwi_refimage.h"          // KiwiRefImage_Count - the overlay only matters over pictures

#include "kiwi_command.h"
#include "kiwi_droptrace.h"
#include "kiwi_fmt.h"
#include "kiwi_grass.h"
#include "kiwi_lines.h"
#include "kiwi_material.h"
#include "kiwi_matwriter.h"
#include "kiwi_numeric.h"    // KiwiNum_EvalDisplay - height fields take the viewport's unit grammar
#include "kiwi_pick.h"
#include "kiwi_terrain.h"
#include "kiwi_units.h"      // KiwiUnits_Format / Units_FromDisplay
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern int          Sys_Printf( const char *fmt, ... );
extern int          g_nUpdateBits;
extern camera_s    *Ed_Camera();                                       // camwnd.cpp:165 (re-tint order)
extern selbrush_t   selected_brushes;                       // map.cpp 0x23F1864
extern selbrush_t   active_brushes;                         // map.cpp 0x23F189C
extern entity_s    *world_entity;
extern char         FilterBrush( selbrush_t *b, int updateFilters );   // filters.cpp 0x46A1F0
extern char         PMESH_51( const float *org, const float *dir, patch_t *pm,
                              float *outDist, int *outCol, int *outRow,
                              byte *outColor, float *outPlane );       // pmesh.cpp 0x43DDE0
extern void         Patch_Rebuild( patchMesh_t *p, char doBounds );    // pmesh.cpp 0x438D90
extern void         Patch_Paint( selbrush_t *list );                   // pmesh.cpp 0x43EB70 (clear xx22b)
extern void         Patch_PaintMarkUndo( patchMesh_t *def );           // pmesh.cpp (sub_45E770 wrapper)
extern void         Patch_PaintFinish( selbrush_t *list );             // pmesh.cpp (PMESH_18 wrapper)
extern patchMesh_t *MakeNewPatch();                                    // pmesh.cpp:137
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent ); // pmesh.cpp:841
extern void         Patch_KiwiTextureAndBuild( patchMesh_t *p, float texScale ); // pmesh.cpp (tail)
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );  // brush.cpp:670
extern void         SetMaterial( const char *name, patchMesh_material *out ); // materialdef.cpp:101
extern qtexture_s  *MaterialDef_GetLayeredMaterial( MaterialDef *mtlDef ); // materialdef.cpp
extern void         Select_Deselect( int deselectFaces );              // select.cpp
extern void         Select_Brush( selbrush_t *brush, char some_overwrite, char bStatus, char center ); // select.cpp:904
extern void         Select_Delete();                                   // select.cpp:1524
extern void         Undo_ClearRedo();
extern void         Undo_GeneralStart( const char *operation );
extern void         Undo_AddBrushList( selbrush_t *list );             // undo.cpp:551
extern void         Undo_EndBrushList( selbrush_t *list );             // undo.cpp:576
extern void         Undo_AddEntity_W( entity_s *ent );                 // undo.cpp:633
extern void         Undo_End();
extern void         Undo_KiwiMarkCreated( brush_t *def );              // undo.cpp (KIWI tail)
extern void         Brush_Free( selbrush_t *b );                       // brush.cpp:1002 (0x475BA0)
extern void         Sel_InvalidateFromLegacy();                        // kiwi_selection.cpp:486
extern void         Undo_AddBrush( entity_brush_s *pBrushInst );       // undo.cpp:494 (takes the brush DEF)
extern entity_s    *Brush_Move( const float *move, brush_t *def, char snap ); // brush.cpp 0x4782A0: faces, patch ctrl, entity origin
// Flatten-to-brushes and brush-face painting.
extern brush_t     *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );          // brush.cpp
extern void         Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:510
extern void         Brush_BuildWindings( brush_t *def, int bFull );                  // brush.cpp:1434
extern void         Ed_EnsureCurrentMaterial_Kiwi();                                  // texwnd.cpp
extern selbrush_t  *KiwiExtrude_LandDef( brush_t *def );                              // kiwi_extrude.cpp (world + selected)
extern void         sub_47B940( brush_t *def );                                       // brush.cpp:5841 Brush_UpdateSpecialMaterialFlag
extern void         MarkMapModified();                                                // win_qe3.cpp
extern void         KiwiMtl_RealizeFace( face_t *f );                                 // kiwi_material.h
// The weight overlay re-emits a patch's flat-colour run after the reference images.
extern int          Editor_MaterialSortKey( Material *handle );        // r_ed_scene.cpp 0x4FDBB0
extern void         Editor_AddMeshCmd( Material *handle, int techType, int sortKey,
                        int vertCount, int vbIndexAndOffs, int indexCount, const uint16_t *indexTable ); // r_ed_scene.cpp 0x4FDA50
extern void        *R_AddEditorSurfsCmd();                             // r_ed_scene.cpp 0x4FDA10
extern void         R_SortMaterials();                                 // r_ed_scene.cpp
// Legacy soft-select vertex drag (Advanced Patch Editor mode 1).
extern int          AdvPatchEdit_GetMode();                            // patchdialog.cpp
extern void         AdvPatchEdit_SetMode( int mode );
extern void         AdvPatchEdit_ApplySlotValue( int slot, float typed );

namespace
{
    const char *KTER_PROFILE = "KiwiTerrain";
    const float KTER_PI      = 3.14159265358979323846f;
    enum { KTER_SLOTS = 4 };

    enum kterTool_t
    {
        KTER_RAISE = 0,     // raise (Ctrl: dig)
        KTER_SETHEIGHT,     // snap toward a global target Z (Ctrl+click: pick height)
        KTER_SMOOTH,        // neighbour average
        KTER_NOISE,         // additive value noise
        KTER_TEXTURE,       // texture layers: paint a slot's weight
        KTER_BLEND,         // blend: smooth every layer's weights with its neighbours (seams too)
        KTER_GRASS,         // Grass Scatter (kiwi_grass.cpp) as a mode of this panel
        KTER_TRIM,          // remove terrain chunks under the brush
        KTER_TOOL_COUNT
    };

    enum kterShape_t   { KTER_CIRCLE = 0, KTER_SQUARE };
    enum kterFalloff_t { KTER_FO_SMOOTH = 0, KTER_FO_LINEAR, KTER_FO_SHARP, KTER_FO_CONSTANT };

    const char *KTER_TOOL_NAME[KTER_TOOL_COUNT] =
        { "Raise / Dig", "Set height", "Smooth", "Noise", "Texture paint", "Blend", "Grass", "Trim" };
    // KIWI (2026-09-10, user): "lowering" is called "digging" everywhere the tool speaks.
    const char *KTER_TOOL_HINT[KTER_TOOL_COUNT] =
    {
        "LMB raise   Ctrl+LMB dig   Shift+LMB smooth   V pick base height",
        "LMB sets everything inside the outer ring to the target Z, instantly   V / Ctrl+LMB pick the height under the pointer   Alt+wheel target   Shift+LMB smooth",
        "LMB smooth",
        "LMB add noise   Ctrl+LMB subtract   Shift+LMB smooth",
        "LMB paint the brush material onto any terrain touched   Ctrl+LMB paint it out   Shift+LMB smooth   I eyedropper",
        "LMB blends every layer's weights with their neighbours, across patch seams too",
        "LMB scatter models along the stroke   Esc disarms",
        "LMB removes every terrain chunk whose centre is under the brush",
    };

    // ── settings (persisted) ────────────────────────────────────────────────
    int   s_tool        = KTER_RAISE;
    int   s_shape       = KTER_CIRCLE;
    int   s_falloff     = KTER_FO_SMOOTH;
    float s_outer       = 256.0f;
    float s_inner       = 64.0f;
    float s_strength    = 1.0f;      // 0..2
    float s_squareRot   = 0.0f;      // degrees, square brush only
    float s_amount      = 128.0f;    // raise speed: units per second at full weight, 128 radius
    float s_targetZ     = 0.0f;      // set-height target: an absolute world Z
    float s_noiseScale  = 32.0f;
    float s_noiseFreq   = 0.004f;
    float s_blendWeight = 1.0f;      // paint ceiling for a layer weight (0..1 -> 255)
    bool  s_previewBlend = true;     // draw the layer runs in the camera
    int   s_blendRings  = 2;         // Blend: neighbourhood radius in grid points (1..4)
    float s_flatTol     = 1.0f;      // Flatten: a patch whose heights span <= this becomes one brush
    float s_flatThick   = 16.0f;     // Flatten: brush thickness below the surface
    bool  s_affectUnselected = false;
    // KIWI (2026-09-16, user): height strokes also lift / lower whatever rests on the
    // terrain (brushes, models, entities, non-terrain patches) by the terrain's own
    // height change under each object, so placed props survive a resculpt. Off by
    // default; see CaptureRiders / CarryRiders.
    bool  s_carryObjects = false;
    bool  s_setHeightFeather = false;   // Set height: ramp the edge by falloff x strength (off = exact, hard edge)
    bool  s_setHeightContain = true;    // ALL height tools: never change anything outside the ring (SetHeightFits)
    // OFF by default (2026-09-18, user: "it keeps making the patch more and more dense with
    // tri's ... I dont want to waste tri's like this unless I specify it with the tesselate
    // button"): density is the user's call.  It was ON for half a day and silently added
    // ~12k triangles in a few strokes of a 177-unit ring.
    bool  s_autoRefine  = false;        // height tools: re-grid coarse terrain under the ring first (RefineUnderRing)
    float s_refineMin   = 16.0f;        // ... never finer than this cell size
    bool  s_layerDepthEqual = true;     // paint layers draw depth-EQUAL to their own base (see StripDecalOffset)
    float s_chunkSize   = 2048.0f;   // max patch side; the expander/split chunk size
    float s_tessCell    = 64.0f;     // Tessellate: ABSOLUTE cell size in world units (>15 cells an axis splits)
    bool  s_expand      = false;     // Raise: "Allow terrain creation" - lay chunks in empty lattice cells
    float s_createZ     = 0.0f;      // creation: base height where nothing at all is under the cursor
    int   s_createCells = 8;         // creation: cells per side of a chunk laid with no terrain in reach
    bool  s_createOnSurfaces = true; // creation: the cursor lands on brushes/models before the base plane
    bool  s_softSelect  = false;
    bool  s_hideWire    = false;     // armed: hide the patch wireframe entirely (Tab toggles)
    float s_wireReach   = 1.25f;     // armed: wireframe shown within outer radius x this
    bool  s_heatmap     = true;      // armed height tools: patches wear a height gradient
    bool  s_heatAlways  = false;     // ...and with this, also while NOT armed (off by default)

    // ── session ──────────────────────────────────────────────────────────────
    bool  s_loaded = false;
    bool  s_show   = false;
    bool  s_armed  = false;

    bool  s_cursorHave = false;
    float s_cursor[3]  = { 0.0f, 0.0f, 0.0f };
    selbrush_t *s_cursorNode = nullptr;      // the patch under the cursor (ring drop target)
    // Its def at pick time, as a KEY ONLY (never dereferenced).  The node can be freed
    // behind the tool's back - an undo, Delete, another tool - so nothing may read
    // s_cursorNode without LiveCursorNode() first (2026-09-18 crash: ForgetDef read
    // s_cursorNode->patch of a node an undo had freed; patch was 0xFFFFFFFFFFFFFFFF).
    const patchMesh_t *s_cursorDef = nullptr;
    // What the cursor landed on.  Only "Allow terrain creation" lets it leave the patches.
    enum kterCursor_t { KCUR_NONE = 0, KCUR_PATCH, KCUR_SURFACE, KCUR_PLANE };
    int   s_cursorKind = KCUR_NONE;
    patchMesh_t *s_scratchLike = nullptr;    // template for chunks laid with no terrain in reach
    int   s_created    = 0;                  // chunks laid by the current stroke
    // Height gradient ("heatmap") range over the eligible patches while armed.
    bool  s_heatValid  = false;
    float s_heatMinZ   = 0.0f;
    float s_heatMaxZ   = 1.0f;

    bool  s_stroke      = false;
    bool  s_undoOpen    = false;
    bool  s_modShift    = false;
    bool  s_modCtrl     = false;
    int   s_stamps      = 0;
    int   s_touched     = 0;
    float s_noiseSeed   = 0.0f;
    float s_accumDt     = 0.0f;
    float s_lastCenter[3] = { 0.0f, 0.0f, 0.0f };
    bool  s_haveLastCenter = false;
    char  s_status[160] = "Disarmed.";

    // Texture paint carries its MATERIAL on the brush: whichever terrain the brush
    // touches gets that material as a layer slot (added on first touch, 4 max) and
    // its weight painted - no selection needed.  "Erase to base" paints every layer
    // out instead.
    char  s_paintMaterial[64] = "";
    bool  s_paintBase   = false;
    bool  s_paintBrushes = false;            // Texture paint: brush faces under the brush take the material too
    bool  s_weightView  = true;              // armed Texture paint: layers draw as flat colours by weight
    int   s_layersAdded = 0;                 // per stroke: slots created on touched patches
    int   s_layersFull  = 0;                 // per stroke: patches skipped (4 slots used)
    int   s_facesPainted = 0;                // per stroke: brush faces that took the material
    int   s_carried      = 0;                // per stroke: objects "Carry objects" moved
    std::vector<selbrush_t *> s_targets;     // the patches one stroke touches
    std::vector<patchMesh_t *> s_dirtyDefs;  // changed this frame; rebuilt once
    bool  s_dirtyBounds = false;

    std::map<std::string, Material *>  s_blendTwins;   // material -> preview twin (null = failed)
    std::map<std::string, std::string> s_twinErr;      // material -> why
    void RestripTwins();                               // below: re-apply the twins' depth state

    // Auto-transition bands, per patch and slot.
    struct kterBand_t
    {
        bool  enabled;
        float minZ, maxZ, fadeZ;
        float minSlope, maxSlope, fadeSlope;
    };
    struct kterBands_t { kterBand_t slot[KTER_SLOTS]; };
    std::map<patchMesh_t *, kterBands_t> s_bands;
    bool  s_bandTreeOpen = false;
    bool  s_autoUndoOpen = false;
    float s_bandFade     = 64.0f;

    // Ring overlay cache.
    enum { KTER_RING_MAX = 64 };
    float s_ringOuter[KTER_RING_MAX][3];
    float s_ringInner[KTER_RING_MAX][3];
    int   s_ringCount = 0;
    float s_expandCells[64][5];              // minx, miny, sizeX, sizeY, z
    int   s_expandCellCount = 0;

    // ── small helpers ────────────────────────────────────────────────────────
    float ClampF( float v, float lo, float hi )
    {
        if ( !_finite( v ) ) return lo;
        return v < lo ? lo : ( v > hi ? hi : v );
    }

    void SetStatus( const char *fmt, ... )
    {
        va_list args;
        va_start( args, fmt );
        _vsnprintf( s_status, sizeof( s_status ), fmt, args );
        va_end( args );
        s_status[sizeof( s_status ) - 1] = '\0';
    }

    float ReadFloat( const char *entry, float def )
    {
        const std::string text = Radiant_ProfileGetString( KTER_PROFILE, entry, "" );
        if ( text.empty() )
            return def;
        char *end = 0;
        const double v = strtod( text.c_str(), &end );
        if ( end == text.c_str() || !_finite( v ) )
            return def;
        return (float)v;
    }

    void WriteFloat( const char *entry, float v )
    {
        char text[64];
        KiwiFmt_Num( text, sizeof( text ), v, 6 );
        Radiant_ProfileSetString( KTER_PROFILE, entry, text );
    }

    void Sanitize()
    {
        if ( s_tool < 0 || s_tool >= KTER_TOOL_COUNT ) s_tool = KTER_RAISE;
        if ( s_shape != KTER_SQUARE ) s_shape = KTER_CIRCLE;
        if ( s_falloff < 0 || s_falloff > KTER_FO_CONSTANT ) s_falloff = KTER_FO_SMOOTH;
        s_outer     = ClampF( s_outer, 4.0f, 12288.0f );
        s_inner     = ClampF( s_inner, 0.0f, s_outer );
        s_strength  = ClampF( s_strength, 0.01f, 2.0f );
        s_squareRot = ClampF( s_squareRot, -180.0f, 180.0f );
        s_amount    = ClampF( s_amount, 1.0f, 4096.0f );
        s_targetZ   = ClampF( s_targetZ, -65536.0f, 65536.0f );
        s_refineMin = ClampF( s_refineMin, 4.0f, 256.0f );
        s_noiseScale = ClampF( s_noiseScale, 0.25f, 2048.0f );
        s_noiseFreq  = ClampF( s_noiseFreq, 0.0001f, 1.0f );
        s_blendWeight = ClampF( s_blendWeight, 0.0f, 1.0f );
        if ( s_blendRings < 1 ) s_blendRings = 1;
        if ( s_blendRings > 4 ) s_blendRings = 4;
        s_chunkSize = ClampF( s_chunkSize, 256.0f, 8192.0f );
        s_tessCell  = ClampF( s_tessCell, 4.0f, 4096.0f );
        s_createZ = ClampF( s_createZ, -65536.0f, 65536.0f );
        if ( s_createCells < 1 )  s_createCells = 1;
        if ( s_createCells > 15 ) s_createCells = 15;
        s_wireReach = ClampF( s_wireReach, 1.0f, 4.0f );
    }

    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded = true;
        s_tool      = Radiant_ProfileGetInt( KTER_PROFILE, "Tool", KTER_RAISE );
        // A profile saved on the retired Smooth tool lands on Raise (Shift+LMB smooths).
        // Only here: the test DSL may still select the slot explicitly.
        if ( s_tool == KTER_SMOOTH ) s_tool = KTER_RAISE;
        s_shape     = Radiant_ProfileGetInt( KTER_PROFILE, "Shape", KTER_CIRCLE );
        s_falloff   = Radiant_ProfileGetInt( KTER_PROFILE, "Falloff", KTER_FO_SMOOTH );
        s_outer     = ReadFloat( "Outer", 256.0f );
        s_inner     = ReadFloat( "Inner", 64.0f );
        s_strength  = ReadFloat( "Strength", 1.0f );
        s_squareRot = ReadFloat( "SquareRot", 0.0f );
        s_amount    = ReadFloat( "RaiseSpeed", 64.0f );
        s_targetZ   = ReadFloat( "TargetZ", 0.0f );
        s_noiseScale = ReadFloat( "NoiseScale", 32.0f );
        s_noiseFreq  = ReadFloat( "NoiseFreq", 0.004f );
        s_blendWeight = ReadFloat( "BlendWeight", 1.0f );
        s_previewBlend = Radiant_ProfileGetInt( KTER_PROFILE, "PreviewBlend", 1 ) != 0;
        s_blendRings = Radiant_ProfileGetInt( KTER_PROFILE, "BlendRings", 2 );
        s_flatTol    = ReadFloat( "FlatTol", 1.0f );
        s_flatThick  = ReadFloat( "FlatThick", 16.0f );
        s_paintBrushes = Radiant_ProfileGetInt( KTER_PROFILE, "PaintBrushes", 0 ) != 0;
        s_affectUnselected = Radiant_ProfileGetInt( KTER_PROFILE, "AffectUnselected", 0 ) != 0;
        s_carryObjects     = Radiant_ProfileGetInt( KTER_PROFILE, "CarryObjects", 0 ) != 0;
        s_setHeightFeather = Radiant_ProfileGetInt( KTER_PROFILE, "SetHeightFeather", 0 ) != 0;
        // "ContainRing": third key in two days (Set-height-only ON, then OFF); now every
        // height tool, ON, workable because RefineUnderRing keeps the grid fine under the ring.
        s_setHeightContain = Radiant_ProfileGetInt( KTER_PROFILE, "ContainRing", 1 ) != 0;
        // "RefineUnderRing": a new key, so the ON the old default saved is not carried over.
        s_autoRefine = Radiant_ProfileGetInt( KTER_PROFILE, "RefineUnderRing", 0 ) != 0;
        s_refineMin  = ReadFloat( "SetHeightRefineMin", 16.0f );
        {
            // a twin can be loaded (and given the default test) by a draw before the first Load()
            const bool was = s_layerDepthEqual;
            s_layerDepthEqual = Radiant_ProfileGetInt( KTER_PROFILE, "LayerDepthEqual", 1 ) != 0;
            if ( was != s_layerDepthEqual )
                RestripTwins();
        }
        s_chunkSize = ReadFloat( "ChunkSize2", 2048.0f );
        s_tessCell  = ReadFloat( "TessCellSize", 64.0f );
        s_expand    = Radiant_ProfileGetInt( KTER_PROFILE, "Expand", 0 ) != 0;
        s_createZ   = ReadFloat( "CreateZ", 0.0f );
        s_createCells = Radiant_ProfileGetInt( KTER_PROFILE, "CreateCells", 8 );
        s_createOnSurfaces = Radiant_ProfileGetInt( KTER_PROFILE, "CreateOnSurfaces", 1 ) != 0;
        s_wireReach = ReadFloat( "WireReach", 1.25f );
        s_heatmap   = Radiant_ProfileGetInt( KTER_PROFILE, "Heatmap", 1 ) != 0;
        s_heatAlways = Radiant_ProfileGetInt( KTER_PROFILE, "HeatmapAlways", 0 ) != 0;
        {
            const std::string pm = Radiant_ProfileGetString( KTER_PROFILE, "PaintMaterial", "" );
            strncpy( s_paintMaterial, pm.c_str(), sizeof( s_paintMaterial ) - 1 );
            s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
        }
        s_paintBase = Radiant_ProfileGetInt( KTER_PROFILE, "PaintBase", 0 ) != 0;
        s_weightView = Radiant_ProfileGetInt( KTER_PROFILE, "WeightView", 1 ) != 0;
        Sanitize();
    }

    void Save()
    {
        Sanitize();
        Radiant_ProfileSetInt( KTER_PROFILE, "Tool", s_tool );
        Radiant_ProfileSetInt( KTER_PROFILE, "Shape", s_shape );
        Radiant_ProfileSetInt( KTER_PROFILE, "Falloff", s_falloff );
        WriteFloat( "Outer", s_outer );
        WriteFloat( "Inner", s_inner );
        WriteFloat( "Strength", s_strength );
        WriteFloat( "SquareRot", s_squareRot );
        WriteFloat( "RaiseSpeed", s_amount );
        WriteFloat( "TargetZ", s_targetZ );
        WriteFloat( "NoiseScale", s_noiseScale );
        WriteFloat( "NoiseFreq", s_noiseFreq );
        WriteFloat( "BlendWeight", s_blendWeight );
        Radiant_ProfileSetInt( KTER_PROFILE, "PreviewBlend", s_previewBlend ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "BlendRings", s_blendRings );
        WriteFloat( "FlatTol", s_flatTol );
        WriteFloat( "FlatThick", s_flatThick );
        Radiant_ProfileSetInt( KTER_PROFILE, "PaintBrushes", s_paintBrushes ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "AffectUnselected", s_affectUnselected ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "CarryObjects", s_carryObjects ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "SetHeightFeather", s_setHeightFeather ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "ContainRing", s_setHeightContain ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "RefineUnderRing", s_autoRefine ? 1 : 0 );
        WriteFloat( "SetHeightRefineMin", s_refineMin );
        Radiant_ProfileSetInt( KTER_PROFILE, "LayerDepthEqual", s_layerDepthEqual ? 1 : 0 );
        WriteFloat( "ChunkSize2", s_chunkSize );
        WriteFloat( "TessCellSize", s_tessCell );
        Radiant_ProfileSetInt( KTER_PROFILE, "Expand", s_expand ? 1 : 0 );
        WriteFloat( "CreateZ", s_createZ );
        Radiant_ProfileSetInt( KTER_PROFILE, "CreateCells", s_createCells );
        Radiant_ProfileSetInt( KTER_PROFILE, "CreateOnSurfaces", s_createOnSurfaces ? 1 : 0 );
        WriteFloat( "WireReach", s_wireReach );
        Radiant_ProfileSetInt( KTER_PROFILE, "Heatmap", s_heatmap ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "HeatmapAlways", s_heatAlways ? 1 : 0 );
        Radiant_ProfileSetString( KTER_PROFILE, "PaintMaterial", s_paintMaterial );
        Radiant_ProfileSetInt( KTER_PROFILE, "PaintBase", s_paintBase ? 1 : 0 );
        Radiant_ProfileSetInt( KTER_PROFILE, "WeightView", s_weightView ? 1 : 0 );
    }

    // ── patches / picking ────────────────────────────────────────────────────
    bool NodeIsPatch( selbrush_t *b )
    {
        return b && b->patch && b->def && b->patch->def;
    }

    // Any visible, unfiltered patch of either kind: the "convert bezier to terrain
    // mesh" button is the only user of this.
    bool PatchEligibleAnyType( selbrush_t *b )
    {
        if ( !NodeIsPatch( b ) )
            return false;
        if ( FilterBrush( b, 0 ) || ( b->brushFlags & 0x20 ) != 0 )
            return false;
        return true;
    }

    // KIWI FIX (2026-09-05): a patch the sculpt / seam / paint passes may touch is a
    // TERRAIN MESH (CoD4 "mesh": the control grid IS the surface) and nothing else.
    // This used to be the "Terrain patches only" checkbox, default OFF, so every stroke
    // and every seam weld also ran over bezier CURVES within reach - a cylinder standing
    // on the ground had its rings flattened to the ground height and its border ring
    // welded to the terrain's, which is what "fubar'd" curves in kisak_trash.map (they
    // then saved and compiled exactly that way - the loader was never at fault).  A
    // bezier's control points are not its surface; the tool has no business with them.
    bool PatchEligible( selbrush_t *b )
    {
        if ( !PatchEligibleAnyType( b ) )
            return false;
        if ( ( b->patch->def->type & PATCH_TERRAIN ) == 0 )
            return false;
        return true;
    }

    // Slab test: does the ray hit the node's AABB before `maxT`?  PMESH_51 walks the
    // WHOLE tessellation (a 16x16 bezier at subdivision 8 is ~29k triangles), so every
    // pick must reject on bounds first.
    bool RayHitsBounds( const float *org, const float *dir, const float *mins, const float *maxs, float maxT )
    {
        float t0 = 0.0f, t1 = maxT;
        for ( int a = 0; a < 3; ++a )
        {
            if ( fabsf( dir[a] ) < 1e-8f )
            {
                if ( org[a] < mins[a] - 1.0f || org[a] > maxs[a] + 1.0f )
                    return false;
                continue;
            }
            const float inv = 1.0f / dir[a];
            float ta = ( mins[a] - 1.0f - org[a] ) * inv;
            float tb = ( maxs[a] + 1.0f - org[a] ) * inv;
            if ( ta > tb ) { const float t = ta; ta = tb; tb = t; }
            if ( ta > t0 ) t0 = ta;
            if ( tb < t1 ) t1 = tb;
            if ( t0 > t1 )
                return false;
        }
        return true;
    }

    // Nearest patch hit along the ray over the selected list (+ active when asked).
    bool PickPatches( const float *org, const float *dir, bool alsoActive,
                      float outPoint[3], byte outColor[4], selbrush_t **outNode = nullptr,
                      selbrush_t *skip = nullptr )
    {
        float best = FLT_MAX;
        byte  cell[4] = { 0, 0, 0, 0 };
        selbrush_t *bestNode = nullptr;
        for ( int pass = 0; pass < 2; ++pass )
        {
            if ( pass == 1 && !alsoActive )
                break;
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( b == skip || !PatchEligible( b ) )
                    continue;
                if ( !RayHitsBounds( org, dir, b->def->mins, b->def->maxs, best ) )
                    continue;
                float dist;
                byte  c[4];
                if ( PMESH_51( org, dir, b->patch, &dist, nullptr, nullptr, c, nullptr ) && dist < best )
                {
                    best = dist;
                    bestNode = b;
                    memcpy( cell, c, 4 );
                }
            }
        }
        if ( best == FLT_MAX )
            return false;
        for ( int i = 0; i < 3; ++i )
            outPoint[i] = org[i] + dir[i] * best;
        if ( outColor )
            memcpy( outColor, cell, 4 );
        if ( outNode )
            *outNode = bestNode;
        return true;
    }

    bool CreationAllowed()
    {
        return s_tool == KTER_RAISE && s_expand;
    }

    // Cursor resolution.  Patches first (every mode).  With terrain creation allowed
    // the cursor then lands on any world surface (brushes, models - KiwiDrop_Trace) and
    // finally on the horizontal base plane, so the brush works over an empty zone.
    bool ResolveCursor( const ray_t &ray, float outPoint[3], byte outColor[4] )
    {
        if ( PickPatches( ray.origin, ray.dir, true, outPoint, outColor, &s_cursorNode ) )
        {
            s_cursorKind = KCUR_PATCH;
            s_cursorDef  = ( s_cursorNode && s_cursorNode->patch ) ? s_cursorNode->patch->def : nullptr;
            return true;
        }
        s_cursorNode = nullptr;
        s_cursorDef  = nullptr;
        if ( outColor )
            memset( outColor, 255, 4 );
        // Off the patches: terrain creation lands on surfaces / the base plane, and
        // the texture painter lands on brush faces when it paints brushes too.
        const bool brushPaint = s_tool == KTER_TEXTURE && s_paintBrushes && !s_paintBase && s_paintMaterial[0];
        if ( !CreationAllowed() && !brushPaint )
        {
            s_cursorKind = KCUR_NONE;
            return false;
        }
        if ( brushPaint )
        {
            kiwiDropHit_t hit;
            if ( KiwiDrop_Trace( ray, false, &hit ) )
            {
                memcpy( outPoint, hit.point, sizeof( hit.point ) );
                s_cursorKind = KCUR_SURFACE;
                return true;
            }
            s_cursorKind = KCUR_NONE;
            return false;
        }
        if ( s_createOnSurfaces )
        {
            kiwiDropHit_t hit;
            if ( KiwiDrop_Trace( ray, false, &hit ) )
            {
                memcpy( outPoint, hit.point, sizeof( hit.point ) );
                s_cursorKind = KCUR_SURFACE;
                return true;
            }
        }
        if ( fabsf( ray.dir[2] ) > 1e-6f )
        {
            const float t = ( s_createZ - ray.origin[2] ) / ray.dir[2];
            if ( t > 0.0f && t < 131072.0f )
            {
                for ( int i = 0; i < 3; ++i )
                    outPoint[i] = ray.origin[i] + ray.dir[i] * t;
                outPoint[2] = s_createZ;
                s_cursorKind = KCUR_PLANE;
                return true;
            }
        }
        s_cursorKind = KCUR_NONE;
        return false;
    }

    bool PickCursor( int imgX, int imgY, float outPoint[3], byte outColor[4] )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;
        return ResolveCursor( ray, outPoint, outColor );
    }

    void SampleGrid( const patchMesh_t *src, float x, float y, float *outZ, byte outColor[4] );

    // Is the control grid a regular axis-aligned sheet (x along i, y along j)?  Then the
    // ring can read heights off it directly instead of ray-casting the tessellation.
    bool GridIsSheet( const patchMesh_t *def )
    {
        if ( def->width < 2 || def->height < 2 )
            return false;
        const float *p00 = def->ctrl[0][0].xyz, *p10 = def->ctrl[def->width - 1][0].xyz;
        const float *p01 = def->ctrl[0][def->height - 1].xyz, *p11 = def->ctrl[def->width - 1][def->height - 1].xyz;
        const float ex = p10[0] - p00[0], ey = p01[1] - p00[1];
        if ( fabsf( ex ) < 1.0f || fabsf( ey ) < 1.0f )
            return false;
        if ( !( fabsf( p10[1] - p00[1] ) < 1.0f && fabsf( p01[0] - p00[0] ) < 1.0f
             && fabsf( p11[0] - p10[0] ) < 1.0f && fabsf( p11[1] - p01[1] ) < 1.0f ) )
            return false;
        // KIWI (2026-09-18, user screenshots of "redundant geo"): the four corners are not
        // enough.  A patch with square corners but CURVED edges / an irregular interior
        // passed as a sheet, and Tessellate / Split / the chunk lattice then rebuilt it from
        // its bounding box: rectangular chunks poking out past the curve and lying on the
        // neighbours.  A sheet has EVERY point on the regular lattice its corners span.
        const float sx = ex / (float)( def->width - 1 ), sy = ey / (float)( def->height - 1 );
        for ( int i = 0; i < def->width; ++i )
            for ( int j = 0; j < def->height; ++j )
            {
                const float *p = def->ctrl[i][j].xyz;
                if ( fabsf( p[0] - ( p00[0] + sx * (float)i ) ) > 1.0f
                  || fabsf( p[1] - ( p00[1] + sy * (float)j ) ) > 1.0f )
                    return false;
            }
        return true;
    }

    // Height of the cursor patch at (x,y): bilinear off the control grid for a sheet
    // (O(1)), the cursor height otherwise.  No ray casts: the ring is 64 of these per move.
    // The cursor node only if it is still linked in a display list AND still the patch
    // that was picked (a freed node's address can be reused); otherwise it is dropped.
    // One list walk - call it once per operation, not per ring point.
    selbrush_t *LiveCursorNode()
    {
        if ( !s_cursorNode )
            return nullptr;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( b == s_cursorNode )
                {
                    if ( b->patch && b->patch->def == s_cursorDef )
                        return b;
                    pass = 2;
                    break;
                }
        }
        s_cursorNode = nullptr;
        s_cursorDef  = nullptr;
        return nullptr;
    }

    // RebuildRing validates the node once (LiveCursorNode) before its 64 calls here.
    bool DropToSurface( float x, float y, float zGuess, float *outZ )
    {
        if ( !s_cursorNode || !s_cursorNode->patch )
            return false;
        const patchMesh_t *def = s_cursorNode->patch->def;
        if ( !GridIsSheet( def ) )
        {
            *outZ = zGuess;
            return true;
        }
        byte c[4];
        SampleGrid( def, x, y, outZ, c );
        return true;
    }

    // ── brush weight ─────────────────────────────────────────────────────────
    float BrushDistance( const float *c, const float *p )
    {
        float dx = p[0] - c[0];
        float dy = p[1] - c[1];
        if ( s_shape == KTER_SQUARE )
        {
            if ( s_squareRot != 0.0f )
            {
                const float a = -s_squareRot * KTER_PI / 180.0f;
                const float ca = cosf( a ), sa = sinf( a );
                const float rx = dx * ca - dy * sa;
                const float ry = dx * sa + dy * ca;
                dx = rx; dy = ry;
            }
            const float ax = fabsf( dx ), ay = fabsf( dy );
            return ax > ay ? ax : ay;
        }
        return sqrtf( dx * dx + dy * dy );
    }

    float Falloff( float d )
    {
        if ( d <= s_inner )
            return 1.0f;
        if ( d >= s_outer )
            return 0.0f;
        const float span = s_outer - s_inner;
        const float t = span > 0.0f ? ( d - s_inner ) / span : 1.0f;
        switch ( s_falloff )
        {
        case KTER_FO_LINEAR:   return 1.0f - t;
        case KTER_FO_SHARP:    return ( 1.0f - t ) * ( 1.0f - t );
        case KTER_FO_CONSTANT: return 1.0f;
        default:               return 1.0f - t * t * ( 3.0f - 2.0f * t );
        }
    }

    float Hash2( int x, int y, int seed )
    {
        unsigned n = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 1274126177u;
        n = ( n ^ ( n >> 13 ) ) * 1274126177u;
        n ^= n >> 16;
        return (float)( n & 0xFFFFu ) / 65535.0f * 2.0f - 1.0f;
    }

    float ValueNoise2( float x, float y, int seed )
    {
        const float fx = floorf( x ), fy = floorf( y );
        const int ix = (int)fx, iy = (int)fy;
        float tx = x - fx, ty = y - fy;
        tx = tx * tx * ( 3.0f - 2.0f * tx );
        ty = ty * ty * ( 3.0f - 2.0f * ty );
        const float a = Hash2( ix, iy, seed ),     b = Hash2( ix + 1, iy, seed );
        const float c = Hash2( ix, iy + 1, seed ), d = Hash2( ix + 1, iy + 1, seed );
        const float top = a + ( b - a ) * tx;
        const float bot = c + ( d - c ) * tx;
        return top + ( bot - top ) * ty;
    }

    // ── texture layers on one patch ──────────────────────────────────────────
    bool SlotUsed( const patchMesh_t *def, int slot )
    {
        return slot >= 0 && slot < KTER_SLOTS && def->kiwiLayer[slot][0] != 0;
    }

    int UsedSlotCount( const patchMesh_t *def )
    {
        int n = 0;
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( SlotUsed( def, k ) ) ++n;
        return n;
    }

    // The slot behind the i-th used slot (i counted from 0), or -1.
    int NthUsedSlot( const patchMesh_t *def, int i )
    {
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( SlotUsed( def, k ) && i-- == 0 )
                return k;
        return -1;
    }

    int FirstFreeSlot( const patchMesh_t *def )
    {
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( !SlotUsed( def, k ) )
                return k;
        return -1;
    }

    const char *BaseMaterialName( const patchMesh_t *def )
    {
        return def->texture.radMtl && def->texture.radMtl->name ? def->texture.radMtl->name : "(none)";
    }

    int FindSlotByName( const patchMesh_t *def, const char *name )
    {
        if ( !name || !name[0] )
            return -1;
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( SlotUsed( def, k ) && !_stricmp( def->kiwiLayer[k], name ) )
                return k;
        return -1;
    }

    // KIWI (2026-09-19): "kiwi_blend_<name>" is the EDITOR'S OWN preview copy of <name> (an
    // alpha-blend decal clone written into materials/ so the renderer can load it).  Being a
    // real file it shows in the texture browser, and the user's map had it painted as a
    // terrain LAYER on 49 patches (found by peeking the running editor's twin table: a
    // "kiwi_blend_kiwi_blend_..." twin-of-a-twin).  Such a layer compiles as a decal material,
    // and the same grass then lives under two names, which Blend / the seam pass cannot join.
    // The real name is what belongs in a map: paint material names are unwrapped on the way
    // in, and a patch that already carries a wrapped layer is healed when a paint / blend
    // stroke touches it.
    const char *UnwrapTwinName( const char *name )
    {
        while ( name && !_strnicmp( name, "kiwi_blend_", 11 ) )
            name += 11;
        return name;
    }

    void HealTwinLayers( patchMesh_t *def )
    {
        for ( int k = 0; k < KTER_SLOTS; ++k )
        {
            if ( !SlotUsed( def, k ) || _strnicmp( def->kiwiLayer[k], "kiwi_blend_", 11 ) != 0 )
                continue;
            char real[64];
            strncpy( real, UnwrapTwinName( def->kiwiLayer[k] ), sizeof( real ) - 1 );
            real[sizeof( real ) - 1] = '\0';
            if ( !real[0] )
                continue;
            const int other = FindSlotByName( def, real );
            if ( other >= 0 && other != k )
            {
                // the real layer is there too: keep the stronger weight per point, drop the wrapped one
                for ( int i = 0; i < def->width; ++i )
                    for ( int j = 0; j < def->height; ++j )
                    {
                        byte *c = (byte *)&def->ctrl[i][j].vert_color;
                        if ( c[k] > c[other] )
                            c[other] = c[k];
                        c[k] = 0;
                    }
                def->kiwiLayer[k][0] = '\0';
            }
            else
            {
                strncpy( def->kiwiLayer[k], real, 63 );
                def->kiwiLayer[k][63] = '\0';
            }
        }
    }

    // Texture paint with a material on the brush (or "erase to base") needs no
    // selection: every eligible patch is a target.
    bool PaintAnywhere()
    {
        return s_tool == KTER_TEXTURE && ( s_paintBase || s_paintMaterial[0] != 0 );
    }

    // KIWI FIX (2026-09-15, user: "the railroad track isn't showing over the terrain;
    // it does in height-colour mode"): the l_sm_b0c0* templates the layer-run materials
    // are cloned from are DECALS, and their state bits carry a polygon offset that pulls
    // the run toward the camera. A model lying on the terrain (rails, a road piece) is
    // then behind the offset run and vanishes; the heat view has no layer runs, which is
    // why it showed there. A layer run is the SAME grid as the opaque base run, so it
    // needs no offset at all: clear it and make the depth test LESSEQUAL so the coplanar
    // run still passes over its own base. stateBitsTable is per material
    // (Material_SetStateBits copies it), and these materials are editor-only.
    //
    // KIWI FIX (2026-09-19, user: "some of the materials are rendering through each other" -
    // gravel paint over rail ties that the Height-colours view shows IN FRONT of the ground).
    // Settled by reading the state back from the D3D device (a temporary GetRenderState dump
    // in r_ed_scene.cpp's pass setup, removed again once it had answered): the twins
    // had no bias and the right test, everything else was opaque and depth-writing.  The
    // cause was LESSEQUAL itself.  A layer run is blended, writes no depth and draws LAST
    // (sort key base + L), so wherever another surface lies within one depth-buffer step of
    // the ground - tie tops on a rail bed flattened to their height - "less OR EQUAL" let
    // the paint win every near-tie, systematically, while in the heat view (no layer runs)
    // the ties were the last opaque thing drawn and won the very same ties.
    // A layer belongs on pixels whose visible surface is ITS OWN base run and nowhere else:
    // depth EQUAL.  The layer run is the base run's grid vertex for vertex, and this
    // renderer already relies on exactly that for the sun re-add (technique 26,
    // "additive_stencil", depth EQUAL against the base pass - camwnd.cpp
    // Cam_DrawBrushList_SunPreview).  The wireframe entries keep LESSEQUAL (lines are not
    // the base's triangles).  `s_layerDepthEqual` (pref LayerDepthEqual, default on) is the
    // escape hatch should some techset's vertex shader not reproduce the base's depth
    // bit for bit - the symptom would be paint flickering or vanishing.
    void StripDecalOffset( Material *m )
    {
        if ( !m || !m->stateBitsTable )
            return;
        const int wireA = m->stateBitsEntry[TECHNIQUE_WIREFRAME_SOLID];
        const int wireB = m->stateBitsEntry[TECHNIQUE_WIREFRAME_SHADED];
        for ( int e = 0; e < (int)m->stateBitsCount; ++e )
        {
            unsigned int &bits1 = m->stateBitsTable[e].loadBits[1];
            bits1 &= ~(unsigned int)GFXS1_POLYGON_OFFSET_MASK;
            if ( bits1 & GFXS1_DEPTHTEST_DISABLE )
                continue;
            const bool wire = ( e == wireA || e == wireB );
            const unsigned int func = ( s_layerDepthEqual && !wire ) ? GFXS1_DEPTHTEST_EQUAL : GFXS1_DEPTHTEST_LESSEQUAL;
            bits1 = ( bits1 & ~(unsigned int)GFXS1_DEPTHTEST_MASK ) | func;
        }
    }

    // Material state is read at draw time, so this takes effect on the next frame - no patch
    // re-upload, no mesh-run rebuild.
    void RestripTwins()
    {
        for ( std::map<std::string, Material *>::iterator it = s_blendTwins.begin(); it != s_blendTwins.end(); ++it )
            StripDecalOffset( it->second );
        g_nUpdateBits |= W_CAMERA;
    }

    // Preview twin: "kiwi_blend_<name>", an alpha-blend (l_sm_b0c0[n0][s0]) material over
    // the SAME images, written once through the material writer and loaded by the
    // renderer.  Editor-only; the .map never references it.
    Material *BlendTwin( const char *name )
    {
        if ( !name || !name[0] )
            return nullptr;
        std::map<std::string, Material *>::iterator it = s_blendTwins.find( name );
        if ( it != s_blendTwins.end() )
            return it->second;

        Material *twin = nullptr;
        char twinName[64];
        _snprintf( twinName, sizeof( twinName ), "kiwi_blend_%s", name );
        twinName[sizeof( twinName ) - 1] = '\0';
        char err[256] = { 0 };
        bool ok = KiwiMat_ExistsOnDisk( twinName );
        if ( !ok )
        {
            kiwiMatSource_t src;
            if ( KiwiMat_ReadSource( name, &src, err, sizeof( err ) ) )
            {
                const bool haveN = src.normalMapImage[0] && strcmp( src.normalMapImage, src.colorMapImage ) != 0;
                const bool haveS = src.specularMapImage[0] && strcmp( src.specularMapImage, src.colorMapImage ) != 0;
                char family[64];
                _snprintf( family, sizeof( family ), "l_sm_b0c0%s%s", haveN ? "n0" : "", haveS ? "s0" : "" );
                int tpl = -1;
                for ( int i = 0; i < KiwiMat_TemplateCount() && tpl < 0; ++i )
                {
                    const kiwiMatTemplateInfo_t *info = KiwiMat_TemplateInfo( i );
                    if ( info && info->techSet && !_stricmp( info->techSet, family ) )
                        tpl = i;
                }
                if ( tpl >= 0 )
                {
                    kiwiMatFields_t f;
                    memset( &f, 0, sizeof( f ) );
                    _snprintf( f.name, sizeof( f.name ), "%s", twinName );
                    _snprintf( f.imageName, sizeof( f.imageName ), "%s", src.colorMapImage );
                    if ( haveN ) _snprintf( f.normalImageName, sizeof( f.normalImageName ), "%s", src.normalMapImage );
                    if ( haveS ) _snprintf( f.specularImageName, sizeof( f.specularImageName ), "%s", src.specularMapImage );
                    f.usage  = src.usage ? src.usage : 1;
                    f.locale = src.locale ? src.locale : 1u;
                    f.autoTexScaleWidth  = src.autoTexScaleWidth  ? src.autoTexScaleWidth  : 512;
                    f.autoTexScaleHeight = src.autoTexScaleHeight ? src.autoTexScaleHeight : 512;
                    f.surfaceType = src.surfaceFlags & 0x1F00000;
                    ok = KiwiMat_Write( tpl, &f, err, sizeof( err ) );
                }
                else
                    _snprintf( err, sizeof( err ), "no shipped template for techset '%s'", family );
            }
        }
        if ( ok )
        {
            char asset[80];
            _snprintf( asset, sizeof( asset ), "wc/%s", twinName );
            twin = Material_Load( asset, 0 );
            if ( twin && Material_IsDefault( twin ) )
            {
                twin = nullptr;
                _snprintf( err, sizeof( err ), "'%s' loaded as the default material", twinName );
            }
            else if ( !twin )
                _snprintf( err, sizeof( err ), "Material_Load('%s') failed", twinName );
        }
        if ( twin )
            StripDecalOffset( twin );
        if ( !twin )
        {
            Sys_Printf( "Terrain Sculpt: no blend preview for '%s' (%s); the layer draws opaque in the camera.\n",
                        name, err[0] ? err : "unknown" );
            s_twinErr[name] = err[0] ? err : "unknown";
        }
        s_blendTwins[name] = twin;
        return twin;
    }

    // The height-gradient material: a clone of the plain lit world template
    // (l_sm_r0c0) whose colormap is the engine's builtin $white, so it carries every
    // camera technique (the editor's $opaque has no lit technique and white_tools
    // does not write depth - neither can stand in for a terrain surface) and the
    // vertex colour alone paints the surface.  Written once as kiwi_heat, cached.
    // Null = fall back to the patch's own material, tinted.
    Material *HeatMaterial()
    {
        static Material *s_heat     = nullptr;
        static bool      s_heatTried = false;
        if ( s_heatTried )
            return s_heat;
        s_heatTried = true;
        const char *heatName = "kiwi_heat";
        char err[256] = { 0 };
        bool ok = KiwiMat_ExistsOnDisk( heatName );
        if ( !ok )
        {
            int tpl = -1;
            for ( int i = 0; i < KiwiMat_TemplateCount() && tpl < 0; ++i )
            {
                const kiwiMatTemplateInfo_t *info = KiwiMat_TemplateInfo( i );
                if ( info && info->techSet && !_stricmp( info->techSet, "l_sm_r0c0" ) )
                    tpl = i;
            }
            if ( tpl >= 0 )
            {
                kiwiMatFields_t f;
                memset( &f, 0, sizeof( f ) );
                _snprintf( f.name, sizeof( f.name ), "%s", heatName );
                _snprintf( f.imageName, sizeof( f.imageName ), "%s", "$white" );
                f.usage  = 1;
                f.locale = 1u;
                f.autoTexScaleWidth  = 512;
                f.autoTexScaleHeight = 512;
                f.surfaceType = -1;                   // keep the template's surface flags
                ok = KiwiMat_Write( tpl, &f, err, sizeof( err ) );
            }
            else
                _snprintf( err, sizeof( err ), "no shipped template for techset 'l_sm_r0c0'" );
        }
        if ( ok )
        {
            s_heat = Material_Load( (char*)"wc/kiwi_heat", 0 );
            if ( s_heat && Material_IsDefault( s_heat ) )
            {
                s_heat = nullptr;
                _snprintf( err, sizeof( err ), "'kiwi_heat' loaded as the default material" );
            }
            else if ( !s_heat )
                _snprintf( err, sizeof( err ), "Material_Load('kiwi_heat') failed" );
        }
        if ( !s_heat )
            Sys_Printf( "Terrain Sculpt: no height-colour material (%s); the gradient tints the real textures instead.\n",
                        err[0] ? err : "unknown" );
        return s_heat;
    }

    // The weight-view material: the alpha-blend world template (l_sm_b0c0) over the
    // builtin $white colormap, so a layer run shows its slot colour at the painted
    // weight.  Written once as kiwi_weight, cached.  Null = the blend twin as usual.
    Material *WeightMaterial()
    {
        static Material *s_mat   = nullptr;
        static bool      s_tried = false;
        if ( s_tried )
            return s_mat;
        s_tried = true;
        const char *name = "kiwi_weight";
        char err[256] = { 0 };
        bool ok = KiwiMat_ExistsOnDisk( name );
        if ( !ok )
        {
            int tpl = -1;
            for ( int i = 0; i < KiwiMat_TemplateCount() && tpl < 0; ++i )
            {
                const kiwiMatTemplateInfo_t *info = KiwiMat_TemplateInfo( i );
                if ( info && info->techSet && !_stricmp( info->techSet, "l_sm_b0c0" ) )
                    tpl = i;
            }
            if ( tpl >= 0 )
            {
                kiwiMatFields_t f;
                memset( &f, 0, sizeof( f ) );
                _snprintf( f.name, sizeof( f.name ), "%s", name );
                _snprintf( f.imageName, sizeof( f.imageName ), "%s", "$white" );
                f.usage  = 1;
                f.locale = 1u;
                f.autoTexScaleWidth  = 512;
                f.autoTexScaleHeight = 512;
                f.surfaceType = -1;
                ok = KiwiMat_Write( tpl, &f, err, sizeof( err ) );
            }
            else
                _snprintf( err, sizeof( err ), "no shipped template for techset 'l_sm_b0c0'" );
        }
        if ( ok )
        {
            s_mat = Material_Load( (char*)"wc/kiwi_weight", 0 );
            if ( s_mat && Material_IsDefault( s_mat ) )
            {
                s_mat = nullptr;
                _snprintf( err, sizeof( err ), "'kiwi_weight' loaded as the default material" );
            }
            else if ( !s_mat )
                _snprintf( err, sizeof( err ), "Material_Load('kiwi_weight') failed" );
            StripDecalOffset( s_mat );      // same decal template, same coplanar run
        }
        if ( !s_mat )
            Sys_Printf( "Terrain Sculpt: no weight-view material (%s); painted layers show their blended texture instead.\n",
                        err[0] ? err : "unknown" );
        return s_mat;
    }

    // Undo bracket for one non-stroke edit of a patch (layer slot changes, etc.).
    void EditPatchBegin( patchMesh_t *def, const char *label )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( label );
        Patch_Paint( &selected_brushes );
        Patch_Paint( &active_brushes );
        def->xx22b = 1;
        Patch_PaintMarkUndo( def );
    }
    void EditPatchEnd( patchMesh_t *def, bool bounds )
    {
        Patch_Rebuild( def, bounds ? 1 : 0 );
        ++def->version;
        Patch_PaintFinish( &selected_brushes );
        Patch_PaintFinish( &active_brushes );
        Undo_End();
        g_nUpdateBits = -1;
    }

    void ClearChannel( patchMesh_t *def, int slot )
    {
        for ( int i = 0; i < def->width; ++i )
            for ( int j = 0; j < def->height; ++j )
                ( (byte *)&def->ctrl[i][j].vert_color )[slot] = 0;
    }

    // Layer slot edits apply to EVERY patch the brush would touch (the selection, plus
    // the active list under "Affect unselected"), so a layer added while several
    // patches are selected exists on all of them, and removing it takes every weight
    // painted with it along.  One undo record per edit.
    void TargetPatches( std::vector<patchMesh_t *> &out )
    {
        out.clear();
        for ( int pass = 0; pass < 2; ++pass )
        {
            if ( pass == 1 && !s_affectUnselected )
                break;
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( PatchEligible( b ) )
                    out.push_back( b->patch->def );
        }
    }

    void MultiEditBegin( const std::vector<patchMesh_t *> &defs, const char *label )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( label );
        Patch_Paint( &selected_brushes );
        Patch_Paint( &active_brushes );
        for ( size_t i = 0; i < defs.size(); ++i )
        {
            defs[i]->xx22b = 1;
            Patch_PaintMarkUndo( defs[i] );
        }
    }

    void MultiEditEnd( const std::vector<patchMesh_t *> &defs )
    {
        for ( size_t i = 0; i < defs.size(); ++i )
        {
            Patch_Rebuild( defs[i], 0 );
            ++defs[i]->version;
        }
        Patch_PaintFinish( &selected_brushes );
        Patch_PaintFinish( &active_brushes );
        Undo_End();
        g_nUpdateBits = -1;
    }

    // First layer on a patch: the colour bytes become weights, so start from zero.
    void AddLayerSlot( patchMesh_t *lead, const char *material )
    {
        const int slot = FirstFreeSlot( lead );
        if ( slot < 0 || !material || !material[0] )
            return;
        std::vector<patchMesh_t *> defs;
        TargetPatches( defs );
        MultiEditBegin( defs, "add terrain layer" );
        for ( size_t d = 0; d < defs.size(); ++d )
        {
            patchMesh_t *def = defs[d];
            if ( SlotUsed( def, slot ) && strcmp( def->kiwiLayer[slot], material ) != 0 )
                continue;                                  // that slot means something else here
            if ( UsedSlotCount( def ) == 0 )
                for ( int i = 0; i < def->width; ++i )
                    for ( int j = 0; j < def->height; ++j )
                        *(unsigned int *)&def->ctrl[i][j].vert_color = 0u;
            strncpy( def->kiwiLayer[slot], material, 63 );
            def->kiwiLayer[slot][63] = '\0';
        }
        MultiEditEnd( defs );
        strncpy( s_paintMaterial, material, sizeof( s_paintMaterial ) - 1 );   // and paint with it
        s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
        s_paintBase = false;
        Save();
    }

    // Removes the slot AND every weight ever painted with it, on every target patch.
    void RemoveLayerSlot( patchMesh_t *lead, int slot )
    {
        if ( !SlotUsed( lead, slot ) )
            return;
        std::vector<patchMesh_t *> defs;
        TargetPatches( defs );
        MultiEditBegin( defs, "remove terrain layer" );
        for ( size_t d = 0; d < defs.size(); ++d )
        {
            patchMesh_t *def = defs[d];
            if ( !SlotUsed( def, slot ) )
                continue;
            def->kiwiLayer[slot][0] = '\0';
            ClearChannel( def, slot );
            if ( UsedSlotCount( def ) == 0 )              // back to a plain patch: white
                for ( int i = 0; i < def->width; ++i )
                    for ( int j = 0; j < def->height; ++j )
                        *(unsigned int *)&def->ctrl[i][j].vert_color = 0xFFFFFFFFu;
        }
        MultiEditEnd( defs );
    }

    void SwapLayerMaterial( patchMesh_t *lead, int slot, const char *material )
    {
        if ( !SlotUsed( lead, slot ) || !material || !material[0] )
            return;
        std::vector<patchMesh_t *> defs;
        TargetPatches( defs );
        MultiEditBegin( defs, "terrain layer material" );
        for ( size_t d = 0; d < defs.size(); ++d )
        {
            if ( !SlotUsed( defs[d], slot ) )
                continue;
            strncpy( defs[d]->kiwiLayer[slot], material, 63 );
            defs[d]->kiwiLayer[slot][63] = '\0';
        }
        MultiEditEnd( defs );
    }

    // Bezier patches re-tessellate on every edit (subdivision 8 on a 16x16 grid =
    // 121x121 vertices); a terrain mesh IS its control grid.  Ground should be terrain.
    int SelectedBezierCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( PatchEligibleAnyType( b ) && ( b->patch->def->type & PATCH_TERRAIN ) == 0 )
                ++n;
        return n;
    }

    void ConvertSelectedToTerrain()
    {
        int n = 0;
        Undo_ClearRedo();
        Undo_GeneralStart( "convert to terrain mesh" );
        Patch_Paint( &selected_brushes );
        Patch_Paint( &active_brushes );
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !PatchEligibleAnyType( b ) || ( b->patch->def->type & PATCH_TERRAIN ) != 0 )
                continue;
            patchMesh_t *def = b->patch->def;
            def->xx22b = 1;
            Patch_PaintMarkUndo( def );
            def->type = (PATCH_TYPES)( def->type | PATCH_TERRAIN );
            Patch_Rebuild( def, 1 );
            ++def->version;
            ++n;
        }
        Patch_PaintFinish( &selected_brushes );
        Patch_PaintFinish( &active_brushes );
        Undo_End();
        g_nUpdateBits = -1;
        Sys_Printf( "Terrain Sculpt: converted %i bezier patch%s to terrain mesh.\n", n, n == 1 ? "" : "es" );
    }

    // The first selected eligible patch — the one the Texture paint UI edits.
    selbrush_t *FirstSelectedPatch()
    {
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( PatchEligible( b ) )
                return b;
        return nullptr;
    }

    // ── auto-transition ──────────────────────────────────────────────────────
    kterBand_t &BandFor( patchMesh_t *def, int slot )
    {
        std::map<patchMesh_t *, kterBands_t>::iterator it = s_bands.find( def );
        if ( it == s_bands.end() )
        {
            kterBands_t all;
            for ( int k = 0; k < KTER_SLOTS; ++k )
            {
                kterBand_t &b = all.slot[k];
                b.enabled = false;
                b.minZ = -65536.0f; b.maxZ = 65536.0f; b.fadeZ = 64.0f;
                b.minSlope = 0.0f;  b.maxSlope = 90.0f; b.fadeSlope = 8.0f;
                if ( k == 1 )      b.minZ = 256.0f;
                else if ( k >= 2 ) b.minSlope = 35.0f;
            }
            s_bands[def] = all;
            it = s_bands.find( def );
        }
        return it->second.slot[slot];
    }

    float Band( float v, float lo, float hi, float fade )
    {
        if ( fade < 0.001f ) fade = 0.001f;
        float a = ( v - ( lo - fade ) ) / fade;
        float b = ( ( hi + fade ) - v ) / fade;
        if ( a < 0.0f ) a = 0.0f; if ( a > 1.0f ) a = 1.0f;
        if ( b < 0.0f ) b = 0.0f; if ( b > 1.0f ) b = 1.0f;
        a = a * a * ( 3.0f - 2.0f * a );
        b = b * b * ( 3.0f - 2.0f * b );
        return a < b ? a : b;
    }

    float SlopeAt( const patchMesh_t *def, int i, int j )
    {
        const int i0 = i > 0 ? i - 1 : i, i1 = i < def->width - 1 ? i + 1 : i;
        const int j0 = j > 0 ? j - 1 : j, j1 = j < def->height - 1 ? j + 1 : j;
        const float *a = def->ctrl[i0][j].xyz, *b = def->ctrl[i1][j].xyz;
        const float *c = def->ctrl[i][j0].xyz, *d = def->ctrl[i][j1].xyz;
        const float du = sqrtf( ( b[0] - a[0] ) * ( b[0] - a[0] ) + ( b[1] - a[1] ) * ( b[1] - a[1] ) );
        const float dv = sqrtf( ( d[0] - c[0] ) * ( d[0] - c[0] ) + ( d[1] - c[1] ) * ( d[1] - c[1] ) );
        const float gu = du > 0.001f ? ( b[2] - a[2] ) / du : 0.0f;
        const float gv = dv > 0.001f ? ( d[2] - c[2] ) / dv : 0.0f;
        return atanf( sqrtf( gu * gu + gv * gv ) ) * 180.0f / KTER_PI;
    }

    // Write every enabled slot's weights from its bands (caller owns the undo bracket).
    void ApplyAutoTransition( patchMesh_t *def )
    {
        bool any = false;
        for ( int k = 0; k < KTER_SLOTS; ++k )
        {
            if ( !SlotUsed( def, k ) || !BandFor( def, k ).enabled )
                continue;
            const kterBand_t &b = BandFor( def, k );
            any = true;
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                {
                    const float z = def->ctrl[i][j].xyz[2];
                    const float w = Band( z, b.minZ, b.maxZ, b.fadeZ )
                                  * Band( SlopeAt( def, i, j ), b.minSlope, b.maxSlope, b.fadeSlope );
                    ( (byte *)&def->ctrl[i][j].vert_color )[k] =
                        (byte)(int)( ClampF( w * 255.0f, 0.0f, 255.0f ) + 0.5f );
                }
        }
        if ( any )
        {
            def->xx22b = 1;
            Patch_PaintMarkUndo( def );
            Patch_Rebuild( def, 0 );
            ++def->version;
            g_nUpdateBits = -1;
        }
    }

    void DrawBandRings( selbrush_t *node )
    {
        patchMesh_t *def = node->patch->def;
        const float *mins = node->def->mins, *maxs = node->def->maxs;
        const float cx = ( mins[0] + maxs[0] ) * 0.5f, cy = ( mins[1] + maxs[1] ) * 0.5f;
        const float ex = maxs[0] - mins[0], ey = maxs[1] - mins[1];
        const float radius = sqrtf( ex * ex + ey * ey ) * 0.5f + 64.0f;
        const int   segs = 48;
        int rings = 0;
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( SlotUsed( def, k ) && BandFor( def, k ).enabled ) rings += 2;
        if ( !rings )
            return;
        KiwiLines_Begin( rings * segs + 4 * rings, 1 );
        for ( int k = 0; k < KTER_SLOTS; ++k )
        {
            if ( !SlotUsed( def, k ) || !BandFor( def, k ).enabled )
                continue;
            const kterBand_t &b = BandFor( def, k );
            for ( int edge = 0; edge < 2; ++edge )
            {
                const float z = edge == 0 ? b.minZ : b.maxZ;
                if ( z < -60000.0f || z > 60000.0f )
                    continue;
                if ( edge == 0 ) KiwiLines_Color( 0.25f, 0.8f, 0.35f );
                else             KiwiLines_Color( 0.45f, 1.0f, 0.5f );
                float prev[3] = { cx + radius, cy, z };
                for ( int i = 1; i <= segs; ++i )
                {
                    const float a = 2.0f * KTER_PI * (float)i / (float)segs;
                    float pt[3] = { cx + radius * cosf( a ), cy + radius * sinf( a ), z };
                    if ( !KiwiLines_Add( prev, pt ) )
                        break;
                    memcpy( prev, pt, sizeof( pt ) );
                }
                float t0[3] = { cx + radius, cy, z };
                float t1[3] = { cx + radius + 32.0f * (float)( k + 1 ), cy, z };
                KiwiLines_Add( t0, t1 );
            }
        }
        KiwiLines_Flush();
    }

    // ── stamping ─────────────────────────────────────────────────────────────
    enum kterOp_t { OP_RAISE, OP_SETHEIGHT, OP_SMOOTH, OP_NOISE, OP_TEXTURE, OP_BLEND };

    struct gridSnap_t
    {
        float z[16][16];
        byte  c[16][16][4];
    };

    void MarkTouched( patchMesh_t *def )
    {
        if ( !def->xx22b )
        {
            def->xx22b = 1;
            Patch_PaintMarkUndo( def );
            ++s_touched;
        }
    }

    // Raise speed grows with the brush (units per second at a 128 outer radius, never
    // below that): a wide brush moving a hill should not crawl.  Always on - the old
    // "Speed grows with radius" checkbox was confusing and its off state too slow.
    float AdditiveRate()
    {
        const float r = s_outer > 128.0f ? s_outer / 128.0f : 1.0f;
        return s_amount * r;
    }

    float LerpStep( float w, float dt )
    {
        const float t = 1.0f - expf( -w * dt * 20.0f );
        return t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
    }

    byte LerpByte( byte cur, float target, float t )
    {
        float v = (float)cur + ( target - (float)cur ) * t;
        v = ClampF( v, 0.0f, 255.0f );
        return (byte)(int)( v + 0.5f );
    }

    void NoteDirty( patchMesh_t *def, bool bounds )
    {
        bool have = false;
        for ( size_t i = 0; i < s_dirtyDefs.size() && !have; ++i )
            have = ( s_dirtyDefs[i] == def );
        if ( !have )
            s_dirtyDefs.push_back( def );
        if ( bounds )
            s_dirtyBounds = true;
    }

    // Seams: after a height op the border vertices of the stroke's patches LOCK to the
    // terrain around them.
    //   * A border point with a partner point on another patch within KTER_WELD units
    //     (XY) is welded: every member takes one height (the mean over the stroke's own
    //     patches) and the partner's exact XY, so grids that drifted a hair apart snap
    //     back together.
    //   * A border point with no partner (a finer edge meeting a coarser one, or an
    //     irregular grid) conforms to the neighbour's border SEGMENT it lies on: its
    //     height becomes the segment's height there, which is what closes the crack.
    // Neighbours that were not in the stroke are pulled along and undo-marked.
    const float KTER_WELD = 1.0f;

    struct seamPatch_t
    {
        selbrush_t  *node;
        patchMesh_t *def;
        bool         target;
    };

    // Changed THIS flush.  The seam passes anchor on these, never on the whole target
    // list: with "Affect unselected" every patch of a 1,286-chunk map is a target, and
    // anchoring on targets meant every border point of every patch was tested against
    // every other patch on every frame of a stroke (~10^8 checks: the 15 fps blend).
    bool IsDirtyDef( patchMesh_t *def )
    {
        for ( size_t i = 0; i < s_dirtyDefs.size(); ++i )
            if ( s_dirtyDefs[i] == def )
                return true;
        return false;
    }

    bool IsTargetDef( patchMesh_t *def )
    {
        for ( size_t t = 0; t < s_targets.size(); ++t )
            if ( s_targets[t]->patch->def == def )
                return true;
        return false;
    }

    bool IsBorder( const patchMesh_t *def, int i, int j )
    {
        return i == 0 || j == 0 || i == def->width - 1 || j == def->height - 1;
    }

    // Walk a patch's border as consecutive (i,j) pairs; returns the point count.
    int BorderRing( const patchMesh_t *def, int ii[64], int jj[64] )
    {
        int n = 0;
        const int w = def->width, h = def->height;
        for ( int i = 0; i < w && n < 64; ++i )         { ii[n] = i;     jj[n] = 0;     ++n; }
        for ( int j = 1; j < h && n < 64; ++j )         { ii[n] = w - 1; jj[n] = j;     ++n; }
        for ( int i = w - 2; i >= 0 && n < 64; --i )    { ii[n] = i;     jj[n] = h - 1; ++n; }
        for ( int j = h - 2; j >= 1 && n < 64; --j )    { ii[n] = 0;     jj[n] = j;     ++n; }
        return n;
    }

    void TouchNeighbour( patchMesh_t *def, bool target )
    {
        if ( target )
            return;
        if ( !def->xx22b )
        {
            def->xx22b = 1;
            Patch_PaintMarkUndo( def );
        }
        NoteDirty( def, true );
    }

    // Is point p (XY) on the straight segment q0-q1 within KTER_WELD?  *outT = 0..1.
    bool OnSegment( const float *p, const float *q0, const float *q1, float *outT )
    {
        const float dx = q1[0] - q0[0], dy = q1[1] - q0[1];
        const float len2 = dx * dx + dy * dy;
        if ( len2 < 1e-4f )
            return false;
        float t = ( ( p[0] - q0[0] ) * dx + ( p[1] - q0[1] ) * dy ) / len2;
        if ( t < -0.001f || t > 1.001f )
            return false;
        if ( t < 0.0f ) t = 0.0f;
        if ( t > 1.0f ) t = 1.0f;
        const float ex = p[0] - ( q0[0] + dx * t ), ey = p[1] - ( q0[1] + dy * t );
        if ( ex * ex + ey * ey > KTER_WELD * KTER_WELD )
            return false;
        *outT = t;
        return true;
    }

    // The stamp centres of the ApplyStroke being flushed.  With "never change anything
    // outside the ring" on, the seam pass only touches border points one of those rings
    // covered: it used to run over a stamped patch's WHOLE border and so "healed" old
    // cracks on neighbours far from the brush (2026-09-18).  Empty = no restriction.
    float s_reachCenters[8][3];
    int   s_reachCount = 0;

    // Control points a height stamp moved since the last flush.  The weld uses it: where
    // two patches share a point and only ONE side's copy moved (the other was shut out -
    // its cells are coarser and stick out of the ring - or simply is not a target), the
    // moved copy wins.  A plain mean halved the stroke along such a seam on every flush:
    // the other half of the "moat" (2026-09-18).
    std::set<const drawVert_t *> s_movedPts;

    bool InStrokeReach( const float *p )
    {
        if ( !s_setHeightContain || s_reachCount == 0 )
            return true;
        for ( int i = 0; i < s_reachCount; ++i )
            if ( BrushDistance( s_reachCenters[i], p ) <= s_outer )
                return true;
        return false;
    }

    void StitchSeams()
    {
        if ( s_targets.empty() || s_dirtyDefs.empty() )
            return;
        std::vector<seamPatch_t> all;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( PatchEligible( b ) )
                {
                    seamPatch_t e = { b, b->patch->def, IsDirtyDef( b->patch->def ) };
                    all.push_back( e );
                }
        }

        for ( size_t a = 0; a < all.size(); ++a )
        {
            if ( !all[a].target )
                continue;
            patchMesh_t *A = all[a].def;
            int ai[64], aj[64];
            const int an = BorderRing( A, ai, aj );
            for ( int k = 0; k < an; ++k )
            {
                drawVert_t *P = &A->ctrl[ai[k]][aj[k]];
                if ( !InStrokeReach( P->xyz ) )
                    continue;
                // Pass 1: weld with coincident border points on other patches.
                float  zSum = P->xyz[2];
                int    zN = 1;
                const bool pMoved = s_movedPts.count( P ) != 0;
                float  mSum = pMoved ? P->xyz[2] : 0.0f;       // mean over the copies a stamp moved
                int    mN   = pMoved ? 1 : 0;
                std::vector<drawVert_t *> group;
                std::vector<size_t>       groupPatch;
                for ( size_t o = 0; o < all.size(); ++o )
                {
                    if ( o == a )
                        continue;
                    const float *mins = all[o].node->def->mins, *maxs = all[o].node->def->maxs;
                    if ( P->xyz[0] < mins[0] - KTER_WELD || P->xyz[0] > maxs[0] + KTER_WELD
                      || P->xyz[1] < mins[1] - KTER_WELD || P->xyz[1] > maxs[1] + KTER_WELD )
                        continue;
                    patchMesh_t *B = all[o].def;
                    int bi[64], bj[64];
                    const int bn = BorderRing( B, bi, bj );
                    for ( int m = 0; m < bn; ++m )
                    {
                        drawVert_t *Q = &B->ctrl[bi[m]][bj[m]];
                        if ( fabsf( Q->xyz[0] - P->xyz[0] ) <= KTER_WELD && fabsf( Q->xyz[1] - P->xyz[1] ) <= KTER_WELD )
                        {
                            group.push_back( Q );
                            groupPatch.push_back( o );
                            if ( all[o].target ) { zSum += Q->xyz[2]; ++zN; }
                            if ( s_movedPts.count( Q ) ) { mSum += Q->xyz[2]; ++mN; }
                        }
                    }
                }
                if ( !group.empty() )
                {
                    const float z = mN ? mSum / (float)mN : zSum / (float)zN;
                    P->xyz[2] = z;
                    for ( size_t g = 0; g < group.size(); ++g )
                    {
                        drawVert_t *Q = group[g];
                        if ( fabsf( Q->xyz[2] - z ) > 0.001f || Q->xyz[0] != P->xyz[0] || Q->xyz[1] != P->xyz[1] )
                        {
                            TouchNeighbour( all[groupPatch[g]].def, all[groupPatch[g]].target );
                            Q->xyz[0] = P->xyz[0];
                            Q->xyz[1] = P->xyz[1];
                            Q->xyz[2] = z;
                        }
                    }
                }
            }
        }

        // Pass 2 (T-junctions), after EVERY weld so the segment ends are final.
        // KIWI FIX (2026-09-15, user: "make the smooth tool fix these gaps"): the old
        // pass only conformed the STROKED patch's partnerless points to its neighbours'
        // segments. Sculpt or smooth the COARSE side of a coarse/fine seam and the fine
        // neighbour's in-between points were never touched: they kept their old heights
        // under the coarse patch's straight edge, and the crack showed as the dark
        // slivers and long thin wedges along the seam. Both directions now conform:
        //   (a) a stroked patch's border point with no coincident partner takes the
        //       height of the neighbour segment it lies on;
        //   (b) a neighbour's border point lying inside one of the stroked patch's
        //       border segments takes that segment's height (neighbour undo-marked).
        for ( size_t a = 0; a < all.size(); ++a )
        {
            if ( !all[a].target )
                continue;
            patchMesh_t *A = all[a].def;
            int ai[64], aj[64];
            const int an = BorderRing( A, ai, aj );
            for ( size_t o = 0; o < all.size(); ++o )
            {
                if ( o == a )
                    continue;
                const float *amins = all[a].node->def->mins, *amaxs = all[a].node->def->maxs;
                const float *bmins = all[o].node->def->mins, *bmaxs = all[o].node->def->maxs;
                if ( amaxs[0] < bmins[0] - KTER_WELD || amins[0] > bmaxs[0] + KTER_WELD
                  || amaxs[1] < bmins[1] - KTER_WELD || amins[1] > bmaxs[1] + KTER_WELD )
                    continue;
                patchMesh_t *B = all[o].def;
                int bi[64], bj[64];
                const int bn = BorderRing( B, bi, bj );

                // KIWI (2026-09-18, user: "at the edge of 2 terrain patches, it forms a moat
                // ... the raise tool doesn't seem to like raising this area"): (a) used to run
                // FIRST.  It snapped the stroked patch's seam points down onto the neighbour's
                // edge while that edge still had its OLD heights, and only then (b) copied the
                // - now lowered - seam back to the neighbour.  Every flush undid the stroke
                // along the seam, so the ground rose on both sides of a line that could not:
                // a moat.  The STROKED patch is the master: (b) first lifts the neighbour's
                // seam vertices onto the stroked profile, then (a) lays the stroked patch's
                // in-between points on the neighbour's (now moved) edge to close the crack.
                // (A seam between two neighbour vertices that are both outside every ring
                // still cannot bend - RefineUnderRing re-grids the neighbour for that.)

                // (b) B's points inside A's segments (strictly between the ends).
                for ( int m = 0; m < bn; ++m )
                {
                    drawVert_t *Q = &B->ctrl[bi[m]][bj[m]];
                    if ( !InStrokeReach( Q->xyz ) )
                        continue;
                    for ( int k = 0; k < an; ++k )
                    {
                        const float *p0 = A->ctrl[ai[k]][aj[k]].xyz;
                        const float *p1 = A->ctrl[ai[( k + 1 ) % an]][aj[( k + 1 ) % an]].xyz;
                        if ( ( fabsf( p0[0] - Q->xyz[0] ) <= KTER_WELD && fabsf( p0[1] - Q->xyz[1] ) <= KTER_WELD )
                          || ( fabsf( p1[0] - Q->xyz[0] ) <= KTER_WELD && fabsf( p1[1] - Q->xyz[1] ) <= KTER_WELD ) )
                            continue;                       // an end: pass 1 welded it
                        float t;
                        if ( !OnSegment( Q->xyz, p0, p1, &t ) )
                            continue;
                        const float z = p0[2] + ( p1[2] - p0[2] ) * t;
                        if ( fabsf( Q->xyz[2] - z ) > 0.001f )
                        {
                            TouchNeighbour( B, all[o].target );
                            Q->xyz[2] = z;
                        }
                        break;
                    }
                }

                // (a) A's partnerless points onto B's segments (B's seam is final now).
                for ( int k = 0; k < an; ++k )
                {
                    drawVert_t *P = &A->ctrl[ai[k]][aj[k]];
                    if ( !InStrokeReach( P->xyz ) )
                        continue;
                    bool partnered = false;
                    for ( int m = 0; m < bn && !partnered; ++m )
                    {
                        const float *q = B->ctrl[bi[m]][bj[m]].xyz;
                        partnered = fabsf( q[0] - P->xyz[0] ) <= KTER_WELD && fabsf( q[1] - P->xyz[1] ) <= KTER_WELD;
                    }
                    if ( partnered )
                        continue;
                    for ( int m = 0; m < bn; ++m )
                    {
                        const float *q0 = B->ctrl[bi[m]][bj[m]].xyz;
                        const float *q1 = B->ctrl[bi[( m + 1 ) % bn]][bj[( m + 1 ) % bn]].xyz;
                        float t;
                        if ( !OnSegment( P->xyz, q0, q1, &t ) )
                            continue;
                        // P sits on B's edge between two of B's vertices: only B's straight
                        // segment can be honoured, so P takes its height there.
                        P->xyz[2] = q0[2] + ( q1[2] - q0[2] ) * t;
                        break;
                    }
                }
            }
        }
    }

    // Seams for WEIGHTS: after a texture / blend stroke, every border point of a
    // stroke patch takes, together with the coincident border points of other
    // patches (within KTER_WELD in XY), the MEAN of all their colour bytes, so a
    // painted transition continues across the chunk edge instead of stepping.
    // Neighbours pulled along are undo-marked and rebuilt, like StitchSeams.
    void StitchWeights()
    {
        if ( s_targets.empty() || s_dirtyDefs.empty() )
            return;
        std::vector<seamPatch_t> all;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( PatchEligible( b ) && UsedSlotCount( b->patch->def ) )
                {
                    seamPatch_t e = { b, b->patch->def, IsDirtyDef( b->patch->def ) };
                    all.push_back( e );
                }
        }
        for ( size_t a = 0; a < all.size(); ++a )
        {
            if ( !all[a].target )
                continue;
            patchMesh_t *A = all[a].def;
            int ai[64], aj[64];
            const int an = BorderRing( A, ai, aj );
            for ( int k = 0; k < an; ++k )
            {
                drawVert_t *P = &A->ctrl[ai[k]][aj[k]];
                std::vector<drawVert_t *> group;
                std::vector<size_t>       groupPatch;
                for ( size_t o = 0; o < all.size(); ++o )
                {
                    if ( o == a )
                        continue;
                    const float *mins = all[o].node->def->mins, *maxs = all[o].node->def->maxs;
                    if ( P->xyz[0] < mins[0] - KTER_WELD || P->xyz[0] > maxs[0] + KTER_WELD
                      || P->xyz[1] < mins[1] - KTER_WELD || P->xyz[1] > maxs[1] + KTER_WELD )
                        continue;
                    patchMesh_t *B = all[o].def;
                    int bi[64], bj[64];
                    const int bn = BorderRing( B, bi, bj );
                    for ( int m = 0; m < bn; ++m )
                    {
                        drawVert_t *Q = &B->ctrl[bi[m]][bj[m]];
                        if ( fabsf( Q->xyz[0] - P->xyz[0] ) <= KTER_WELD && fabsf( Q->xyz[1] - P->xyz[1] ) <= KTER_WELD )
                        {
                            group.push_back( Q );
                            groupPatch.push_back( o );
                        }
                    }
                }
                if ( group.empty() )
                    continue;
                // KIWI (2026-09-18): BY LAYER NAME, not by byte index.  This used to average
                // (and then memcpy) the four raw colour bytes across the seam, which is only
                // right when both patches keep the same material in the same slot - and
                // ShareLayersUnderRing now adds layers wherever a slot happens to be free.
                // A member that does not carry the layer is left alone for that layer.
                for ( int c = 0; c < KTER_SLOTS; ++c )
                {
                    if ( !SlotUsed( A, c ) )
                        continue;
                    float sum = (float)( (const byte *)&P->vert_color )[c];
                    int   cnt = 1;
                    int   slotOf[64];
                    const size_t gn = group.size() < 64 ? group.size() : 64;
                    for ( size_t g = 0; g < gn; ++g )
                    {
                        slotOf[g] = FindSlotByName( all[groupPatch[g]].def, A->kiwiLayer[c] );
                        if ( slotOf[g] >= 0 )
                        {
                            sum += (float)( (const byte *)&group[g]->vert_color )[slotOf[g]];
                            ++cnt;
                        }
                    }
                    if ( cnt < 2 )
                        continue;
                    const byte mean = (byte)(int)( sum / (float)cnt + 0.5f );
                    ( (byte *)&P->vert_color )[c] = mean;
                    for ( size_t g = 0; g < gn; ++g )
                    {
                        if ( slotOf[g] < 0 || ( (const byte *)&group[g]->vert_color )[slotOf[g]] == mean )
                            continue;
                        TouchNeighbour( all[groupPatch[g]].def, all[groupPatch[g]].target );
                        NoteDirty( all[groupPatch[g]].def, false );
                        ( (byte *)&group[g]->vert_color )[slotOf[g]] = mean;
                    }
                }
            }
        }
    }

    void FlushDirty()
    {
        if ( s_dirtyBounds && !s_dirtyDefs.empty() )
            StitchSeams();
        if ( !s_dirtyDefs.empty() && ( s_tool == KTER_TEXTURE || s_tool == KTER_BLEND ) )
            StitchWeights();
        for ( size_t i = 0; i < s_dirtyDefs.size(); ++i )
            Patch_Rebuild( s_dirtyDefs[i], s_dirtyBounds ? 1 : 0 );
        s_dirtyDefs.clear();
        s_dirtyBounds = false;
        s_movedPts.clear();
    }

    float CellSizeOf( const patchMesh_t *def );     // below: the grid's world cell size
    bool  s_coarseSpill = false;                    // Set height reached a grid coarser than the brush
    int   s_fitSet = 0, s_fitShut = 0;              // Set height, this stamp: points set / points held back
    bool  s_containBypass = false;                  // Stamp(): second pass when containment shut EVERY point out

    // KIWI (2026-09-17, user: "the set height tool is still causing some sort of lean on
    // areas not affected by the circle"): a terrain mesh is flat triangles between control
    // points, so moving ONE point tilts every triangle that uses it - and those reach a
    // full cell past the point.  Setting every point inside the ring therefore always
    // leaned the cells just OUTSIDE it.  Contained mode moves a point only when every
    // triangle it would tilt lies inside the ring: both edge neighbours of each of its
    // four cells, plus the far corner when that cell's diagonal runs through the point.
    // The slope between old and new height then sits inside the ring and the ground
    // outside keeps its shape exactly.  At a patch border the missing cells are taken as
    // the mirror image of the ones that exist, so the two patches sharing the seam reach
    // the same verdict for their coincident points (a regular grid continues that way).
    bool InsideRing( const float *center, float x, float y )
    {
        const float p[3] = { x, y, 0.0f };
        return BrushDistance( center, p ) <= s_outer;
    }

    bool SetHeightFits( const patchMesh_t *def, int i, int j, const float *center )
    {
        if ( def->width < 2 || def->height < 2 )
            return true;
        const float *v = def->ctrl[i][j].xyz;
        for ( int di = -1; di <= 1; di += 2 )
            for ( int dj = -1; dj <= 1; dj += 2 )
            {
                const int  ni = i + di, nj = j + dj;
                const bool okI = ni >= 0 && ni < def->width;
                const bool okJ = nj >= 0 && nj < def->height;
                const int  ri = okI ? ni : i - di;          // real column / row used (mirrored if !ok)
                const int  rj = okJ ? nj : j - dj;
                const float *eI = def->ctrl[ri][j].xyz;
                const float *eJ = def->ctrl[i][rj].xyz;
                const float *dg = def->ctrl[ri][rj].xyz;
                const float sI = okI ? 1.0f : -1.0f, sJ = okJ ? 1.0f : -1.0f;
                // offsets from v, mirrored across the border where the cell does not exist
                const float eIx = v[0] + ( eI[0] - v[0] ) * sI, eIy = v[1] + ( eI[1] - v[1] ) * sI;
                const float eJx = v[0] + ( eJ[0] - v[0] ) * sJ, eJy = v[1] + ( eJ[1] - v[1] ) * sJ;
                if ( !InsideRing( center, eIx, eIy ) || !InsideRing( center, eJx, eJy ) )
                    return false;
                bool needDiag = true;
                if ( okI && okJ )
                {
                    const int  qi = ni < i ? ni : i, qj = nj < j ? nj : j;
                    const bool turned   = ( def->ctrl[qi][qj].turned_edge & 1 ) != 0;   // diagonal v00-v11
                    const bool onMain   = di == dj;                                     // v is v00 or v11
                    needDiag = turned ? onMain : !onMain;
                }
                if ( needDiag )
                {
                    const float dx = ( eIx - v[0] ) + ( eJx - v[0] ) + ( ( dg[0] - eI[0] - eJ[0] + v[0] ) * sI * sJ );
                    const float dy = ( eIy - v[1] ) + ( eJy - v[1] ) + ( ( dg[1] - eI[1] - eJ[1] + v[1] ) * sI * sJ );
                    if ( !InsideRing( center, v[0] + dx, v[1] + dy ) )
                        return false;
                }
            }
        return true;
    }

    bool StampPatch( selbrush_t *b, kterOp_t op, const float *center, float sign, float dt )
    {
        patchMesh_t *def = b->patch->def;
        if ( def->width <= 0 || def->height <= 0 )
            return false;
        {
            const float *mins = b->def->mins, *maxs = b->def->maxs;
            const float r = s_shape == KTER_SQUARE ? s_outer * 1.42f : s_outer;
            if ( center[0] + r < mins[0] || center[0] - r > maxs[0]
              || center[1] + r < mins[1] || center[1] - r > maxs[1] )
                return false;
        }
        const bool texOp = ( op == OP_TEXTURE || ( op == OP_SMOOTH && s_tool == KTER_TEXTURE ) );
        // a layer stored under the editor's preview name goes back to the real material first
        if ( texOp || op == OP_BLEND )
        {
            bool wrapped = false;
            for ( int k = 0; k < KTER_SLOTS && !wrapped; ++k )
                wrapped = SlotUsed( def, k ) && !_strnicmp( def->kiwiLayer[k], "kiwi_blend_", 11 );
            if ( wrapped )
            {
                MarkTouched( def );
                HealTwinLayers( def );
                NoteDirty( def, false );
            }
        }
        // Texture paint: which slot on THIS patch the brush material means.
        //   -1        = base / erase every layer (needs at least one layer to erase)
        //   existing  = paint (or erase / smooth) that slot
        //   none yet  = painting IN adds the slot on the first point it reaches
        //               (needAdd); erasing or smoothing a layer the patch lacks is a no-op.
        int  slot    = -1;
        bool needAdd = false;
        if ( texOp )
        {
            if ( s_paintBase || !s_paintMaterial[0] || !_stricmp( s_paintMaterial, BaseMaterialName( def ) ) )
            {
                if ( UsedSlotCount( def ) == 0 )
                    return false;
            }
            else
            {
                slot = FindSlotByName( def, s_paintMaterial );
                if ( slot < 0 )
                {
                    if ( op != OP_TEXTURE || sign < 0.0f )
                        return false;
                    slot = FirstFreeSlot( def );
                    if ( slot < 0 )
                    {
                        ++s_layersFull;
                        return false;
                    }
                    needAdd = true;
                }
            }
        }

        // Blend needs layers to blend; it reads the whole grid, like Smooth.
        if ( op == OP_BLEND && UsedSlotCount( def ) == 0 )
            return false;
        const bool neighbour = ( op == OP_SMOOTH || op == OP_BLEND );
        gridSnap_t snap;
        if ( neighbour )
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                {
                    snap.z[i][j] = def->ctrl[i][j].xyz[2];
                    memcpy( snap.c[i][j], &def->ctrl[i][j].vert_color, 4 );
                }

        const float texTarget = ( slot < 0 || sign < 0.0f ) ? 0.0f : s_blendWeight * 255.0f;
        bool changed = false;
        for ( int i = 0; i < def->width; ++i )
        {
            for ( int j = 0; j < def->height; ++j )
            {
                drawVert_t *cp = &def->ctrl[i][j];
                if ( ( cp->turned_edge & 2 ) != 0 )
                    continue;
                const float w = Falloff( BrushDistance( center, cp->xyz ) ) * s_strength;
                if ( w <= 0.0f )
                    continue;
                // Weights and colours respond three times faster than heights: a
                // paint stroke should reach its ceiling in a fraction of a second.
                const float lt = LerpStep( ( texOp || op == OP_BLEND ) ? w * 3.0f : w, dt );
                const float at = w * dt * AdditiveRate();

                // EVERY height op (2026-09-18: Raise / Dig / Noise / Smooth leaked past the
                // ring exactly like Set height did): a point moves only if all the
                // triangles it tilts lie inside the ring.  RefineUnderRing keeps the grid
                // under the ring fine enough for that to leave plenty of points.
                const bool heightOp = op == OP_RAISE || op == OP_SETHEIGHT || op == OP_NOISE
                                   || ( op == OP_SMOOTH && s_tool != KTER_TEXTURE && s_tool != KTER_BLEND );
                if ( heightOp && s_setHeightContain && !s_containBypass )
                {
                    if ( !SetHeightFits( def, i, j, center ) )
                    {
                        ++s_fitShut;                 // its cells stick out of the ring: leave it alone
                        continue;
                    }
                    ++s_fitSet;
                }

                MarkTouched( def );
                changed = true;
                if ( heightOp )
                    s_movedPts.insert( cp );     // the weld lets a moved copy win over an unmoved one
                byte *col = (byte *)&cp->vert_color;
                switch ( op )
                {
                case OP_RAISE:
                    cp->xyz[2] += sign * at;
                    break;
                case OP_SETHEIGHT:
                {
                    // KIWI (2026-09-17, user: "It should only affect the area in the circle
                    // and it should be an instant height change ... an area i've already
                    // flattened is being risen up"): this used to LERP toward the target by
                    // falloff x strength, re-applied on every mouse move.  So the core only
                    // reached the target at strength 1, and the whole band between the inner
                    // and outer ring was dragged part of the way - which is what lifted
                    // ground next to the spot being set, including ground already flattened
                    // to a different height.  Now every control point inside the OUTER ring
                    // is set to exactly the target, at once, and nothing outside it is
                    // touched; strength and falloff do not apply.  "Feather the edge" brings
                    // the old ramp back for blending a plateau into a slope.
                    if ( s_setHeightFeather )
                    {
                        const float f = w > 1.0f ? 1.0f : w;
                        cp->xyz[2] += ( s_targetZ - cp->xyz[2] ) * f;
                    }
                    else
                        cp->xyz[2] = s_targetZ;
                    if ( ( s_setHeightFeather || !s_setHeightContain ) && CellSizeOf( def ) > s_outer )
                        s_coarseSpill = true;    // one moved point drags cells wider than the brush
                    break;
                }
                case OP_NOISE:
                {
                    const float n = ValueNoise2( cp->xyz[0] * s_noiseFreq, cp->xyz[1] * s_noiseFreq,
                                                 (int)s_noiseSeed );
                    cp->xyz[2] += sign * n * at * ( s_noiseScale / 64.0f );
                    break;
                }
                case OP_SMOOTH:
                {
                    float sumZ = 0.0f, sumC[4] = { 0, 0, 0, 0 };
                    int   n = 0;
                    for ( int di = -1; di <= 1; ++di )
                        for ( int dj = -1; dj <= 1; ++dj )
                        {
                            const int ni = i + di, nj = j + dj;
                            if ( ni < 0 || nj < 0 || ni >= def->width || nj >= def->height )
                                continue;
                            sumZ += snap.z[ni][nj];
                            for ( int k = 0; k < 4; ++k )
                                sumC[k] += (float)snap.c[ni][nj][k];
                            ++n;
                        }
                    if ( n == 0 )
                        break;
                    const float inv = 1.0f / (float)n;
                    if ( s_tool == KTER_TEXTURE )
                    {
                        if ( slot >= 0 )
                            col[slot] = LerpByte( col[slot], sumC[slot] * inv, lt );
                        else
                            for ( int k = 0; k < KTER_SLOTS; ++k )
                                if ( SlotUsed( def, k ) )
                                    col[k] = LerpByte( col[k], sumC[k] * inv, lt );
                    }
                    else
                        cp->xyz[2] += ( sumZ * inv - cp->xyz[2] ) * lt;
                    break;
                }
                case OP_TEXTURE:
                    if ( needAdd )
                    {
                        // First point reached: the patch gets the brush material as a
                        // layer (MarkTouched above already took the undo copy).  A
                        // patch with no layers yet has white colour bytes that mean
                        // nothing as weights, so they start from zero.
                        if ( UsedSlotCount( def ) == 0 )
                            for ( int ci = 0; ci < def->width; ++ci )
                                for ( int cj = 0; cj < def->height; ++cj )
                                    *(unsigned int *)&def->ctrl[ci][cj].vert_color = 0u;
                        strncpy( def->kiwiLayer[slot], s_paintMaterial, 63 );
                        def->kiwiLayer[slot][63] = '\0';
                        needAdd = false;
                        ++s_layersAdded;
                    }
                    if ( slot >= 0 )
                        col[slot] = LerpByte( col[slot], texTarget, lt );
                    else
                        for ( int k = 0; k < KTER_SLOTS; ++k )     // base: erase the layers
                            if ( SlotUsed( def, k ) )
                                col[k] = LerpByte( col[k], 0.0f, lt );
                    break;
                case OP_BLEND:
                {
                    // Every used layer's weight moves toward the mean over a
                    // (2R+1)^2 neighbourhood of the pre-stamp grid, so the per-point
                    // steps the coarse grid leaves between two textures fade into a
                    // gradient.  The seam pass (StitchWeights) carries it across patches.
                    const int R = s_blendRings;
                    float sumC[4] = { 0, 0, 0, 0 };
                    int   n = 0;
                    for ( int di = -R; di <= R; ++di )
                        for ( int dj = -R; dj <= R; ++dj )
                        {
                            const int ni = i + di, nj = j + dj;
                            if ( ni < 0 || nj < 0 || ni >= def->width || nj >= def->height )
                                continue;
                            for ( int k = 0; k < 4; ++k )
                                sumC[k] += (float)snap.c[ni][nj][k];
                            ++n;
                        }
                    if ( n == 0 )
                        break;
                    const float inv = 1.0f / (float)n;
                    for ( int k = 0; k < KTER_SLOTS; ++k )
                        if ( SlotUsed( def, k ) )
                            col[k] = LerpByte( col[k], sumC[k] * inv, lt );
                    break;
                }
                }
            }
        }
        if ( changed )
            NoteDirty( def, op == OP_RAISE || op == OP_SETHEIGHT || op == OP_NOISE
                            || ( op == OP_SMOOTH && s_tool != KTER_TEXTURE && s_tool != KTER_BLEND ) );
        return changed;
    }

    // Texture paint on BRUSHES: every upward face (normal z > 0.5) of a visible,
    // unfiltered brush whose winding centre lies inside the brush ring takes the
    // paint material whole (a face has no per-vertex weights).  Ctrl (erase) and
    // "Erase to base" do nothing here.  Each brush is Undo_AddBrush'ed on first touch
    // inside the stroke's record, so Ctrl+Z restores its faces with the terrain.
    int PaintBrushFaces( const float *center )
    {
        if ( !s_paintBrushes || s_paintBase || !s_paintMaterial[0] )
            return 0;
        int painted = 0;
        const float r = s_shape == KTER_SQUARE ? s_outer * 1.42f : s_outer;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                brush_t *def = b->def;
                if ( !def || def->patch || !def->faces || FilterBrush( b, 0 ) || ( b->brushFlags & 0x20 ) != 0 )
                    continue;
                if ( center[0] + r < def->mins[0] || center[0] - r > def->maxs[0]
                  || center[1] + r < def->mins[1] || center[1] - r > def->maxs[1] )
                    continue;
                bool touched = false;
                for ( int f = 0; f < def->faceCount; ++f )
                {
                    face_t *face = &def->faces[f];
                    const winding_t *w = face->w;
                    if ( !w || w->numpoints < 3 || face->plane.normal[2] < 0.5f )
                        continue;
                    float c[3] = { 0.0f, 0.0f, 0.0f };
                    for ( int i = 0; i < w->numpoints; ++i )
                        for ( int k = 0; k < 3; ++k )
                            c[k] += w->p[i][k];
                    for ( int k = 0; k < 3; ++k )
                        c[k] /= (float)w->numpoints;
                    if ( BrushDistance( center, c ) > s_outer )
                        continue;
                    const qtexture_s *cur = face->mtldef[0].radMtl;
                    if ( cur && cur->name && !_stricmp( cur->name, s_paintMaterial ) )
                        continue;                              // already wears it
                    if ( !touched )
                    {
                        Undo_AddBrush( (entity_brush_s *)def );  // skipped if already in this record
                        touched = true;
                    }
                    SetMaterial( s_paintMaterial, (patchMesh_material *)&face->mtldef[0] );
                    KiwiMtl_RealizeFace( face );
                    ++painted;
                }
                if ( touched )
                {
                    ++def->version;
                    Brush_BuildWindings( def, 0 );
                    sub_47B940( def );
                    MarkMapModified();
                }
            }
        }
        return painted;
    }

    // KIWI (2026-09-18, user: "when im blending, I can't blend areas that dont have the paint
    // already ... I can fix this manually by dabbing a bit of paint on that area and then
    // blending, but that's a hassle").  Blend skipped every patch without layers and only
    // averaged inside each patch's own grid, so paint stopped dead at the border of a patch
    // that did not carry that material: the sharp axis-aligned lines.  The manual dab worked
    // because it ADDS the layer slot.  Blend now does that itself: every layer that has
    // weight inside the ring on one target patch is given (at weight zero) to every other
    // target patch the ring touches that lacks it - same slot index when that is free, else
    // the first free one; StitchWeights matches layers by NAME, so either works.  The seam
    // pass then carries the weight across the border and the blend diffuses it inward.
    void ShareLayersUnderRing( const float *center )
    {
        const float r = s_shape == KTER_SQUARE ? s_outer * 1.42f : s_outer;
        std::vector<selbrush_t *> under;
        for ( size_t t = 0; t < s_targets.size(); ++t )
        {
            const float *mins = s_targets[t]->def->mins, *maxs = s_targets[t]->def->maxs;
            if ( center[0] + r < mins[0] || center[0] - r > maxs[0]
              || center[1] + r < mins[1] || center[1] - r > maxs[1] )
                continue;
            under.push_back( s_targets[t] );
        }
        if ( under.size() < 2 )
            return;
        for ( size_t a = 0; a < under.size(); ++a )
        {
            const patchMesh_t *A = under[a]->patch->def;
            for ( int k = 0; k < KTER_SLOTS; ++k )
            {
                if ( !SlotUsed( A, k ) )
                    continue;
                // only a layer that really has paint under the ring is worth a draw run elsewhere
                bool painted = false;
                for ( int i = 0; i < A->width && !painted; ++i )
                    for ( int j = 0; j < A->height && !painted; ++j )
                        painted = ( (const byte *)&A->ctrl[i][j].vert_color )[k] != 0
                               && BrushDistance( center, A->ctrl[i][j].xyz ) <= s_outer;
                if ( !painted )
                    continue;
                for ( size_t b = 0; b < under.size(); ++b )
                {
                    patchMesh_t *B = under[b]->patch->def;
                    if ( b == a || FindSlotByName( B, A->kiwiLayer[k] ) >= 0 )
                        continue;
                    const int slot = SlotUsed( B, k ) ? FirstFreeSlot( B ) : k;
                    if ( slot < 0 )
                    {
                        ++s_layersFull;                  // four layers already: reported at stroke end
                        continue;
                    }
                    MarkTouched( B );                    // undo copy before the first change
                    const bool first = UsedSlotCount( B ) == 0;
                    for ( int i = 0; i < B->width; ++i )
                        for ( int j = 0; j < B->height; ++j )
                        {
                            if ( first )                 // white bytes mean nothing as weights
                                *(unsigned int *)&B->ctrl[i][j].vert_color = 0u;
                            else
                                ( (byte *)&B->ctrl[i][j].vert_color )[slot] = 0;
                        }
                    strncpy( B->kiwiLayer[slot], A->kiwiLayer[k], 63 );
                    B->kiwiLayer[slot][63] = '\0';
                    ++s_layersAdded;
                    NoteDirty( B, false );
                }
            }
        }
    }

    void Stamp( const float *center, kterOp_t op, float sign, float dt )
    {
        if ( dt <= 0.0f )
            return;
        // Blend, and the Texture tool's Shift-smooth (same "nothing to smooth here" wall)
        if ( op == OP_BLEND || ( op == OP_SMOOTH && s_tool == KTER_TEXTURE ) )
            ShareLayersUnderRing( center );
        bool any = false;
        s_fitSet = s_fitShut = 0;
        s_containBypass = false;
        for ( size_t i = 0; i < s_targets.size(); ++i )
            any |= StampPatch( s_targets[i], op, center, sign, dt );
        // "Never change anything outside the ring" on a grid too coarse for this ring: NO
        // point has all its triangles inside it, and the brush would simply be dead.  With
        // automatic re-gridding now opt-in that is the normal case on coarse terrain, so
        // this stamp falls back to moving the points inside the ring (their cells then lean
        // up to one cell past it - the patch mesh cannot do better without more triangles)
        // and the status line says how to confine it.
        if ( s_fitSet == 0 && s_fitShut > 0 )
        {
            s_containBypass = true;
            for ( size_t i = 0; i < s_targets.size(); ++i )
                any |= StampPatch( s_targets[i], op, center, sign, dt );
            s_containBypass = false;
            s_coarseSpill = true;
        }
        if ( op == OP_TEXTURE && sign > 0.0f )
        {
            const int faces = PaintBrushFaces( center );
            if ( faces > 0 )
            {
                s_facesPainted += faces;
                any = true;
            }
        }
        if ( any )
        {
            ++s_stamps;
            g_nUpdateBits |= W_CAMERA;
        }
    }

    kterOp_t OpForStroke()
    {
        if ( s_modShift )
            return OP_SMOOTH;
        switch ( s_tool )
        {
        case KTER_SETHEIGHT: return OP_SETHEIGHT;
        case KTER_SMOOTH:    return OP_SMOOTH;
        case KTER_NOISE:     return OP_NOISE;
        case KTER_TEXTURE:   return OP_TEXTURE;
        case KTER_BLEND:     return OP_BLEND;
        default:             return OP_RAISE;
        }
    }

    // ── Carry objects (KIWI 2026-09-16, user: "raising / lowering / modifying the
    // terrain in any way will also move the models up or down according to the terrain
    // changes ... everything, not just models (brushes too), disabled by default") ────
    //
    // A "rider" is any visible, unfiltered brush, patch or entity that is not itself a
    // terrain patch and whose bottom sits within KTER_CARRY_TOL of the stroke's terrain
    // under its footprint centre when the stroke begins. Each flush re-samples the
    // terrain there and moves the rider by (terrain now - terrain at the start), applied
    // as a delta from what was already applied, so a long stroke never accumulates
    // error. Brush_Move carries faces, patch control points and entity origins alike,
    // and every rider is Undo_AddBrush'ed once inside the stroke's own record so Ctrl+Z
    // puts the props back together with the terrain.
    const float KTER_CARRY_TOL = 48.0f;

    struct kterRider_t
    {
        selbrush_t *node;
        brush_t    *def;
        float       x, y;           // footprint centre
        float       baseTerrainZ;   // terrain under it at the stroke start
        float       applied;        // z already applied this stroke
        bool        undoAdded;
        bool        reached;        // a stamp's ring covered its footprint centre (else it never moves)
    };
    std::vector<kterRider_t> s_riders;

    // Carry objects: only what a ring actually reached may move (2026-09-18: fence panels
    // OUTSIDE the ring rode along on terrain spill).  Called for every stamp centre.
    void MarkRidersReached( const float *center )
    {
        for ( size_t i = 0; i < s_riders.size(); ++i )
            if ( !s_riders[i].reached )
            {
                const float p[3] = { s_riders[i].x, s_riders[i].y, 0.0f };
                s_riders[i].reached = BrushDistance( center, p ) <= s_outer;
            }
    }

    bool HeightStroke()
    {
        const kterOp_t op = OpForStroke();
        return op == OP_RAISE || op == OP_SETHEIGHT || op == OP_NOISE
            || ( op == OP_SMOOTH && s_tool != KTER_TEXTURE && s_tool != KTER_BLEND );
    }

    // Terrain height under (x, y) across the stroke's target patches: the highest
    // sheet that covers the point (irregular grids cannot be sampled and are skipped).
    bool TerrainZAt( float x, float y, float *outZ )
    {
        bool  have = false;
        float best = 0.0f;
        for ( size_t t = 0; t < s_targets.size(); ++t )
        {
            selbrush_t *b = s_targets[t];
            if ( !b || !b->def || !b->patch || !b->patch->def )
                continue;
            const float *mins = b->def->mins, *maxs = b->def->maxs;
            if ( x < mins[0] - 0.5f || x > maxs[0] + 0.5f || y < mins[1] - 0.5f || y > maxs[1] + 0.5f )
                continue;
            const patchMesh_t *def = b->patch->def;
            if ( !GridIsSheet( def ) )
                continue;
            float z;
            byte  c[4];
            SampleGrid( def, x, y, &z, c );
            if ( !have || z > best )
            {
                best = z;
                have = true;
            }
        }
        if ( have )
            *outZ = best;
        return have;
    }

    void CaptureRiders()
    {
        s_riders.clear();
        if ( !s_carryObjects || !HeightStroke() )
            return;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                brush_t *def = b->def;
                if ( !def || FilterBrush( b, 0 ) || ( b->brushFlags & 0x20 ) != 0 )
                    continue;
                if ( b->patch && PatchEligible( b ) )
                    continue;                           // terrain is what moves, never a rider
                const float x = 0.5f * ( def->mins[0] + def->maxs[0] );
                const float y = 0.5f * ( def->mins[1] + def->maxs[1] );
                float z;
                if ( !TerrainZAt( x, y, &z ) )
                    continue;
                if ( fabsf( def->mins[2] - z ) > KTER_CARRY_TOL )
                    continue;                           // floating or buried: leave it
                kterRider_t r;
                r.node = b;
                r.def  = def;
                r.x = x;
                r.y = y;
                r.baseTerrainZ = z;
                r.applied  = 0.0f;
                r.undoAdded = false;
                r.reached  = false;
                s_riders.push_back( r );
            }
        }
    }

    int CarryRiders()
    {
        int moved = 0;
        for ( size_t i = 0; i < s_riders.size(); ++i )
        {
            kterRider_t &r = s_riders[i];
            if ( !r.reached )
                continue;                               // no ring covered it: it stays put
            if ( !r.node || r.node->def != r.def )
                continue;                               // went away mid-stroke
            float z;
            if ( !TerrainZAt( r.x, r.y, &z ) )
                continue;
            const float want = z - r.baseTerrainZ;
            const float step = want - r.applied;
            if ( fabsf( step ) < 0.01f )
                continue;
            if ( !r.undoAdded )
            {
                // KIWI (2026-09-17, user: "when objects are carried up by terrain changes,
                // it's not undo-able. FIX THIS!"): Brush_Move on an entity's brush also moves
                // the ENTITY (a model's origin lives on its def, and the model draws from
                // it).  Saving only the brush meant Undo_Undo put the old bounding box back
                // under an entity that still sat at the raised origin - the prop stayed up.
                // An entity-owned rider is recorded the way Edit>Delete records one:
                // Undo_AddEntity_W clones the def (origin + epairs) and every brush of it, so
                // phase 2/3 swap the whole entity back.  Worldspawn brushes and patches need
                // only themselves.  Both calls skip what the record already holds.
                entity_s *ownerDef = r.def->owner;
                if ( ownerDef && world_entity && ownerDef != (entity_s *)world_entity->def )
                    Undo_AddEntity_W( ownerDef );
                else
                    Undo_AddBrush( (entity_brush_s *)r.def );
                r.undoAdded = true;
            }
            const float move[3] = { 0.0f, 0.0f, step };
            Brush_Move( move, r.def, 0 );
            r.applied = want;
            ++moved;
        }
        if ( moved )
            g_nUpdateBits = -1;
        return moved;
    }

    void BuildTargets()
    {
        s_targets.clear();
        for ( int pass = 0; pass < 2; ++pass )
        {
            if ( pass == 1 && !s_affectUnselected && !PaintAnywhere() )
                break;
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( PatchEligible( b ) )
                    s_targets.push_back( b );
        }
    }

    bool AnyTargetPatch()
    {
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( PatchEligible( b ) )
                return true;
        if ( s_affectUnselected || PaintAnywhere() )
            for ( selbrush_t *b = active_brushes.next; b && b != &active_brushes; b = b->next )
                if ( PatchEligible( b ) )
                    return true;
        return false;
    }

    // ── chunks (split / expander / trim) ─────────────────────────────────────
    void ClearCursor();

    void SampleGrid( const patchMesh_t *src, float x, float y, float *outZ, byte outColor[4] )
    {
        const float *p00 = src->ctrl[0][0].xyz;
        const float *p10 = src->ctrl[src->width - 1][0].xyz;
        const float *p01 = src->ctrl[0][src->height - 1].xyz;
        const float ex = p10[0] - p00[0], ey = p01[1] - p00[1];
        float u = ex != 0.0f ? ( x - p00[0] ) / ex * (float)( src->width - 1 ) : 0.0f;
        float v = ey != 0.0f ? ( y - p00[1] ) / ey * (float)( src->height - 1 ) : 0.0f;
        u = ClampF( u, 0.0f, (float)( src->width - 1 ) );
        v = ClampF( v, 0.0f, (float)( src->height - 1 ) );
        int i0 = (int)u, j0 = (int)v;
        if ( i0 >= src->width - 1 )  i0 = src->width - 2 < 0 ? 0 : src->width - 2;
        if ( j0 >= src->height - 1 ) j0 = src->height - 2 < 0 ? 0 : src->height - 2;
        const int i1 = i0 + 1 < src->width ? i0 + 1 : i0;
        const int j1 = j0 + 1 < src->height ? j0 + 1 : j0;
        const float fu = u - (float)i0, fv = v - (float)j0;
        const drawVert_t &a = src->ctrl[i0][j0], &b = src->ctrl[i1][j0];
        const drawVert_t &c = src->ctrl[i0][j1], &d = src->ctrl[i1][j1];
        *outZ = ( a.xyz[2] * ( 1 - fu ) + b.xyz[2] * fu ) * ( 1 - fv )
              + ( c.xyz[2] * ( 1 - fu ) + d.xyz[2] * fu ) * fv;
        for ( int k = 0; k < 4; ++k )
        {
            const float ca = ( (const byte *)&a.vert_color )[k], cb = ( (const byte *)&b.vert_color )[k];
            const float cc = ( (const byte *)&c.vert_color )[k], cd = ( (const byte *)&d.vert_color )[k];
            const float m = ( ca * ( 1 - fu ) + cb * fu ) * ( 1 - fv ) + ( cc * ( 1 - fu ) + cd * fu ) * fv;
            outColor[k] = (byte)(int)( ClampF( m, 0.0f, 255.0f ) + 0.5f );
        }
    }

    // World size of one grid cell of a sheet patch along X (axis 0) or Y (axis 1).
    float CellSizeAxis( const patchMesh_t *def, int axis )
    {
        const int n = axis == 0 ? def->width : def->height;
        if ( n < 2 )
            return 64.0f;
        const float *a = def->ctrl[0][0].xyz;
        const float *b = axis == 0 ? def->ctrl[def->width - 1][0].xyz : def->ctrl[0][def->height - 1].xyz;
        const float c = fabsf( b[axis] - a[axis] ) / (float)( n - 1 );
        return c > 1.0f ? c : 64.0f;
    }

    float CellSizeOf( const patchMesh_t *def )
    {
        return CellSizeAxis( def, 0 );
    }

    // Grid points per side that give a `size`-wide chunk the cell size `cell` (2..16).
    int PointsFor( float size, float cell )
    {
        int n = (int)( size / cell + 0.5f ) + 1;
        if ( n < 2 )  n = 2;
        if ( n > 16 ) n = 16;
        return n;
    }

    // A square TERRAIN chunk of `points` x `points` modelled on `like` (materials,
    // flags, layer slots).
    selbrush_t *CreateChunkXY( const patchMesh_t *like, entity_s *owner,
                               float minx, float miny, float sizeX, float sizeY,
                               int pointsX, int pointsY,
                               const patchMesh_t *sampleFrom, float flatZ )
    {
        if ( pointsX < 2 )  pointsX = 2;
        if ( pointsX > 16 ) pointsX = 16;
        if ( pointsY < 2 )  pointsY = 2;
        if ( pointsY > 16 ) pointsY = 16;
        patchMesh_t *p = MakeNewPatch();
        p->width  = pointsX;
        p->height = pointsY;
        p->type       = (PATCH_TYPES)( like->type | PATCH_TERRAIN );   // terrain mesh, never bezier
        p->contents   = like->contents;
        p->flags      = like->flags;
        p->subDivType = like->subDivType;
        p->texture    = like->texture;
        p->lightmap   = like->lightmap;
        p->smoothing  = like->smoothing;
        memcpy( p->kiwiLayer, like->kiwiLayer, sizeof( p->kiwiLayer ) );
        const unsigned int flatColor = UsedSlotCount( like ) ? 0u : 0xFFFFFFFFu;
        const float stepX = sizeX / (float)( pointsX - 1 );
        const float stepY = sizeY / (float)( pointsY - 1 );
        for ( int i = 0; i < pointsX; ++i )
            for ( int j = 0; j < pointsY; ++j )
            {
                drawVert_t *cp = &p->ctrl[i][j];
                cp->xyz[0] = minx + stepX * (float)i;
                cp->xyz[1] = miny + stepY * (float)j;
                byte col[4];
                memcpy( col, &flatColor, 4 );
                float z = flatZ;
                if ( sampleFrom )
                    SampleGrid( sampleFrom, cp->xyz[0], cp->xyz[1], &z, col );
                cp->xyz[2] = z;
                memcpy( &cp->vert_color, col, 4 );
            }
        Patch_KiwiTextureAndBuild( p, g_qeglobals.random_texture_stuff[0].sampleSize );
        brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
        selbrush_t *inst = Brush_AddToList( pdef, owner );
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
        return inst;
    }

    selbrush_t *CreateChunk( const patchMesh_t *like, entity_s *owner,
                             float minx, float miny, float size, int points,
                             const patchMesh_t *sampleFrom, float flatZ )
    {
        return CreateChunkXY( like, owner, minx, miny, size, size, points, points, sampleFrom, flatZ );
    }

    void ForgetDef( patchMesh_t *def )
    {
        s_bands.erase( def );
        if ( s_cursorDef == def )                // by key: the node itself may already be freed
        {
            s_cursorNode = nullptr;
            s_cursorDef  = nullptr;
        }
    }

    // Edit->Delete's bracket over the current selection.
    void DeleteSelectionWithUndo( const char *label )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( label );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
        Select_Delete();
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
    }

    void SplitOversized()
    {
        std::vector<selbrush_t *> originals;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            // Regular sheets only: the split rebuilds from the bounding rectangle, which is
            // not the shape of a curved / irregular patch (chunks would land on neighbours).
            if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                continue;
            const float *mins = b->def->mins, *maxs = b->def->maxs;
            if ( maxs[0] - mins[0] <= s_chunkSize + 0.5f && maxs[1] - mins[1] <= s_chunkSize + 0.5f )
                continue;
            originals.push_back( b );
        }
        if ( originals.empty() )
        {
            Sys_Printf( "Terrain Sculpt: no selected terrain exceeds %.0f units.\n", s_chunkSize );
            return;
        }
        Select_Deselect( 1 );
        Undo_ClearRedo();
        Undo_GeneralStart( "split terrain" );
        for ( size_t i = 0; i < originals.size(); ++i )
            Select_Brush( originals[i], 0, 0, 0 );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
        std::vector<selbrush_t *> created;
        int made = 0;
        for ( size_t bi = 0; bi < originals.size(); ++bi )
        {
            selbrush_t *src = originals[bi];
            const float *mins = src->def->mins, *maxs = src->def->maxs;
            const float ex = maxs[0] - mins[0], ey = maxs[1] - mins[1];
            const int nx = (int)ceilf( ex / s_chunkSize ), ny = (int)ceilf( ey / s_chunkSize );
            const float sx = ex / (float)nx, sy = ey / (float)ny;
            const float size = sx < sy ? sx : sy;
            for ( int cy = 0; cy < ny; ++cy )
                for ( int cx = 0; cx < nx; ++cx )
                {
                    created.push_back( CreateChunk( src->patch->def, src->owner,
                                                    mins[0] + sx * (float)cx, mins[1] + sy * (float)cy,
                                                    size, PointsFor( size, CellSizeOf( src->patch->def ) ),
                                                    src->patch->def, 0.0f ) );
                    ++made;
                }
        }
        for ( size_t i = 0; i < originals.size(); ++i )
            ForgetDef( originals[i]->patch->def );
        Select_Delete();
        Undo_EndBrushList( &selected_brushes );
        for ( size_t i = 0; i < created.size(); ++i )
            Undo_KiwiMarkCreated( created[i]->def );   // same omission as Tessellate (2026-09-17)
        Undo_End();
        for ( size_t i = 0; i < created.size(); ++i )
            Select_Brush( created[i], 0, 0, 0 );
        s_targets.clear();
        g_nUpdateBits = -1;
        Sys_Printf( "Terrain Sculpt: split %i patch%s into %i chunk%s (%.0f).\n",
                    (int)originals.size(), originals.size() == 1 ? "" : "es",
                    made, made == 1 ? "" : "s", s_chunkSize );
    }

    // ── Flatten: flat selected terrain -> one brush each ─────────────────────
    // A terrain patch whose control heights all lie within `s_flatTol` is replaced by
    // a single axis-aligned brush: top at that height, `s_flatThick` deep, the patch's
    // base material on the top face and caulk on the rest.  Flat ground as a brush is
    // 12 triangles and one collision volume instead of up to 450 triangles; the Set
    // height tool makes more patches qualify.  One undo record (deleted patches saved,
    // created brushes stamped); non-flat patches are left alone and counted.
    void FlattenSelectedToBrushes()
    {
        std::vector<selbrush_t *> flat;
        int skipped = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                continue;
            const patchMesh_t *def = b->patch->def;
            float lo = FLT_MAX, hi = -FLT_MAX;
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                {
                    const float z = def->ctrl[i][j].xyz[2];
                    if ( z < lo ) lo = z;
                    if ( z > hi ) hi = z;
                }
            if ( hi - lo <= s_flatTol )
                flat.push_back( b );
            else
                ++skipped;
        }
        if ( flat.empty() )
        {
            Sys_Printf( "Terrain Sculpt: no selected terrain patch is flat within %.1f units (%i checked).\n",
                        s_flatTol, skipped );
            return;
        }
        if ( !world_entity )
            return;

        // Remember what to build before the patches go.
        struct flatRec_t { float mins[3], maxs[3]; char material[64]; };
        std::vector<flatRec_t> recs;
        for ( size_t i = 0; i < flat.size(); ++i )
        {
            const patchMesh_t *def = flat[i]->patch->def;
            flatRec_t r;
            r.mins[0] = flat[i]->def->mins[0]; r.mins[1] = flat[i]->def->mins[1];
            r.maxs[0] = flat[i]->def->maxs[0]; r.maxs[1] = flat[i]->def->maxs[1];
            float z = 0.0f;
            for ( int a = 0; a < def->width; ++a )
                for ( int c = 0; c < def->height; ++c )
                    z += def->ctrl[a][c].xyz[2];
            z /= (float)( def->width * def->height );
            r.maxs[2] = z;
            r.mins[2] = z - ( s_flatThick > 1.0f ? s_flatThick : 1.0f );
            // The top face wears the patch's DOMINANT material: the painted layer with
            // the highest mean weight when it covers at least half the patch, else the
            // base.  A face has no weights, so the paint cannot carry over any finer.
            const char *top = BaseMaterialName( def );
            float bestMean = 127.0f;
            for ( int k = 0; k < KTER_SLOTS; ++k )
            {
                if ( !SlotUsed( def, k ) )
                    continue;
                float sum = 0.0f;
                for ( int a = 0; a < def->width; ++a )
                    for ( int c = 0; c < def->height; ++c )
                        sum += (float)( (const byte *)&def->ctrl[a][c].vert_color )[k];
                const float mean = sum / (float)( def->width * def->height );
                if ( mean > bestMean )
                {
                    bestMean = mean;
                    top = def->kiwiLayer[k];
                }
            }
            strncpy( r.material, top, 63 );
            r.material[63] = '\0';
            recs.push_back( r );
        }

        // Delete the patches inside the record (Edit->Delete's bracket)...
        Select_Deselect( 1 );
        for ( size_t i = 0; i < flat.size(); ++i )
        {
            ForgetDef( flat[i]->patch->def );
            Select_Brush( flat[i], 0, 0, 0 );
        }
        Undo_ClearRedo();
        Undo_GeneralStart( "flatten terrain to brushes" );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
        Select_Delete();

        // ...then lay the brushes, stamped as created so undo frees them.
        Ed_EnsureCurrentMaterial_Kiwi();
        int made = 0;
        for ( size_t i = 0; i < recs.size(); ++i )
        {
            brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
            if ( !def )
                continue;
            Brush_Create( recs[i].mins, recs[i].maxs, def, nullptr );
            Brush_BuildWindings( def, 1 );               // planes first: the top test reads them
            for ( int f = 0; f < def->faceCount; ++f )
            {
                face_t *face = &def->faces[f];
                const bool top = face->plane.normal[2] > 0.9f;
                SetMaterial( top ? recs[i].material : "caulk", (patchMesh_material *)&face->mtldef[0] );
                KiwiMtl_RealizeFace( face );
            }
            Brush_BuildWindings( def, 1 );
            KiwiExtrude_LandDef( def );                  // world entity + selected
            Undo_KiwiMarkCreated( def );
            ++made;
        }
        Undo_End();
        s_targets.clear();
        g_nUpdateBits = -1;
        char kept[64] = "";
        if ( skipped )
            _snprintf( kept, sizeof( kept ), " (%i not flat, kept)", skipped );
        kept[sizeof( kept ) - 1] = '\0';
        Sys_Printf( "Terrain Sculpt: %i flat patch%s became %i brush%s%s.\n",
                    (int)flat.size(), flat.size() == 1 ? "" : "es", made, made == 1 ? "" : "es", kept );
    }

    bool AnyPatchCovers( float x, float y )
    {
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !NodeIsPatch( b ) || FilterBrush( b, 0 ) )
                    continue;
                const float *mins = b->def->mins, *maxs = b->def->maxs;
                if ( x >= mins[0] - 1.0f && x <= maxs[0] + 1.0f && y >= mins[1] - 1.0f && y <= maxs[1] + 1.0f )
                    return true;
            }
        }
        return false;
    }

    // Does any terrain SURFACE lie under the rectangle?  4 x 4 vertical rays, inset a tenth
    // of the cell so a neighbour that merely shares the edge does not count.
    bool CellTouchesTerrain( float minx, float miny, float sx, float sy )
    {
        for ( int a = 0; a < 4; ++a )
            for ( int b = 0; b < 4; ++b )
            {
                const float org[3] = { minx + sx * ( 0.1f + 0.8f * (float)a / 3.0f ),
                                       miny + sy * ( 0.1f + 0.8f * (float)b / 3.0f ), 65536.0f };
                const float dir[3] = { 0.0f, 0.0f, -1.0f };
                float hit[3];
                if ( PickPatches( org, dir, true, hit, nullptr ) )
                    return true;
            }
        return false;
    }

    // The lattice a stroke lays chunks on.  With terrain under the cursor - or, when
    // creation is allowed, within reach of the brush - the chunks CONTINUE that sheet:
    // its corner anchors the lattice, the chunk sides are the nearest multiple of its
    // cell per axis (so every new edge point lands on an existing grid point and the
    // seams share vertices; a rectangular-celled source gets rectangular chunks), and
    // the new patch copies its materials, flags and layer slots.  With nothing in reach
    // the lattice is world-origin aligned at the chunk size, "Cells per new chunk"
    // dense, and the patch wears the texture browser's current material.
    struct kterLattice_t
    {
        const patchMesh_t *like;      // template patch; null = fresh (FreshTemplate)
        entity_s          *owner;
        float ax, ay;                 // lattice anchor
        float SX, SY;                 // chunk sides
        float cellX, cellY;           // grid cell of the chunks
    };

    // XY distance from a point to a node's bounds rectangle (0 inside).
    float BoundsDistanceXY( const selbrush_t *b, const float *p )
    {
        const float *mins = b->def->mins, *maxs = b->def->maxs;
        float dx = 0.0f, dy = 0.0f;
        if ( p[0] < mins[0] ) dx = mins[0] - p[0]; else if ( p[0] > maxs[0] ) dx = p[0] - maxs[0];
        if ( p[1] < mins[1] ) dy = mins[1] - p[1]; else if ( p[1] > maxs[1] ) dy = p[1] - maxs[1];
        return sqrtf( dx * dx + dy * dy );
    }

    // The closest eligible SHEET patch within `reach` of the cursor (XY), so a stroke
    // that starts beside existing terrain continues its lattice instead of starting a
    // new one that would never seam with it.
    selbrush_t *NearestEligiblePatch( const float *p, float reach )
    {
        selbrush_t *best = nullptr;
        float bestD = reach;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                    continue;
                const float d = BoundsDistanceXY( b, p );
                if ( d < bestD )
                {
                    bestD = d;
                    best  = b;
                }
            }
        }
        return best;
    }

    // Template for a chunk laid with no terrain in reach: a terrain mesh wearing the
    // texture browser's current material and lightmap (Create_Terrain's rule, pmesh.cpp
    // 0x43b841), no layers, no contents/tool flags.  Kept as one scratch def.
    const patchMesh_t *FreshTemplate()
    {
        if ( !s_scratchLike )
            s_scratchLike = MakeNewPatch();
        patchMesh_t *p = s_scratchLike;
        p->type       = PATCH_TERRAIN;
        p->contents   = 0;
        p->flags      = 0;
        p->subDivType = 8;
        memset( p->kiwiLayer, 0, sizeof( p->kiwiLayer ) );
        const curTexWndLayer_t *cur = g_qeglobals.random_texture_stuff;
        if ( cur[0].mtl.lyrMtl )
        {
            p->texture.lyrMtl = cur[0].mtl.lyrMtl;
            p->texture.radMtl = cur[0].mtl.radMtl;
        }
        if ( cur[1].mtl.lyrMtl )
        {
            p->lightmap.lyrMtl = cur[1].mtl.lyrMtl;
            p->lightmap.radMtl = cur[1].mtl.radMtl;
        }
        return p;
    }

    bool ResolveLattice( kterLattice_t *L )
    {
        if ( !s_cursorHave )
            return false;
        selbrush_t *node = LiveCursorNode();
        if ( !node && CreationAllowed() )
            node = NearestEligiblePatch( s_cursor, s_outer + s_chunkSize );
        // KIWI (2026-09-18): the lattice is anchored on a patch's mins and copies its cell
        // size, and RefineUnderRing now cuts the sheet under the cursor into small fine
        // PIECES whose mins sit anywhere inside the old chunk.  Anchor on the LARGEST sheet
        // in reach instead - an uncut chunk, whose mins still is a lattice point and whose
        // cells are the terrain's real cell size - so new chunks keep lining up.  (Until
        // this, the re-grid was simply switched off while "Allow terrain creation" was
        // ticked, which is how the user always sculpts.)
        if ( node )
        {
            const float reach = s_outer + s_chunkSize;
            float bestArea = ( node->def->maxs[0] - node->def->mins[0] ) * ( node->def->maxs[1] - node->def->mins[1] );
            for ( int pass = 0; pass < 2; ++pass )
            {
                selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
                for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                {
                    if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                        continue;
                    if ( BoundsDistanceXY( b, s_cursor ) > reach )
                        continue;
                    const float area = ( b->def->maxs[0] - b->def->mins[0] ) * ( b->def->maxs[1] - b->def->mins[1] );
                    if ( area > bestArea * 1.01f )
                    {
                        bestArea = area;
                        node = b;
                    }
                }
            }
        }
        if ( node )
        {
            L->like  = node->patch->def;
            L->owner = node->owner;
            L->cellX = CellSizeAxis( L->like, 0 );
            L->cellY = CellSizeAxis( L->like, 1 );
            L->SX = floorf( s_chunkSize / L->cellX + 0.5f ) * L->cellX;
            L->SY = floorf( s_chunkSize / L->cellY + 0.5f ) * L->cellY;
            if ( L->SX < L->cellX ) L->SX = L->cellX;
            if ( L->SY < L->cellY ) L->SY = L->cellY;
            const float *bm = node->def->mins;
            L->ax = bm[0] - floorf( bm[0] / L->SX ) * L->SX;
            L->ay = bm[1] - floorf( bm[1] / L->SY ) * L->SY;
            return true;
        }
        if ( !CreationAllowed() || !world_entity )
            return false;
        int cells = s_createCells;
        if ( cells < 1 )  cells = 1;
        if ( cells > 15 ) cells = 15;
        L->like  = nullptr;
        L->owner = world_entity;
        L->SX = L->SY = s_chunkSize;
        L->cellX = L->cellY = s_chunkSize / (float)cells;
        L->ax = L->ay = 0.0f;
        return true;
    }

    int EmptyCellsUnderBrush( float cells[64][5], const kterLattice_t &L )
    {
        if ( !s_cursorHave )
            return 0;
        const float SX = L.SX, SY = L.SY, ax = L.ax, ay = L.ay;
        const float r = s_outer;
        const int cx0 = (int)floorf( ( s_cursor[0] - r - ax ) / SX ), cx1 = (int)floorf( ( s_cursor[0] + r - ax ) / SX );
        const int cy0 = (int)floorf( ( s_cursor[1] - r - ay ) / SY ), cy1 = (int)floorf( ( s_cursor[1] + r - ay ) / SY );
        int n = 0;
        for ( int cy = cy0; cy <= cy1 && n < 64; ++cy )
            for ( int cx = cx0; cx <= cx1 && n < 64; ++cx )
            {
                const float minx = ax + SX * (float)cx, miny = ay + SY * (float)cy;
                const float mx = minx + SX * 0.5f, my = miny + SY * 0.5f;
                // Any part of the brush over the cell counts: test the brush against the
                // cell's nearest point, not its centre.
                float near2[3] = { s_cursor[0], s_cursor[1], 0.0f };
                if ( near2[0] < minx ) near2[0] = minx; else if ( near2[0] > minx + SX ) near2[0] = minx + SX;
                if ( near2[1] < miny ) near2[1] = miny; else if ( near2[1] > miny + SY ) near2[1] = miny + SY;
                if ( BrushDistance( s_cursor, near2 ) > r )
                    continue;
                // KIWI (2026-09-18, user: "the terrain tool creating redundant terrain patches
                // on top of each other"): this used to test the cell's CENTRE against patch
                // BOUNDING BOXES.  Beside irregular terrain that fails both ways - a cell whose
                // centre sits in a gap got a whole chunk laid over its neighbours, and a real
                // gap whose centre lies inside a curved neighbour's box could never be filled.
                // A cell is empty only if NO terrain surface lies under any of a 4 x 4 spread
                // of points inside it (real ray tests); a partly covered cell is left to the
                // gap filler (FillHoleUnderCursor), which follows the hole's own outline.
                if ( CellTouchesTerrain( minx, miny, SX, SY ) )
                    continue;
                float zSum = 0.0f; int zN = 0;
                const float probes[4][2] = { { minx - 8.0f, my }, { minx + SX + 8.0f, my },
                                             { mx, miny - 8.0f }, { mx, miny + SY + 8.0f } };
                for ( int k = 0; k < 4; ++k )
                {
                    const float org[3] = { probes[k][0], probes[k][1], s_cursor[2] + 16384.0f };
                    const float dir[3] = { 0.0f, 0.0f, -1.0f };
                    float hit[3];
                    if ( PickPatches( org, dir, true, hit, nullptr ) ) { zSum += hit[2]; ++zN; }
                }
                cells[n][0] = minx; cells[n][1] = miny; cells[n][2] = SX; cells[n][3] = SY;
                cells[n][4] = zN ? zSum / (float)zN : s_cursor[2];
                ++n;
            }
        return n;
    }

    // Hover preview: the cells the next stroke would fill.
    int PreviewCells()
    {
        kterLattice_t L;
        if ( !CreationAllowed() || !ResolveLattice( &L ) )
            return 0;
        return EmptyCellsUnderBrush( s_expandCells, L );
    }

    // ── fill a hole (KIWI 2026-09-18, user: "I can't fill this gap. Make it so I can fill
    // this gap with the terrain creation option on the dig/raise tool") ──────────────────
    // The chunk lattice is axis-aligned squares; a gap between curved / irregular patches is
    // not, so no lattice cell can ever fill it without lying on a neighbour.  With the cursor
    // over NO terrain, Raise + "Allow terrain creation" first looks for a HOLE around it:
    //   1. every border segment of the terrain in reach that no other patch shares (its
    //      midpoint is on nobody else's border and has no other terrain under it) is OPEN;
    //   2. open segments are chained end to end from the one nearest the cursor; a chain that
    //      closes around the cursor is the hole's outline;
    //   3. the four sharpest turns are its corners, giving four sides.  Opposite sides get the
    //      same point count by splitting the longest segments of the shorter one - so EVERY
    //      neighbour vertex stays a fill vertex and added points lie on a neighbour's straight
    //      edge: the seam cannot crack;
    //   4. the inside is a Coons blend of the four sides (positions, heights and layer
    //      weights), cut into patches of at most 16 x 16 points.
    struct kterHoleEdge_t
    {
        float       a[7], b[7];         // x y z + the four colour bytes
        selbrush_t *node;
        bool        used;
    };

    void HoleVert( const drawVert_t &v, float out[7] )
    {
        out[0] = v.xyz[0]; out[1] = v.xyz[1]; out[2] = v.xyz[2];
        const byte *c = (const byte *)&v.vert_color;
        for ( int k = 0; k < 4; ++k )
            out[3 + k] = (float)c[k];
    }

    float PointSegDist2XY( const float *p, const float *a, const float *b )
    {
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        const float len2 = dx * dx + dy * dy;
        float t = len2 > 1e-6f ? ( ( p[0] - a[0] ) * dx + ( p[1] - a[1] ) * dy ) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
        const float ex = p[0] - ( a[0] + dx * t ), ey = p[1] - ( a[1] + dy * t );
        return ex * ex + ey * ey;
    }

    // Bring a side up to `count` points by splitting its longest segment until it fits.
    void HoleUpsample( std::vector<float> &side, int count )
    {
        while ( (int)( side.size() / 7 ) < count )
        {
            const int n = (int)( side.size() / 7 );
            int   best = 0;
            float bestLen = -1.0f;
            for ( int k = 0; k + 1 < n; ++k )
            {
                const float *a = &side[k * 7], *b = &side[( k + 1 ) * 7];
                const float l = ( b[0] - a[0] ) * ( b[0] - a[0] ) + ( b[1] - a[1] ) * ( b[1] - a[1] );
                if ( l > bestLen ) { bestLen = l; best = k; }
            }
            float mid[7];
            for ( int c = 0; c < 7; ++c )
                mid[c] = ( side[best * 7 + c] + side[( best + 1 ) * 7 + c] ) * 0.5f;
            side.insert( side.begin() + ( best + 1 ) * 7, mid, mid + 7 );
        }
    }

    selbrush_t *CreatePatchFromGrid( const patchMesh_t *like, entity_s *owner, const std::vector<float> &grid,
                                     int W, int i0, int i1, int j0, int j1 )
    {
        const int nx = i1 - i0 + 1, ny = j1 - j0 + 1;
        if ( nx < 2 || ny < 2 || nx > 16 || ny > 16 )
            return nullptr;
        patchMesh_t *p = MakeNewPatch();
        p->width  = nx;
        p->height = ny;
        p->type       = (PATCH_TYPES)( like->type | PATCH_TERRAIN );
        p->contents   = like->contents;
        p->flags      = like->flags;
        p->subDivType = like->subDivType;
        p->texture    = like->texture;
        p->lightmap   = like->lightmap;
        p->smoothing  = like->smoothing;
        memcpy( p->kiwiLayer, like->kiwiLayer, sizeof( p->kiwiLayer ) );
        const bool layered = UsedSlotCount( like ) != 0;
        for ( int a = 0; a < nx; ++a )
            for ( int b = 0; b < ny; ++b )
            {
                const float *g = &grid[( ( j0 + b ) * W + ( i0 + a ) ) * 7];
                drawVert_t *cp = &p->ctrl[a][b];
                cp->xyz[0] = g[0]; cp->xyz[1] = g[1]; cp->xyz[2] = g[2];
                byte *c = (byte *)&cp->vert_color;
                for ( int k = 0; k < 4; ++k )
                    c[k] = layered ? (byte)(int)( ClampF( g[3 + k], 0.0f, 255.0f ) + 0.5f ) : (byte)255;
            }
        Patch_KiwiTextureAndBuild( p, g_qeglobals.random_texture_stuff[0].sampleSize );
        brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
        selbrush_t *inst = Brush_AddToList( pdef, owner );
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
        return inst;
    }

    bool  s_holeFailValid = false;          // do not re-search the same spot every frame
    float s_holeFailAt[2] = { 0.0f, 0.0f };

    bool FillOutline( std::vector<float> &loop, const patchMesh_t *like, entity_s *owner, bool verbose );

    // The first outline search: classify whole border segments near the ring, chain them by
    // endpoint.  Kept as the FALLBACK of TraceHoleOutline (below), which replaced it.
    bool FillHoleByChain( bool verbose )
    {
        if ( !s_cursorHave )
            return false;
        const float tol = 2.0f, tol2 = tol * tol;
        const float bridge = 96.0f;                     // widest break in an outline that is closed anyway
        // Only terrain near the ring: the gap has to be UNDER the ring, and the pairwise
        // border test below grows with the square of the patches looked at.
        const float reach = s_outer + 512.0f;

        // 1. the terrain in reach and its border rings
        struct ring_t { selbrush_t *node; int n; int ii[64], jj[64]; };
        std::vector<ring_t> rings;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !PatchEligible( b ) || BoundsDistanceXY( b, s_cursor ) > reach )
                    continue;
                ring_t r;
                r.node = b;
                r.n = BorderRing( b->patch->def, r.ii, r.jj );
                if ( r.n >= 4 )
                    rings.push_back( r );
            }
        }
        if ( rings.empty() )
            return false;

        std::vector<kterHoleEdge_t> open;
        for ( size_t p = 0; p < rings.size(); ++p )
        {
            const patchMesh_t *A = rings[p].node->patch->def;
            for ( int k = 0; k < rings[p].n; ++k )
            {
                const int k1 = ( k + 1 ) % rings[p].n;
                const drawVert_t &va = A->ctrl[rings[p].ii[k]][rings[p].jj[k]];
                const drawVert_t &vb = A->ctrl[rings[p].ii[k1]][rings[p].jj[k1]];
                const float mid[3] = { ( va.xyz[0] + vb.xyz[0] ) * 0.5f, ( va.xyz[1] + vb.xyz[1] ) * 0.5f,
                                       ( va.xyz[2] + vb.xyz[2] ) * 0.5f };
                if ( ( va.xyz[0] - vb.xyz[0] ) * ( va.xyz[0] - vb.xyz[0] )
                   + ( va.xyz[1] - vb.xyz[1] ) * ( va.xyz[1] - vb.xyz[1] ) < 0.01f )
                    continue;
                bool shared = false;
                for ( size_t q = 0; q < rings.size() && !shared; ++q )
                {
                    if ( q == p )
                        continue;
                    const float *mins = rings[q].node->def->mins, *maxs = rings[q].node->def->maxs;
                    if ( mid[0] < mins[0] - tol || mid[0] > maxs[0] + tol || mid[1] < mins[1] - tol || mid[1] > maxs[1] + tol )
                        continue;
                    const patchMesh_t *B = rings[q].node->patch->def;
                    for ( int m = 0; m < rings[q].n && !shared; ++m )
                    {
                        const int m1 = ( m + 1 ) % rings[q].n;
                        shared = PointSegDist2XY( mid, B->ctrl[rings[q].ii[m]][rings[q].jj[m]].xyz,
                                                  B->ctrl[rings[q].ii[m1]][rings[q].jj[m1]].xyz ) <= tol2;
                    }
                }
                if ( !shared )
                {
                    // other terrain under / over it (stacked patches): not a hole edge either
                    const float org[3] = { mid[0], mid[1], 65536.0f };
                    const float dir[3] = { 0.0f, 0.0f, -1.0f };
                    float hit[3];
                    shared = PickPatches( org, dir, true, hit, nullptr, nullptr, rings[p].node );
                }
                if ( shared )
                    continue;
                kterHoleEdge_t e;
                HoleVert( va, e.a );
                HoleVert( vb, e.b );
                e.node = rings[p].node;
                e.used = false;
                open.push_back( e );
            }
        }
        if ( open.size() < 3 )
            return false;

        // 2. chain from the open edge nearest the cursor
        // The gap must be under the RING, not under the exact cursor point: a sliver a few
        // units wide cannot be aimed at, and the cursor then sits on the terrain beside it.
        int start = -1;
        float startD = s_outer * s_outer;
        for ( size_t i = 0; i < open.size(); ++i )
        {
            const float d = PointSegDist2XY( s_cursor, open[i].a, open[i].b );
            if ( d < startD ) { startD = d; start = (int)i; }
        }
        if ( start < 0 )
            return false;                               // no open edge under the ring: nothing to say
        std::vector<float> loop;                        // 7 floats a vertex
        loop.insert( loop.end(), open[start].a, open[start].a + 7 );
        loop.insert( loop.end(), open[start].b, open[start].b + 7 );
        open[start].used = true;
        bool closed = false;
        for ( int guard = 0; guard < 400 && !closed; ++guard )
        {
            const float *cur = &loop[loop.size() - 7];
            const float *first = &loop[0];
            // KIWI (2026-09-18, user video: a thin SLIVER gap would not fill): at a sliver's
            // tips its two sides run within `tol` of each other, so the end segments test as
            // "shared" and drop out of the open set - the outline then has a break at each
            // tip and never closed.  Two passes: an exact continuation first, else BRIDGE the
            // break to the nearest unused open endpoint (or back to the start) within
            // `bridge` units.  The bridged tip leaves a crack narrower than `tol`.
            int   best = -1;
            bool  bestFlip = false, bridged = false;
            for ( int attempt = 0; attempt < 2 && best < 0 && !closed; ++attempt )
            {
                float bestD = attempt == 0 ? tol2 : bridge * bridge;
                for ( size_t i = 0; i < open.size(); ++i )
                {
                    if ( open[i].used )
                        continue;
                    const float da = ( open[i].a[0] - cur[0] ) * ( open[i].a[0] - cur[0] ) + ( open[i].a[1] - cur[1] ) * ( open[i].a[1] - cur[1] );
                    const float db = ( open[i].b[0] - cur[0] ) * ( open[i].b[0] - cur[0] ) + ( open[i].b[1] - cur[1] ) * ( open[i].b[1] - cur[1] );
                    if ( da <= bestD ) { bestD = da; best = (int)i; bestFlip = false; }
                    if ( db <= bestD ) { bestD = db; best = (int)i; bestFlip = true; }
                }
                if ( attempt == 1 && loop.size() / 7 >= 3 )
                {
                    // closing the outline beats bridging further away
                    const float dc = ( first[0] - cur[0] ) * ( first[0] - cur[0] ) + ( first[1] - cur[1] ) * ( first[1] - cur[1] );
                    if ( dc <= bestD )
                    {
                        closed = true;
                        best = -1;
                    }
                }
                bridged = attempt == 1;
            }
            if ( closed )
                break;
            if ( best < 0 )
            {
                if ( verbose )
                Sys_Printf( "Terrain Sculpt: gap fill - the open edges around the cursor do not close into an outline "
                            "(%i vertices chained, no open edge within %.0f units of the last one). Not a hole, or its "
                            "edge is covered by stacked terrain - try 'Select terrain stacked on other terrain'.\n",
                            (int)( loop.size() / 7 ), bridge );
                return false;
            }
            open[best].used = true;
            if ( bridged )
            {
                // the far endpoint of the bridge joins the outline as well
                const float *nearEnd = bestFlip ? open[best].b : open[best].a;
                loop.insert( loop.end(), nearEnd, nearEnd + 7 );
            }
            const float *next = bestFlip ? open[best].a : open[best].b;
            first = &loop[0];
            if ( ( next[0] - first[0] ) * ( next[0] - first[0] ) + ( next[1] - first[1] ) * ( next[1] - first[1] ) <= tol2 )
                closed = true;
            else
                loop.insert( loop.end(), next, next + 7 );
        }
        if ( !closed )
            return false;
        return FillOutline( loop, open[start].node->patch->def, open[start].node->owner, verbose );
    }

    // Stages 3 + 4: a closed outline (7 floats a vertex) -> corners -> Coons grid -> patches.
    bool FillOutline( std::vector<float> &loop, const patchMesh_t *like, entity_s *owner, bool verbose )
    {
        int n = (int)( loop.size() / 7 );
        if ( n < 3 || !like || !owner )
            return false;

        // A closed chain of open edges is either a HOLE (terrain outside it) or the OUTER
        // boundary of a piece of terrain (terrain inside it) - and filling the latter would
        // pave over the map.  Probe just inside the outline at three edges: a hole has no
        // terrain there.  (Orientation-free: the probe side comes from the outline's own
        // winding, not from the patches' grid direction.)
        {
            float area = 0.0f;
            for ( int i = 0, j = n - 1; i < n; j = i++ )
                area += loop[j * 7] * loop[i * 7 + 1] - loop[i * 7] * loop[j * 7 + 1];
            const float sideSign = area >= 0.0f ? 1.0f : -1.0f;     // CCW: interior is to the left
            int terrainInside = 0, probes = 0;
            for ( int t = 0; t < 3; ++t )
            {
                const int i = ( n * t ) / 3, i1 = ( i + 1 ) % n;
                const float *p0 = &loop[i * 7], *p1 = &loop[i1 * 7];
                float dx = p1[0] - p0[0], dy = p1[1] - p0[1];
                const float len = sqrtf( dx * dx + dy * dy );
                if ( len < 0.5f )
                    continue;
                dx /= len; dy /= len;
                const float org[3] = { ( p0[0] + p1[0] ) * 0.5f - dy * 0.75f * sideSign,
                                       ( p0[1] + p1[1] ) * 0.5f + dx * 0.75f * sideSign, 65536.0f };
                const float dir[3] = { 0.0f, 0.0f, -1.0f };
                float hit[3];
                ++probes;
                if ( PickPatches( org, dir, true, hit, nullptr ) )
                    ++terrainInside;
            }
            if ( probes == 0 || terrainInside * 2 > probes )
                return false;                           // the outer edge of terrain, not a hole
        }
        if ( n == 3 )                                    // a triangle: give it a fourth corner
        {
            std::vector<float> tri( loop );
            HoleUpsample( tri, 4 );
            loop.swap( tri );
            n = 4;
        }
        // counter-clockwise from above, so the patch faces up like every other sheet
        {
            float area = 0.0f;
            for ( int i = 0, j = n - 1; i < n; j = i++ )
                area += loop[j * 7] * loop[i * 7 + 1] - loop[i * 7] * loop[j * 7 + 1];
            if ( area < 0.0f )
            {
                std::vector<float> rev;
                for ( int i = n - 1; i >= 0; --i )
                    rev.insert( rev.end(), &loop[i * 7], &loop[i * 7] + 7 );
                loop.swap( rev );
            }
        }

        // 3. corners = the four sharpest turns, kept apart along the outline
        std::vector<float> turn( n ), arc( n + 1, 0.0f );
        for ( int i = 0; i < n; ++i )
        {
            const float *p0 = &loop[( ( i + n - 1 ) % n ) * 7], *p1 = &loop[i * 7], *p2 = &loop[( ( i + 1 ) % n ) * 7];
            float ax = p1[0] - p0[0], ay = p1[1] - p0[1], bx = p2[0] - p1[0], by = p2[1] - p1[1];
            const float la = sqrtf( ax * ax + ay * ay ), lb = sqrtf( bx * bx + by * by );
            float c = ( la > 1e-4f && lb > 1e-4f ) ? ( ax * bx + ay * by ) / ( la * lb ) : 1.0f;
            c = c < -1.0f ? -1.0f : ( c > 1.0f ? 1.0f : c );
            turn[i] = acosf( c );
            arc[i + 1] = arc[i] + lb;
        }
        const float perimeter = arc[n];
        int corner[4] = { -1, -1, -1, -1 };
        for ( int attempt = 0; attempt < 2 && corner[3] < 0; ++attempt )
        {
            const float apart = attempt == 0 ? perimeter * 0.08f : 0.0f;
            std::vector<unsigned char> taken( n, 0 );
            for ( int c = 0; c < 4; ++c )
            {
                corner[c] = -1;
                float best = -1.0f;
                for ( int i = 0; i < n; ++i )
                {
                    if ( taken[i] || turn[i] <= best )
                        continue;
                    bool crowded = false;               // ("near" is a windef.h macro)
                    for ( int o = 0; o < c && !crowded; ++o )
                    {
                        float d = fabsf( arc[i] - arc[corner[o]] );
                        if ( d > perimeter * 0.5f ) d = perimeter - d;
                        crowded = d < apart || i == corner[o];
                    }
                    if ( crowded )
                        continue;
                    best = turn[i];
                    corner[c] = i;
                }
                if ( corner[c] < 0 )
                    break;
                taken[corner[c]] = 1;
            }
        }
        // A RAGGED outline (the user's 2026-09-18 hole: stair-steps left by deleted fine
        // pieces, dozens of right-angle turns) has no four "sharpest" turns - every step ties,
        // and corners picked among them bunch up on one side.  With more than six real turns
        // the corners come from the outline's overall shape instead: its extreme points along
        // the two diagonals (min / max of x+y and x-y), which always spread round the hole.
        {
            int realTurns = 0;
            for ( int i = 0; i < n; ++i )
                if ( turn[i] > 0.6f )                    // ~35 degrees
                    ++realTurns;
            if ( realTurns > 6 || corner[3] < 0 )
            {
                int ext[4] = { 0, 0, 0, 0 };
                for ( int i = 1; i < n; ++i )
                {
                    const float *p = &loop[i * 7];
                    const float s = p[0] + p[1], d = p[0] - p[1];
                    if ( s < loop[ext[0] * 7] + loop[ext[0] * 7 + 1] ) ext[0] = i;   // bottom-left
                    if ( d > loop[ext[1] * 7] - loop[ext[1] * 7 + 1] ) ext[1] = i;   // bottom-right
                    if ( s > loop[ext[2] * 7] + loop[ext[2] * 7 + 1] ) ext[2] = i;   // top-right
                    if ( d < loop[ext[3] * 7] - loop[ext[3] * 7 + 1] ) ext[3] = i;   // top-left
                }
                const bool distinct = ext[0] != ext[1] && ext[0] != ext[2] && ext[0] != ext[3]
                                   && ext[1] != ext[2] && ext[1] != ext[3] && ext[2] != ext[3];
                if ( distinct )
                    memcpy( corner, ext, sizeof( corner ) );
            }
        }
        if ( corner[3] < 0 )
        {
            if ( verbose )
                Sys_Printf( "Terrain Sculpt: gap fill - the %i-vertex outline has no four usable corners.\n", n );
            return false;
        }
        std::sort( corner, corner + 4 );

        // 4. four sides, opposite sides equalised, Coons blend
        std::vector<float> side[4];
        for ( int s = 0; s < 4; ++s )
        {
            int i = corner[s];
            const int end = corner[( s + 1 ) % 4];
            for ( ;; )
            {
                side[s].insert( side[s].end(), &loop[i * 7], &loop[i * 7] + 7 );
                if ( i == end && side[s].size() > 7 )
                    break;
                i = ( i + 1 ) % n;
                if ( side[s].size() / 7 > 2000 )
                    return false;
            }
        }
        int W = (int)( side[0].size() / 7 ), W2 = (int)( side[2].size() / 7 );
        int H = (int)( side[1].size() / 7 ), H2 = (int)( side[3].size() / 7 );
        if ( W2 > W ) W = W2;
        if ( H2 > H ) H = H2;
        if ( W < 2 || H < 2 || W > 241 || H > 241 )
        {
            if ( verbose )
                Sys_Printf( "Terrain Sculpt: gap fill - the gap's outline needs a %i x %i point grid (limit 241). Fill part "
                            "of it with ordinary creation chunks first, then the rest.\n", W, H );
            return false;
        }
        HoleUpsample( side[0], W ); HoleUpsample( side[2], W );
        HoleUpsample( side[1], H ); HoleUpsample( side[3], H );

        std::vector<float> grid( (size_t)W * H * 7 );
        const float *c0 = &side[0][0], *c1 = &side[1][0], *c2 = &side[2][0], *c3 = &side[3][0];
        for ( int j = 0; j < H; ++j )
            for ( int i = 0; i < W; ++i )
            {
                const float u = W > 1 ? (float)i / (float)( W - 1 ) : 0.0f;
                const float v = H > 1 ? (float)j / (float)( H - 1 ) : 0.0f;
                const float *B = &side[0][i * 7], *T = &side[2][( W - 1 - i ) * 7];
                const float *R = &side[1][j * 7], *L = &side[3][( H - 1 - j ) * 7];
                float *g = &grid[( (size_t)j * W + i ) * 7];
                for ( int k = 0; k < 7; ++k )
                {
                    if      ( j == 0 )     g[k] = B[k];
                    else if ( j == H - 1 ) g[k] = T[k];
                    else if ( i == 0 )     g[k] = L[k];
                    else if ( i == W - 1 ) g[k] = R[k];
                    else
                        g[k] = ( 1.0f - v ) * B[k] + v * T[k] + ( 1.0f - u ) * L[k] + u * R[k]
                             - ( ( 1.0f - u ) * ( 1.0f - v ) * c0[k] + u * ( 1.0f - v ) * c1[k]
                               + u * v * c2[k] + ( 1.0f - u ) * v * c3[k] );
                }
            }

        int made = 0;
        for ( int j0 = 0; j0 < H - 1; j0 += 15 )
            for ( int i0 = 0; i0 < W - 1; i0 += 15 )
            {
                selbrush_t *node = CreatePatchFromGrid( like, owner, grid, W, i0, i0 + 15 < W - 1 ? i0 + 15 : W - 1,
                                                        j0, j0 + 15 < H - 1 ? j0 + 15 : H - 1 );
                if ( !node )
                    continue;
                if ( s_undoOpen )
                {
                    node->patch->def->xx22b = 1;
                    Undo_KiwiMarkCreated( node->def );
                }
                Select_Brush( node, 0, 0, 0 );
                s_targets.push_back( node );
                ++s_created;
                ++made;
            }
        if ( !made )
            return false;
        s_holeFailValid = false;
        // The point count is dictated by the gap's OUTLINE (every neighbour vertex has to be a
        // fill vertex or the seam cracks) - so say what it cost; Ctrl+Z takes it back.
        const int tris = ( W - 1 ) * ( H - 1 ) * 2;
        SetStatus( "Filled the gap: %i patch%s, %i x %i points, %i triangles (set by the gap's edge vertices). Ctrl+Z undoes.",
                   made, made == 1 ? "" : "es", W, H, tris );
        Sys_Printf( "Terrain Sculpt: filled a %i-vertex gap with %i patch%s (%i x %i points, %i triangles - the density "
                    "is set by the vertices along the gap's edge).\n", n, made, made == 1 ? "" : "es", W, H, tris );
        g_nUpdateBits = -1;
        return true;
    }

    // ── the outline TRACER (KIWI 2026-09-18, user console: "72 vertices chained, no open edge
    // within 96 units of the last one" on a hole ten times the ring) ───────────────────────
    // FillHoleByChain gathered terrain near the RING only, so the far side of a big hole was
    // never seen, and it judged whole border segments - wrong wherever a fine patch's corner
    // lands part-way along a coarse neighbour's edge (half that segment is seam, half is
    // hole).  The tracer has neither limit.  It treats every patch border as a planar graph
    // whose edges END at any other patch's border vertex lying on them, starts on the open
    // sub-edge nearest the cursor, works out which side of it has no terrain (the hole), and
    // walks node to node always taking the turn that HUGS the hole, until it is back at the
    // start.  Hole kept on the left => a real hole comes out counter-clockwise; a clockwise
    // result is the outside of the terrain and is refused.
    struct kterHRing_t
    {
        selbrush_t        *node;
        const patchMesh_t *def;
        int                n;
        int                ii[64], jj[64];
    };

    struct kterHCand_t
    {
        float              end[7];
        float              theta;
        const kterHRing_t *owner;
    };

    const float KHOLE_TOL = 2.0f;

    const drawVert_t &HRingVert( const kterHRing_t &r, int k )
    {
        return r.def->ctrl[r.ii[k]][r.jj[k]];
    }

    bool HRingNear( const kterHRing_t &r, const float *lo, const float *hi )
    {
        const float *mins = r.node->def->mins, *maxs = r.node->def->maxs;
        return !( hi[0] < mins[0] - KHOLE_TOL || lo[0] > maxs[0] + KHOLE_TOL
               || hi[1] < mins[1] - KHOLE_TOL || lo[1] > maxs[1] + KHOLE_TOL );
    }

    // Is the sub-edge a-b (on `owner`'s border) open: on nobody else's border, no other terrain under it?
    bool HEdgeOpen( const std::vector<kterHRing_t> &rings, const kterHRing_t *owner, const float *a, const float *b )
    {
        const float mid[3] = { ( a[0] + b[0] ) * 0.5f, ( a[1] + b[1] ) * 0.5f, 0.0f };
        for ( size_t q = 0; q < rings.size(); ++q )
        {
            if ( &rings[q] == owner || !HRingNear( rings[q], mid, mid ) )
                continue;
            for ( int m = 0; m < rings[q].n; ++m )
                if ( PointSegDist2XY( mid, HRingVert( rings[q], m ).xyz,
                                      HRingVert( rings[q], ( m + 1 ) % rings[q].n ).xyz ) <= KHOLE_TOL * KHOLE_TOL )
                    return false;
        }
        const float org[3] = { mid[0], mid[1], 65536.0f };
        const float dir[3] = { 0.0f, 0.0f, -1.0f };
        float hit[3];
        return !PickPatches( org, dir, true, hit, nullptr, nullptr, owner->node );
    }

    // From p toward e along owner's border: stop at the first OTHER patch's border vertex on the way.
    void HNextEvent( const std::vector<kterHRing_t> &rings, const kterHRing_t *owner,
                     const float *p, const drawVert_t &e, float out[7] )
    {
        HoleVert( e, out );
        const float dx = e.xyz[0] - p[0], dy = e.xyz[1] - p[1];
        const float len2 = dx * dx + dy * dy;
        if ( len2 < 1e-4f )
            return;
        const float len = sqrtf( len2 );
        const float lo[2] = { p[0] < e.xyz[0] ? p[0] : e.xyz[0], p[1] < e.xyz[1] ? p[1] : e.xyz[1] };
        const float hi[2] = { p[0] > e.xyz[0] ? p[0] : e.xyz[0], p[1] > e.xyz[1] ? p[1] : e.xyz[1] };
        float bestT = 1.0f - KHOLE_TOL / len;
        for ( size_t q = 0; q < rings.size(); ++q )
        {
            if ( &rings[q] == owner || !HRingNear( rings[q], lo, hi ) )
                continue;
            for ( int m = 0; m < rings[q].n; ++m )
            {
                const drawVert_t &v = HRingVert( rings[q], m );
                const float t = ( ( v.xyz[0] - p[0] ) * dx + ( v.xyz[1] - p[1] ) * dy ) / len2;
                if ( t * len <= KHOLE_TOL || t >= bestT )
                    continue;
                const float ex = v.xyz[0] - ( p[0] + dx * t ), ey = v.xyz[1] - ( p[1] + dy * t );
                if ( ex * ex + ey * ey > KHOLE_TOL * KHOLE_TOL )
                    continue;
                bestT = t;
                HoleVert( v, out );
            }
        }
    }

    // Every way on from point p along any patch border passing through it.
    void HOutEdges( const std::vector<kterHRing_t> &rings, const float *p, std::vector<kterHCand_t> &out )
    {
        out.clear();
        const float tol2 = KHOLE_TOL * KHOLE_TOL;
        for ( size_t r = 0; r < rings.size(); ++r )
        {
            if ( !HRingNear( rings[r], p, p ) )
                continue;
            for ( int k = 0; k < rings[r].n; ++k )
            {
                const drawVert_t &A = HRingVert( rings[r], k ), &B = HRingVert( rings[r], ( k + 1 ) % rings[r].n );
                if ( PointSegDist2XY( p, A.xyz, B.xyz ) > tol2 )
                    continue;
                const bool atA = ( A.xyz[0] - p[0] ) * ( A.xyz[0] - p[0] ) + ( A.xyz[1] - p[1] ) * ( A.xyz[1] - p[1] ) <= tol2;
                const bool atB = ( B.xyz[0] - p[0] ) * ( B.xyz[0] - p[0] ) + ( B.xyz[1] - p[1] ) * ( B.xyz[1] - p[1] ) <= tol2;
                for ( int way = 0; way < 2; ++way )
                {
                    if ( way == 0 ? atA : atB )
                        continue;                        // already at that end
                    kterHCand_t c;
                    c.owner = &rings[r];
                    c.theta = 0.0f;
                    HNextEvent( rings, &rings[r], p, way == 0 ? A : B, c.end );
                    if ( ( c.end[0] - p[0] ) * ( c.end[0] - p[0] ) + ( c.end[1] - p[1] ) * ( c.end[1] - p[1] ) <= tol2 )
                        continue;
                    out.push_back( c );
                }
            }
        }
    }

    bool TraceHoleOutline( std::vector<float> &loop, selbrush_t **likeNode, bool verbose )
    {
        loop.clear();
        const float tol2 = KHOLE_TOL * KHOLE_TOL;
        std::vector<kterHRing_t> rings;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !PatchEligible( b ) || BoundsDistanceXY( b, s_cursor ) > 16384.0f )
                    continue;
                kterHRing_t r;
                r.node = b;
                r.def  = b->patch->def;
                r.n    = BorderRing( r.def, r.ii, r.jj );
                if ( r.n >= 4 )
                    rings.push_back( r );
            }
        }

        // ── the start: the open sub-edge under the ring nearest the cursor ──────────
        float sa[7], sb[7], startD = s_outer * s_outer;
        const kterHRing_t *startRing = nullptr;
        for ( size_t r = 0; r < rings.size(); ++r )
        {
            if ( BoundsDistanceXY( rings[r].node, s_cursor ) > s_outer )
                continue;
            for ( int k = 0; k < rings[r].n; ++k )
            {
                const drawVert_t &A = HRingVert( rings[r], k ), &B = HRingVert( rings[r], ( k + 1 ) % rings[r].n );
                if ( PointSegDist2XY( s_cursor, A.xyz, B.xyz ) >= startD )
                    continue;
                // walk this border segment event to event
                float from[7];
                HoleVert( A, from );
                for ( int guard = 0; guard < 64; ++guard )
                {
                    float to[7];
                    HNextEvent( rings, &rings[r], from, B, to );
                    const float d = PointSegDist2XY( s_cursor, from, to );
                    if ( d < startD && HEdgeOpen( rings, &rings[r], from, to ) )
                    {
                        startD = d;
                        startRing = &rings[r];
                        memcpy( sa, from, sizeof( sa ) );
                        memcpy( sb, to, sizeof( sb ) );
                    }
                    if ( ( to[0] - B.xyz[0] ) * ( to[0] - B.xyz[0] ) + ( to[1] - B.xyz[1] ) * ( to[1] - B.xyz[1] ) <= tol2 )
                        break;
                    memcpy( from, to, sizeof( from ) );
                }
            }
        }
        if ( !startRing )
            return false;                               // no open edge under the ring: ordinary ground

        // ── which side is the hole?  (the owner patch is on the other one) ───────────
        {
            float dx = sb[0] - sa[0], dy = sb[1] - sa[1];
            const float len = sqrtf( dx * dx + dy * dy );
            dx /= len; dy /= len;
            bool leftEmpty = false, rightEmpty = false;
            for ( float off = 1.5f; off >= 0.4f && leftEmpty == rightEmpty; off *= 0.5f )
            {
                const float dir[3] = { 0.0f, 0.0f, -1.0f };
                float hit[3];
                const float l[3] = { ( sa[0] + sb[0] ) * 0.5f - dy * off, ( sa[1] + sb[1] ) * 0.5f + dx * off, 65536.0f };
                const float r[3] = { ( sa[0] + sb[0] ) * 0.5f + dy * off, ( sa[1] + sb[1] ) * 0.5f - dx * off, 65536.0f };
                leftEmpty  = !PickPatches( l, dir, true, hit, nullptr );
                rightEmpty = !PickPatches( r, dir, true, hit, nullptr );
            }
            if ( leftEmpty == rightEmpty )
            {
                if ( verbose )
                    Sys_Printf( "Terrain Sculpt: gap fill - cannot tell which side of the open edge at (%.0f %.0f) is the gap.\n",
                                sa[0], sa[1] );
                return false;
            }
            // The void beside an open edge is either a hole or simply the OUTSIDE of the map -
            // and creation strokes happen at the map's edge all the time, where walking the
            // whole boundary just to refuse it would hitch every stroke.  A hole has terrain
            // on its far side: look straight out at doubling distances; nothing within 8192
            // units means "outside", decided with nine rays instead of a boundary walk.
            {
                const float side = leftEmpty ? 1.0f : -1.0f;
                bool farTerrain = false;
                for ( float dist = 32.0f; dist <= 8192.0f && !farTerrain; dist *= 2.0f )
                {
                    const float dir[3] = { 0.0f, 0.0f, -1.0f };
                    float hit[3];
                    const float o[3] = { ( sa[0] + sb[0] ) * 0.5f - dy * dist * side,
                                         ( sa[1] + sb[1] ) * 0.5f + dx * dist * side, 65536.0f };
                    farTerrain = PickPatches( o, dir, true, hit, nullptr );
                }
                if ( !farTerrain )
                    return false;
            }
            if ( !leftEmpty )                           // keep the hole on the LEFT of travel
            {
                float t[7];
                memcpy( t, sa, sizeof( t ) ); memcpy( sa, sb, sizeof( sa ) ); memcpy( sb, t, sizeof( sb ) );
            }
        }

        // ── walk, hugging the hole ──────────────────────────────────────────────────
        loop.insert( loop.end(), sa, sa + 7 );
        loop.insert( loop.end(), sb, sb + 7 );
        std::vector<kterHCand_t> cands;
        bool closed = false;
        for ( int step = 0; step < 2000 && !closed; ++step )
        {
            const size_t nv = loop.size() / 7;
            float cur[7], prev[2];
            memcpy( cur, &loop[( nv - 1 ) * 7], sizeof( cur ) );
            prev[0] = loop[( nv - 2 ) * 7]; prev[1] = loop[( nv - 2 ) * 7 + 1];
            const float dinx = cur[0] - prev[0], diny = cur[1] - prev[1];
            HOutEdges( rings, cur, cands );
            for ( size_t c = 0; c < cands.size(); ++c )
            {
                const float ox = cands[c].end[0] - cur[0], oy = cands[c].end[1] - cur[1];
                cands[c].theta = atan2f( dinx * oy - diny * ox, dinx * ox + diny * oy );   // + = left turn
            }
            std::sort( cands.begin(), cands.end(),
                       []( const kterHCand_t &a, const kterHCand_t &b ) { return a.theta > b.theta; } );
            const kterHCand_t *take = nullptr;
            for ( size_t c = 0; c < cands.size() && !take; ++c )
            {
                if ( fabsf( cands[c].theta ) > 3.12f )
                    continue;                           // straight back the way we came
                if ( HEdgeOpen( rings, cands[c].owner, cur, cands[c].end ) )
                    take = &cands[c];
            }
            if ( !take )
            {
                if ( verbose )
                    Sys_Printf( "Terrain Sculpt: gap fill - the gap's outline breaks off at (%.0f %.0f) after %i vertices: "
                                "no open terrain edge leads on from there (terrain stacked on that edge? try 'Select "
                                "terrain stacked on other terrain').\n", cur[0], cur[1], (int)nv );
                return false;
            }
            if ( ( take->end[0] - loop[0] ) * ( take->end[0] - loop[0] ) + ( take->end[1] - loop[1] ) * ( take->end[1] - loop[1] ) <= tol2 )
                closed = true;
            else
                loop.insert( loop.end(), take->end, take->end + 7 );
        }
        if ( !closed )
        {
            if ( verbose )
                Sys_Printf( "Terrain Sculpt: gap fill - the outline did not close within 2000 vertices (the open map edge?).\n" );
            return false;
        }
        const int n = (int)( loop.size() / 7 );
        float area = 0.0f;
        for ( int i = 0, j = n - 1; i < n; j = i++ )
            area += loop[j * 7] * loop[i * 7 + 1] - loop[i * 7] * loop[j * 7 + 1];
        if ( area <= 0.0f )
            return false;                               // clockwise with the void on the left: the OUTSIDE of the terrain
        *likeNode = startRing->node;
        return true;
    }

    bool FillHoleUnderCursor()
    {
        if ( !s_cursorHave )
            return false;
        if ( s_holeFailValid )
        {
            const float dx = s_cursor[0] - s_holeFailAt[0], dy = s_cursor[1] - s_holeFailAt[1];
            if ( dx * dx + dy * dy < 16.0f * 16.0f )
                return false;
        }
        s_holeFailValid = true;
        s_holeFailAt[0] = s_cursor[0];
        s_holeFailAt[1] = s_cursor[1];

        // This runs on every creation stroke, also over ordinary ground and along the map's
        // edge: it only SPEAKS when the cursor is off the terrain, i.e. aimed at a gap.
        const bool verbose = LiveCursorNode() == nullptr;
        std::vector<float> loop;
        selbrush_t *likeNode = nullptr;
        if ( TraceHoleOutline( loop, &likeNode, verbose ) && likeNode
          && FillOutline( loop, likeNode->patch->def, likeNode->owner, verbose ) )
            return true;
        return FillHoleByChain( false );                // the older near-ring search, silent
    }

    void ExpandUnderBrush()
    {
        // A gap between existing patches is filled along its own outline first; the square
        // lattice below only ever lays chunks on ground that has NO terrain at all.
        if ( FillHoleUnderCursor() )
            return;
        kterLattice_t L;
        if ( !ResolveLattice( &L ) )
            return;
        float cells[64][5];
        const int n = EmptyCellsUnderBrush( cells, L );
        if ( !n )
            return;
        const patchMesh_t *like = L.like ? L.like : FreshTemplate();
        for ( int c = 0; c < n; ++c )
        {
            selbrush_t *node = CreateChunkXY( like, L.owner, cells[c][0], cells[c][1],
                                              cells[c][2], cells[c][3],
                                              PointsFor( cells[c][2], L.cellX ), PointsFor( cells[c][3], L.cellY ),
                                              nullptr, cells[c][4] );
            // Seams: every border point takes the height of the terrain already there
            // (vertical probe, this chunk excluded), so shared edges match exactly.
            patchMesh_t *def = node->patch->def;
            bool seamed = false;
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                {
                    if ( i != 0 && j != 0 && i != def->width - 1 && j != def->height - 1 )
                        continue;
                    drawVert_t *cp = &def->ctrl[i][j];
                    const float org[3] = { cp->xyz[0], cp->xyz[1], cp->xyz[2] + 16384.0f };
                    const float dir[3] = { 0.0f, 0.0f, -1.0f };
                    float hit[3];
                    if ( PickPatches( org, dir, true, hit, nullptr, nullptr, node ) )
                    {
                        cp->xyz[2] = hit[2];
                        seamed = true;
                    }
                }
            if ( seamed )
            {
                Patch_Rebuild( def, 1 );
                ++def->version;
            }
            if ( s_undoOpen )
            {
                // Created INSIDE the stroke's record: stamp it as created (Undo_Undo
                // frees it, nothing restores it) and pre-mark it painted so the first
                // stamp does not save a copy that would come back after the undo.
                def->xx22b = 1;
                Undo_KiwiMarkCreated( node->def );
            }
            // The new chunk joins the SELECTION, so the next stroke (which targets the
            // selection) keeps moving it with its neighbours instead of leaving a step.
            Select_Brush( node, 0, 0, 0 );
            s_targets.push_back( node );
            ++s_created;
        }
        g_nUpdateBits = -1;
    }

    // ── Set height: refine the mesh under the ring ───────────────────────────
    // KIWI (2026-09-18, user: "I want it to simply just set all terrain in the circle to
    // the desired height. Look back at farcry source, it worked great there").  Far Cry's
    // flatten (Editor/Heightmap.cpp CHeightmap::DrawSpot2) is nothing but "every sample
    // inside the radius goes to the height".  It works great because its heightmap is a
    // dense uniform grid: the brush always covers dozens of samples and the spill at the
    // rim is one 2 m unit.  A CoD4 patch sheet can have cells as wide as the ring, and then
    // the same rule moves zero or one point - a spike, or a lean a whole cell past the ring.
    //
    // So before Set height stamps, every coarse sheet the ring reaches is RE-GRIDDED:
    // quartered until the pieces under the ring fit one 16-point patch at the wanted cell
    // size (a quarter of the outer radius, a power of two, never below "Finest cell"), and
    // those pieces are laid fine.  Pieces the ring does not reach keep the sheet's own cell
    // size, so the triangle cost stays local (about ten patches per touched sheet, not
    // hundreds).  Every piece samples the ORIGINAL grid; the seam pass closes the fine /
    // coarse edges.  All inside the stroke's undo record: the original is saved, the
    // pieces are stamped as created, one Ctrl+Z brings the sheet back.
    //
    // KIWI (2026-09-18, later, user: "im still having shit outside of the circle affected
    // by the terrain tools!!!"): the first version quartered the sheet and RE-SAMPLED every
    // piece bilinearly, which shifted ground far from the ring by small amounts (a terrain
    // quad draws as two triangles, not a bilinear surface, and the new vertices did not sit
    // on the old ones).  The re-grid is now EXACT:
    //   * the sheet is only ever cut along its OWN grid lines;
    //   * pieces the ring does not reach are verbatim sub-grids (same points, same
    //     diagonals) - not one height differs;
    //   * the cells under the ring are split k x k (k a power of two <= 8) with every new
    //     point read off the original cell's own TRIANGLE and every sub-quad keeping that
    //     cell's diagonal, so the fine mesh IS the old surface until a stamp moves it.
    // Refined pieces are therefore not handed to the seam pass: their edges already match.
    float RefineCellFor( float outer )
    {
        float want = 256.0f;
        while ( want * 0.5f >= s_refineMin && want > outer / 4.0f )
            want *= 0.5f;
        return want;
    }

    // Mean cell edge along grid axis 0 (i) or 1 (j), measured in XY along the first row /
    // column - valid for ANY terrain grid (rotated, curved), unlike CellSizeAxis.
    float CellSpan( const patchMesh_t *def, int axis )
    {
        const int n = axis == 0 ? def->width : def->height;
        if ( n < 2 )
            return 64.0f;
        float len = 0.0f;
        for ( int k = 0; k + 1 < n; ++k )
        {
            const float *a = axis == 0 ? def->ctrl[k][0].xyz     : def->ctrl[0][k].xyz;
            const float *b = axis == 0 ? def->ctrl[k + 1][0].xyz : def->ctrl[0][k + 1].xyz;
            len += sqrtf( ( b[0] - a[0] ) * ( b[0] - a[0] ) + ( b[1] - a[1] ) * ( b[1] - a[1] ) );
        }
        const float c = len / (float)( n - 1 );
        return c > 1.0f ? c : 64.0f;
    }

    int RefineFactor( float cell, float want )
    {
        int k = 1;
        while ( k < 8 && cell / (float)k > want * 1.5f )
            k *= 2;
        return k;
    }

    // Position + colour at (fu, fv) inside source cell (i, j), read off the triangle that
    // draws there: turned_edge & 1 = diagonal v00-v11, else v01-v10 (as DrawWorld's wire).
    void SampleCellTri( const patchMesh_t *src, int i, int j, float fu, float fv, drawVert_t *out )
    {
        const drawVert_t &v00 = src->ctrl[i][j],     &v10 = src->ctrl[i + 1][j];
        const drawVert_t &v01 = src->ctrl[i][j + 1], &v11 = src->ctrl[i + 1][j + 1];
        // weights of the four corners
        float w00, w10, w01, w11;
        if ( ( v00.turned_edge & 1 ) != 0 )
        {
            if ( fu >= fv ) { w00 = 1.0f - fu; w10 = fu - fv; w11 = fv; w01 = 0.0f; }
            else            { w00 = 1.0f - fv; w01 = fv - fu; w11 = fu; w10 = 0.0f; }
        }
        else
        {
            if ( fu + fv <= 1.0f ) { w00 = 1.0f - fu - fv; w10 = fu; w01 = fv; w11 = 0.0f; }
            else                   { w11 = fu + fv - 1.0f; w01 = 1.0f - fu; w10 = 1.0f - fv; w00 = 0.0f; }
        }
        for ( int a = 0; a < 3; ++a )
            out->xyz[a] = v00.xyz[a] * w00 + v10.xyz[a] * w10 + v01.xyz[a] * w01 + v11.xyz[a] * w11;
        // texture / lightmap / smoothing coordinates are linear over a triangle too, so the
        // new points carry the OLD mapping exactly (no re-projection, nothing shifts)
        {
            const float *t00 = (const float *)&v00.texCoord, *t10 = (const float *)&v10.texCoord;
            const float *t01 = (const float *)&v01.texCoord, *t11 = (const float *)&v11.texCoord;
            float *to = (float *)&out->texCoord;
            for ( int a = 0; a < 6; ++a )
                to[a] = t00[a] * w00 + t10[a] * w10 + t01[a] * w01 + t11[a] * w11;
            const float *s00 = (const float *)&v00.savedTexCoord, *s10 = (const float *)&v10.savedTexCoord;
            const float *s01 = (const float *)&v01.savedTexCoord, *s11 = (const float *)&v11.savedTexCoord;
            float *so = (float *)&out->savedTexCoord;
            for ( int a = 0; a < 6; ++a )
                so[a] = s00[a] * w00 + s10[a] * w10 + s01[a] * w01 + s11[a] * w11;
        }
        const byte *c00 = (const byte *)&v00.vert_color, *c10 = (const byte *)&v10.vert_color;
        const byte *c01 = (const byte *)&v01.vert_color, *c11 = (const byte *)&v11.vert_color;
        byte *co = (byte *)&out->vert_color;
        for ( int k = 0; k < 4; ++k )
            co[k] = (byte)(int)( ClampF( c00[k] * w00 + c10[k] * w10 + c01[k] * w01 + c11[k] * w11, 0.0f, 255.0f ) + 0.5f );
    }

    // Source cells [i0,i1) x [j0,j1), each split k x k.  k = 1 is a verbatim sub-grid.
    selbrush_t *CreatePiece( const patchMesh_t *src, entity_s *owner, int i0, int i1, int j0, int j1, int k )
    {
        const int nx = ( i1 - i0 ) * k + 1, ny = ( j1 - j0 ) * k + 1;
        if ( nx < 2 || ny < 2 || nx > 16 || ny > 16 )
            return nullptr;
        patchMesh_t *p = MakeNewPatch();
        p->width  = nx;
        p->height = ny;
        p->type       = (PATCH_TYPES)( src->type | PATCH_TERRAIN );
        p->contents   = src->contents;
        p->flags      = src->flags;
        p->subDivType = src->subDivType;
        p->texture    = src->texture;
        p->lightmap   = src->lightmap;
        p->smoothing  = src->smoothing;
        memcpy( p->kiwiLayer, src->kiwiLayer, sizeof( p->kiwiLayer ) );
        for ( int a = 0; a < nx; ++a )
            for ( int b = 0; b < ny; ++b )
            {
                int   ci = i0 + a / k, cj = j0 + b / k;
                float fu = (float)( a % k ) / (float)k, fv = (float)( b % k ) / (float)k;
                if ( ci >= i1 ) { ci = i1 - 1; fu = 1.0f; }
                if ( cj >= j1 ) { cj = j1 - 1; fv = 1.0f; }
                const bool onPoint = ( a % k == 0 ) && ( b % k == 0 );
                const drawVert_t &orig = src->ctrl[i0 + a / k][j0 + b / k];     // valid when onPoint
                drawVert_t *cp = &p->ctrl[a][b];
                *cp = onPoint ? orig : src->ctrl[ci][cj];
                if ( !onPoint )
                    SampleCellTri( src, ci, cj, fu, fv, cp );
                // the quad that STARTS here lies in source cell (a/k, b/k): keep its diagonal
                const int qi = i0 + ( a / k < i1 - i0 ? a / k : i1 - i0 - 1 );
                const int qj = j0 + ( b / k < j1 - j0 ? b / k : j1 - j0 - 1 );
                cp->turned_edge = ( src->ctrl[qi][qj].turned_edge & 1 )
                                | ( onPoint ? ( orig.turned_edge & ~1 ) : 0 );
            }
        // Not Patch_KiwiTextureAndBuild: that re-projects the texture, and these pieces must
        // look exactly like the sheet they replace.  The mapping state rides along instead.
        p->bDirty = src->bDirty;
        *(float *)&p->size_of_struct_0x504C = *(const float *)&src->size_of_struct_0x504C;
        Patch_Rebuild( p, 0 );                   // tessellate only (no re-project, no bounds: no brush yet)
        brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
        selbrush_t *inst = Brush_AddToList( pdef, owner );
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
        return inst;
    }

    bool RefineUnderRing( const float *from, const float *to )
    {
        if ( !s_autoRefine )
            return false;
        const float want = RefineCellFor( s_outer );
        const float r = ( s_shape == KTER_SQUARE ? s_outer * 1.42f : s_outer ) + want;
        const float box[4] = { ( from[0] < to[0] ? from[0] : to[0] ) - r, ( from[1] < to[1] ? from[1] : to[1] ) - r,
                               ( from[0] > to[0] ? from[0] : to[0] ) + r, ( from[1] > to[1] ? from[1] : to[1] ) + r };
        // EVERY eligible sheet the ring box reaches, target or not (2026-09-18 "moat"): a seam
        // is a straight line between the COARSER side's vertices, so a coarse neighbour that
        // is not re-gridded pins the seam flat between two far-apart points while the ground
        // rises on both sides.  The seam pass already edits such neighbours; re-gridding them
        // (exactly - nothing moves) is what lets the seam follow the stroke.
        std::vector<selbrush_t *> victims;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *node = head->next; node && node != head; node = node->next )
            {
                if ( !PatchEligible( node ) )
                    continue;
                // ANY terrain grid, not only axis-aligned sheets: CreatePiece cuts along the
                // patch's own grid lines and reads new points off its own triangles, so a
                // curved or rotated patch refines exactly too.
                const patchMesh_t *def = node->patch->def;
                if ( def->width < 2 || def->height < 2 )
                    continue;
                const float *mins = node->def->mins, *maxs = node->def->maxs;
                if ( maxs[0] < box[0] || mins[0] > box[2] || maxs[1] < box[1] || mins[1] > box[3] )
                    continue;
                if ( CellSpan( def, 0 ) <= want * 1.5f && CellSpan( def, 1 ) <= want * 1.5f )
                    continue;                              // already fine enough
                victims.push_back( node );
            }
        }
        if ( victims.empty() )
            return false;

        int pieces = 0;
        for ( size_t v = 0; v < victims.size(); ++v )
        {
            selbrush_t  *node = victims[v];
            patchMesh_t *def  = node->patch->def;
            bool wasSelected = false;
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes && !wasSelected; b = b->next )
                wasSelected = ( b == node );
            std::vector<selbrush_t *> made;
            {
                const int cw = def->width - 1, ch = def->height - 1;     // cells per axis
                int fx = RefineFactor( CellSpan( def, 0 ), want );
                int fy = RefineFactor( CellSpan( def, 1 ), want );
                const int k = fx > fy ? fx : fy;                         // k x k keeps each cell's diagonal exact
                // the block of source cells whose own XY box touches the ring box - per CELL,
                // so it holds for curved / rotated grids as well as sheets
                int ci0 = cw, ci1 = -1, cj0 = ch, cj1 = -1;
                for ( int i = 0; i < cw; ++i )
                    for ( int j = 0; j < ch; ++j )
                    {
                        float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
                        for ( int c = 0; c < 4; ++c )
                        {
                            const float *p = def->ctrl[i + ( c & 1 )][j + ( c >> 1 )].xyz;
                            for ( int a = 0; a < 2; ++a )
                            {
                                if ( p[a] < lo[a] ) lo[a] = p[a];
                                if ( p[a] > hi[a] ) hi[a] = p[a];
                            }
                        }
                        if ( hi[0] < box[0] || lo[0] > box[2] || hi[1] < box[1] || lo[1] > box[3] )
                            continue;
                        if ( i < ci0 ) ci0 = i; if ( i > ci1 ) ci1 = i;
                        if ( j < cj0 ) cj0 = j; if ( j > cj1 ) cj1 = j;
                    }
                if ( k < 2 || ci1 < ci0 || cj1 < cj0 )
                    continue;
                entity_s *owner = node->owner;
                selbrush_t *piece;
                // verbatim frame around the block: left, right, below, above
                if ( ci0 > 0 && ( piece = CreatePiece( def, owner, 0, ci0, 0, ch, 1 ) ) != nullptr )            made.push_back( piece );
                if ( ci1 + 1 < cw && ( piece = CreatePiece( def, owner, ci1 + 1, cw, 0, ch, 1 ) ) != nullptr )  made.push_back( piece );
                if ( cj0 > 0 && ( piece = CreatePiece( def, owner, ci0, ci1 + 1, 0, cj0, 1 ) ) != nullptr )     made.push_back( piece );
                if ( cj1 + 1 < ch && ( piece = CreatePiece( def, owner, ci0, ci1 + 1, cj1 + 1, ch, 1 ) ) != nullptr ) made.push_back( piece );
                // the block itself, k x k per cell, at most 15 fine cells a piece
                const int m = 15 / k;
                for ( int ia = ci0; ia <= ci1; ia += m )
                    for ( int ja = cj0; ja <= cj1; ja += m )
                    {
                        const int ib = ia + m < ci1 + 1 ? ia + m : ci1 + 1;
                        const int jb = ja + m < cj1 + 1 ? ja + m : cj1 + 1;
                        if ( ( piece = CreatePiece( def, owner, ia, ib, ja, jb, k ) ) != nullptr )
                            made.push_back( piece );
                    }
            }
            if ( made.empty() )
                continue;
            if ( s_undoOpen )
                Undo_AddBrush( (entity_brush_s *)node->def );   // a no-op when the stroke already saved it
            for ( size_t m = 0; m < made.size(); ++m )
            {
                patchMesh_t *mdef = made[m]->patch->def;
                if ( s_undoOpen )
                {
                    mdef->xx22b = 1;                         // born in the record: no copy on first stamp
                    Undo_KiwiMarkCreated( made[m]->def );
                }
                if ( wasSelected )
                    Select_Brush( made[m], 0, 0, 0 );
                // NOT NoteDirty: the pieces' edges match by construction, and the seam pass
                // would "heal" old cracks on neighbours far outside the ring.
            }
            pieces += (int)made.size();
            ForgetDef( def );
            Brush_Free( node );
        }
        BuildTargets();
        if ( pieces )
        {
            s_created += pieces;
            Sys_Printf( "Terrain Sculpt: re-gridded %i sheet%s under the ring into %i piece%s (toward %.0f-unit cells there; the rest is unchanged).\n",
                        (int)victims.size(), victims.size() == 1 ? "" : "s", pieces, pieces == 1 ? "" : "s", want );
        }
        g_nUpdateBits = -1;
        return pieces > 0;
    }

    void TrimUnderBrush()
    {
        if ( !s_cursorHave )
            return;
        std::vector<selbrush_t *> victims;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !PatchEligible( b ) )
                    continue;
                const float *mins = b->def->mins, *maxs = b->def->maxs;
                const float c[3] = { ( mins[0] + maxs[0] ) * 0.5f, ( mins[1] + maxs[1] ) * 0.5f, 0.0f };
                if ( BrushDistance( s_cursor, c ) <= s_outer )
                    victims.push_back( b );
            }
        }
        if ( victims.empty() )
            return;
        Select_Deselect( 1 );
        for ( size_t i = 0; i < victims.size(); ++i )
        {
            ForgetDef( victims[i]->patch->def );
            Select_Brush( victims[i], 0, 0, 0 );
        }
        DeleteSelectionWithUndo( "trim terrain" );
        s_targets.clear();
        ClearCursor();
        SetStatus( "Armed. Trimmed %i patch%s.", (int)victims.size(), victims.size() == 1 ? "" : "es" );
    }

    // KIWI (2026-09-17, user: "the tesselate in the terrain sculpt tool does not work how it
    // should. It should be an absolute value not a relative one"): the setting used to be
    // "cells across each selected patch", so a 512-unit chunk and a 4096-unit sheet given
    // the same number came out eight times apart in real density, and a region could not
    // be brought to one resolution.  It is now a CELL SIZE in world units: every selected
    // patch is rebuilt so its cells are that big, whatever its own size - per axis, so a
    // rectangular sheet gets square cells too.  The count per axis is extent / cell
    // rounded to a whole cell (so the real size can differ by a fraction on a sheet whose
    // extent is not a multiple); more than 15 cells on an axis splits that axis into
    // chunks, 16 points being the CoD4 patch cap.  Every chunk samples the ORIGINAL grid,
    // so the shared chunk edges coincide exactly.
    struct kterTessPlan_t
    {
        int cellsX, cellsY;         // whole cells across the patch
        int kx, ky;                 // chunks per axis
        int perX, perY;             // cells per chunk (<= 15)
    };

    kterTessPlan_t TessPlan( float ex, float ey, float cell )
    {
        kterTessPlan_t p;
        if ( cell < 1.0f ) cell = 1.0f;
        p.cellsX = (int)( ex / cell + 0.5f ); if ( p.cellsX < 1 ) p.cellsX = 1;
        p.cellsY = (int)( ey / cell + 0.5f ); if ( p.cellsY < 1 ) p.cellsY = 1;
        p.kx = ( p.cellsX + 14 ) / 15;
        p.ky = ( p.cellsY + 14 ) / 15;
        p.perX = ( p.cellsX + p.kx - 1 ) / p.kx;
        p.perY = ( p.cellsY + p.ky - 1 ) / p.ky;
        p.cellsX = p.perX * p.kx;            // rounded up to fill whole chunks
        p.cellsY = p.perY * p.ky;
        return p;
    }

    void TessellateSelected()
    {
        // KIWI (2026-09-18, user: "it duplicates when tessellating!!!"): a regular sheet is
        // re-gridded to the absolute cell size from its rectangle.  ANY OTHER terrain grid
        // (curved edges, rotated, irregular) must never go down that road - its bounding box
        // is not its shape, and the rectangular chunks landed on the neighbours.  Those are
        // subdivided EXACTLY instead: every cell split k x k along the patch's own grid
        // (CreatePiece), k the power of two that brings its cells to the wanted size.  They
        // can only get finer; one that is already fine enough is left alone and counted.
        std::vector<selbrush_t *> nodes;
        std::vector<int>          exactK;           // 0 = regular sheet, else the k of CreatePiece
        int alreadyFine = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !PatchEligible( b ) || b->patch->def->width < 2 || b->patch->def->height < 2 )
                continue;
            if ( GridIsSheet( b->patch->def ) )
            {
                nodes.push_back( b );
                exactK.push_back( 0 );
                continue;
            }
            const int fx = RefineFactor( CellSpan( b->patch->def, 0 ), s_tessCell );
            const int fy = RefineFactor( CellSpan( b->patch->def, 1 ), s_tessCell );
            const int k = fx > fy ? fx : fy;
            if ( k < 2 )
            {
                ++alreadyFine;
                continue;
            }
            nodes.push_back( b );
            exactK.push_back( k );
        }
        if ( nodes.empty() )
        {
            if ( alreadyFine )
                Sys_Printf( "Terrain Sculpt: the %i selected curved / irregular patch%s already at that cell size or finer "
                            "(such patches can only be subdivided, never made coarser).\n",
                            alreadyFine, alreadyFine == 1 ? " is" : "es are" );
            else
                Sys_Printf( "Terrain Sculpt: select terrain patches to tessellate.\n" );
            return;
        }

        // One path for every patch (a single chunk is just kx = ky = 1) and ONE undo record.
        Select_Deselect( 1 );
        Undo_ClearRedo();
        Undo_GeneralStart( "tessellate terrain" );
        for ( size_t i = 0; i < nodes.size(); ++i )
            Select_Brush( nodes[i], 0, 0, 0 );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
        std::vector<selbrush_t *> created;
        int triangles = 0;
        float cellMin = FLT_MAX, cellMax = 0.0f;
        for ( size_t bi = 0; bi < nodes.size(); ++bi )
        {
            selbrush_t *src = nodes[bi];
            if ( exactK[bi] > 0 )
            {
                // exact k x k subdivision of a non-sheet grid, <= 15 fine cells a piece
                const patchMesh_t *sdef = src->patch->def;
                const int k = exactK[bi], m = 15 / k;
                const int cw = sdef->width - 1, ch = sdef->height - 1;
                for ( int ia = 0; ia < cw; ia += m )
                    for ( int ja = 0; ja < ch; ja += m )
                    {
                        selbrush_t *made = CreatePiece( sdef, src->owner, ia, ia + m < cw ? ia + m : cw,
                                                        ja, ja + m < ch ? ja + m : ch, k );
                        if ( made )
                            created.push_back( made );
                    }
                triangles += cw * ch * k * k * 2;
                const float cx = CellSpan( sdef, 0 ) / (float)k, cy = CellSpan( sdef, 1 ) / (float)k;
                if ( cx < cellMin ) cellMin = cx;
                if ( cy < cellMin ) cellMin = cy;
                if ( cx > cellMax ) cellMax = cx;
                if ( cy > cellMax ) cellMax = cy;
                continue;
            }
            const float *mins = src->def->mins, *maxs = src->def->maxs;
            const float ex = maxs[0] - mins[0], ey = maxs[1] - mins[1];
            const kterTessPlan_t p = TessPlan( ex, ey, s_tessCell );
            const float sx = ex / (float)p.kx, sy = ey / (float)p.ky;
            for ( int cy = 0; cy < p.ky; ++cy )
                for ( int cx = 0; cx < p.kx; ++cx )
                {
                    selbrush_t *made = CreateChunkXY( src->patch->def, src->owner,
                                                      mins[0] + sx * (float)cx, mins[1] + sy * (float)cy,
                                                      sx, sy, p.perX + 1, p.perY + 1, src->patch->def, 0.0f );
                    if ( made )
                        created.push_back( made );
                }
            triangles += p.cellsX * p.cellsY * 2;
            const float realX = ex / (float)p.cellsX, realY = ey / (float)p.cellsY;
            if ( realX < cellMin ) cellMin = realX;
            if ( realY < cellMin ) cellMin = realY;
            if ( realX > cellMax ) cellMax = realX;
            if ( realY > cellMax ) cellMax = realY;
        }
        for ( size_t i = 0; i < nodes.size(); ++i )
            ForgetDef( nodes[i]->patch->def );
        Select_Delete();
        Undo_EndBrushList( &selected_brushes );
        // KIWI (2026-09-17, user: "the operation is not undo-able ... looks like multiple
        // layers"): the chunks were born inside the record but never stamped with its id,
        // so Ctrl+Z put the saved originals back and LEFT the chunks - two sheets on top of
        // each other.  Stamped, undo phase 1 frees them before the originals return.
        for ( size_t i = 0; i < created.size(); ++i )
            Undo_KiwiMarkCreated( created[i]->def );
        Undo_End();
        for ( size_t i = 0; i < created.size(); ++i )
            Select_Brush( created[i], 0, 0, 0 );
        s_targets.clear();
        g_nUpdateBits = -1;
        Sys_Printf( "Terrain Sculpt: %i patch%s tessellated to %.0f-unit cells (actual %.1f..%.1f) as %i chunk%s, %i triangles.\n",
                    (int)nodes.size(), nodes.size() == 1 ? "" : "es", s_tessCell, cellMin, cellMax,
                    (int)created.size(), created.size() == 1 ? "" : "s", triangles );
    }

    // ── find stacked terrain (clean-up for maps the old bugs already touched) ──
    // A patch is "stacked" when other terrain lies under / over at least 7 of 9 points spread
    // over its interior.  Of a mutually overlapping pair only ONE is picked: the smaller in
    // area, or for equal ones the node at the higher address.  Selects; never deletes.
    int SelectStackedTerrain()
    {
        std::vector<selbrush_t *> all;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( PatchEligible( b ) && !FilterBrush( b, 0 ) && b->patch->def->width >= 2 && b->patch->def->height >= 2 )
                    all.push_back( b );
        }
        std::vector<selbrush_t *> stacked;
        for ( size_t n = 0; n < all.size(); ++n )
        {
            selbrush_t *node = all[n];
            const patchMesh_t *def = node->patch->def;
            const float area = ( node->def->maxs[0] - node->def->mins[0] ) * ( node->def->maxs[1] - node->def->mins[1] );
            int covered = 0;
            bool keepThis = false;
            for ( int a = 1; a <= 3 && !keepThis; ++a )
                for ( int b = 1; b <= 3 && !keepThis; ++b )
                {
                    // bilinear point inside the grid at (a/4, b/4), off the vertices
                    const float fu = (float)a * 0.25f * (float)( def->width - 1 );
                    const float fv = (float)b * 0.25f * (float)( def->height - 1 );
                    int i = (int)fu, j = (int)fv;
                    if ( i > def->width - 2 )  i = def->width - 2;
                    if ( j > def->height - 2 ) j = def->height - 2;
                    drawVert_t pt;
                    SampleCellTri( def, i, j, fu - (float)i, fv - (float)j, &pt );
                    const float org[3] = { pt.xyz[0], pt.xyz[1], 65536.0f };
                    const float dir[3] = { 0.0f, 0.0f, -1.0f };
                    float hit[3];
                    selbrush_t *other = nullptr;
                    if ( !PickPatches( org, dir, true, hit, nullptr, &other, node ) || !other )
                        continue;
                    const float oarea = ( other->def->maxs[0] - other->def->mins[0] ) * ( other->def->maxs[1] - other->def->mins[1] );
                    // the bigger one of a pair stays; of equals, the lower address stays
                    if ( area > oarea * 1.02f || ( area >= oarea * 0.98f && node < other ) )
                        keepThis = true;
                    else
                        ++covered;
                }
            if ( !keepThis && covered >= 7 )
                stacked.push_back( node );
        }
        if ( stacked.empty() )
        {
            Sys_Printf( "Terrain Sculpt: no terrain patch lies on top of another.\n" );
            SetStatus( "No stacked terrain found." );
            return 0;
        }
        Select_Deselect( 1 );
        for ( size_t i = 0; i < stacked.size(); ++i )
            Select_Brush( stacked[i], 0, 0, 0 );
        Sel_InvalidateFromLegacy();
        g_nUpdateBits = -1;
        Sys_Printf( "Terrain Sculpt: %i terrain patch%s on top of other terrain - SELECTED (nothing deleted). "
                    "Check them, then press Delete.\n", (int)stacked.size(), stacked.size() == 1 ? " lies" : "es lie" );
        SetStatus( "%i stacked terrain patch%s selected - check, then Delete.", (int)stacked.size(), stacked.size() == 1 ? "" : "es" );
        return (int)stacked.size();
    }

    // ── join adjacent sheets ─────────────────────────────────────────────────
    bool SameMaterials( const patchMesh_t *a, const patchMesh_t *b )
    {
        if ( a->texture.radMtl != b->texture.radMtl || a->lightmap.radMtl != b->lightmap.radMtl )
            return false;
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( strcmp( a->kiwiLayer[k], b->kiwiLayer[k] ) != 0 )
                return false;
        return true;
    }

    // Can B be appended to A along A's +X edge (axis 0) or +Y edge (axis 1)?  Both must
    // be sheets with the same material set, the same point count along the shared edge,
    // the same cell size, and the merged side must fit in 16 points.
    bool JoinableAlong( const patchMesh_t *a, const patchMesh_t *b, int axis )
    {
        if ( !GridIsSheet( a ) || !GridIsSheet( b ) || !SameMaterials( a, b ) )
            return false;
        const int wa = axis == 0 ? a->width : a->height, wb = axis == 0 ? b->width : b->height;
        const int ha = axis == 0 ? a->height : a->width, hb = axis == 0 ? b->height : b->width;
        if ( ha != hb || wa + wb - 1 > 16 )
            return false;
        for ( int j = 0; j < ha; ++j )
        {
            const float *pa = axis == 0 ? a->ctrl[wa - 1][j].xyz : a->ctrl[j][wa - 1].xyz;
            const float *pb = axis == 0 ? b->ctrl[0][j].xyz      : b->ctrl[j][0].xyz;
            if ( fabsf( pa[0] - pb[0] ) > 0.5f || fabsf( pa[1] - pb[1] ) > 0.5f )
                return false;
        }
        return fabsf( CellSizeOf( a ) - CellSizeOf( b ) ) <= 0.5f;
    }

    // Append B to A along `axis` (B's first row/column is A's last).
    void MergeInto( patchMesh_t *a, const patchMesh_t *b, int axis )
    {
        if ( axis == 0 )
        {
            const int wa = a->width;
            for ( int i = 1; i < b->width; ++i )
                for ( int j = 0; j < a->height; ++j )
                    a->ctrl[wa - 1 + i][j] = b->ctrl[i][j];
            a->width = wa + b->width - 1;
        }
        else
        {
            const int ha = a->height;
            for ( int i = 0; i < a->width; ++i )
                for ( int j = 1; j < b->height; ++j )
                    a->ctrl[i][ha - 1 + j] = b->ctrl[i][j];
            a->height = ha + b->height - 1;
        }
    }

    void SelectedSheets( std::vector<selbrush_t *> &out )
    {
        out.clear();
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( PatchEligible( b ) && GridIsSheet( b->patch->def ) )
                out.push_back( b );
    }

    bool FindJoinPair( const std::vector<selbrush_t *> &sheets, size_t *ia, size_t *ib, int *axis )
    {
        for ( size_t x = 0; x < sheets.size(); ++x )
            for ( size_t y = 0; y < sheets.size(); ++y )
            {
                if ( x == y )
                    continue;
                for ( int ax = 0; ax < 2; ++ax )
                    if ( JoinableAlong( sheets[x]->patch->def, sheets[y]->patch->def, ax ) )
                    {
                        *ia = x; *ib = y; *axis = ax;
                        return true;
                    }
            }
        return false;
    }

    // ── legacy duplicate stacks -> layer slots ───────────────────────────────
    bool GridsMatch( const patchMesh_t *a, const patchMesh_t *b )
    {
        if ( a == b || a->width != b->width || a->height != b->height || a->width <= 0 )
            return false;
        for ( int i = 0; i < a->width; ++i )
            for ( int j = 0; j < a->height; ++j )
                for ( int k = 0; k < 3; ++k )
                    if ( fabsf( a->ctrl[i][j].xyz[k] - b->ctrl[i][j].xyz[k] ) > 0.05f )
                        return false;
        return true;
    }

    // Position in the owning entity's DEF-list (= .map order = compiler layer order).
    int DefOrder( selbrush_t *node )
    {
        if ( !node || !node->owner || !node->owner->def || !node->def )
            return 0x7fffffff;
        entity_s_def *ed = (entity_s_def *)node->owner->def;
        brush_t *sentinel = (brush_t *)&ed->def;
        int idx = 0;
        for ( brush_t *b = (brush_t *)ed->brushes.prev; b && b != sentinel && idx < 1000000; b = b->onext, ++idx )
            if ( b == node->def )
                return idx;
        return 0x7fffffff;
    }

    // For each selected patch: fold its overlapping duplicate patches (the old CoD4
    // hand-made layering) into layer slots + weights and delete them.
    void CollapseDuplicates()
    {
        int folded = 0, deleted = 0;
        std::vector<selbrush_t *> victims;
        std::vector<selbrush_t *> bases;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( PatchEligible( b ) )
                bases.push_back( b );
        for ( size_t bi = 0; bi < bases.size(); ++bi )
        {
            selbrush_t *base = bases[bi];
            bool skip = false;
            for ( size_t v = 0; v < victims.size() && !skip; ++v ) skip = ( victims[v] == base );
            if ( skip )
                continue;
            std::vector<selbrush_t *> twins;
            for ( int pass = 0; pass < 2; ++pass )
            {
                selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
                for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                    if ( b != base && NodeIsPatch( b ) && GridsMatch( base->patch->def, b->patch->def ) )
                        twins.push_back( b );
            }
            if ( twins.empty() )
                continue;
            // The lowest def-order member is the base; the rest become slots in order.
            twins.push_back( base );
            for ( size_t i = 1; i < twins.size(); ++i )
                for ( size_t j = i; j > 0 && DefOrder( twins[j] ) < DefOrder( twins[j - 1] ); --j )
                {
                    selbrush_t *t = twins[j]; twins[j] = twins[j - 1]; twins[j - 1] = t;
                }
            selbrush_t *root = twins[0];
            patchMesh_t *rd = root->patch->def;
            EditPatchBegin( rd, "collapse terrain layers" );
            if ( UsedSlotCount( rd ) == 0 )
                for ( int i = 0; i < rd->width; ++i )
                    for ( int j = 0; j < rd->height; ++j )
                        *(unsigned int *)&rd->ctrl[i][j].vert_color = 0u;
            for ( size_t t = 1; t < twins.size(); ++t )
            {
                const int slot = FirstFreeSlot( rd );
                if ( slot < 0 )
                    break;
                patchMesh_t *td = twins[t]->patch->def;
                strncpy( rd->kiwiLayer[slot], BaseMaterialName( td ), 63 );
                rd->kiwiLayer[slot][63] = '\0';
                for ( int i = 0; i < rd->width; ++i )
                    for ( int j = 0; j < rd->height; ++j )
                        ( (byte *)&rd->ctrl[i][j].vert_color )[slot] = ( (byte *)&td->ctrl[i][j].vert_color )[3];
                victims.push_back( twins[t] );
                ++folded;
            }
            EditPatchEnd( rd, false );
        }
        if ( !victims.empty() )
        {
            Select_Deselect( 1 );
            for ( size_t i = 0; i < victims.size(); ++i )
            {
                ForgetDef( victims[i]->patch->def );
                Select_Brush( victims[i], 0, 0, 0 );
            }
            deleted = (int)victims.size();
            DeleteSelectionWithUndo( "collapse terrain layers" );
        }
        Sys_Printf( "Terrain Sculpt: folded %i duplicate patch%s into layer slots, deleted %i.\n",
                    folded, folded == 1 ? "" : "es", deleted );
        g_nUpdateBits = -1;
    }

    // ── cursor / ring ────────────────────────────────────────────────────────
    void RebuildRing()
    {
        s_ringCount = 0;
        if ( !s_cursorHave )
            return;
        LiveCursorNode();                        // drops a node freed since the pick (DropToSurface reads it)
        const int n = 32;
        const float rot = s_squareRot * KTER_PI / 180.0f;
        const float cr = cosf( rot ), sr = sinf( rot );
        for ( int i = 0; i < n && i < KTER_RING_MAX; ++i )
        {
            float ux, uy;
            if ( s_shape == KTER_SQUARE )
            {
                const int side = i / 8, k = i % 8;
                const float t = -1.0f + 2.0f * (float)k / 8.0f;
                switch ( side )
                {
                case 0:  ux = t;     uy = -1.0f; break;
                case 1:  ux = 1.0f;  uy = t;     break;
                case 2:  ux = -t;    uy = 1.0f;  break;
                default: ux = -1.0f; uy = -t;    break;
                }
                const float rx = ux * cr - uy * sr;
                const float ry = ux * sr + uy * cr;
                ux = rx; uy = ry;
            }
            else
            {
                const float a = 2.0f * KTER_PI * (float)i / (float)n;
                ux = cosf( a ); uy = sinf( a );
            }
            for ( int ring = 0; ring < 2; ++ring )
            {
                const float r = ring == 0 ? s_outer : s_inner;
                float *out = ring == 0 ? s_ringOuter[i] : s_ringInner[i];
                out[0] = s_cursor[0] + ux * r;
                out[1] = s_cursor[1] + uy * r;
                float z;
                out[2] = DropToSurface( out[0], out[1], s_cursor[2], &z ) ? z + 1.0f : s_cursor[2] + 1.0f;
            }
            s_ringCount = i + 1;
        }
    }

    void ClearCursor()
    {
        if ( s_cursorHave )
        {
            s_cursorHave = false;
            s_ringCount  = 0;
            g_nUpdateBits |= W_CAMERA;
        }
        s_cursorKind = KCUR_NONE;
    }

    bool UpdateCursor( int imgX, int imgY, byte outColor[4] )
    {
        float hit[3];
        if ( !PickCursor( imgX, imgY, hit, outColor ) )
        {
            ClearCursor();
            return false;
        }
        const bool moved = !s_cursorHave
                        || fabsf( hit[0] - s_cursor[0] ) > 0.01f
                        || fabsf( hit[1] - s_cursor[1] ) > 0.01f
                        || fabsf( hit[2] - s_cursor[2] ) > 0.01f;
        s_cursorHave = true;
        memcpy( s_cursor, hit, sizeof( hit ) );
        if ( moved || s_stroke )
        {
            RebuildRing();
            g_nUpdateBits |= W_CAMERA;
        }
        return true;
    }

    // ── stroke lifecycle ─────────────────────────────────────────────────────
    void ApplyStroke()
    {
        if ( !s_stroke || !s_cursorHave || s_accumDt <= 0.0f )
            return;
        const float sign = s_modCtrl ? -1.0f : 1.0f;
        int n = 1;
        if ( s_haveLastCenter )
        {
            const float dx = s_cursor[0] - s_lastCenter[0];
            const float dy = s_cursor[1] - s_lastCenter[1];
            const float dist = sqrtf( dx * dx + dy * dy );
            const float spacing = s_outer * 0.25f;
            if ( spacing > 0.0f )
                n = (int)ceilf( dist / spacing );
            if ( n < 1 ) n = 1;
            if ( n > 6 ) n = 6;
        }
        // Every height stroke first brings the mesh under the ring to a density the ring can
        // work with - also with "Allow terrain creation" on (ResolveLattice anchors on the
        // largest sheet in reach, so refined pieces do not shift the chunk lattice).
        if ( HeightStroke() )
            RefineUnderRing( s_haveLastCenter ? s_lastCenter : s_cursor, s_cursor );
        s_reachCount = 0;                        // the rings of THIS flush (InStrokeReach)
        const float dtEach = s_accumDt / (float)n;
        for ( int k = 1; k <= n; ++k )
        {
            float c[3];
            const float f = (float)k / (float)n;
            for ( int a = 0; a < 3; ++a )
                c[a] = s_haveLastCenter ? s_lastCenter[a] + ( s_cursor[a] - s_lastCenter[a] ) * f : s_cursor[a];
            Stamp( c, OpForStroke(), sign, dtEach );
            MarkRidersReached( c );
            if ( s_reachCount < 8 )
            {
                memcpy( s_reachCenters[s_reachCount], c, sizeof( c ) );
                ++s_reachCount;
            }
        }
        memcpy( s_lastCenter, s_cursor, sizeof( s_cursor ) );
        s_haveLastCenter = true;
        s_accumDt = 0.0f;
        if ( CreationAllowed() && !s_modShift && !s_modCtrl )
            ExpandUnderBrush();
        FlushDirty();
        s_reachCount = 0;                    // other flushes (stroke end, tools) are unrestricted
        CarryRiders();                       // after the seams: riders read final heights
    }

    // ── height gradient ("heatmap") while a height tool is armed ─────────────
    // The base VB run of every patch is re-uploaded with a blue→cyan→green→yellow→red
    // colour by control-point height over a flat opaque material, so relief reads at a
    // glance instead of hiding under the texture.  Paint modes keep the real look.
    // Armed Texture paint: the layer runs draw as flat slot colours (red, green, blue,
    // yellow) at the painted weight instead of the blended second texture, so the
    // brushwork is unmistakable while painting.
    bool WeightViewActive()
    {
        return s_armed && s_weightView && s_tool == KTER_TEXTURE;
    }

    bool HeatmapActive()
    {
        const bool heightTool = s_tool == KTER_RAISE || s_tool == KTER_SETHEIGHT || s_tool == KTER_SMOOTH
                             || s_tool == KTER_NOISE || s_tool == KTER_TRIM;
        // KIWI (2026-09-17, user: "add an option to turn on height colors while not
        // armed"): the always-on view shows the gradient with the tool put away, for
        // reading relief while placing models or comparing against an elevation map.  It
        // still yields to an ARMED texture / blend / grass tool - those need the real look.
        if ( s_heatAlways && !( s_armed && !heightTool ) )
            return true;
        return s_armed && s_heatmap && heightTool;
    }

    // Min/max control-point Z over the eligible patches; true when the range moved.
    bool ComputeHeatRange()
    {
        float lo = FLT_MAX, hi = -FLT_MAX;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !PatchEligible( b ) )
                    continue;
                const patchMesh_t *def = b->patch->def;
                for ( int i = 0; i < def->width; ++i )
                    for ( int j = 0; j < def->height; ++j )
                    {
                        const float z = def->ctrl[i][j].xyz[2];
                        if ( z < lo ) lo = z;
                        if ( z > hi ) hi = z;
                    }
            }
        }
        if ( lo > hi )
        {
            lo = 0.0f;
            hi = 1.0f;
        }
        if ( hi - lo < 16.0f )                 // a flat sheet: a 16-unit band keeps the colour stable
        {
            const float mid = ( lo + hi ) * 0.5f;
            lo = mid - 8.0f;
            hi = mid + 8.0f;
        }
        // Hysteresis: re-tinting means re-uploading EVERY patch (1,286 VBs on a chunked
        // map), so only a range change worth seeing - 5 % of the span or 32 units,
        // whichever is larger - triggers it; a Raise stroke nudging the peak does not.
        const float tol = ( hi - lo ) * 0.05f > 32.0f ? ( hi - lo ) * 0.05f : 32.0f;
        const bool moved = !s_heatValid || fabsf( lo - s_heatMinZ ) > tol || fabsf( hi - s_heatMaxZ ) > tol;
        if ( !moved && s_heatValid )
            return false;                      // keep the old range: the colours stay put
        s_heatMinZ  = lo;
        s_heatMaxZ  = hi;
        s_heatValid = true;
        return moved;
    }

    // Packed BGRA (the patch VB order) for a 0..1 height fraction.
    unsigned int HeatColor( float t )
    {
        t = ClampF( t, 0.0f, 1.0f );
        float r, g, b;
        if ( t < 0.25f )      { const float k = t / 0.25f;            r = 0.0f;     g = k;        b = 1.0f; }
        else if ( t < 0.5f )  { const float k = ( t - 0.25f ) / 0.25f; r = 0.0f;     g = 1.0f;     b = 1.0f - k; }
        else if ( t < 0.75f ) { const float k = ( t - 0.5f ) / 0.25f;  r = k;        g = 1.0f;     b = 0.0f; }
        else                  { const float k = ( t - 0.75f ) / 0.25f; r = 1.0f;     g = 1.0f - k; b = 0.0f; }
        const unsigned B = (unsigned)( b * 255.0f + 0.5f ), G = (unsigned)( g * 255.0f + 0.5f ), R = (unsigned)( r * 255.0f + 0.5f );
        return B | ( G << 8 ) | ( R << 16 ) | 0xFF000000u;
    }

    // Re-upload every patch's visuals (the VB colours and the run material come from
    // LayerUpload).  Two facts shape this (both learned the hard way on 2026-09-17):
    //
    //  1. A patch instance rebuilds its visuals LAZILY at draw time whenever
    //     inst->version != def->version (pmesh.cpp Patch_Fill).  So a re-tint needs only
    //     `++def->version` - NOT Patch_Rebuild, which also re-tessellates the curveDef and
    //     was most of the old 1-2 s stall.  Off-screen patches then re-upload when they come
    //     into view, for free.
    //  2. Every frame in which ANY patch re-uploads, the world window's mesh runs are
    //     rebuilt ("build mesh runs" in Tracy).  That is cheap while the terrain wears the
    //     single heat material and ~50-100 ms while it is textured.  My first fix spread
    //     every re-tint over ~200 frames, so disarming (heat -> textures) sat at 10 fps for
    //     twenty seconds (user: "when I turn off sculpt mode, the editor now lags like
    //     crazy ... 'build mesh runs' zone ... height colors removes the lag").
    //
    // Hence: a MODE change (arm, disarm, tool, toggle - the material changes, textures may
    // be involved) bumps every version AT ONCE: one frame of re-upload, one run rebuild.
    // Only a RANGE change while the heat view stays on (the original "re-scaling lags"
    // report) uses the wave, where each frame's run rebuild costs almost nothing.
    std::set<const patchMesh_t *> s_retintDone;     // raw def pointers as keys only, never dereferenced
    bool s_retintPending = false;
    const int KTER_RETINT_PER_FRAME = 64;           // wave: patches re-tinted per frame (~20 frames on powerplant)

    void RebuildAllPatchVisuals()
    {
        s_retintPending = false;
        s_retintDone.clear();
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( NodeIsPatch( b ) )
                    ++b->patch->def->version;
        }
        g_nUpdateBits = -1;
    }

    // Heat range moved while the heat view stays on: re-tint as a wave from the camera out.
    void ScheduleRetintWave()
    {
        s_retintDone.clear();
        s_retintPending = true;
        g_nUpdateBits = -1;
    }

    void RetintTick()
    {
        // Always-on view with the tool put away: nothing ends a stroke to refresh the
        // range, so poll it about once a second (a plain walk over the control points,
        // well under a millisecond).  A map load, an undo or a Move of terrain then
        // re-ranges the colours; the 5 % hysteresis keeps small edits from re-tinting.
        if ( s_heatAlways && !s_armed && !s_retintPending )
        {
            static DWORD s_lastPoll = 0;
            const DWORD now = ::GetTickCount();
            if ( now - s_lastPoll > 1000u )
            {
                s_lastPoll = now;
                if ( ComputeHeatRange() )
                    ScheduleRetintWave();
            }
        }
        if ( !s_retintPending )
            return;
        if ( !HeatmapActive() )
        {
            // The heat view went away mid-wave: the mode change already bumped everything.
            s_retintPending = false;
            s_retintDone.clear();
            return;
        }
        const camera_s *c = Ed_Camera();
        std::vector<std::pair<float, selbrush_t *> > todo;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !NodeIsPatch( b ) || s_retintDone.count( b->patch->def ) )
                    continue;
                float d2 = 0.0f;
                if ( b->def )
                    for ( int k = 0; k < 3; ++k )
                    {
                        const float mid = ( b->def->mins[k] + b->def->maxs[k] ) * 0.5f - c->origin[k];
                        d2 += mid * mid;
                    }
                todo.push_back( std::make_pair( d2, b ) );
            }
        }
        if ( todo.empty() )
        {
            s_retintPending = false;
            s_retintDone.clear();
            return;
        }
        std::sort( todo.begin(), todo.end() );

        // The cost lands in the draw (the lazy visual rebuild), not here, so the budget is
        // a patch count rather than a timer.
        int done = 0;
        for ( size_t i = 0; i < todo.size() && done < KTER_RETINT_PER_FRAME; ++i )
        {
            patchMesh_t *def = todo[i].second->patch->def;
            if ( !s_retintDone.insert( def ).second )
                continue;                       // two nodes of one def
            ++def->version;                     // visuals re-upload at the next draw
            ++done;
        }
        g_nUpdateBits = -1;                     // keep frames coming until the wave ends
    }

    // Arm / tool / toggle transitions: refresh the range and the uploads.
    void HeatmapRefresh()
    {
        if ( HeatmapActive() )
            ComputeHeatRange();
        RebuildAllPatchVisuals();
    }

    void EndStroke()
    {
        if ( !s_stroke )
            return;
        if ( s_tool == KTER_TRIM )
        {
            s_stroke = false;
            g_nUpdateBits = -1;
            return;
        }
        FlushDirty();
        CarryRiders();
        s_carried = 0;
        for ( size_t i = 0; i < s_riders.size(); ++i )
            if ( s_riders[i].undoAdded )
                ++s_carried;
        s_riders.clear();
        Patch_PaintFinish( &selected_brushes );
        Patch_PaintFinish( &active_brushes );
        if ( s_undoOpen )
            Undo_End();
        // The gradient range follows the terrain: re-tint everything only when the
        // stroke pushed the extremes (the touched patches re-uploaded already).
        if ( HeatmapActive() && ComputeHeatRange() )
            ScheduleRetintWave();               // heat stays on: the cheap, smooth path
        s_undoOpen = false;
        s_stroke   = false;
        g_nUpdateBits = -1;
        if ( s_touched || s_created || s_facesPainted )
            SetStatus( "Armed. Last stroke: %i stamp%s over %i patch%s, %i chunk%s laid, %i brush face%s painted, %i object%s carried.",
                       s_stamps, s_stamps == 1 ? "" : "s", s_touched, s_touched == 1 ? "" : "es",
                       s_created, s_created == 1 ? "" : "s", s_facesPainted, s_facesPainted == 1 ? "" : "s",
                       s_carried, s_carried == 1 ? "" : "s" );
        else if ( CreationAllowed() )
            SetStatus( "Armed. The stroke reached no control point and every cell under it was covered." );
        else
            SetStatus( "Armed. The stroke reached no control point (grow the radius or select the patch)." );
        if ( s_coarseSpill )
        {
            // Said once per stroke, in the status line too, because the effect looks like a bug.
            SetStatus( "Armed. The terrain under the ring is too coarse for it: moved points drag their whole cells, so "
                       "ground up to one cell past the ring leaned. Tessellate that patch finer to confine strokes." );
            Sys_Printf( "Terrain Sculpt: the grid under the ring is coarser than the ring can contain; the change "
                        "extends up to a full cell past it there. Tessellate that patch to a smaller cell size (your "
                        "call - nothing is re-gridded automatically unless 'Refine the mesh under the ring' is ticked).\n" );
            s_coarseSpill = false;
        }
        if ( s_layersAdded || s_layersFull )
        {
            const bool shared = s_tool == KTER_BLEND || ( s_tool == KTER_TEXTURE && s_modShift );
            if ( shared )
                Sys_Printf( "Terrain Sculpt: blending carried %i layer%s onto neighbouring patch%s that did not have "
                            "%s yet%s.\n", s_layersAdded, s_layersAdded == 1 ? "" : "s",
                            s_layersAdded == 1 ? "" : "es", s_layersAdded == 1 ? "it" : "them",
                            s_layersFull ? " (a patch that already carries 4 layers cannot take another - the hard "
                                           "line stays there)" : "" );
            else
                Sys_Printf( "Terrain Sculpt: '%s' added as a layer on %i patch%s%s.\n",
                            s_paintMaterial, s_layersAdded, s_layersAdded == 1 ? "" : "es",
                            s_layersFull ? " (some patches already carry 4 layers and were skipped)" : "" );
        }
        s_stamps = s_touched = s_created = 0;
    }

    void SetArmed( bool armed )
    {
        if ( s_armed == armed )
            return;
        if ( !armed )
            EndStroke();
        s_armed = armed;
        ClearCursor();
        HeatmapRefresh();                      // armed views on/off: every patch re-uploads
        KiwiGrass_SetArmed( armed && s_tool == KTER_GRASS );
        SetStatus( armed ? ( s_tool == KTER_GRASS ? "Armed. LMB in the 3D camera scatters; Esc disarms."
                                                  : "Armed. LMB in the 3D camera sculpts; Esc disarms." )
                         : "Disarmed." );
        g_nUpdateBits |= W_CAMERA;
    }

    void SetTool( int tool )
    {
        if ( tool < 0 || tool >= KTER_TOOL_COUNT )
            return;
        const bool wasHeat = HeatmapActive(), wasWeight = WeightViewActive();
        s_tool = tool;
        KiwiGrass_SetArmed( s_armed && s_tool == KTER_GRASS );
        if ( s_armed )
            ClearCursor();
        if ( wasHeat != HeatmapActive() || wasWeight != WeightViewActive() )
            HeatmapRefresh();
        g_nUpdateBits = -1;          // wireframe hiding depends on the tool
    }

    void SyncSoftSelect()
    {
        if ( s_softSelect )
        {
            AdvPatchEdit_ApplySlotValue( 0, s_inner );
            AdvPatchEdit_ApplySlotValue( 1, s_outer );
            AdvPatchEdit_ApplySlotValue( 2, 1.0f );
            AdvPatchEdit_SetMode( 1 );
        }
        else if ( AdvPatchEdit_GetMode() == 1 )
            AdvPatchEdit_SetMode( 6 );
    }

    void RadiusStep( float factor )
    {
        s_outer = ClampF( s_outer * factor, 4.0f, 12288.0f );
        s_inner = ClampF( s_inner * factor, 0.0f, s_outer );
        Save();
        SyncSoftSelect();
        RebuildRing();
        g_nUpdateBits |= W_CAMERA;
    }

    // A height field with the viewport's numeric grammar (kiwi_numeric): "12ft 6in",
    // "3yd", "1/8", "-64" (bare numbers are inches, like the transform HUD).  Idle it
    // shows KiwiUnits_Format; a click opens the typed text, Enter or clicking away
    // commits, Esc reverts.  True when a new value landed in *world.
    bool UnitInputWorld( const char *label, float *world, float width )
    {
        static char    s_buf[64];
        static ImGuiID s_editing = 0;
        const ImGuiID  id = ImGui::GetID( label );
        char shown[64];
        KiwiUnits_Format( shown, sizeof( shown ), *world );
        const bool editing = ( s_editing == id );
        ImGui::SetNextItemWidth( width );
        ImGui::InputText( label, editing ? s_buf : shown, editing ? sizeof( s_buf ) : sizeof( shown ),
                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Math and units: 12ft 6in, 3yd, 1/8, -64  (bare numbers are inches)" );
        bool committed = false;
        if ( !editing && ImGui::IsItemActivated() )
        {
            strncpy( s_buf, shown, sizeof( s_buf ) - 1 );
            s_buf[sizeof( s_buf ) - 1] = '\0';
            s_editing = id;
        }
        else if ( editing )
        {
            if ( ImGui::IsItemDeactivatedAfterEdit() )
            {
                float disp = 0.0f;
                if ( KiwiNum_EvalDisplay( s_buf, &disp ) )
                {
                    *world = Units_FromDisplay( disp );
                    committed = true;
                }
                else
                    Sys_Printf( "Terrain Sculpt: '%s' is not a length (try 12ft 6in, 3yd, 1/8).\n", s_buf );
            }
            if ( !ImGui::IsItemActive() )
                s_editing = 0;
        }
        return committed;
    }

    bool AcceptMaterialDrop( char *outName, int outSize )
    {
        if ( !ImGui::BeginDragDropTarget() )
            return false;
        bool got = false;
        if ( const ImGuiPayload *pl = ImGui::AcceptDragDropPayload( KMTL_PAYLOAD ) )
        {
            strncpy( outName, (const char *)pl->Data, (size_t)outSize - 1 );
            outName[outSize - 1] = '\0';
            got = outName[0] != 0;
        }
        ImGui::EndDragDropTarget();
        return got;
    }

    void RebuildVisibleLayerPatches()
    {
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( NodeIsPatch( b ) && UsedSlotCount( b->patch->def ) )
                {
                    Patch_Rebuild( b->patch->def, 0 );
                    ++b->patch->def->version;
                }
        }
        g_nUpdateBits = -1;
    }

    // ── the Texture paint section of the panel ───────────────────────────────
    void DrawTexturePaint( bool &changed )
    {
        char dropped[128];

        // The brush's material: whatever terrain the brush touches gets it as a layer.
        ImGui::SeparatorText( "Paint with" );
        {
            char label[160];
            if ( s_paintBase )
                _snprintf( label, sizeof( label ), "Erase to base" );
            else if ( s_paintMaterial[0] )
                _snprintf( label, sizeof( label ), "%s", s_paintMaterial );
            else
                _snprintf( label, sizeof( label ), "(drop a material here)" );
            label[sizeof( label ) - 1] = '\0';
            ImGui::Button( label, ImVec2( 320.0f, 30.0f ) );
            if ( AcceptMaterialDrop( dropped, sizeof( dropped ) ) )
            {
                strncpy( s_paintMaterial, dropped, sizeof( s_paintMaterial ) - 1 );
                s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
                s_paintBase = false;
                changed = true;
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Drop a thumbnail from the Textures tab, click a layer row below, or press\n"
                                   "I over terrain or a brush face while armed (eyedropper).\n"
                                   "Any terrain the brush touches gets this material as a layer\n"
                                   "(added on first touch, 4 per patch) - no selection needed." );
            ImGui::SameLine();
            if ( ImGui::Button( "Use current" ) )
            {
                qtexture_s *q = g_qeglobals.random_texture_stuff[0].mtl.radMtl;
                if ( q && q->name )
                {
                    strncpy( s_paintMaterial, q->name, sizeof( s_paintMaterial ) - 1 );
                    s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
                    s_paintBase = false;
                    changed = true;
                }
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Take the texture browser's current material." );
            if ( ImGui::Checkbox( "Erase to base (paint every layer out)", &s_paintBase ) )
                changed = true;
            ImGui::SameLine();
            if ( ImGui::Checkbox( "Paint brush faces too", &s_paintBrushes ) )
                changed = true;
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Upward brush faces whose centre is inside the ring take the material\n"
                                   "whole (no weights on a face), so flat spots can be brushes instead of\n"
                                   "terrain and still be painted with the same tool. The cursor lands on\n"
                                   "brushes as well as terrain; Ctrl / Erase to base leave faces alone." );
            ImGui::TextDisabled( s_paintBase
                ? "LMB thins every layer under the brush back to the base material."
                : "LMB paints the material in, Ctrl+LMB paints it out, Shift+LMB smooths it." );
            if ( ImGui::Checkbox( "Show this material's weight in red while armed", &s_weightView ) )
            {
                Save();
                HeatmapRefresh();
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "While painting, the layer that carries the brush material draws red at its\n"
                                   "painted weight (on top of any reference pictures too); every other layer\n"
                                   "keeps its blended texture, so dirt, sand and grass can be layered while the\n"
                                   "one being painted stays obvious. Disarm to see the real look." );
        }

        ImGui::SeparatorText( "Layers of the selected patch" );
        selbrush_t *node = FirstSelectedPatch();
        s_bandTreeOpen = false;
        if ( !node )
        {
            ImGui::TextDisabled( "Select a patch to inspect, swap or remove its layers (painting needs no selection)." );
            return;
        }
        patchMesh_t *def = node->patch->def;

        // Base row: click = paint the base (erase layers).
        {
            char label[160];
            _snprintf( label, sizeof( label ), "Base  %s", BaseMaterialName( def ) );
            label[sizeof( label ) - 1] = '\0';
            if ( ImGui::RadioButton( label, s_paintBase ) )
            {
                s_paintBase = true;
                changed = true;
            }
        }
        // Layer rows.
        for ( int k = 0; k < KTER_SLOTS; ++k )
        {
            if ( !SlotUsed( def, k ) )
                continue;
            ImGui::PushID( k );
            char label[160];
            _snprintf( label, sizeof( label ), "L%i  %s", k + 1, def->kiwiLayer[k] );
            label[sizeof( label ) - 1] = '\0';
            if ( ImGui::RadioButton( label, !s_paintBase && !_stricmp( s_paintMaterial, def->kiwiLayer[k] ) ) )
            {
                strncpy( s_paintMaterial, def->kiwiLayer[k], sizeof( s_paintMaterial ) - 1 );
                s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
                s_paintBase = false;
                changed = true;
            }
            if ( AcceptMaterialDrop( dropped, sizeof( dropped ) ) )
            {
                s_blendTwins.erase( std::string( dropped ) );
                s_twinErr.erase( std::string( dropped ) );
                SwapLayerMaterial( def, k, dropped );
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Remove" ) )
            {
                RemoveLayerSlot( def, k );
                ImGui::PopID();
                break;
            }
            if ( s_previewBlend )
            {
                std::map<std::string, std::string>::iterator te = s_twinErr.find( def->kiwiLayer[k] );
                std::map<std::string, Material *>::iterator tw = s_blendTwins.find( def->kiwiLayer[k] );
                ImGui::SameLine();
                if ( te != s_twinErr.end() )
                {
                    ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.3f, 1.0f ), "(opaque)" );
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "No blend preview: %s", te->second.c_str() );
                }
                else if ( tw != s_blendTwins.end() && tw->second )
                    ImGui::TextDisabled( "(blended)" );
                else
                    ImGui::TextDisabled( "(preview pending)" );
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled( "Click a row to paint with it; drop onto a row to swap its material." );

        // Add a layer by dragging a thumbnail out of the Textures tab.
        const bool full = FirstFreeSlot( def ) < 0;
        ImGui::BeginDisabled( full );
        ImGui::Button( full ? "All 4 layer slots used" : "Drop a material here to add a layer",
                       ImVec2( 320.0f, 36.0f ) );
        ImGui::EndDisabled();
        if ( !full && AcceptMaterialDrop( dropped, sizeof( dropped ) ) )
            AddLayerSlot( def, dropped );
        ImGui::TextDisabled( "Drop onto a layer row above to swap its material." );
        if ( ImGui::Button( "Fold overlapping duplicate patches into layers" ) )
            CollapseDuplicates();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Old-style CoD4 terrain layering stacks identical patches. This folds\n"
                               "each stack found among the selected patches into one patch with\n"
                               "layer slots (weights from the duplicates' alpha) and deletes the copies." );

        // Auto-transition.
        if ( UsedSlotCount( def ) && ImGui::TreeNode( "Auto-transition by height / slope" ) )
        {
            s_bandTreeOpen = true;
            ImGui::TextDisabled( "Each layer fills between its two heights, up to its max slope. Live." );
            bool edited = false;
            for ( int k = 0; k < KTER_SLOTS; ++k )
            {
                if ( !SlotUsed( def, k ) )
                    continue;
                kterBand_t &b = BandFor( def, k );
                ImGui::PushID( k + 100 );
                char hdr[160];
                _snprintf( hdr, sizeof( hdr ), "L%i  %s", k + 1, def->kiwiLayer[k] );
                hdr[sizeof( hdr ) - 1] = '\0';
                edited |= ImGui::Checkbox( hdr, &b.enabled );
                ImGui::BeginDisabled( !b.enabled );
                ImGui::SetNextItemWidth( 90.0f );
                edited |= ImGui::DragFloat( "from Z", &b.minZ, 4.0f, -65536.0f, 65536.0f, "%.0f" );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 90.0f );
                edited |= ImGui::DragFloat( "to Z", &b.maxZ, 4.0f, -65536.0f, 65536.0f, "%.0f" );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 90.0f );
                edited |= ImGui::DragFloat( "max slope", &b.maxSlope, 0.5f, 0.0f, 90.0f, "%.0f deg" );
                if ( b.maxZ < b.minZ ) b.maxZ = b.minZ;
                b.fadeZ = s_bandFade; b.fadeSlope = 8.0f; b.minSlope = 0.0f;
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::SetNextItemWidth( 120.0f );
            edited |= ImGui::DragFloat( "Blend width", &s_bandFade, 1.0f, 1.0f, 4096.0f, "%.0f" );
            if ( edited )
            {
                if ( !s_autoUndoOpen )
                {
                    Undo_ClearRedo();
                    Undo_GeneralStart( "terrain auto-transition" );
                    Patch_Paint( &selected_brushes );
                    Patch_Paint( &active_brushes );
                    s_autoUndoOpen = true;
                }
                ApplyAutoTransition( def );
            }
            if ( s_autoUndoOpen && !ImGui::IsAnyItemActive() )
            {
                Patch_PaintFinish( &selected_brushes );
                Patch_PaintFinish( &active_brushes );
                Undo_End();
                s_autoUndoOpen = false;
            }
            if ( ImGui::Button( "Apply to all selected patches" ) )
            {
                Undo_ClearRedo();
                Undo_GeneralStart( "terrain auto-transition" );
                Patch_Paint( &selected_brushes );
                Patch_Paint( &active_brushes );
                for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
                {
                    if ( !PatchEligible( b ) || b == node )
                        continue;
                    for ( int k = 0; k < KTER_SLOTS; ++k )
                        if ( SlotUsed( b->patch->def, k ) )
                            BandFor( b->patch->def, k ) = BandFor( def, k );
                    ApplyAutoTransition( b->patch->def );
                }
                ApplyAutoTransition( def );
                Patch_PaintFinish( &selected_brushes );
                Patch_PaintFinish( &active_brushes );
                Undo_End();
            }
            ImGui::TreePop();
        }

        changed |= ImGui::SliderFloat( "Paint weight", &s_blendWeight, 0.0f, 1.0f, "%.2f" );
        if ( ImGui::Checkbox( "Preview blending in the camera", &s_previewBlend ) )
        {
            changed = true;
            RebuildVisibleLayerPatches();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Each layer draws as one extra alpha-blended run with a twin material\n"
                               "(kiwi_blend_<name>, written once next to the real one). The .map keeps\n"
                               "one patch with 'kiwilayer' lines; cod4map expands them at compile time." );
    }
}

// ── panel ────────────────────────────────────────────────────────────────────
void KiwiTerrain_MenuItem()
{
    Load();
    if ( ImGui::Checkbox( "Terrain Sculpt (Y)", &s_show ) && !s_show )
        SetArmed( false );
}

void KiwiTerrain_TogglePanel()
{
    Load();
    s_show = !s_show;
    if ( !s_show )
        SetArmed( false );
    extern void KiwiWindows_SyncMenu();
    KiwiWindows_SyncMenu();
}

bool KiwiTerrain_PanelVisible()
{
    Load();
    return s_show;
}

void KiwiTerrain_OpenWithTool( int tool )
{
    Load();
    s_show = true;
    SetTool( tool );
    Save();
    extern void KiwiWindows_SyncMenu();
    KiwiWindows_SyncMenu();
}

// While a sculpt tool is armed this module OWNS the patch wireframe: the ported
// draws (the selected white tech-29 mesh, the unselected pref grid) stand down and
// DrawWorld draws the grid only within the brush's reach around the cursor - or
// nothing at all once Tab has hidden it.  Disarmed = the ported behaviour.
bool KiwiTerrain_HideWireframe()
{
    return s_armed && s_tool != KTER_GRASS;
}

int KiwiTerrain_ExtraLayerCount( patchMesh_t *def )
{
    Load();
    if ( !def || !s_previewBlend || HeatmapActive() )   // the gradient replaces the layers
        return 0;
    return UsedSlotCount( def );
}

// Called from Patch_Fill_BuildVisuals for EVERY VB run of a patch.  Run 0 is the base:
// its alpha must be opaque (the colour bytes are weights, not a tint).  Runs past the
// material def's own count are the used layer slots in order: alpha := that slot's
// channel and the material := the layer's blend twin (or the base, opaque, if the twin
// is unavailable — the row in the panel says why).
void KiwiTerrain_LayerUpload( patchMesh_t *def, int run, int baseRuns,
                              unsigned int *color, const curveVert_t *verts,
                              int vertCount, Material **material )
{
    if ( !def || !color )
        return;
    if ( run < baseRuns )
    {
        // Height gradient while a height tool is armed: the base run turns into a
        // flat opaque material with the colour by control-point height (a terrain
        // mesh's render verts ARE its control points; a bezier's are interpolated).
        if ( run == 0 && HeatmapActive() && verts )
        {
            if ( !s_heatValid )
                ComputeHeatRange();
            const float span = s_heatMaxZ - s_heatMinZ;
            for ( int i = 0; i < vertCount; ++i )
                color[i] = HeatColor( span > 0.0f ? ( verts[i].xyz[2] - s_heatMinZ ) / span : 0.5f );
            // kiwi_heat (lit, depth-writing, white colormap) shows the colour alone;
            // without it the patch keeps its own material and the colour tints it.
            Material *heat = HeatMaterial();
            if ( heat && material )
                *material = heat;
            return;
        }
        if ( run == 0 && UsedSlotCount( def ) )
            for ( int i = 0; i < vertCount; ++i )
                color[i] |= 0xFF000000u;
        return;
    }
    const int slot = NthUsedSlot( def, run - baseRuns );
    if ( slot < 0 || !verts )
        return;
    // Weight view: ONLY the brush material's own layer draws red at its painted
    // weight; every other layer keeps its blended texture, so dirt / sand / grass can
    // be layered while the one being painted stays obvious.
    if ( WeightViewActive() && !s_paintBase && !_stricmp( def->kiwiLayer[slot], s_paintMaterial ) )
    {
        Material *flat = WeightMaterial();
        for ( int i = 0; i < vertCount; ++i )
        {
            const unsigned a = ( (const byte *)&verts[i].vert_color )[slot];
            // Packed B | G<<8 | R<<16 (the patch VB order): red.
            color[i] = ( flat ? 0xFF0000u : ( color[i] & 0x00FFFFFFu ) ) | ( a << 24 );
        }
        Material *use = flat ? flat : BlendTwin( def->kiwiLayer[slot] );
        if ( use && material )
            *material = use;
        return;
    }
    for ( int i = 0; i < vertCount; ++i )
    {
        const unsigned a = ( (const byte *)&verts[i].vert_color )[slot];
        color[i] = ( color[i] & 0x00FFFFFFu ) | ( a << 24 );
    }
    Material *twin = BlendTwin( def->kiwiLayer[slot] );
    if ( twin && material )
        *material = twin;
}

void KiwiTerrain_Draw()
{
    Load();
    RetintTick();                               // the scheduled heat / weight-view re-upload
    if ( s_stroke && s_tool != KTER_TRIM && !( s_tool == KTER_SETHEIGHT && !s_modShift ) )
    {
        float dt = ImGui::GetIO().DeltaTime;
        if ( dt > 0.05f ) dt = 0.05f;
        s_accumDt += dt;
        ApplyStroke();
    }
    if ( !s_show )
        return;

    const bool wasShown = s_show;
    if ( ImGui::Begin( "Terrain Sculpt", &s_show, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        bool changed = false;

        // KIWI (2026-09-15, user): two columns. LEFT = what changes with the tool
        // (the brush and the tool's own settings); RIGHT = what never changes (tool
        // pick, arm, display, chunks, flatten, density, scope). Fixed column widths
        // keep the auto-resized window from oscillating; text wraps at the cell edge.
        const float KTER_COL_LEFT  = 380.0f;
        const float KTER_COL_RIGHT = 420.0f;
        const bool  table = ImGui::BeginTable( "##terrain_columns", 2,
                                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit );
        if ( table )
        {
            ImGui::TableSetupColumn( "##tool",   ImGuiTableColumnFlags_WidthFixed, KTER_COL_LEFT );
            ImGui::TableSetupColumn( "##always", ImGuiTableColumnFlags_WidthFixed, KTER_COL_RIGHT );
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
        }
        ImGui::PushTextWrapPos( 0.0f );
        ImGui::PushItemWidth( 190.0f );

        // ── LEFT: the current tool ────────────────────────────────────────────
        ImGui::SeparatorText( KTER_TOOL_NAME[s_tool] );
        ImGui::TextDisabled( "%s", KTER_TOOL_HINT[s_tool] );

        if ( s_tool == KTER_GRASS )
        {
            ImGui::SeparatorText( "Grass Scatter" );
            KiwiGrass_DrawSettings();
        }
        else
        {
            ImGui::SeparatorText( "Brush" );
            if ( ImGui::RadioButton( "Circle", s_shape == KTER_CIRCLE ) ) { s_shape = KTER_CIRCLE; changed = true; }
            ImGui::SameLine();
            if ( ImGui::RadioButton( "Square", s_shape == KTER_SQUARE ) ) { s_shape = KTER_SQUARE; changed = true; }
            if ( s_shape == KTER_SQUARE )
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 120.0f );
                changed |= ImGui::SliderFloat( "Rotation", &s_squareRot, -180.0f, 180.0f, "%.0f deg" );
            }
            static const char *s_foNames[] = { "Smooth", "Linear", "Sharp", "Constant" };
            ImGui::SetNextItemWidth( 120.0f );
            changed |= ImGui::Combo( "Falloff", &s_falloff, s_foNames, 4 );
            changed |= ImGui::SliderFloat( "Outer radius", &s_outer, 4.0f, 3072.0f, "%.0f", ImGuiSliderFlags_Logarithmic );
            changed |= ImGui::SliderFloat( "Inner radius", &s_inner, 0.0f, 3072.0f, "%.0f", ImGuiSliderFlags_Logarithmic );
            if ( s_inner > s_outer )
                s_inner = s_outer;
            changed |= ImGui::SliderFloat( "Strength", &s_strength, 0.01f, 2.0f, "%.2f" );
            ImGui::TextDisabled( "Resize while sculpting: [ ] or + / - (hold to repeat), Ctrl+wheel.  "
                                 "Strength: Shift+wheel or Alt+wheel (Set height: Alt+wheel moves the target)." );
            if ( s_tool == KTER_RAISE || s_tool == KTER_SETHEIGHT || s_tool == KTER_SMOOTH || s_tool == KTER_NOISE )
            {
                changed |= ImGui::Checkbox( "Never change anything outside the ring", &s_setHeightContain );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Every height tool. A terrain point moves only if ALL the triangles it tilts\n"
                                       "lie inside the ring, so the ramp between old and new ground stays inside it:\n"
                                       "no ground, seam or carried object outside the ring ever changes.\n"
                                       "Off: every point inside the ring moves and its cells lean past the ring." );
                changed |= ImGui::Checkbox( "Refine the mesh under the ring", &s_autoRefine );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "OFF by default - it ADDS TRIANGLES (a few thousand per touched area).\n"
                                       "Terrain whose cells are coarser than about a quarter of the outer radius is\n"
                                       "re-gridded under the ring before the stroke, so the ring always has points\n"
                                       "to work with. Exact: the sheet is cut along its own grid lines, pieces the\n"
                                       "ring misses are verbatim copies, and refined cells reproduce the old\n"
                                       "triangles until a stamp moves them. Part of the stroke's undo.\n"
                                       "Both sides of a seam are re-gridded (selected or not), so a fine sheet\n"
                                       "next to a coarse one gets matching seam points and no 'moat' forms." );
                if ( s_autoRefine )
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth( 90.0f );
                    changed |= ImGui::InputFloat( "Finest cell", &s_refineMin, 0.0f, 0.0f, "%.0f" );
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "The re-grid never goes below this cell size, however small the ring.\n"
                                           "Now: %.0f-unit cells for a %.0f outer radius.", RefineCellFor( s_outer ), s_outer );
                }
            }
        }

        if ( s_tool != KTER_GRASS )
            ImGui::SeparatorText( "Settings" );
        switch ( s_tool )
        {
        case KTER_RAISE:
            changed |= ImGui::SliderFloat( "Raise speed", &s_amount, 1.0f, 4096.0f, "%.0f",
                                           ImGuiSliderFlags_Logarithmic );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Units per second at full brush weight with a 128 outer radius;\n"
                                   "wider brushes rise proportionally faster." );
            changed |= ImGui::Checkbox( "Allow terrain creation", &s_expand );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "While raising, every empty chunk cell under the brush gets a new terrain\n"
                                   "patch that then rises with the stroke - anywhere, no patch needed.\n"
                                   "Next to existing terrain the chunk continues its lattice, materials and\n"
                                   "layers at the neighbouring height (seams matched). With no terrain in\n"
                                   "reach the cursor lands on brushes and models, else on the base height,\n"
                                   "and the chunk wears the texture browser's current material.\n"
                                   "Green squares preview the cells; a blue ring = base height, cyan = surface." );
            if ( s_expand )
            {
                ImGui::Indent();
                changed |= UnitInputWorld( "Base height (Z)", &s_createZ, 160.0f );
                ImGui::SameLine();
                if ( ImGui::Button( "From cursor##create" ) && s_cursorHave )
                {
                    s_createZ = s_cursor[2];
                    changed = true;
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Take the height under the hover ring (patch, surface or plane)." );
                ImGui::SetNextItemWidth( 160.0f );
                changed |= ImGui::SliderInt( "Cells per new chunk", &s_createCells, 1, 15 );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Grid density of a chunk laid with no terrain within reach\n"
                                       "(reach = outer radius + chunk size). Chunks beside existing\n"
                                       "terrain copy its cell size instead so the seams share points." );
                changed |= ImGui::Checkbox( "Land on brushes and models", &s_createOnSurfaces );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Off the patches the cursor first tries the world's brushes and models;\n"
                                       "untick to sculpt on the base height only, ignoring what is below." );
                ImGui::Unindent();
            }
            break;
        case KTER_SETHEIGHT:
            changed |= UnitInputWorld( "Target height (Z)", &s_targetZ, 160.0f );
            ImGui::SameLine();
            if ( ImGui::Button( "From cursor" ) && s_cursorHave )
            {
                s_targetZ = s_cursor[2];
                changed = true;
            }
            ImGui::TextDisabled( "Every point inside the OUTER ring is set to exactly this height, at once\n"
                                 "(the Far Cry flatten). Strength and falloff do not apply." );
            changed |= ImGui::Checkbox( "Feather the edge (use falloff + strength)", &s_setHeightFeather );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Off (default): an exact, hard-edged set inside the ring.\n"
                                   "On: the band between the inner and outer ring is eased toward the\n"
                                   "target instead - for blending a plateau into a slope. That ramp moves\n"
                                   "ground around the spot, so leave it off next to finished work." );
            ImGui::TextDisabled( "A moved point drags the whole grid cells around it: on a sheet whose cells\n"
                                 "are wider than the brush the change spills a full cell past the ring.\n"
                                 "Tessellate that sheet to a smaller cell size first (Density, below)." );
            break;
        case KTER_NOISE:
            changed |= ImGui::SliderFloat( "Noise height", &s_noiseScale, 0.25f, 512.0f, "%.1f",
                                           ImGuiSliderFlags_Logarithmic );
            changed |= ImGui::SliderFloat( "Noise frequency", &s_noiseFreq, 0.0005f, 0.1f, "%.4f",
                                           ImGuiSliderFlags_Logarithmic );
            break;
        case KTER_TEXTURE:
            DrawTexturePaint( changed );
            break;
        case KTER_BLEND:
            ImGui::SetNextItemWidth( 160.0f );
            changed |= ImGui::SliderInt( "Blend reach (grid points)", &s_blendRings, 1, 4 );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "How many grid points each weight averages with. 1 softens a single\n"
                                   "step, 3-4 turn a hard edge into a wide gradient." );
            ImGui::TextDisabled( "Blends every layer's weights toward their neighbours, seams included;\n"
                                 "textures that meet in steps become smooth transitions." );
            break;
        default:
            break;
        }

        // ── RIGHT: everything that does not depend on the tool ────────────────
        ImGui::PopItemWidth();
        ImGui::PopTextWrapPos();
        if ( table )
            ImGui::TableSetColumnIndex( 1 );
        ImGui::PushTextWrapPos( 0.0f );
        ImGui::PushItemWidth( 190.0f );

        ImGui::SeparatorText( "Tool" );
        // KIWI (2026-09-17, user: "remove the smooth tool and just encourage shift-click
        // usage"): Smooth is no longer a tool of its own - Shift+LMB smooths from every
        // sculpt tool, without a trip to this panel.  The enum slot stays (saved profiles
        // and the test DSL's `terrain tool smooth` still resolve) but is never listed.
        int shown = 0;
        for ( int i = 0; i < KTER_TOOL_COUNT; ++i )
        {
            if ( i == KTER_SMOOTH )
                continue;
            if ( shown++ % 2 )
                ImGui::SameLine( 200.0f );
            if ( ImGui::RadioButton( KTER_TOOL_NAME[i], s_tool == i ) )
            {
                SetTool( i );
                changed = true;
            }
        }
        ImGui::TextColored( ImVec4( 1.0f, 0.82f, 0.35f, 1.0f ),
                            "Smooth: hold Shift and drag with any sculpt tool." );

        ImGui::Spacing();
        if ( ImGui::Button( s_armed ? "Disarm (Esc)"
                                    : ( s_tool == KTER_GRASS ? "Scatter (LMB in camera)" : "Sculpt (LMB in camera)" ),
                            ImVec2( -FLT_MIN, 0.0f ) ) )
        {
            if ( !s_armed )
            {
                s_softSelect = false;
                SyncSoftSelect();
            }
            SetArmed( !s_armed );
        }
        if ( s_armed && s_tool != KTER_GRASS && s_tool != KTER_TRIM && !AnyTargetPatch() && !CreationAllowed() )
            ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.3f, 1.0f ),
                                "No patch selected - select the terrain patch(es), tick 'Affect unselected',\n"
                                "or tick 'Allow terrain creation' under Raise to sculpt on empty ground." );
        if ( s_armed )
            ImGui::TextColored( ImVec4( 0.42f, 0.92f, 0.48f, 1.0f ), "%s", s_status );
        else
            ImGui::TextDisabled( "%s", s_status );

        if ( s_tool != KTER_GRASS )
        {
            ImGui::SeparatorText( "Display" );
            if ( ImGui::Checkbox( "Hide wireframe (Tab)", &s_hideWire ) )
                g_nUpdateBits = -1;
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "While armed the patch wireframe is drawn only around the brush\n"
                                   "(white = patches the stroke moves, grey = the rest). Tab hides it." );
            if ( !s_hideWire )
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 100.0f );
                changed |= ImGui::SliderFloat( "Wire reach", &s_wireReach, 1.0f, 4.0f, "x%.2f" );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Wireframe radius as a multiple of the outer radius." );
            }
            if ( ImGui::Checkbox( "Height colours while armed", &s_heatmap ) )
            {
                Save();
                HeatmapRefresh();
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Height tools (Raise, Set height, Smooth, Noise, Trim) show every patch\n"
                                   "as a blue -> green -> red gradient by height instead of its texture,\n"
                                   "so relief reads at a glance. Texture / colour paint keep the real look." );
            if ( ImGui::Checkbox( "Height colours while NOT armed too", &s_heatAlways ) )
            {
                Save();
                HeatmapRefresh();
            }
            if ( ImGui::Checkbox( "Paint never draws over other geometry", &s_layerDepthEqual ) )
            {
                Save();
                RestripTwins();
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "On (default): a paint layer only lands on pixels where its OWN ground is the\n"
                                   "visible surface (depth EQUAL). A prop lying almost flush with the ground -\n"
                                   "rail ties on a flattened bed - then looks the same as with Height colours.\n"
                                   "Off: the old less-or-equal test; the paint, drawn last, wins every near-tie\n"
                                   "and creeps over such props. Turn it off only if paint flickers or vanishes." );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Keeps the height gradient (and its legend) on with the tool put away -\n"
                                   "for reading relief while placing models or matching an elevation map.\n"
                                   "An armed Texture / Blend / Grass tool still shows the real look.\n"
                                   "The colour range re-checks about once a second." );

            ImGui::SeparatorText( "Chunks" );
            changed |= ImGui::SliderFloat( "Chunk size", &s_chunkSize, 256.0f, 8192.0f, "%.0f",
                                           ImGuiSliderFlags_Logarithmic );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Largest patch side and the cell of terrain creation. New chunks copy\n"
                                   "the density of the terrain under or near the cursor (same cell size),\n"
                                   "on a lattice that is a multiple of that cell so the seams line up;\n"
                                   "with no terrain in reach the lattice starts at the world origin." );
            if ( ImGui::Button( "Split oversized selected patches" ) )
                SplitOversized();
            if ( ImGui::Button( "Select terrain stacked on other terrain" ) )
                SelectStackedTerrain();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Finds patches that lie on top of other terrain (left behind by older\n"
                                   "Tessellate / creation bugs) and SELECTS them - nothing is deleted.\n"
                                   "Of two overlapping patches the smaller one is picked (of two equal\n"
                                   "ones, one of them). Check the selection, then press Delete." );
            ImGui::SeparatorText( "Flatten to brushes" );
            ImGui::SetNextItemWidth( 120.0f );
            changed |= ImGui::InputFloat( "Flatness tolerance", &s_flatTol, 0.5f, 4.0f, "%.1f" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 120.0f );
            changed |= ImGui::InputFloat( "Thickness", &s_flatThick, 1.0f, 16.0f, "%.0f" );
            if ( ImGui::Button( "Flat selected patches -> brushes" ) )
                FlattenSelectedToBrushes();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Every selected terrain patch whose heights all lie within the tolerance\n"
                                   "becomes ONE brush: 12 triangles and one collision volume instead of a grid.\n"
                                   "Top face = the patch's dominant material (the painted layer with the\n"
                                   "highest average weight if it covers half the patch, else the base); sides\n"
                                   "and bottom caulk. A face has no weights, so blends do not carry over.\n"
                                   "Patches that are not flat are kept; Set height them first to qualify.\n"
                                   "The new brushes stay SELECTED (flat highlight in the camera) - Esc to see\n"
                                   "their texture. Undoable." );
            ImGui::SeparatorText( "Density" );
            changed |= UnitInputWorld( "Cell size (absolute)", &s_tessCell, 160.0f );
            s_tessCell = ClampF( s_tessCell, 4.0f, 4096.0f );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "/2" ) ) { s_tessCell = ClampF( s_tessCell * 0.5f, 4.0f, 4096.0f ); changed = true; }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x2" ) ) { s_tessCell = ClampF( s_tessCell * 2.0f, 4.0f, 4096.0f ); changed = true; }
            {
                // What the selection would become, so the number is never a guess.
                int patches = 0, chunks = 0, tris = 0, nowTris = 0;
                for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
                {
                    if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) || !b->def )
                        continue;
                    const kterTessPlan_t p = TessPlan( b->def->maxs[0] - b->def->mins[0],
                                                       b->def->maxs[1] - b->def->mins[1], s_tessCell );
                    ++patches;
                    chunks  += p.kx * p.ky;
                    tris    += p.cellsX * p.cellsY * 2;
                    nowTris += ( b->patch->def->width - 1 ) * ( b->patch->def->height - 1 ) * 2;
                }
                if ( patches )
                    ImGui::TextDisabled( "%i selected patch%s: %i -> %i triangles, %i chunk%s",
                                         patches, patches == 1 ? "" : "es", nowTris, tris,
                                         chunks, chunks == 1 ? "" : "s" );
                else
                    ImGui::TextDisabled( "select terrain patches to see the result" );
            }
            if ( ImGui::Button( "Tessellate" ) )
                TessellateSelected();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Rebuilds every selected terrain patch so its cells are THIS BIG in world\n"
                                   "units, whatever the patch's own size - a small chunk and a huge sheet end up\n"
                                   "at the same real density. More than 15 cells on an axis splits the patch\n"
                                   "into chunks (a patch holds 16 points). Heights, layer weights and texcoords\n"
                                   "are interpolated from the current grid; chunk edges coincide exactly.\n"
                                   "A sheet whose size is not a whole number of cells gets the nearest fit.\n"
                                   "One undo record." );
            {
                const int bez = SelectedBezierCount();
                if ( bez > 0 )
                {
                    ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.3f, 1.0f ),
                                        "%i selected patch%s %s bezier: every stroke re-tessellates it "
                                        "(~%i tris each). Terrain meshes paint at full speed.",
                                        bez, bez == 1 ? "" : "es", bez == 1 ? "is" : "are", 15 * 8 * 15 * 8 * 2 );
                    if ( ImGui::Button( "Convert selected to terrain mesh" ) )
                        ConvertSelectedToTerrain();
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Sets the CoD4 'mesh' type: the patch renders as its control grid\n"
                                           "(no bezier smoothing), exactly like a Terrain-dialog patch. Undoable." );
                }
            }

            ImGui::SeparatorText( "Scope" );
            changed |= ImGui::Checkbox( "Affect unselected patches too", &s_affectUnselected );
            changed |= ImGui::Checkbox( "Carry objects resting on the terrain", &s_carryObjects );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Height strokes (Raise / Dig, Set height, Smooth, Noise) also move whatever\n"
                                   "sits on the sculpted terrain - models, entities, brushes and non-terrain\n"
                                   "patches - up or down by the terrain's own change under each object, so\n"
                                   "placed props survive a resculpt. An object counts when its bottom is\n"
                                   "within 48 units of the terrain at the start of the stroke; floating or\n"
                                   "buried things stay put. Undo restores them with the terrain." );
            ImGui::TextDisabled( "Terrain meshes only - bezier curves are never sculpted, welded or painted." );
            if ( ImGui::Checkbox( "Soft-select vertex drags (legacy Drag Up/Down)", &s_softSelect ) )
            {
                SyncSoftSelect();
                if ( s_softSelect )
                    SetArmed( false );
            }
        }

        ImGui::PopItemWidth();
        ImGui::PopTextWrapPos();
        if ( table )
            ImGui::EndTable();

        if ( changed )
        {
            Save();
            SyncSoftSelect();
            RebuildRing();
            g_nUpdateBits |= W_CAMERA;
        }
    }
    ImGui::End();

    if ( !s_show )
        SetArmed( false );
    if ( wasShown != s_show )
    {
        extern void KiwiWindows_SyncMenu();
        KiwiWindows_SyncMenu();
    }
}

// The press, once the cursor is resolved (mouse path and the -kiwitest stroke share it).
// `picked` = the vertex colour under the cursor (white off the patches).
static bool BeginStroke( bool shift, bool ctrl, const byte picked[4] )
{
    s_modShift = shift;
    s_modCtrl  = ctrl;

    // Never paint the editor's own preview copy ("kiwi_blend_<name>", pickable in the texture
    // browser because it is a real file): the layer is the real material.
    if ( !_strnicmp( s_paintMaterial, "kiwi_blend_", 11 ) )
    {
        const char *real = UnwrapTwinName( s_paintMaterial );
        Sys_Printf( "Terrain Sculpt: '%s' is the editor's preview copy - painting '%s' instead.\n", s_paintMaterial, real );
        memmove( s_paintMaterial, real, strlen( real ) + 1 );
        Save();
    }

    if ( ctrl && !shift )
    {
        if ( s_tool == KTER_SETHEIGHT )
        {
            s_targetZ = s_cursor[2];
            Save();
            SetStatus( "Armed. Target height picked: %.1f", s_targetZ );
            return false;
        }
    }
    (void)picked;

    if ( s_tool == KTER_TRIM )
    {
        s_stroke = true;
        TrimUnderBrush();
        return true;
    }

    // With creation allowed a stroke needs no patch at all: the chunks it lays join
    // the selection and the targets as they appear.
    if ( !AnyTargetPatch() && !CreationAllowed() && !PaintAnywhere() )
    {
        SetStatus( "Armed. Select the patch(es) to sculpt, or tick 'Affect unselected'." );
        return false;
    }
    BuildTargets();

    Undo_ClearRedo();
    Undo_GeneralStart( "terrain sculpt" );
    s_undoOpen = true;
    Patch_Paint( &selected_brushes );
    Patch_Paint( &active_brushes );          // seam stitching may touch unselected neighbours
    CaptureRiders();                         // "Carry objects": what rests on the targets now

    s_stroke    = true;
    s_stamps    = 0;
    s_touched   = 0;
    s_created   = 0;
    s_layersAdded = 0;
    s_layersFull  = 0;
    s_facesPainted = 0;
    s_carried   = 0;
    s_coarseSpill = false;
    s_holeFailValid = false;                 // a new press may search for a gap again
    s_noiseSeed += 1.0f;
    s_haveLastCenter = false;
    s_accumDt   = 1.0f / 60.0f;
    SetStatus( s_cursorKind == KCUR_PLANE   ? "Sculpting on the base height..."
             : s_cursorKind == KCUR_SURFACE ? "Sculpting on a world surface..."
                                            : "Sculpting..." );
    ApplyStroke();
    return true;
}

// ── viewport bridge ──────────────────────────────────────────────────────────
bool KiwiTerrain_IsArmed()
{
    return s_armed;
}

// KIWI (2026-09-15, user: "I forgot how to prime the set-height tool to whatever's
// under the cursor. Make this show up while I'm using the tool"): the armed tool's
// grammar goes to the camera's bottom-left hint strip. Set height also shows its
// live target so a pick is visibly confirmed without looking at the panel.
static void HudAdd( kiwiPrompt_t *prompts, int *n, const char *key, const char *label )
{
    if ( *n >= 12 )
        return;
    prompts[*n].key   = key;
    prompts[*n].label = label;
    ++( *n );
}

int KiwiTerrain_HudPrompts( const kiwiPrompt_t **out )
{
    static kiwiPrompt_t prompts[12];
    static char         targetText[48];
    static char         baseText[48];
    if ( !out || !s_armed )
        return 0;
    int n = 0;
    switch ( s_tool )
    {
    case KTER_RAISE:
        HudAdd( prompts, &n, "LMB",       "Raise" );
        HudAdd( prompts, &n, "Ctrl+LMB",  "Dig" );
        HudAdd( prompts, &n, "Shift+LMB", "Smooth" );
        if ( CreationAllowed() )
        {
            KiwiUnits_Format( baseText, sizeof( baseText ), s_createZ );
            HudAdd( prompts, &n, "V",    "Pick base height under cursor" );
            HudAdd( prompts, &n, "Base", baseText );
        }
        break;
    case KTER_SETHEIGHT:
        KiwiUnits_Format( targetText, sizeof( targetText ), s_targetZ );
        HudAdd( prompts, &n, "LMB",        "Snap to target" );
        HudAdd( prompts, &n, "V",          "Pick height under cursor" );
        HudAdd( prompts, &n, "Ctrl+LMB",   "Pick height under cursor" );
        HudAdd( prompts, &n, "Target",     targetText );
        HudAdd( prompts, &n, "Alt+wheel",  "Target height" );
        HudAdd( prompts, &n, "Shift+LMB",  "Smooth" );
        break;
    case KTER_SMOOTH:
        HudAdd( prompts, &n, "LMB", "Smooth" );
        break;
    case KTER_NOISE:
        HudAdd( prompts, &n, "LMB",       "Add noise" );
        HudAdd( prompts, &n, "Ctrl+LMB",  "Subtract" );
        HudAdd( prompts, &n, "Shift+LMB", "Smooth" );
        break;
    case KTER_TEXTURE:
        HudAdd( prompts, &n, "LMB",       "Paint" );
        HudAdd( prompts, &n, "Ctrl+LMB",  "Paint out" );
        HudAdd( prompts, &n, "Shift+LMB", "Smooth" );
        HudAdd( prompts, &n, "I",         "Eyedropper" );
        break;
    case KTER_BLEND:
        HudAdd( prompts, &n, "LMB", "Blend layers" );
        break;
    case KTER_GRASS:
        HudAdd( prompts, &n, "LMB", "Scatter along stroke" );
        break;
    case KTER_TRIM:
        HudAdd( prompts, &n, "LMB", "Remove chunks under brush" );
        break;
    default:
        break;
    }
    if ( s_tool != KTER_GRASS )
    {
        HudAdd( prompts, &n, "[ ]",         "Radius" );
        HudAdd( prompts, &n, "Ctrl+wheel",  "Radius" );
        HudAdd( prompts, &n, s_tool == KTER_SETHEIGHT ? "Shift+wheel" : "Alt/Shift+wheel", "Strength" );
    }
    HudAdd( prompts, &n, "Esc", "Disarm" );
    *out = prompts;
    return n;
}

bool KiwiTerrain_HandleDown( int imgX, int imgY, bool shift, bool ctrl )
{
    Load();
    if ( !s_armed )
        return false;
    if ( s_tool == KTER_GRASS )
        return KiwiGrass_HandleDown( imgX, imgY );

    if ( KiwiEditorCommand *live = KiwiCmd_Active() )
    {
        if ( live->PreemptIdle() )
            KiwiCmd_Cancel();
        else
        {
            Sys_Printf( "Terrain Sculpt: finish or cancel \"%s\" before sculpting.\n", live->Name() );
            SetStatus( "Armed. Finish or cancel the active command first." );
            return false;
        }
    }

    byte picked[4];
    if ( !UpdateCursor( imgX, imgY, picked ) )
    {
        SetStatus( "Armed. No patch under the cursor." );
        return false;
    }
    return BeginStroke( shift, ctrl, picked );
}

void KiwiTerrain_HandleDrag( int imgX, int imgY )
{
    if ( s_armed && s_tool == KTER_GRASS )
    {
        KiwiGrass_HandleDrag( imgX, imgY );
        return;
    }
    if ( !s_stroke )
        return;
    if ( !UpdateCursor( imgX, imgY, nullptr ) )
        return;
    if ( s_tool == KTER_TRIM )
    {
        TrimUnderBrush();
        return;
    }
    if ( s_tool == KTER_SETHEIGHT && !s_modShift )
    {
        s_accumDt = 1.0f;
        ApplyStroke();
    }
}

void KiwiTerrain_HandleUp()
{
    if ( s_armed && s_tool == KTER_GRASS )
        KiwiGrass_HandleUp();
    EndStroke();
}

void KiwiTerrain_HandleAbort()
{
    if ( s_armed && s_tool == KTER_GRASS )
        KiwiGrass_HandleAbort();
    EndStroke();
}

bool KiwiTerrain_HandleEscape()
{
    if ( !s_armed )
        return false;
    SetArmed( false );
    return true;
}

// I: eyedropper.  Loads "Paint with" from whatever is under the pointer: on a patch,
// the layer with the heaviest weight at the nearest control point (the base when no
// layer carries at least 25 %); on a brush, the face the pointer is on.
static bool EyedropMaterial()
{
    ray_t ray;
    if ( !Pick_RayFromCursor( &ray ) )
        return false;
    float hit[3];
    byte  col[4];
    selbrush_t *node = nullptr;
    if ( PickPatches( ray.origin, ray.dir, true, hit, col, &node ) && node )
    {
        const patchMesh_t *def = node->patch->def;
        // Nearest control point in XY.
        int bi = 0, bj = 0;
        float best = FLT_MAX;
        for ( int i = 0; i < def->width; ++i )
            for ( int j = 0; j < def->height; ++j )
            {
                const float dx = def->ctrl[i][j].xyz[0] - hit[0], dy = def->ctrl[i][j].xyz[1] - hit[1];
                const float d = dx * dx + dy * dy;
                if ( d < best ) { best = d; bi = i; bj = j; }
            }
        const byte *w = (const byte *)&def->ctrl[bi][bj].vert_color;
        int slot = -1, weight = 63;                    // below 25 % the base shows through
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( SlotUsed( def, k ) && (int)w[k] > weight ) { weight = w[k]; slot = k; }
        const char *name = slot >= 0 ? def->kiwiLayer[slot] : BaseMaterialName( def );
        strncpy( s_paintMaterial, name, sizeof( s_paintMaterial ) - 1 );
        s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
        s_paintBase = false;
        Save();
        SetStatus( "Armed. Eyedropper: %s (%s)", s_paintMaterial, slot >= 0 ? "layer" : "base" );
        return true;
    }
    const pick_result_t face = Pick( ray, SEL_MASK_FACE );
    if ( face.valid && face.item.kind == SEL_FACE && face.item.brush && face.item.brush->def
      && face.item.faceIndex >= 0 && face.item.faceIndex < face.item.brush->def->faceCount )
    {
        const qtexture_s *q = face.item.brush->def->faces[face.item.faceIndex].mtldef[0].radMtl;
        if ( q && q->name )
        {
            strncpy( s_paintMaterial, q->name, sizeof( s_paintMaterial ) - 1 );
            s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
            s_paintBase = false;
            Save();
            SetStatus( "Armed. Eyedropper: %s (brush face)", s_paintMaterial );
            return true;
        }
    }
    SetStatus( "Armed. Eyedropper: nothing under the pointer." );
    return false;
}

bool KiwiTerrain_HandleKey( int vk )
{
    if ( !s_armed )
        return false;
    if ( vk == 0x1B )                          // VK_ESCAPE
    {
        SetArmed( false );
        return true;
    }
    if ( vk == 0x09 && s_tool != KTER_GRASS )  // VK_TAB: hide / show the wireframe around the brush
    {
        s_hideWire = !s_hideWire;
        g_nUpdateBits = -1;
        return true;
    }
    if ( vk == 0x49 && s_tool == KTER_TEXTURE ) // I: eyedropper into "Paint with"
    {
        EyedropMaterial();
        g_nUpdateBits |= W_CAMERA;
        return true;
    }
    if ( vk == 0x56 )                          // V: pick the height under the pointer
    {
        if ( !s_cursorHave )
        {
            SetStatus( "Armed. V needs the pointer over terrain (or the base plane)." );
            return true;
        }
        if ( s_tool == KTER_SETHEIGHT )
        {
            s_targetZ = s_cursor[2];
            Save();
            char h[64];
            KiwiUnits_Format( h, sizeof( h ), s_targetZ );
            SetStatus( "Armed. Target height picked: %s", h );
            g_nUpdateBits |= W_CAMERA;
            return true;
        }
        if ( CreationAllowed() )
        {
            s_createZ = s_cursor[2];
            Save();
            char h[64];
            KiwiUnits_Format( h, sizeof( h ), s_createZ );
            SetStatus( "Armed. Base height picked: %s", h );
            return true;
        }
        return false;
    }
    if ( vk == 0xDD || vk == 0xDB )            // ] / [ : brush radius up / down (auto-repeats)
    {
        RadiusStep( vk == 0xDD ? 1.15f : 1.0f / 1.15f );
        return true;
    }
    if ( vk == 0x6B || vk == 0xBB )            // VK_ADD / VK_OEM_PLUS
    {
        RadiusStep( 1.25f );
        return true;
    }
    if ( vk == 0x6D || vk == 0xBD )            // VK_SUBTRACT / VK_OEM_MINUS
    {
        RadiusStep( 0.8f );
        return true;
    }
    return false;
}

bool KiwiTerrain_HandleWheel( float steps, bool shift, bool ctrl, bool alt )
{
    if ( !s_armed || steps == 0.0f )
        return false;
    // KIWI (2026-09-17, user: "when using the set-height tool, alt-scroll should adjust the
    // set-height.  alt-scroll in raise/dig mode should adjust strength"): Alt+wheel drives
    // the value that matters most for the armed tool.
    if ( alt && s_tool != KTER_GRASS )
    {
        if ( s_tool == KTER_SETHEIGHT )
        {
            const float step = shift ? 1.0f : 8.0f;
            s_targetZ = ClampF( s_targetZ + ( steps > 0.0f ? step : -step ), -65536.0f, 65536.0f );
            char h[48];
            KiwiUnits_Format( h, sizeof( h ), s_targetZ );
            SetStatus( "Armed. Target height %s (Alt+wheel; Shift for 1-unit steps).", h );
        }
        else
        {
            s_strength = ClampF( s_strength + ( steps > 0.0f ? 0.1f : -0.1f ), 0.01f, 2.0f );
            SetStatus( "Armed. Strength %.2f (Alt+wheel).", s_strength );
        }
        Save();
        g_nUpdateBits |= W_CAMERA;
        return true;
    }
    if ( ctrl )
    {
        RadiusStep( steps > 0.0f ? 1.25f : 0.8f );
        return true;
    }
    if ( shift )
    {
        s_strength = ClampF( s_strength + ( steps > 0.0f ? 0.1f : -0.1f ), 0.01f, 2.0f );
        Save();
        return true;
    }
    return false;
}

void KiwiTerrain_Hover( int imgX, int imgY, bool over )
{
    if ( s_armed && s_tool == KTER_GRASS )
    {
        KiwiGrass_Hover( imgX, imgY, over );
        ClearCursor();
        return;
    }
    if ( !s_armed || !over )
    {
        ClearCursor();
        s_expandCellCount = 0;
        return;
    }
    UpdateCursor( imgX, imgY, nullptr );
    s_expandCellCount = PreviewCells();
}

// The armed-tool wireframe: every eligible patch's render grid (the same cells,
// edges and turned-edge diagonals DrawPatchesWireframeGrid emits), but only the
// segments with an end within outer radius x "Wire reach" of the cursor, in the
// brush's shape.  Patches the stroke moves (the selection, plus the active list
// under "Affect unselected") are white; the rest grey.
// The camera draws the reference images AFTER a depth clear, on top of everything, so
// the red weight run under a picture was dimmed by that picture's opacity (and gone
// behind an opaque one).  While the weight view is on and pictures exist, re-emit
// every patch's red run here - KiwiHover_DrawWorld calls this right after
// KiwiRefImage_DrawWorld - as an unlit, vertex-coloured, depth-tested blend over
// them.  Over bare terrain it merely restates the world pass's own run.
static void DrawWeightOverlay()
{
    if ( !WeightViewActive() || s_paintBase || KiwiRefImage_Count() <= 0 )
        return;
    Material *flat = WeightMaterial();
    if ( !flat )
        return;
    const float neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // w = 0: the vertex colour drives
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool any = false;
    const int sortKey = Editor_MaterialSortKey( flat );
    for ( int pass = 0; pass < 2; ++pass )
    {
        selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( !PatchEligible( b ) )
                continue;
            const patch_t *inst = b->patch;
            if ( !inst->visArray || inst->vertCount <= 0 || !inst->indicesFront )
                continue;
            for ( int L = 0; L < inst->visCount; ++L )
            {
                if ( inst->visArray[L].material != flat )
                    continue;
                if ( !any )
                {
                    R_SortMaterials();                    // open this pass's accumulation
                    R_AddCmdSetMaterialColor( neutral );
                    any = true;
                }
                Editor_AddMeshCmd( flat, TECHNIQUE_UNLIT, sortKey + L, inst->vertCount,
                                   inst->visArray[L].vertHandle, inst->indexCount,
                                   inst->indicesFront );
            }
        }
    }
    if ( any )
    {
        R_AddEditorSurfsCmd();
        R_AddCmdSetMaterialColor( white );
    }
}

// A patch whose bounds meet the reach box (the candidate test the wireframe uses).
static bool WirePatchInReach( selbrush_t *b, float pad )
{
    if ( !PatchEligible( b ) )
        return false;
    const float *mins = b->def->mins, *maxs = b->def->maxs;
    if ( s_cursor[0] + pad < mins[0] || s_cursor[0] - pad > maxs[0]
      || s_cursor[1] + pad < mins[1] || s_cursor[1] - pad > maxs[1] )
        return false;
    const curvePatchDef_t *mesh = b->patch->def->curveDef;
    return mesh && mesh->width > 1 && mesh->height > 1 && mesh->verts;
}

// Never a hole: when the reach covers more grid than the line budget allows, the
// whole wireframe coarsens uniformly (every 2nd / 4th / 8th grid line, diagonals
// dropped) instead of some patches drawing and the rest being cut off.  The budget
// is what one frame's render-command buffer takes comfortably (16k segments).
static void DrawWireframeAoE()
{
    if ( !s_armed || s_tool == KTER_GRASS || s_hideWire || !s_cursorHave )
        return;
    const float reach  = s_outer * s_wireReach;
    const float pad    = s_shape == KTER_SQUARE ? reach * 1.42f : reach;
    const int   budget = 16384;

    // Pass 1: how many segments would the full grid take?  Only cells inside the
    // brush's bounding square count (the reach circle is a little less).
    int estimate = 0;
    for ( int pass = 0; pass < 2; ++pass )
    {
        selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( !WirePatchInReach( b, pad ) )
                continue;
            const curvePatchDef_t *mesh = b->patch->def->curveDef;
            const float *mins = b->def->mins, *maxs = b->def->maxs;
            const float ex = maxs[0] - mins[0], ey = maxs[1] - mins[1];
            // Fraction of the patch inside the reach square, per axis.
            float fx = 1.0f, fy = 1.0f;
            if ( ex > 1.0f )
            {
                const float x0 = mins[0] > s_cursor[0] - pad ? mins[0] : s_cursor[0] - pad;
                const float x1 = maxs[0] < s_cursor[0] + pad ? maxs[0] : s_cursor[0] + pad;
                fx = ( x1 - x0 ) / ex;
            }
            if ( ey > 1.0f )
            {
                const float y0 = mins[1] > s_cursor[1] - pad ? mins[1] : s_cursor[1] - pad;
                const float y1 = maxs[1] < s_cursor[1] + pad ? maxs[1] : s_cursor[1] + pad;
                fy = ( y1 - y0 ) / ey;
            }
            const float cells = (float)( ( mesh->width - 1 ) * ( mesh->height - 1 ) ) * ClampF( fx, 0.0f, 1.0f ) * ClampF( fy, 0.0f, 1.0f );
            estimate += (int)( cells * 3.0f ) + mesh->width + mesh->height;
        }
    }
    int stride = 1;
    while ( stride < 8 && estimate / ( stride * stride ) > budget )
        stride *= 2;

    // Pass 2: draw.  With a stride the cell corners are every stride-th grid line,
    // the last line clamped to the border so the patch edge always closes.
    KiwiLines_Begin( budget + 1024, 1 );
    for ( int pass = 0; pass < 2; ++pass )
    {
        const bool target = pass == 0 || s_affectUnselected;
        if ( target ) KiwiLines_Color( 0.95f, 0.95f, 0.95f );
        else          KiwiLines_Color( 0.5f, 0.5f, 0.55f );
        selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( !WirePatchInReach( b, pad ) )
                continue;
            const patchMesh_t *def = b->patch->def;
            const curvePatchDef_t *mesh = def->curveDef;
            const int mw = mesh->width, mh = mesh->height;
            const curveVert_t *verts = mesh->verts;
            const bool terrain = ( def->type & PATCH_TERRAIN ) != 0;
            for ( int row = 0; row + 1 < mh; row += stride )
            {
                int row1 = row + stride; if ( row1 > mh - 1 ) row1 = mh - 1;
                for ( int col = 0; col + 1 < mw; col += stride )
                {
                    int col1 = col + stride; if ( col1 > mw - 1 ) col1 = mw - 1;
                    const float *v00 = verts[col  + row  * mw].xyz;
                    const float *v10 = verts[col1 + row  * mw].xyz;
                    const float *v01 = verts[col  + row1 * mw].xyz;
                    const float *v11 = verts[col1 + row1 * mw].xyz;
                    const bool in00 = BrushDistance( s_cursor, v00 ) <= reach;
                    const bool in10 = BrushDistance( s_cursor, v10 ) <= reach;
                    const bool in01 = BrushDistance( s_cursor, v01 ) <= reach;
                    const bool in11 = BrushDistance( s_cursor, v11 ) <= reach;
                    if ( !in00 && !in10 && !in01 && !in11 )
                        continue;
                    if ( col == 0 && ( in00 || in01 ) ) KiwiLines_Add( v00, v01 );
                    if ( row == 0 && ( in00 || in10 ) ) KiwiLines_Add( v00, v10 );
                    if ( in11 || in10 ) KiwiLines_Add( v11, v10 );
                    if ( in11 || in01 ) KiwiLines_Add( v11, v01 );
                    if ( stride == 1 )
                    {
                        const bool turned = terrain && ( def->ctrl[col][row].turned_edge & 1 );
                        if ( turned ) { if ( in00 || in11 ) KiwiLines_Add( v00, v11 ); }
                        else          { if ( in01 || in10 ) KiwiLines_Add( v01, v10 ); }
                    }
                }
            }
        }
    }
    KiwiLines_Flush();
}

void KiwiTerrain_DrawWorld()
{
    if ( s_show && s_tool == KTER_TEXTURE && s_bandTreeOpen )
    {
        selbrush_t *node = FirstSelectedPatch();
        if ( node )
            DrawBandRings( node );
    }
    if ( s_armed && s_tool == KTER_GRASS )
        return;
    DrawWeightOverlay();                       // needs no cursor: the paint must read over pictures
    if ( !s_armed || !s_cursorHave || s_ringCount < 2 )
        return;

    DrawWireframeAoE();

    const bool showInner = s_inner > 0.0f && s_inner < s_outer && s_falloff != KTER_FO_CONSTANT;
    KiwiLines_Begin( s_ringCount * 2 + 4, 2 );
    // Off the patches (creation allowed) the ring turns blue on the base plane and
    // cyan on a brush/model so the operator knows where the chunks will land.
    if ( s_cursorKind == KCUR_PLANE )        KiwiLines_Color( 0.35f, 0.65f, 1.0f );
    else if ( s_cursorKind == KCUR_SURFACE ) KiwiLines_Color( 0.35f, 0.95f, 1.0f );
    else                                     KiwiLines_Color( 0.3f, 1.0f, 0.4f );
    for ( int i = 0; i < s_ringCount; ++i )
        if ( !KiwiLines_Add( s_ringOuter[i], s_ringOuter[( i + 1 ) % s_ringCount] ) )
            break;
    if ( showInner )
    {
        KiwiLines_Color( 0.6f, 1.0f, 0.65f );
        for ( int i = 0; i < s_ringCount; ++i )
            if ( !KiwiLines_Add( s_ringInner[i], s_ringInner[( i + 1 ) % s_ringCount] ) )
                break;
    }
    KiwiLines_Flush();

    if ( CreationAllowed() && s_expandCellCount > 0 )
    {
        KiwiLines_Begin( s_expandCellCount * 4, 1 );
        KiwiLines_Color( 0.3f, 1.0f, 0.4f );
        for ( int c = 0; c < s_expandCellCount; ++c )
        {
            const float x0 = s_expandCells[c][0], y0 = s_expandCells[c][1];
            const float SX = s_expandCells[c][2], SY = s_expandCells[c][3], z = s_expandCells[c][4] + 1.0f;
            float q[4][3] = { { x0, y0, z }, { x0 + SX, y0, z }, { x0 + SX, y0 + SY, z }, { x0, y0 + SY, z } };
            for ( int k = 0; k < 4; ++k )
                KiwiLines_Add( q[k], q[( k + 1 ) % 4] );
        }
        KiwiLines_Flush();
    }

    if ( s_tool == KTER_SETHEIGHT && !s_modShift )
    {
        KiwiLines_Begin( s_ringCount * 2 + 2, 1 );
        KiwiLines_Color( 0.3f, 1.0f, 0.4f );
        for ( int ring = 0; ring < ( showInner ? 2 : 1 ); ++ring )
        {
            const float (*src)[3] = ring == 0 ? s_ringOuter : s_ringInner;
            for ( int i = 0; i < s_ringCount; ++i )
            {
                float a[3] = { src[i][0], src[i][1], s_targetZ };
                const int j = ( i + 1 ) % s_ringCount;
                float b[3] = { src[j][0], src[j][1], s_targetZ };
                if ( !KiwiLines_Add( a, b ) )
                    break;
            }
        }
        float top[3] = { s_cursor[0], s_cursor[1], s_targetZ };
        KiwiLines_Add( s_cursor, top );
        KiwiLines_Flush();
    }
}

// ── camera overlay: the height colour scale ──────────────────────────────────
void KiwiTerrain_DrawOverlay( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( !HeatmapActive() || !s_heatValid || imgH < 160.0f || imgW < 240.0f )
        return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if ( !dl )
        return;

    // Geometry: a bar on the left edge, vertically centred, labels to its right.
    const float barW   = 16.0f;
    float       barH   = imgH * 0.45f;
    if ( barH > 260.0f ) barH = 260.0f;
    if ( barH < 120.0f ) barH = 120.0f;
    const float x0 = imgMinX + 14.0f;
    const float y0 = imgMinY + ( imgH - barH ) * 0.5f;
    const float x1 = x0 + barW;
    const float y1 = y0 + barH;
    const float lineH = ImGui::GetTextLineHeight();

    // Widest label decides the backdrop.
    const int ticks = 5;
    char labels[ticks][64];
    float labelW = 0.0f;
    for ( int i = 0; i < ticks; ++i )
    {
        const float t = (float)i / (float)( ticks - 1 );
        KiwiUnits_Format( labels[i], sizeof( labels[i] ), s_heatMinZ + ( s_heatMaxZ - s_heatMinZ ) * t );
        const float w = ImGui::CalcTextSize( labels[i] ).x;
        if ( w > labelW ) labelW = w;
    }
    const char *title = "height";
    const float titleW = ImGui::CalcTextSize( title ).x;
    float boxW = barW + 8.0f + labelW + 14.0f;
    if ( boxW < titleW + 8.0f ) boxW = titleW + 8.0f;
    dl->AddRectFilled( ImVec2( x0 - 6.0f, y0 - lineH - 8.0f ), ImVec2( x0 - 6.0f + boxW, y1 + lineH * 0.5f + 6.0f ),
                       IM_COL32( 18, 18, 22, 175 ), 3.0f );
    dl->AddText( ImVec2( x0 - 2.0f, y0 - lineH - 4.0f ), IM_COL32( 190, 196, 208, 225 ), title );

    // The gradient, top = high (red) to bottom = low (blue), in 32 bands.
    const int bands = 32;
    for ( int i = 0; i < bands; ++i )
    {
        const float tTop = 1.0f - (float)i / (float)bands;
        const float tBot = 1.0f - (float)( i + 1 ) / (float)bands;
        const unsigned cT = HeatColor( tTop ), cB = HeatColor( tBot );   // packed BGRA
        const ImU32 top = IM_COL32( ( cT >> 16 ) & 255, ( cT >> 8 ) & 255, cT & 255, 255 );
        const ImU32 bot = IM_COL32( ( cB >> 16 ) & 255, ( cB >> 8 ) & 255, cB & 255, 255 );
        const float ya = y0 + barH * (float)i / (float)bands;
        const float yb = y0 + barH * (float)( i + 1 ) / (float)bands;
        dl->AddRectFilledMultiColor( ImVec2( x0, ya ), ImVec2( x1, yb ), top, top, bot, bot );
    }
    dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ), IM_COL32( 80, 84, 96, 200 ), 0.0f, 0, 1.0f );

    // Ticks and labels (top = max).
    for ( int i = 0; i < ticks; ++i )
    {
        const float t = (float)i / (float)( ticks - 1 );
        const float y = y1 - barH * t;
        dl->AddLine( ImVec2( x1, y ), ImVec2( x1 + 5.0f, y ), IM_COL32( 220, 224, 232, 220 ), 1.0f );
        dl->AddText( ImVec2( x1 + 8.0f, y - lineH * 0.5f ), IM_COL32( 220, 224, 232, 230 ), labels[i] );
    }

    // The cursor's height as a marker on the bar.
    if ( s_cursorHave )
    {
        const float span = s_heatMaxZ - s_heatMinZ;
        const float t = span > 0.0f ? ClampF( ( s_cursor[2] - s_heatMinZ ) / span, 0.0f, 1.0f ) : 0.5f;
        const float y = y1 - barH * t;
        dl->AddTriangleFilled( ImVec2( x0 - 7.0f, y - 4.0f ), ImVec2( x0 - 7.0f, y + 4.0f ), ImVec2( x0 - 1.0f, y ),
                               IM_COL32( 255, 255, 255, 240 ) );
        dl->AddLine( ImVec2( x0, y ), ImVec2( x1, y ), IM_COL32( 255, 255, 255, 200 ), 1.0f );
    }

    // KIWI (2026-09-17, user: "add a 2nd line for the set height target height on this
    // graph"): the Set height target as a magenta line with its marker on the bar's RIGHT
    // (the cursor's white one sits on the left) and the value beside the legend, so the
    // two read apart and Alt+wheel has something to watch.  A target outside the colour
    // range pins to that end of the bar and says which way it lies.
    if ( s_armed && s_tool == KTER_SETHEIGHT )
    {
        const float span = s_heatMaxZ - s_heatMinZ;
        const float raw  = span > 0.0f ? ( s_targetZ - s_heatMinZ ) / span : 0.5f;
        const float t    = ClampF( raw, 0.0f, 1.0f );
        const float y    = y1 - barH * t;
        const ImU32 col  = IM_COL32( 255, 80, 235, 255 );
        dl->AddLine( ImVec2( x0 - 1.0f, y ), ImVec2( x1 + 1.0f, y ), col, 2.0f );
        dl->AddTriangleFilled( ImVec2( x1 + 8.0f, y - 4.0f ), ImVec2( x1 + 8.0f, y + 4.0f ), ImVec2( x1 + 2.0f, y ), col );

        char value[64], text[96];
        KiwiUnits_Format( value, sizeof( value ), s_targetZ );
        _snprintf( text, sizeof( text ), "target %s%s", value, raw > 1.0f ? " (above)" : ( raw < 0.0f ? " (below)" : "" ) );
        text[sizeof( text ) - 1] = '\0';
        const ImVec2 ts = ImGui::CalcTextSize( text );
        const float tx = x0 - 6.0f + boxW + 4.0f;
        float ty = y - lineH * 0.5f;
        if ( ty < y0 - lineH ) ty = y0 - lineH;
        dl->AddRectFilled( ImVec2( tx - 3.0f, ty - 1.0f ), ImVec2( tx + ts.x + 3.0f, ty + ts.y + 1.0f ),
                           IM_COL32( 18, 18, 22, 190 ), 3.0f );
        dl->AddText( ImVec2( tx, ty ), col, text );
    }
}

// ── J: join selected terrain sheets ──────────────────────────────────────────
bool KiwiTerrain_CanJoinSelected()
{
    Load();
    std::vector<selbrush_t *> sheets;
    SelectedSheets( sheets );
    if ( sheets.size() < 2 )
        return false;
    size_t ia, ib; int axis;
    return FindJoinPair( sheets, &ia, &ib, &axis );
}

int KiwiTerrain_JoinSelected()
{
    Load();
    std::vector<selbrush_t *> sheets;
    SelectedSheets( sheets );
    if ( sheets.size() < 2 )
        return -1;
    // Only a pure patch selection takes this arm; mixed selections fall through.
    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        if ( !NodeIsPatch( b ) )
            return -1;

    int joins = 0;
    for ( ;; )
    {
        size_t ia, ib; int axis;
        if ( !FindJoinPair( sheets, &ia, &ib, &axis ) )
            break;
        selbrush_t *na = sheets[ia], *nb = sheets[ib];
        patchMesh_t *a = na->patch->def;

        // One undo record per join: A through the paint marker, B through the
        // Edit->Delete bracket.
        Undo_ClearRedo();
        Undo_GeneralStart( "join terrain" );
        Patch_Paint( &selected_brushes );
        Patch_Paint( &active_brushes );
        a->xx22b = 1;
        Patch_PaintMarkUndo( a );
        MergeInto( a, nb->patch->def, axis );
        ForgetDef( nb->patch->def );
        Select_Deselect( 1 );
        Select_Brush( nb, 0, 0, 0 );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
        Select_Delete();
        Undo_EndBrushList( &selected_brushes );
        Patch_Rebuild( a, 1 );
        ++a->version;
        Patch_PaintFinish( &selected_brushes );
        Patch_PaintFinish( &active_brushes );
        Undo_End();

        sheets.erase( sheets.begin() + ib );
        for ( size_t i = 0; i < sheets.size(); ++i )
            Select_Brush( sheets[i], 0, 0, 0 );
        ++joins;
    }
    s_targets.clear();
    g_nUpdateBits = -1;
    if ( joins )
        Sys_Printf( "Join: merged %i terrain patch pair%s.\n", joins, joins == 1 ? "" : "s" );
    else
        Sys_Printf( "Join: the selected terrain patches do not share a full edge with matching "
                    "materials, layers and cell size, or the result would exceed 16 points across.\n" );
    return joins;
}

// ── -kiwitest entry points (kiwi_test.cpp `terrain` verb) ───────────────────
bool KiwiTerrain_TestSetTool( const char *name )
{
    Load();
    static const char *const names[KTER_TOOL_COUNT] =
        { "raise", "setheight", "smooth", "noise", "texture", "blend", "grass", "trim" };
    for ( int i = 0; i < KTER_TOOL_COUNT; ++i )
        if ( !_stricmp( name, names[i] ) )
        {
            SetTool( i );
            Save();
            return true;
        }
    return false;
}

bool KiwiTerrain_TestSet( const char *key, float value )
{
    Load();
    if      ( !_stricmp( key, "outer" ) )       s_outer = value;
    else if ( !_stricmp( key, "inner" ) )       s_inner = value;
    else if ( !_stricmp( key, "strength" ) )    s_strength = value;
    else if ( !_stricmp( key, "speed" ) )       s_amount = value;
    else if ( !_stricmp( key, "falloff" ) )     s_falloff = (int)value;
    else if ( !_stricmp( key, "shape" ) )       s_shape = (int)value;
    else if ( !_stricmp( key, "chunk" ) )       s_chunkSize = value;
    else if ( !_stricmp( key, "expand" ) )      s_expand = value != 0.0f;
    else if ( !_stricmp( key, "basez" ) )       s_createZ = value;
    else if ( !_stricmp( key, "cells" ) )       s_createCells = (int)value;
    else if ( !_stricmp( key, "surfaces" ) )    s_createOnSurfaces = value != 0.0f;
    else if ( !_stricmp( key, "targetz" ) )     s_targetZ = value;
    else if ( !_stricmp( key, "unselected" ) )  s_affectUnselected = value != 0.0f;
    else if ( !_stricmp( key, "carry" ) )       s_carryObjects = value != 0.0f;
    else if ( !_stricmp( key, "feather" ) )     s_setHeightFeather = value != 0.0f;
    else if ( !_stricmp( key, "contain" ) )     s_setHeightContain = value != 0.0f;
    else if ( !_stricmp( key, "refine" ) )      s_autoRefine = value != 0.0f;
    else if ( !_stricmp( key, "layerequal" ) )  { s_layerDepthEqual = value != 0.0f; RestripTwins(); }
    else if ( !_stricmp( key, "refinemin" ) )   s_refineMin = value;
    else if ( !_stricmp( key, "hidewire" ) )    s_hideWire = value != 0.0f;
    else if ( !_stricmp( key, "wirereach" ) )   s_wireReach = value;
    else if ( !_stricmp( key, "heatmap" ) )     { s_heatmap = value != 0.0f; HeatmapRefresh(); }
    else if ( !_stricmp( key, "heatalways" ) )  { s_heatAlways = value != 0.0f; HeatmapRefresh(); }
    else if ( !_stricmp( key, "paintbrushes" ) ) s_paintBrushes = value != 0.0f;
    else if ( !_stricmp( key, "flattol" ) )     s_flatTol = value;
    else if ( !_stricmp( key, "flatthick" ) )   s_flatThick = value;
    else if ( !_stricmp( key, "weightview" ) )  { s_weightView = value != 0.0f; HeatmapRefresh(); }
    else
        return false;
    Sanitize();
    Save();
    RebuildRing();
    g_nUpdateBits |= W_CAMERA;
    return true;
}

// Tessellate / Split the selection exactly as the panel buttons do (test DSL).
static int CountPatches()
{
    int n = 0;
    for ( int pass = 0; pass < 2; ++pass )
    {
        selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            if ( b->patch )
                ++n;
    }
    return n;
}

bool KiwiTerrain_TestTessellate( float cell )
{
    Load();
    if ( cell > 0.0f )
        s_tessCell = ClampF( cell, 4.0f, 4096.0f );
    const int before = CountPatches();
    TessellateSelected();
    return CountPatches() != before;
}

// The panel's "Select terrain stacked on other terrain"; returns how many were selected.
int KiwiTerrain_TestSelectStacked()
{
    Load();
    return SelectStackedTerrain();
}

bool KiwiTerrain_TestSplit( float chunk )
{
    Load();
    if ( chunk > 0.0f )
        s_chunkSize = chunk;
    const int before = CountPatches();
    SplitOversized();
    return CountPatches() != before;
}

bool KiwiTerrain_TestSetPaintMaterial( const char *name )
{
    Load();
    if ( !name )
        return false;
    if ( !_stricmp( name, "base" ) )
    {
        s_paintBase = true;
    }
    else
    {
        strncpy( s_paintMaterial, name, sizeof( s_paintMaterial ) - 1 );
        s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
        s_paintBase = false;
    }
    Save();
    return true;
}

void KiwiTerrain_TestArm( bool armed )
{
    Load();
    if ( armed )
    {
        s_softSelect = false;
        SyncSoftSelect();
    }
    SetArmed( armed );
}

// One whole stroke without a mouse: a vertical ray dropped through (x, y) is resolved
// exactly like the camera cursor (patches, then - with creation allowed - surfaces and
// the base plane), the press runs, the stroke is held for `seconds`, then released.
bool KiwiTerrain_TestStroke( float x, float y, float seconds, bool shift, bool ctrl )
{
    Load();
    if ( !s_armed || s_tool == KTER_GRASS )
        return false;
    if ( KiwiCmd_Active() )
        return false;
    ray_t ray;
    ray.origin[0] = x; ray.origin[1] = y; ray.origin[2] = 65536.0f;
    ray.dir[0] = 0.0f; ray.dir[1] = 0.0f; ray.dir[2] = -1.0f;
    float hit[3];
    byte  picked[4];
    if ( !ResolveCursor( ray, hit, picked ) )
    {
        ClearCursor();
        SetStatus( "Armed. No patch under the cursor." );
        return false;
    }
    s_cursorHave = true;
    memcpy( s_cursor, hit, sizeof( hit ) );
    RebuildRing();
    if ( !BeginStroke( shift, ctrl, picked ) )
        return false;
    if ( s_stroke && s_tool != KTER_TRIM && seconds > 0.0f )
    {
        s_accumDt = seconds;
        ApplyStroke();
    }
    EndStroke();
    return true;
}
