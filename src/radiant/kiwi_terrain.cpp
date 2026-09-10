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

#include <imgui/imgui.h>

#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>   // TECHNIQUE_UNLIT, R_AddCmdSetMaterialColor - the weight overlay
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
#include <map>
#include <string>
#include <vector>

extern int          Sys_Printf( const char *fmt, ... );
extern int          g_nUpdateBits;
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
extern void         Undo_AddBrush( entity_brush_s *pBrushInst );       // undo.cpp:494 (takes the brush DEF)
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
        "LMB snap to target Z   V / Ctrl+LMB pick the height under the pointer   Shift+LMB smooth",
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
    float s_chunkSize   = 2048.0f;   // max patch side; the expander/split chunk size
    int   s_density     = 8;         // Tessellate: cells across the whole patch (any size; >15 splits)
    bool  s_expand      = false;     // Raise: "Allow terrain creation" - lay chunks in empty lattice cells
    float s_createZ     = 0.0f;      // creation: base height where nothing at all is under the cursor
    int   s_createCells = 8;         // creation: cells per side of a chunk laid with no terrain in reach
    bool  s_createOnSurfaces = true; // creation: the cursor lands on brushes/models before the base plane
    bool  s_softSelect  = false;
    bool  s_hideWire    = false;     // armed: hide the patch wireframe entirely (Tab toggles)
    float s_wireReach   = 1.25f;     // armed: wireframe shown within outer radius x this
    bool  s_heatmap     = true;      // armed height tools: patches wear a height gradient

    // ── session ──────────────────────────────────────────────────────────────
    bool  s_loaded = false;
    bool  s_show   = false;
    bool  s_armed  = false;

    bool  s_cursorHave = false;
    float s_cursor[3]  = { 0.0f, 0.0f, 0.0f };
    selbrush_t *s_cursorNode = nullptr;      // the patch under the cursor (ring drop target)
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
    std::vector<selbrush_t *> s_targets;     // the patches one stroke touches
    std::vector<patchMesh_t *> s_dirtyDefs;  // changed this frame; rebuilt once
    bool  s_dirtyBounds = false;

    std::map<std::string, Material *>  s_blendTwins;   // material -> preview twin (null = failed)
    std::map<std::string, std::string> s_twinErr;      // material -> why

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
        s_noiseScale = ClampF( s_noiseScale, 0.25f, 2048.0f );
        s_noiseFreq  = ClampF( s_noiseFreq, 0.0001f, 1.0f );
        s_blendWeight = ClampF( s_blendWeight, 0.0f, 1.0f );
        if ( s_blendRings < 1 ) s_blendRings = 1;
        if ( s_blendRings > 4 ) s_blendRings = 4;
        s_chunkSize = ClampF( s_chunkSize, 256.0f, 8192.0f );
        if ( s_density < 1 ) s_density = 1;
        if ( s_density > 120 ) s_density = 120;
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
        s_chunkSize = ReadFloat( "ChunkSize2", 2048.0f );
        s_density   = Radiant_ProfileGetInt( KTER_PROFILE, "DensityCells", 8 );
        s_expand    = Radiant_ProfileGetInt( KTER_PROFILE, "Expand", 0 ) != 0;
        s_createZ   = ReadFloat( "CreateZ", 0.0f );
        s_createCells = Radiant_ProfileGetInt( KTER_PROFILE, "CreateCells", 8 );
        s_createOnSurfaces = Radiant_ProfileGetInt( KTER_PROFILE, "CreateOnSurfaces", 1 ) != 0;
        s_wireReach = ReadFloat( "WireReach", 1.25f );
        s_heatmap   = Radiant_ProfileGetInt( KTER_PROFILE, "Heatmap", 1 ) != 0;
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
        WriteFloat( "ChunkSize2", s_chunkSize );
        Radiant_ProfileSetInt( KTER_PROFILE, "DensityCells", s_density );
        Radiant_ProfileSetInt( KTER_PROFILE, "Expand", s_expand ? 1 : 0 );
        WriteFloat( "CreateZ", s_createZ );
        Radiant_ProfileSetInt( KTER_PROFILE, "CreateCells", s_createCells );
        Radiant_ProfileSetInt( KTER_PROFILE, "CreateOnSurfaces", s_createOnSurfaces ? 1 : 0 );
        WriteFloat( "WireReach", s_wireReach );
        Radiant_ProfileSetInt( KTER_PROFILE, "Heatmap", s_heatmap ? 1 : 0 );
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
            return true;
        }
        s_cursorNode = nullptr;
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
        return fabsf( p10[1] - p00[1] ) < 1.0f && fabsf( p01[0] - p00[0] ) < 1.0f
            && fabsf( p11[0] - p10[0] ) < 1.0f && fabsf( p11[1] - p01[1] ) < 1.0f;
    }

    // Height of the cursor patch at (x,y): bilinear off the control grid for a sheet
    // (O(1)), the cursor height otherwise.  No ray casts: the ring is 64 of these per move.
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

    // Texture paint with a material on the brush (or "erase to base") needs no
    // selection: every eligible patch is a target.
    bool PaintAnywhere()
    {
        return s_tool == KTER_TEXTURE && ( s_paintBase || s_paintMaterial[0] != 0 );
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
                // Pass 1: weld with coincident border points on other patches.
                float  zSum = P->xyz[2];
                int    zN = 1;
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
                        }
                    }
                }
                if ( !group.empty() )
                {
                    const float z = zSum / (float)zN;
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
                    continue;
                }
                // Pass 2: no partner — conform to the neighbour border segment P lies on.
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
                    bool done = false;
                    for ( int m = 0; m < bn && !done; ++m )
                    {
                        const float *q0 = B->ctrl[bi[m]][bj[m]].xyz;
                        const float *q1 = B->ctrl[bi[( m + 1 ) % bn]][bj[( m + 1 ) % bn]].xyz;
                        const float dx = q1[0] - q0[0], dy = q1[1] - q0[1];
                        const float len2 = dx * dx + dy * dy;
                        if ( len2 < 1e-4f )
                            continue;
                        float t = ( ( P->xyz[0] - q0[0] ) * dx + ( P->xyz[1] - q0[1] ) * dy ) / len2;
                        if ( t < -0.001f || t > 1.001f )
                            continue;
                        if ( t < 0.0f ) t = 0.0f;
                        if ( t > 1.0f ) t = 1.0f;
                        const float ex = P->xyz[0] - ( q0[0] + dx * t ), ey = P->xyz[1] - ( q0[1] + dy * t );
                        if ( ex * ex + ey * ey > KTER_WELD * KTER_WELD )
                            continue;
                        // P sits on B's edge between two of B's vertices: only B's straight
                        // segment can be honoured, so P takes its height there.
                        P->xyz[2] = q0[2] + ( q1[2] - q0[2] ) * t;
                        done = true;
                    }
                    if ( done )
                        break;
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
                float mean[4];
                for ( int c = 0; c < 4; ++c )
                {
                    mean[c] = (float)( (const byte *)&P->vert_color )[c];
                    for ( size_t g = 0; g < group.size(); ++g )
                        mean[c] += (float)( (const byte *)&group[g]->vert_color )[c];
                    mean[c] /= (float)( group.size() + 1 );
                }
                for ( int c = 0; c < 4; ++c )
                    ( (byte *)&P->vert_color )[c] = (byte)(int)( mean[c] + 0.5f );
                for ( size_t g = 0; g < group.size(); ++g )
                {
                    bool differs = false;
                    for ( int c = 0; c < 4 && !differs; ++c )
                        differs = ( (const byte *)&group[g]->vert_color )[c] != ( (const byte *)&P->vert_color )[c];
                    if ( !differs )
                        continue;
                    TouchNeighbour( all[groupPatch[g]].def, all[groupPatch[g]].target );
                    NoteDirty( all[groupPatch[g]].def, false );
                    memcpy( &group[g]->vert_color, &P->vert_color, 4 );
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

                MarkTouched( def );
                changed = true;
                byte *col = (byte *)&cp->vert_color;
                switch ( op )
                {
                case OP_RAISE:
                    cp->xyz[2] += sign * at;
                    break;
                case OP_SETHEIGHT:
                {
                    const float f = w > 1.0f ? 1.0f : w;
                    cp->xyz[2] += ( s_targetZ - cp->xyz[2] ) * f;
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

    void Stamp( const float *center, kterOp_t op, float sign, float dt )
    {
        if ( dt <= 0.0f )
            return;
        bool any = false;
        for ( size_t i = 0; i < s_targets.size(); ++i )
            any |= StampPatch( s_targets[i], op, center, sign, dt );
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
        if ( s_cursorNode && s_cursorNode->patch && s_cursorNode->patch->def == def )
            s_cursorNode = nullptr;
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
            if ( !PatchEligible( b ) )
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
        selbrush_t *node = s_cursorNode;
        if ( !node && CreationAllowed() )
            node = NearestEligiblePatch( s_cursor, s_outer + s_chunkSize );
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
                if ( AnyPatchCovers( mx, my ) )
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

    void ExpandUnderBrush()
    {
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

    // Resample a sheet patch to `points` x `points` (heights, colours and texcoords
    // bilinear from the old grid; borders reproduce the old border exactly at the new
    // points).  16 is the format's cap: for more, split into smaller chunks first.
    void ResamplePatch( patchMesh_t *def, int points )
    {
        if ( points < 2 || points > 16 || def->width < 2 || def->height < 2 )
            return;
        patchMesh_t *old = (patchMesh_t *)operator new( sizeof( patchMesh_t ) );
        memcpy( old, def, sizeof( patchMesh_t ) );
        const float *p00 = old->ctrl[0][0].xyz;
        const float *p10 = old->ctrl[old->width - 1][0].xyz;
        const float *p01 = old->ctrl[0][old->height - 1].xyz;
        const float ex = p10[0] - p00[0], ey = p01[1] - p00[1];
        for ( int i = 0; i < points; ++i )
            for ( int j = 0; j < points; ++j )
            {
                const float fu = (float)i / (float)( points - 1 );
                const float fv = (float)j / (float)( points - 1 );
                // Fractional old-grid coordinates.
                const float u = fu * (float)( old->width - 1 ), v = fv * (float)( old->height - 1 );
                int i0 = (int)u, j0 = (int)v;
                if ( i0 > old->width - 2 )  i0 = old->width - 2;
                if ( j0 > old->height - 2 ) j0 = old->height - 2;
                const int i1 = i0 + 1, j1 = j0 + 1;
                const float tu = u - (float)i0, tv = v - (float)j0;
                const drawVert_t &a = old->ctrl[i0][j0], &b = old->ctrl[i1][j0];
                const drawVert_t &c = old->ctrl[i0][j1], &d = old->ctrl[i1][j1];
                drawVert_t *cp = &def->ctrl[i][j];
                memset( cp, 0, sizeof( *cp ) );
                cp->xyz[0] = p00[0] + ex * fu;
                cp->xyz[1] = p00[1] + ey * fv;
                cp->xyz[2] = ( a.xyz[2] * ( 1 - tu ) + b.xyz[2] * tu ) * ( 1 - tv )
                           + ( c.xyz[2] * ( 1 - tu ) + d.xyz[2] * tu ) * tv;
                const float *ta = (const float *)&a.texCoord, *tb = (const float *)&b.texCoord;
                const float *tc = (const float *)&c.texCoord, *td = (const float *)&d.texCoord;
                float *tt = (float *)&cp->texCoord;
                for ( int k = 0; k < 6; ++k )
                    tt[k] = ( ta[k] * ( 1 - tu ) + tb[k] * tu ) * ( 1 - tv ) + ( tc[k] * ( 1 - tu ) + td[k] * tu ) * tv;
                cp->savedTexCoord = cp->texCoord;
                for ( int k = 0; k < 4; ++k )
                {
                    const float ca = ( (const byte *)&a.vert_color )[k], cb = ( (const byte *)&b.vert_color )[k];
                    const float cc = ( (const byte *)&c.vert_color )[k], cd = ( (const byte *)&d.vert_color )[k];
                    const float m = ( ca * ( 1 - tu ) + cb * tu ) * ( 1 - tv ) + ( cc * ( 1 - tu ) + cd * tu ) * tv;
                    ( (byte *)&cp->vert_color )[k] = (byte)(int)( ClampF( m, 0.0f, 255.0f ) + 0.5f );
                }
            }
        def->width = def->height = points;
        operator delete( old );
    }

    // Tessellate the selected terrain to `cells` cells across the whole patch.  Up to 15
    // cells it resamples in place (16 points is the CoD4 patch cap); above that the patch
    // becomes k x k chunks of cells/k cells each, every chunk sampled from the original
    // grid so the shared chunk edges coincide exactly.  `cells` is rounded up to a
    // multiple of k when needed.
    void TessellateSelected()
    {
        int cells = s_density < 1 ? 1 : s_density;
        int k = ( cells + 14 ) / 15;                       // chunks per side
        if ( k < 1 ) k = 1;
        const int perChunk = ( cells + k - 1 ) / k;        // cells per chunk side (<= 15)
        cells = perChunk * k;

        std::vector<selbrush_t *> nodes;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( PatchEligible( b ) && GridIsSheet( b->patch->def ) )
                nodes.push_back( b );
        if ( nodes.empty() )
        {
            Sys_Printf( "Terrain Sculpt: select flat-sheet terrain patches to tessellate.\n" );
            return;
        }

        if ( k == 1 )
        {
            std::vector<patchMesh_t *> defs;
            for ( size_t i = 0; i < nodes.size(); ++i )
                defs.push_back( nodes[i]->patch->def );
            MultiEditBegin( defs, "tessellate terrain" );
            for ( size_t i = 0; i < defs.size(); ++i )
            {
                ResamplePatch( defs[i], cells + 1 );
                Patch_Rebuild( defs[i], 1 );
            }
            MultiEditEnd( defs );
            Sys_Printf( "Terrain Sculpt: %i patch%s tessellated to %i cells across.\n",
                        (int)nodes.size(), nodes.size() == 1 ? "" : "es", cells );
            return;
        }

        // Split path: k x k chunks, each perChunk+1 points, sampled from the original.
        Select_Deselect( 1 );
        Undo_ClearRedo();
        Undo_GeneralStart( "tessellate terrain" );
        for ( size_t i = 0; i < nodes.size(); ++i )
            Select_Brush( nodes[i], 0, 0, 0 );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
        std::vector<selbrush_t *> created;
        for ( size_t bi = 0; bi < nodes.size(); ++bi )
        {
            selbrush_t *src = nodes[bi];
            const float *mins = src->def->mins, *maxs = src->def->maxs;
            const float ex = maxs[0] - mins[0], ey = maxs[1] - mins[1];
            const float sx = ex / (float)k, sy = ey / (float)k;
            const float size = sx < sy ? sx : sy;
            for ( int cy = 0; cy < k; ++cy )
                for ( int cx = 0; cx < k; ++cx )
                    created.push_back( CreateChunk( src->patch->def, src->owner,
                                                    mins[0] + sx * (float)cx, mins[1] + sy * (float)cy,
                                                    size, perChunk + 1, src->patch->def, 0.0f ) );
        }
        for ( size_t i = 0; i < nodes.size(); ++i )
            ForgetDef( nodes[i]->patch->def );
        Select_Delete();
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
        for ( size_t i = 0; i < created.size(); ++i )
            Select_Brush( created[i], 0, 0, 0 );
        s_targets.clear();
        g_nUpdateBits = -1;
        Sys_Printf( "Terrain Sculpt: %i patch%s tessellated to %i cells across as %ix%i chunks of %i cells.\n",
                    (int)nodes.size(), nodes.size() == 1 ? "" : "es", cells, k, k, perChunk );
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
        const float dtEach = s_accumDt / (float)n;
        for ( int k = 1; k <= n; ++k )
        {
            float c[3];
            const float f = (float)k / (float)n;
            for ( int a = 0; a < 3; ++a )
                c[a] = s_haveLastCenter ? s_lastCenter[a] + ( s_cursor[a] - s_lastCenter[a] ) * f : s_cursor[a];
            Stamp( c, OpForStroke(), sign, dtEach );
        }
        memcpy( s_lastCenter, s_cursor, sizeof( s_cursor ) );
        s_haveLastCenter = true;
        s_accumDt = 0.0f;
        if ( CreationAllowed() && !s_modShift && !s_modCtrl )
            ExpandUnderBrush();
        FlushDirty();
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
        return s_armed && s_heatmap
            && ( s_tool == KTER_RAISE || s_tool == KTER_SETHEIGHT || s_tool == KTER_SMOOTH
              || s_tool == KTER_NOISE || s_tool == KTER_TRIM );
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

    // Re-upload every patch's visuals (the VB colours come from LayerUpload).
    void RebuildAllPatchVisuals()
    {
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( NodeIsPatch( b ) )
                {
                    Patch_Rebuild( b->patch->def, 0 );
                    ++b->patch->def->version;
                }
        }
        g_nUpdateBits = -1;
    }

    // Arm / tool / toggle transitions: refresh the range and the uploads.
    void HeatmapRefresh()
    {
        if ( s_armed && s_heatmap )
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
        Patch_PaintFinish( &selected_brushes );
        Patch_PaintFinish( &active_brushes );
        if ( s_undoOpen )
            Undo_End();
        // The gradient range follows the terrain: re-tint everything only when the
        // stroke pushed the extremes (the touched patches re-uploaded already).
        if ( HeatmapActive() && ComputeHeatRange() )
            RebuildAllPatchVisuals();
        s_undoOpen = false;
        s_stroke   = false;
        g_nUpdateBits = -1;
        if ( s_touched || s_created || s_facesPainted )
            SetStatus( "Armed. Last stroke: %i stamp%s over %i patch%s, %i chunk%s laid, %i brush face%s painted.",
                       s_stamps, s_stamps == 1 ? "" : "s", s_touched, s_touched == 1 ? "" : "es",
                       s_created, s_created == 1 ? "" : "s", s_facesPainted, s_facesPainted == 1 ? "" : "s" );
        else if ( CreationAllowed() )
            SetStatus( "Armed. The stroke reached no control point and every cell under it was covered." );
        else
            SetStatus( "Armed. The stroke reached no control point (grow the radius or select the patch)." );
        if ( s_layersAdded || s_layersFull )
            Sys_Printf( "Terrain Sculpt: '%s' added as a layer on %i patch%s%s.\n",
                        s_paintMaterial, s_layersAdded, s_layersAdded == 1 ? "" : "es",
                        s_layersFull ? " (some patches already carry 4 layers and were skipped)" : "" );
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

        ImGui::SeparatorText( "Tool" );
        for ( int i = 0; i < KTER_TOOL_COUNT; ++i )
        {
            if ( i % 4 )
                ImGui::SameLine();
            if ( ImGui::RadioButton( KTER_TOOL_NAME[i], s_tool == i ) )
            {
                SetTool( i );
                changed = true;
            }
        }
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
            ImGui::TextDisabled( "[ ] or + / - resize (hold to repeat)   Ctrl+wheel resize   Shift+wheel strength" );
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
        }

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
            ImGui::TextDisabled( "Snaps instantly; the green ghost ring shows the target height." );
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

        if ( s_tool != KTER_GRASS )
        {
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
            ImGui::SetNextItemWidth( 160.0f );
            changed |= ImGui::SliderInt( "Cells across each selected patch", &s_density, 1, 120,
                                         "%d", ImGuiSliderFlags_Logarithmic );
            {
                const int k = ( s_density + 14 ) / 15;
                const int per = ( s_density + k - 1 ) / k;
                if ( k > 1 )
                    ImGui::TextDisabled( "= %ix%i chunks of %i cells (a patch holds 16 points, so it splits)",
                                         k, k, per );
                else
                    ImGui::TextDisabled( "= one %ix%i grid (%i triangles)", s_density + 1, s_density + 1,
                                         s_density * s_density * 2 );
            }
            if ( ImGui::Button( "Tessellate" ) )
                TessellateSelected();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Rebuilds each selected terrain patch at this many cells across. Heights,\n"
                                   "layer weights and texcoords are interpolated from the current grid;\n"
                                   "chunk edges coincide exactly. Neighbouring patches keep their own grid,\n"
                                   "so tessellate a whole region to the same value. Undoable." );
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
            ImGui::TextDisabled( "Terrain meshes only - bezier curves are never sculpted, welded or painted." );
            if ( ImGui::Checkbox( "Soft-select vertex drags (legacy Drag Up/Down)", &s_softSelect ) )
            {
                SyncSoftSelect();
                if ( s_softSelect )
                    SetArmed( false );
            }
        }

        if ( changed )
        {
            Save();
            SyncSoftSelect();
            RebuildRing();
            g_nUpdateBits |= W_CAMERA;
        }

        ImGui::Separator();
        if ( ImGui::Button( s_armed ? "Disarm (Esc)"
                                    : ( s_tool == KTER_GRASS ? "Scatter (LMB in camera)" : "Sculpt (LMB in camera)" ) ) )
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

    s_stroke    = true;
    s_stamps    = 0;
    s_touched   = 0;
    s_created   = 0;
    s_layersAdded = 0;
    s_layersFull  = 0;
    s_facesPainted = 0;
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

bool KiwiTerrain_HandleWheel( float steps, bool shift, bool ctrl )
{
    if ( !s_armed || steps == 0.0f )
        return false;
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
    else if ( !_stricmp( key, "hidewire" ) )    s_hideWire = value != 0.0f;
    else if ( !_stricmp( key, "wirereach" ) )   s_wireReach = value;
    else if ( !_stricmp( key, "heatmap" ) )     { s_heatmap = value != 0.0f; HeatmapRefresh(); }
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
