#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Terrain Sculpt — see kiwi_terrain.h.
//
// Brush tools over the control points of terrain patches (the selection, or every
// visible patch): raise/dig, set height, smooth, noise, texture layers, blend, grass,
// trim; plus chunk creation ("Allow terrain creation"), split, tessellate, flatten, join.
//
// HEIGHT strokes work on one vertex graph over the terrain around the brush (see "the
// height graph").  PAINT strokes work per patch, with a weight seam pass under the brush.
//
// TEXTURE LAYERS live on ONE patch: patchMesh_t.kiwiLayer[4] names up to four extra
// materials, the weight of layer k is vert_color byte k.  The .map carries them as
// "kiwilayer <slot> <material>" and cod4map expands them into the stock duplicate-patch
// layering at parse time; the camera draws each used slot as one extra blended run.

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
#include <unordered_map>
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
    bool  s_carryObjects = false;       // height strokes also move what rests on the terrain (CarryRiders)
    bool  s_setHeightFeather = false;   // Set height: ramp the edge by falloff x strength (off = exact, hard edge)
    bool  s_setHeightContain = true;    // ALL height tools: never change anything outside the ring (VertReach)
    bool  s_layerDepthEqual = true;     // paint layers draw depth-EQUAL to their own base (see StripDecalOffset)
    float s_chunkSize   = 2048.0f;   // max patch side; the expander/split chunk size
    float s_tessCell    = 64.0f;     // Tessellate: ABSOLUTE cell size in world units (>15 cells an axis splits)
    bool  s_expand      = false;     // Raise: "Allow terrain creation" - lay chunks in empty lattice cells
    float s_createZ     = 0.0f;      // creation: base height where nothing at all is under the cursor
    int   s_createCells = 8;         // creation: cells per side of a chunk laid with no terrain in reach
    bool  s_createOnSurfaces = true; // creation: the cursor lands on brushes/models before the base plane
    bool  s_hideWire   = false;     // armed: hide the patch wireframe entirely (Tab toggles)
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
    // Its def at pick time, a KEY ONLY: an undo / Delete can free the node behind the tool's
    // back, so nothing reads s_cursorNode without LiveCursorNode() first.
    const patchMesh_t *s_cursorDef = nullptr;
    // What the cursor landed on.  Only "Allow terrain creation" lets it leave the patches.
    enum kterCursor_t { KCUR_NONE = 0, KCUR_PATCH, KCUR_GAP, KCUR_SURFACE, KCUR_PLANE };
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

    // Texture paint carries its MATERIAL on the brush: any terrain touched gets it as a
    // layer (added on first touch, 4 max).  "Erase to base" paints every layer out.
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

    // Every node of the selected list, then (alsoActive) of the active list.
    template <class F> void ForEachNode( bool alsoActive, F fn )
    {
        for ( int pass = 0; pass < ( alsoActive ? 2 : 1 ); ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                fn( b );
        }
    }

    // Does the XY box mins..maxs meet box[4] (minx miny maxx maxy), `pad` units of slack?
    bool BoundsMeet( const float *mins, const float *maxs, const float box[4], float pad )
    {
        return !( maxs[0] < box[0] - pad || mins[0] > box[2] + pad
               || maxs[1] < box[1] - pad || mins[1] > box[3] + pad );
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
        s_tessCell  = ClampF( s_tessCell, 4.0f, 4096.0f );
        s_createZ = ClampF( s_createZ, -65536.0f, 65536.0f );
        if ( s_createCells < 1 )  s_createCells = 1;
        if ( s_createCells > 15 ) s_createCells = 15;
        s_wireReach = ClampF( s_wireReach, 1.0f, 4.0f );
    }

    // Every persisted setting: profile key, storage kind, variable, default.
    enum kterPrefKind_t { KP_INT, KP_BOOL, KP_FLOAT };
    struct kterPref_t
    {
        const char     *key;
        kterPrefKind_t  kind;
        void           *value;
        float           def;
    };
    const kterPref_t KTER_PREFS[] =
    {
        { "Tool",             KP_INT,   &s_tool,             (float)KTER_RAISE },
        { "Shape",            KP_INT,   &s_shape,            (float)KTER_CIRCLE },
        { "Falloff",          KP_INT,   &s_falloff,          (float)KTER_FO_SMOOTH },
        { "Outer",            KP_FLOAT, &s_outer,            256.0f },
        { "Inner",            KP_FLOAT, &s_inner,            64.0f },
        { "Strength",         KP_FLOAT, &s_strength,         1.0f },
        { "SquareRot",        KP_FLOAT, &s_squareRot,        0.0f },
        { "RaiseSpeed",       KP_FLOAT, &s_amount,           64.0f },
        { "TargetZ",          KP_FLOAT, &s_targetZ,          0.0f },
        { "NoiseScale",       KP_FLOAT, &s_noiseScale,       32.0f },
        { "NoiseFreq",        KP_FLOAT, &s_noiseFreq,        0.004f },
        { "BlendWeight",      KP_FLOAT, &s_blendWeight,      1.0f },
        { "PreviewBlend",     KP_BOOL,  &s_previewBlend,     1.0f },
        { "BlendRings",       KP_INT,   &s_blendRings,       2.0f },
        { "FlatTol",          KP_FLOAT, &s_flatTol,          1.0f },
        { "FlatThick",        KP_FLOAT, &s_flatThick,        16.0f },
        { "PaintBrushes",     KP_BOOL,  &s_paintBrushes,     0.0f },
        { "AffectUnselected", KP_BOOL,  &s_affectUnselected, 0.0f },
        { "CarryObjects",     KP_BOOL,  &s_carryObjects,     0.0f },
        { "SetHeightFeather", KP_BOOL,  &s_setHeightFeather, 0.0f },
        { "ContainRing",      KP_BOOL,  &s_setHeightContain, 1.0f },
        { "LayerDepthEqual",  KP_BOOL,  &s_layerDepthEqual,  1.0f },
        { "ChunkSize2",       KP_FLOAT, &s_chunkSize,        2048.0f },
        { "TessCellSize",     KP_FLOAT, &s_tessCell,         64.0f },
        { "Expand",           KP_BOOL,  &s_expand,           0.0f },
        { "CreateZ",          KP_FLOAT, &s_createZ,          0.0f },
        { "CreateCells",      KP_INT,   &s_createCells,      8.0f },
        { "CreateOnSurfaces", KP_BOOL,  &s_createOnSurfaces, 1.0f },
        { "WireReach",        KP_FLOAT, &s_wireReach,        1.25f },
        { "Heatmap",          KP_BOOL,  &s_heatmap,          1.0f },
        { "HeatmapAlways",    KP_BOOL,  &s_heatAlways,       0.0f },
        { "PaintBase",        KP_BOOL,  &s_paintBase,        0.0f },
        { "WeightView",       KP_BOOL,  &s_weightView,       1.0f },
    };

    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded = true;
        const bool depthWas = s_layerDepthEqual;
        for ( const kterPref_t &p : KTER_PREFS )
        {
            if ( p.kind == KP_FLOAT )
            {
                *(float *)p.value = ReadFloat( p.key, p.def );
                continue;
            }
            const int v = Radiant_ProfileGetInt( KTER_PROFILE, p.key, (int)p.def );
            if ( p.kind == KP_INT )
                *(int *)p.value = v;
            else
                *(bool *)p.value = v != 0;
        }
        const std::string pm = Radiant_ProfileGetString( KTER_PROFILE, "PaintMaterial", "" );
        strncpy( s_paintMaterial, pm.c_str(), sizeof( s_paintMaterial ) - 1 );
        s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
        if ( s_tool == KTER_SMOOTH )            // the retired Smooth tool: Shift+LMB smooths now
            s_tool = KTER_RAISE;
        Sanitize();
        if ( depthWas != s_layerDepthEqual )    // a draw can load a twin before the first Load()
            RestripTwins();
    }

    void Save()
    {
        Sanitize();
        for ( const kterPref_t &p : KTER_PREFS )
        {
            if ( p.kind == KP_FLOAT )
                WriteFloat( p.key, *(const float *)p.value );
            else
                Radiant_ProfileSetInt( KTER_PROFILE, p.key,
                                       p.kind == KP_INT ? *(const int *)p.value : ( *(const bool *)p.value ? 1 : 0 ) );
        }
        Radiant_ProfileSetString( KTER_PROFILE, "PaintMaterial", s_paintMaterial );
    }

    // ── patches / picking ────────────────────────────────────────────────────
    bool NodeIsPatch( selbrush_t *b )
    {
        return b && b->patch && b->def && b->patch->def;
    }

    // Any visible, unfiltered patch of either kind (only "convert to terrain mesh" wants beziers).
    bool PatchEligibleAnyType( selbrush_t *b )
    {
        return NodeIsPatch( b ) && !FilterBrush( b, 0 ) && ( b->brushFlags & 0x20 ) == 0;
    }

    // Everything else touches TERRAIN MESHES only: a bezier's control points are not its
    // surface (strokes once flattened curves standing on the ground).
    bool PatchEligible( selbrush_t *b )
    {
        return PatchEligibleAnyType( b ) && ( b->patch->def->type & PATCH_TERRAIN ) != 0;
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
        ForEachNode( alsoActive, [&]( selbrush_t *b )
        {
            if ( b == skip || !PatchEligible( b ) || !RayHitsBounds( org, dir, b->def->mins, b->def->maxs, best ) )
                return;
            float dist;
            byte  c[4];
            if ( PMESH_51( org, dir, b->patch, &dist, nullptr, nullptr, c, nullptr ) && dist < best )
            {
                best = dist;
                bestNode = b;
                memcpy( cell, c, 4 );
            }
        } );
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

    bool GapCursor( const ray_t &ray, float outPoint[3] );

    // Cursor resolution.  Patches first (every mode), then a GAP the terrain closes in
    // around (every mode: the ring stays at the mouse over holes and models standing in
    // them).  With terrain creation allowed the cursor then lands on any world surface
    // (brushes, model boxes - KiwiDrop_Trace) and finally on the horizontal base plane, so
    // the brush works over an empty zone.
    bool ResolveCursor( const ray_t &ray, float outPoint[3] )
    {
        if ( PickPatches( ray.origin, ray.dir, true, outPoint, nullptr, &s_cursorNode ) )
        {
            s_cursorKind = KCUR_PATCH;
            s_cursorDef  = ( s_cursorNode && s_cursorNode->patch ) ? s_cursorNode->patch->def : nullptr;
            return true;
        }
        s_cursorNode = nullptr;
        s_cursorDef  = nullptr;
        s_cursorKind = KCUR_NONE;
        if ( GapCursor( ray, outPoint ) )
        {
            s_cursorKind = KCUR_GAP;
            return true;
        }
        // Off the patches: the texture painter lands on brush faces when it paints them too;
        // terrain creation lands on surfaces, then on the base plane.
        const bool brushPaint = s_tool == KTER_TEXTURE && s_paintBrushes && !s_paintBase && s_paintMaterial[0];
        if ( !CreationAllowed() && !brushPaint )
            return false;
        kiwiDropHit_t hit;
        if ( ( brushPaint || s_createOnSurfaces ) && KiwiDrop_Trace( ray, false, &hit ) )
        {
            memcpy( outPoint, hit.point, sizeof( hit.point ) );
            s_cursorKind = KCUR_SURFACE;
            return true;
        }
        if ( brushPaint || fabsf( ray.dir[2] ) <= 1e-6f )
            return false;
        const float t = ( s_createZ - ray.origin[2] ) / ray.dir[2];
        if ( t <= 0.0f || t >= 131072.0f )
            return false;
        for ( int i = 0; i < 3; ++i )
            outPoint[i] = ray.origin[i] + ray.dir[i] * t;
        outPoint[2]  = s_createZ;
        s_cursorKind = KCUR_PLANE;
        return true;
    }

    bool PickCursor( int imgX, int imgY, float outPoint[3] )
    {
        ray_t ray;
        return Pick_RayFromImagePos( imgX, imgY, &ray ) && ResolveCursor( ray, outPoint );
    }

    // Is the control grid a regular axis-aligned sheet (x along i, y along j)?  Then a
    // point's cell is found by arithmetic, and the grid can be rebuilt from its rectangle.
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
        // EVERY point on the lattice, not just the corners: a curved patch with square
        // corners rebuilt from its bounding box lays chunks on its neighbours.
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

    // Corner weights (v00, v10, v01, v11) of the TRIANGLE that draws point (fu, fv) of cell
    // (i, j): the renderer splits a terrain quad along v00-v11 when turned_edge & 1, else
    // along v01-v10.  (A bilinear blend is not the drawn surface.)
    void CellTriWeights( const patchMesh_t *def, int i, int j, float fu, float fv, float w[4] )
    {
        if ( ( def->ctrl[i][j].turned_edge & 1 ) != 0 )
        {
            if ( fu >= fv ) { w[0] = 1.0f - fu; w[1] = fu - fv; w[2] = 0.0f;    w[3] = fv; }
            else            { w[0] = 1.0f - fv; w[1] = 0.0f;    w[2] = fv - fu; w[3] = fu; }
        }
        else if ( fu + fv <= 1.0f ) { w[0] = 1.0f - fu - fv; w[1] = fu;        w[2] = fv;        w[3] = 0.0f; }
        else                        { w[0] = 0.0f;           w[1] = 1.0f - fv; w[2] = 1.0f - fu; w[3] = fu + fv - 1.0f; }
    }

    // Position, texture coordinates and colour at (fu, fv) inside cell (i, j).  Everything
    // is linear over a triangle, so a point read this way carries the OLD mapping exactly.
    void SampleCellTri( const patchMesh_t *src, int i, int j, float fu, float fv, drawVert_t *out )
    {
        float w[4];
        CellTriWeights( src, i, j, fu, fv, w );
        const drawVert_t *v[4] = { &src->ctrl[i][j], &src->ctrl[i + 1][j], &src->ctrl[i][j + 1], &src->ctrl[i + 1][j + 1] };
        for ( int a = 0; a < 3; ++a )
            out->xyz[a] = v[0]->xyz[a] * w[0] + v[1]->xyz[a] * w[1] + v[2]->xyz[a] * w[2] + v[3]->xyz[a] * w[3];
        float *to = (float *)&out->texCoord, *so = (float *)&out->savedTexCoord;
        for ( int a = 0; a < 6; ++a )
        {
            to[a] = so[a] = 0.0f;
            for ( int c = 0; c < 4; ++c )
            {
                to[a] += ( (const float *)&v[c]->texCoord )[a] * w[c];
                so[a] += ( (const float *)&v[c]->savedTexCoord )[a] * w[c];
            }
        }
        byte *co = (byte *)&out->vert_color;
        for ( int k = 0; k < 4; ++k )
        {
            float m = 0.0f;
            for ( int c = 0; c < 4; ++c )
                m += (float)( (const byte *)&v[c]->vert_color )[k] * w[c];
            co[k] = (byte)(int)( ClampF( m, 0.0f, 255.0f ) + 0.5f );
        }
    }

    // Height and colour of a SHEET at (x, y), clamped onto the sheet.  O(1).
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
        if ( i0 > src->width - 2 )  i0 = src->width - 2;
        if ( j0 > src->height - 2 ) j0 = src->height - 2;
        drawVert_t s;
        SampleCellTri( src, i0, j0, u - (float)i0, v - (float)j0, &s );
        *outZ = s.xyz[2];
        memcpy( outColor, &s.vert_color, 4 );
    }

    // Height at (x, y) of the triangle a-b-c when the point lies on it in XY.
    bool TriHeightXY( const float *a, const float *b, const float *c, float x, float y, float *outZ )
    {
        const float d = ( b[1] - c[1] ) * ( a[0] - c[0] ) + ( c[0] - b[0] ) * ( a[1] - c[1] );
        if ( fabsf( d ) < 1e-6f )
            return false;
        const float l0 = ( ( b[1] - c[1] ) * ( x - c[0] ) + ( c[0] - b[0] ) * ( y - c[1] ) ) / d;
        const float l1 = ( ( c[1] - a[1] ) * ( x - c[0] ) + ( a[0] - c[0] ) * ( y - c[1] ) ) / d;
        const float l2 = 1.0f - l0 - l1;
        if ( l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f )
            return false;
        *outZ = a[2] * l0 + b[2] * l1 + c[2] * l2;
        return true;
    }

    // Height of ANY terrain grid at (x, y) - the triangle over the point - or false when the
    // point is off the grid.  Sheets by arithmetic, curved / rotated grids by a cell scan.
    bool GridHeightAt( const patchMesh_t *def, float x, float y, float *outZ )
    {
        if ( def->width < 2 || def->height < 2 )
            return false;
        if ( GridIsSheet( def ) )
        {
            const float *a = def->ctrl[0][0].xyz, *b = def->ctrl[def->width - 1][def->height - 1].xyz;
            if ( x < ( a[0] < b[0] ? a[0] : b[0] ) - 0.5f || x > ( a[0] > b[0] ? a[0] : b[0] ) + 0.5f
              || y < ( a[1] < b[1] ? a[1] : b[1] ) - 0.5f || y > ( a[1] > b[1] ? a[1] : b[1] ) + 0.5f )
                return false;
            byte c[4];
            SampleGrid( def, x, y, outZ, c );
            return true;
        }
        for ( int i = 0; i + 1 < def->width; ++i )
            for ( int j = 0; j + 1 < def->height; ++j )
            {
                const float *v00 = def->ctrl[i][j].xyz,     *v10 = def->ctrl[i + 1][j].xyz;
                const float *v01 = def->ctrl[i][j + 1].xyz, *v11 = def->ctrl[i + 1][j + 1].xyz;
                const bool turned = ( def->ctrl[i][j].turned_edge & 1 ) != 0;
                if ( turned ? ( TriHeightXY( v00, v10, v11, x, y, outZ ) || TriHeightXY( v00, v11, v01, x, y, outZ ) )
                            : ( TriHeightXY( v00, v10, v01, x, y, outZ ) || TriHeightXY( v10, v11, v01, x, y, outZ ) ) )
                    return true;
            }
        return false;
    }

    // The terrain height at (x, y) closest to zGuess over every eligible patch, so the ring
    // lies on the ground across patch borders too.
    bool TerrainHeightNear( float x, float y, float zGuess, float *outZ )
    {
        bool  have = false;
        float best = 0.0f;
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( !b->def || x < b->def->mins[0] - 0.5f || x > b->def->maxs[0] + 0.5f
              || y < b->def->mins[1] - 0.5f || y > b->def->maxs[1] + 0.5f || !PatchEligible( b ) )
                return;
            float z;
            if ( GridHeightAt( b->patch->def, x, y, &z ) && ( !have || fabsf( z - zGuess ) < fabsf( best - zGuess ) ) )
            {
                best = z;
                have = true;
            }
        } );
        if ( have )
            *outZ = best;
        return have;
    }

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

    // A ring point on the ground: the cursor patch first (RebuildRing validates the node
    // once, LiveCursorNode, before its 64 calls here), then any terrain under the point.
    bool DropToSurface( float x, float y, float zGuess, float *outZ )
    {
        if ( s_cursorNode && s_cursorNode->patch && GridHeightAt( s_cursorNode->patch->def, x, y, outZ ) )
            return true;
        return TerrainHeightNear( x, y, zGuess, outZ );
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

    // Can the brush at `center` reach anything inside this node's XY bounds?
    bool UnderBrush( const brush_t *def, const float *center )
    {
        const float r = s_shape == KTER_SQUARE ? s_outer * 1.42f : s_outer;
        const float box[4] = { center[0] - r, center[1] - r, center[0] + r, center[1] + r };
        return BoundsMeet( def->mins, def->maxs, box, 0.0f );
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

    // "kiwi_blend_<name>" is the editor's own preview copy of <name>.  Being a real file it
    // shows in the texture browser and got painted as a LAYER (it compiles as a decal, and
    // the same grass then lives under two names).  Paint names are unwrapped on the way in,
    // and a stroke heals a patch that already carries a wrapped layer.
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

    // "Paint with" (unwrapped from an editor preview twin name).
    void SetPaintMaterial( const char *name )
    {
        strncpy( s_paintMaterial, UnwrapTwinName( name ), sizeof( s_paintMaterial ) - 1 );
        s_paintMaterial[sizeof( s_paintMaterial ) - 1] = '\0';
        s_paintBase = false;
    }

    // Texture paint with a material on the brush (or "erase to base") needs no
    // selection: every eligible patch is a target.
    bool PaintAnywhere()
    {
        return s_tool == KTER_TEXTURE && ( s_paintBase || s_paintMaterial[0] != 0 );
    }

    // The layer-run materials are cloned from DECAL templates (l_sm_b0c0*), whose polygon
    // offset hid models lying on the terrain.  A layer run is the base run's grid vertex for
    // vertex, so: no offset, and depth EQUAL - the layer lands only where its own base is the
    // visible surface (LESSEQUAL let the paint, drawn last, win every near-tie with rail ties
    // flush with the ground).  Wireframe entries keep LESSEQUAL.  `s_layerDepthEqual` is the
    // escape hatch should a techset not reproduce the base's depth bit for bit.
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

    // Loads editor-only material `name` (never referenced by a .map); when it is not on disk
    // yet it is first written from `f` over the shipped template of `techSet`.  Null on
    // failure, with the reason in err[256].
    Material *EditorMaterial( const char *name, const char *techSet, const kiwiMatFields_t &f, char *err )
    {
        if ( !KiwiMat_ExistsOnDisk( name ) )
        {
            int tpl = -1;
            for ( int i = 0; i < KiwiMat_TemplateCount() && tpl < 0; ++i )
            {
                const kiwiMatTemplateInfo_t *info = KiwiMat_TemplateInfo( i );
                if ( info && info->techSet && !_stricmp( info->techSet, techSet ) )
                    tpl = i;
            }
            if ( tpl < 0 )
            {
                _snprintf( err, 256, "no shipped template for techset '%s'", techSet );
                return nullptr;
            }
            if ( !KiwiMat_Write( tpl, &f, err, 256 ) )
                return nullptr;
        }
        char asset[80];
        _snprintf( asset, sizeof( asset ), "wc/%s", name );
        asset[sizeof( asset ) - 1] = '\0';
        Material *m = Material_Load( asset, 0 );
        if ( m && Material_IsDefault( m ) )
            m = nullptr;
        if ( !m )
            _snprintf( err, 256, "'%s' did not load (or loaded as the default material)", name );
        return m;
    }

    // Preview twin "kiwi_blend_<name>": an alpha-blend (l_sm_b0c0[n0][s0]) copy over the SAME
    // images.  Null = the layer draws opaque; the panel row says why.
    Material *BlendTwin( const char *name )
    {
        if ( !name || !name[0] )
            return nullptr;
        std::map<std::string, Material *>::iterator it = s_blendTwins.find( name );
        if ( it != s_blendTwins.end() )
            return it->second;

        char twinName[64], family[64] = "l_sm_b0c0", err[256] = { 0 };
        _snprintf( twinName, sizeof( twinName ), "kiwi_blend_%s", name );
        twinName[sizeof( twinName ) - 1] = '\0';
        kiwiMatFields_t f;
        memset( &f, 0, sizeof( f ) );
        bool ready = KiwiMat_ExistsOnDisk( twinName );
        kiwiMatSource_t src;
        if ( !ready && KiwiMat_ReadSource( name, &src, err, sizeof( err ) ) )
        {
            ready = true;
            const bool haveN = src.normalMapImage[0] && strcmp( src.normalMapImage, src.colorMapImage ) != 0;
            const bool haveS = src.specularMapImage[0] && strcmp( src.specularMapImage, src.colorMapImage ) != 0;
            _snprintf( family, sizeof( family ), "l_sm_b0c0%s%s", haveN ? "n0" : "", haveS ? "s0" : "" );
            _snprintf( f.name, sizeof( f.name ), "%s", twinName );
            _snprintf( f.imageName, sizeof( f.imageName ), "%s", src.colorMapImage );
            if ( haveN ) _snprintf( f.normalImageName, sizeof( f.normalImageName ), "%s", src.normalMapImage );
            if ( haveS ) _snprintf( f.specularImageName, sizeof( f.specularImageName ), "%s", src.specularMapImage );
            f.usage  = src.usage ? src.usage : 1;
            f.locale = src.locale ? src.locale : 1u;
            f.autoTexScaleWidth  = src.autoTexScaleWidth  ? src.autoTexScaleWidth  : 512;
            f.autoTexScaleHeight = src.autoTexScaleHeight ? src.autoTexScaleHeight : 512;
            f.surfaceType = src.surfaceFlags & 0x1F00000;
        }
        Material *twin = ready ? EditorMaterial( twinName, family, f, err ) : nullptr;
        if ( twin )
            StripDecalOffset( twin );
        else
        {
            Sys_Printf( "Terrain Sculpt: no blend preview for '%s' (%s); the layer draws opaque in the camera.\n",
                        name, err[0] ? err : "unknown" );
            s_twinErr[name] = err[0] ? err : "unknown";
        }
        s_blendTwins[name] = twin;
        return twin;
    }

    // An editor-only material over the builtin $white colormap: the vertex colour alone
    // paints the surface.  `fallback` says what happens without it.
    Material *WhiteMaterial( const char *name, const char *techSet, const char *fallback )
    {
        kiwiMatFields_t f;
        memset( &f, 0, sizeof( f ) );
        _snprintf( f.name, sizeof( f.name ), "%s", name );
        _snprintf( f.imageName, sizeof( f.imageName ), "%s", "$white" );
        f.usage  = 1;
        f.locale = 1u;
        f.autoTexScaleWidth  = 512;
        f.autoTexScaleHeight = 512;
        f.surfaceType = -1;                           // keep the template's surface flags
        char err[256] = { 0 };
        Material *m = EditorMaterial( name, techSet, f, err );
        if ( !m )
            Sys_Printf( "Terrain Sculpt: no %s material (%s); %s\n", name, err[0] ? err : "unknown", fallback );
        return m;
    }

    // The height-gradient material: the plain lit world template (l_sm_r0c0), so it carries
    // every camera technique (the editor's $opaque has no lit technique and white_tools does
    // not write depth - neither can stand in for a terrain surface).  Null = the patch's own
    // material, tinted.
    Material *HeatMaterial()
    {
        static Material *s_heat  = nullptr;
        static bool      s_tried = false;
        if ( !s_tried )
        {
            s_tried = true;
            s_heat  = WhiteMaterial( "kiwi_heat", "l_sm_r0c0", "the gradient tints the real textures instead." );
        }
        return s_heat;
    }

    // The weight-view material: the alpha-blend world template (l_sm_b0c0), so a layer run
    // shows its slot colour at the painted weight.  Null = the blend twin as usual.
    Material *WeightMaterial()
    {
        static Material *s_mat   = nullptr;
        static bool      s_tried = false;
        if ( !s_tried )
        {
            s_tried = true;
            s_mat   = WhiteMaterial( "kiwi_weight", "l_sm_b0c0", "painted layers show their blended texture instead." );
            StripDecalOffset( s_mat );      // same decal template, same coplanar run
        }
        return s_mat;
    }

    // ── patch edits outside a stroke: one undo record each ───────────────────
    // PatchEditBegin opens the record, PatchEditMark saves a patch BEFORE its first change
    // (Patch_PaintMarkUndo, once per record), PatchEditEnd rebuilds what was marked and closes.
    std::vector<patchMesh_t *> s_editDefs;

    void PatchEditBegin( const char *label )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( label );
        Patch_Paint( &selected_brushes );
        Patch_Paint( &active_brushes );
        s_editDefs.clear();
    }

    // Saves a patch into the open undo record before its first change (once per record,
    // Patch_Paint clears the flag); true when this call saved it.
    bool MarkUndo( patchMesh_t *def )
    {
        if ( def->xx22b )
            return false;
        def->xx22b = 1;
        Patch_PaintMarkUndo( def );
        return true;
    }

    void PatchEditMark( patchMesh_t *def )
    {
        if ( MarkUndo( def ) )
            s_editDefs.push_back( def );
    }

    void PatchEditEnd( bool bounds )
    {
        for ( size_t i = 0; i < s_editDefs.size(); ++i )
        {
            Patch_Rebuild( s_editDefs[i], bounds ? 1 : 0 );
            ++s_editDefs[i]->version;
        }
        s_editDefs.clear();
        Patch_PaintFinish( &selected_brushes );
        Patch_PaintFinish( &active_brushes );
        Undo_End();
        g_nUpdateBits = -1;
    }

    void SetAllWeights( patchMesh_t *def, unsigned int packed )
    {
        for ( int i = 0; i < def->width; ++i )
            for ( int j = 0; j < def->height; ++j )
                *(unsigned int *)&def->ctrl[i][j].vert_color = packed;
    }

    // Layer slot edits apply to every patch the brush would touch (the selection, plus the
    // active list under "Affect unselected"); removing a slot takes its weights along.
    void TargetPatches( std::vector<patchMesh_t *> &out )
    {
        out.clear();
        ForEachNode( s_affectUnselected, [&]( selbrush_t *b )
        {
            if ( PatchEligible( b ) )
                out.push_back( b->patch->def );
        } );
    }

    // First layer on a patch: the colour bytes become weights, so start from zero.
    void AddLayerSlot( patchMesh_t *lead, const char *material )
    {
        const int slot = FirstFreeSlot( lead );
        if ( slot < 0 || !material || !material[0] )
            return;
        std::vector<patchMesh_t *> defs;
        TargetPatches( defs );
        PatchEditBegin( "add terrain layer" );
        for ( size_t d = 0; d < defs.size(); ++d )
        {
            patchMesh_t *def = defs[d];
            if ( SlotUsed( def, slot ) || FindSlotByName( def, material ) >= 0 )
                continue;                                  // the slot means something else here, or it has the layer
            PatchEditMark( def );
            if ( UsedSlotCount( def ) == 0 )
                SetAllWeights( def, 0u );
            strncpy( def->kiwiLayer[slot], material, 63 );
            def->kiwiLayer[slot][63] = '\0';
        }
        PatchEditEnd( false );
        SetPaintMaterial( material );                      // and paint with it
        Save();
    }

    // Removes the slot AND every weight ever painted with it, on every target patch.
    void RemoveLayerSlot( patchMesh_t *lead, int slot )
    {
        if ( !SlotUsed( lead, slot ) )
            return;
        std::vector<patchMesh_t *> defs;
        TargetPatches( defs );
        PatchEditBegin( "remove terrain layer" );
        for ( size_t d = 0; d < defs.size(); ++d )
        {
            patchMesh_t *def = defs[d];
            if ( !SlotUsed( def, slot ) )
                continue;
            PatchEditMark( def );
            def->kiwiLayer[slot][0] = '\0';
            if ( UsedSlotCount( def ) == 0 )
                SetAllWeights( def, 0xFFFFFFFFu );          // back to a plain patch: white
            else
                for ( int i = 0; i < def->width; ++i )
                    for ( int j = 0; j < def->height; ++j )
                        ( (byte *)&def->ctrl[i][j].vert_color )[slot] = 0;
        }
        PatchEditEnd( false );
    }

    void SwapLayerMaterial( patchMesh_t *lead, int slot, const char *material )
    {
        if ( !SlotUsed( lead, slot ) || !material || !material[0] )
            return;
        std::vector<patchMesh_t *> defs;
        TargetPatches( defs );
        PatchEditBegin( "terrain layer material" );
        for ( size_t d = 0; d < defs.size(); ++d )
        {
            if ( !SlotUsed( defs[d], slot ) )
                continue;
            PatchEditMark( defs[d] );
            strncpy( defs[d]->kiwiLayer[slot], material, 63 );
            defs[d]->kiwiLayer[slot][63] = '\0';
        }
        PatchEditEnd( false );
    }

    // Bezier patches re-tessellate on every edit; a terrain mesh IS its control grid.
    bool IsSelectedBezier( selbrush_t *b )
    {
        return PatchEligibleAnyType( b ) && ( b->patch->def->type & PATCH_TERRAIN ) == 0;
    }

    int SelectedBezierCount()
    {
        int n = 0;
        ForEachNode( false, [&]( selbrush_t *b ) { n += IsSelectedBezier( b ) ? 1 : 0; } );
        return n;
    }

    void ConvertSelectedToTerrain()
    {
        int n = 0;
        PatchEditBegin( "convert to terrain mesh" );
        ForEachNode( false, [&]( selbrush_t *b )
        {
            if ( !IsSelectedBezier( b ) )
                return;
            PatchEditMark( b->patch->def );
            b->patch->def->type = (PATCH_TYPES)( b->patch->def->type | PATCH_TERRAIN );
            ++n;
        } );
        PatchEditEnd( true );
        Sys_Printf( "Terrain Sculpt: converted %i bezier patch%s to terrain mesh.\n", n, n == 1 ? "" : "es" );
    }

    // The first selected eligible patch — the one the Texture paint UI edits.
    selbrush_t *FirstSelectedPatch()
    {
        selbrush_t *first = nullptr;
        ForEachNode( false, [&]( selbrush_t *b )
        {
            if ( !first && PatchEligible( b ) )
                first = b;
        } );
        return first;
    }

    // ── stamping ─────────────────────────────────────────────────────────────
    enum kterOp_t { OP_RAISE, OP_SETHEIGHT, OP_SMOOTH, OP_NOISE, OP_TEXTURE, OP_BLEND };

    // Shift+LMB smooths with every tool: the heights, except under Texture paint (the
    // painted weights).  Blend already smooths weights, so Shift changes nothing there -
    // it used to smooth HEIGHTS with no seam pass behind it and open cracks.
    kterOp_t OpForStroke()
    {
        if ( s_modShift && s_tool != KTER_BLEND )
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

    bool IsHeightOp( kterOp_t op )
    {
        if ( op == OP_SMOOTH )
            return s_tool != KTER_TEXTURE;
        return op == OP_RAISE || op == OP_SETHEIGHT || op == OP_NOISE;
    }

    bool HeightStroke()
    {
        return IsHeightOp( OpForStroke() );
    }

    void MarkTouched( patchMesh_t *def )    // a stroke TARGET's first change (counted)
    {
        if ( MarkUndo( def ) )
            ++s_touched;
    }

    // Raise speed: units per second at a 128 outer radius, growing with wider brushes.
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

    // Border points of two patches this close in XY are the same point of the terrain...
    const float KTER_WELD   = 1.0f;
    // ...unless their heights differ by more than this (a wall, a second layer).
    const float KTER_WELD_Z = 32.0f;

    // Changed THIS flush (the weight seam pass works only around these).
    bool IsDirtyDef( patchMesh_t *def )
    {
        for ( size_t i = 0; i < s_dirtyDefs.size(); ++i )
            if ( s_dirtyDefs[i] == def )
                return true;
        return false;
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

    // The stamp centres of the flush being applied: seam work happens under these rings only
    // (it once ran over a stroked patch's whole border and moved its far edge).
    float s_reachCenters[8][3];
    int   s_reachCount = 0;

    bool InStrokeReach( const float *p )
    {
        for ( int i = 0; i < s_reachCount; ++i )
            if ( BrushDistance( s_reachCenters[i], p ) <= s_outer )
                return true;
        return false;
    }

    bool s_coarseSpill = false;      // a contained stamp found no point that fits and moved them anyway

    // ── the terrain graph ────────────────────────────────────────────────────
    // One graph over the terrain around the brush (rebuilt when the stroke leaves the area it
    // covers, or chunks appear):
    //   * a VERTEX is a control point, or all the coincident border points of the patches
    //     meeting at a seam - they move together by construction, nothing welds afterwards;
    //   * a DEPENDENT lies part-way along another patch's border segment (fine edge against
    //     coarse).  Never stamped: it follows that segment's line, so the seam stays shut;
    //   * a vertex MOVES only if it is on a stroke target, no copy is locked (turned_edge & 2),
    //     the brush reaches it and - contained - every triangle its move tilts, in EVERY patch
    //     sharing it and through the dependents it drags, lies inside the ring (VertReach);
    //   * Smooth averages over graph neighbours, across seams alike; paint seams average the
    //     weights of a vertex's copies (StitchWeights).
    // The only writes are to the vertices that changed, on every copy.
    struct kterRef_t
    {
        patchMesh_t *def;
        int          i, j;
        bool         target;
    };

    struct kterVert_t
    {
        float x, y, z;
        bool  stampable;            // on a stroke target, no copy locked, not a dependent
        bool  moved;                // since the last write-back
        int   host0, host1;         // dependent: follows the segment host0-host1 at hostT; -1 = free
        float hostT;
        int   ref0, refN;           // its copies: refs[ref0 ..]
        int   nb0, nbN;             // smoothing neighbours: nbs[nb0 ..]
        int   fit0, fitN;           // vertices that must lie in the ring for it to move: fits[fit0 ..]
    };

    struct kterGraph_t
    {
        bool  valid;
        float box[4];               // minx miny maxx maxy the graph covers
        std::vector<kterVert_t> verts;
        std::vector<kterRef_t>  refs;
        std::vector<int>        nbs, fits, deps;
    };
    kterGraph_t s_graph;

    void GraphInvalidate()
    {
        s_graph.valid = false;
    }

    const float KTER_BUCKET = 64.0f;            // border-point hash cell

    long long BucketKey( int bx, int by )
    {
        return ( (long long)bx << 32 ) ^ (long long)(unsigned int)by;
    }

    // Host chains: a dependent's host can be a dependent in turn where several densities meet
    // along one edge.  Edge densities that do not NEST (48- beside 64-unit cells) would make
    // the chain a cycle - 48 on 0..64, 64 on 48..96 - so such a link is refused; that seam
    // then cracks when it moves, as it did before (straight edges cannot follow both).
    const int KTER_HOST_DEPTH = 8;

    bool HostChainReaches( int from, int target )
    {
        int stack[2 * KTER_HOST_DEPTH + 2], sp = 0, steps = 0;
        stack[sp++] = from;
        while ( sp > 0 && ++steps < 256 )
        {
            const int x = stack[--sp];
            if ( x == target )
                return true;
            const kterVert_t &X = s_graph.verts[x];
            if ( X.host0 >= 0 && sp + 2 <= (int)( sizeof( stack ) / sizeof( stack[0] ) ) )
            {
                stack[sp++] = X.host0;
                stack[sp++] = X.host1;
            }
        }
        return false;
    }

    // The free vertices at the root of a dependent's host chain.
    void DepRoots( int v, int depth, std::vector<int> &out )
    {
        const kterVert_t &d = s_graph.verts[v];
        if ( d.host0 < 0 )
        {
            out.push_back( v );
            return;
        }
        if ( depth > KTER_HOST_DEPTH )
            return;
        DepRoots( d.host0, depth + 1, out );
        DepRoots( d.host1, depth + 1, out );
    }

    void GraphBuild( const float box[4] )
    {
        kterGraph_t &g = s_graph;
        g.verts.clear();
        g.refs.clear();
        g.nbs.clear();
        g.fits.clear();
        g.deps.clear();
        memcpy( g.box, box, sizeof( g.box ) );
        g.valid = true;

        // 1. the patches: every eligible grid meeting the box, then every grid meeting one of
        //    THOSE - a fine patch outside the box can have seam points riding on the edge of a
        //    coarse patch inside it.
        std::set<const patchMesh_t *> targets;
        for ( size_t t = 0; t < s_targets.size(); ++t )
            targets.insert( s_targets[t]->patch->def );
        std::vector<selbrush_t *> all;
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( PatchEligible( b ) && b->patch->def->width >= 2 && b->patch->def->height >= 2 )
                all.push_back( b );
        } );
        std::vector<char>   take( all.size(), 0 );
        std::vector<size_t> inBox;
        for ( size_t n = 0; n < all.size(); ++n )
            if ( BoundsMeet( all[n]->def->mins, all[n]->def->maxs, box, KTER_WELD ) )
            {
                take[n] = 1;
                inBox.push_back( n );
            }
        for ( size_t n = 0; n < all.size(); ++n )
            for ( size_t f = 0; f < inBox.size() && !take[n]; ++f )
            {
                const float *mins = all[inBox[f]]->def->mins, *maxs = all[inBox[f]]->def->maxs;
                const float fb[4] = { mins[0], mins[1], maxs[0], maxs[1] };
                take[n] = BoundsMeet( all[n]->def->mins, all[n]->def->maxs, fb, KTER_WELD ) ? 1 : 0;
            }

        // 2. vertices: an interior point is its own; a border point joins a coincident border
        //    point of ANOTHER patch (the nearest in height, within KTER_WELD_Z)
        struct gPatch_t { patchMesh_t *def; bool target; int base; };
        std::vector<gPatch_t> P;
        int points = 0;
        for ( size_t n = 0; n < all.size(); ++n )
            if ( take[n] )
            {
                const gPatch_t p = { all[n]->patch->def, targets.count( all[n]->patch->def ) != 0, points };
                points += p.def->width * p.def->height;
                P.push_back( p );
            }
        std::vector<int>       vid( points, -1 );           // patch point -> vertex
        std::vector<int>       refVert, lastPatch;
        std::vector<float>     zSum;
        std::vector<char>      locked;
        std::vector<kterRef_t> refs;
        std::unordered_map<long long, std::vector<int> > buckets;   // border vertices by XY cell
        for ( size_t pi = 0; pi < P.size(); ++pi )
        {
            patchMesh_t *def = P[pi].def;
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                {
                    const drawVert_t &cp = def->ctrl[i][j];
                    const bool border = i == 0 || j == 0 || i == def->width - 1 || j == def->height - 1;
                    const int  bx = (int)floorf( cp.xyz[0] / KTER_BUCKET ), by = (int)floorf( cp.xyz[1] / KTER_BUCKET );
                    int v = -1;
                    if ( border )
                    {
                        float bestDz = KTER_WELD_Z;
                        for ( int dx = -1; dx <= 1; ++dx )
                            for ( int dy = -1; dy <= 1; ++dy )
                            {
                                const std::unordered_map<long long, std::vector<int> >::const_iterator it =
                                    buckets.find( BucketKey( bx + dx, by + dy ) );
                                if ( it == buckets.end() )
                                    continue;
                                for ( size_t c = 0; c < it->second.size(); ++c )
                                {
                                    const int q = it->second[c];
                                    if ( lastPatch[q] == (int)pi )
                                        continue;           // one copy per patch
                                    if ( fabsf( g.verts[q].x - cp.xyz[0] ) > KTER_WELD || fabsf( g.verts[q].y - cp.xyz[1] ) > KTER_WELD )
                                        continue;
                                    const float dz = fabsf( zSum[q] / (float)g.verts[q].refN - cp.xyz[2] );
                                    if ( dz <= bestDz )
                                    {
                                        bestDz = dz;
                                        v = q;
                                    }
                                }
                            }
                    }
                    if ( v < 0 )
                    {
                        v = (int)g.verts.size();
                        kterVert_t nv;
                        memset( &nv, 0, sizeof( nv ) );
                        nv.x = cp.xyz[0];
                        nv.y = cp.xyz[1];
                        nv.host0 = nv.host1 = -1;
                        g.verts.push_back( nv );
                        zSum.push_back( 0.0f );
                        lastPatch.push_back( -1 );
                        locked.push_back( 0 );
                        if ( border )
                            buckets[BucketKey( bx, by )].push_back( v );
                    }
                    kterVert_t &vert = g.verts[v];
                    zSum[v] += cp.xyz[2];
                    ++vert.refN;                                // counted here, laid out below
                    lastPatch[v] = (int)pi;
                    if ( ( cp.turned_edge & 2 ) != 0 )
                        locked[v] = 1;
                    if ( P[pi].target )
                        vert.stampable = true;
                    vid[P[pi].base + i * def->height + j] = v;
                    const kterRef_t r = { def, i, j, P[pi].target };
                    refs.push_back( r );
                    refVert.push_back( v );
                }
        }
        const int nv = (int)g.verts.size();
        {
            int at = 0;
            for ( int v = 0; v < nv; ++v )
            {
                kterVert_t &vert = g.verts[v];
                vert.z = zSum[v] / (float)vert.refN;
                vert.stampable = vert.stampable && !locked[v];
                vert.ref0 = at;
                at += vert.refN;
                vert.refN = 0;
            }
            g.refs.resize( refs.size() );
            for ( size_t r = 0; r < refs.size(); ++r )
            {
                kterVert_t &vert = g.verts[refVert[r]];
                g.refs[vert.ref0 + vert.refN++] = refs[r];
            }
        }
        const auto vidAt = [&]( const gPatch_t &p, int i, int j ) { return vid[p.base + i * p.def->height + j]; };

        // 3. dependents: a border vertex part-way along another patch's border segment
        for ( size_t pi = 0; pi < P.size(); ++pi )
        {
            const patchMesh_t *def = P[pi].def;
            int ii[64], jj[64];
            const int n = BorderRing( def, ii, jj );
            for ( int k = 0; k < n; ++k )
            {
                const int a = vidAt( P[pi], ii[k], jj[k] ), b = vidAt( P[pi], ii[( k + 1 ) % n], jj[( k + 1 ) % n] );
                if ( a == b )
                    continue;
                const float ax = g.verts[a].x, ay = g.verts[a].y, az = g.verts[a].z;
                const float dx = g.verts[b].x - ax, dy = g.verts[b].y - ay, dz = g.verts[b].z - az;
                const float len2 = dx * dx + dy * dy;
                if ( len2 <= 4.0f * KTER_WELD * KTER_WELD )
                    continue;
                const float len = sqrtf( len2 );
                const int bx0 = (int)floorf( ( ( dx < 0.0f ? ax + dx : ax ) - KTER_WELD ) / KTER_BUCKET );
                const int bx1 = (int)floorf( ( ( dx < 0.0f ? ax : ax + dx ) + KTER_WELD ) / KTER_BUCKET );
                const int by0 = (int)floorf( ( ( dy < 0.0f ? ay + dy : ay ) - KTER_WELD ) / KTER_BUCKET );
                const int by1 = (int)floorf( ( ( dy < 0.0f ? ay : ay + dy ) + KTER_WELD ) / KTER_BUCKET );
                for ( int bx = bx0; bx <= bx1; ++bx )
                    for ( int by = by0; by <= by1; ++by )
                    {
                        const std::unordered_map<long long, std::vector<int> >::const_iterator it =
                            buckets.find( BucketKey( bx, by ) );
                        if ( it == buckets.end() )
                            continue;
                        for ( size_t c = 0; c < it->second.size(); ++c )
                        {
                            const int q = it->second[c];
                            kterVert_t &V = g.verts[q];
                            if ( q == a || q == b || V.host0 >= 0 )
                                continue;
                            const float t = ( ( V.x - ax ) * dx + ( V.y - ay ) * dy ) / len2;
                            if ( t * len <= KTER_WELD || ( 1.0f - t ) * len <= KTER_WELD )
                                continue;                   // off the segment, or at an end
                            const float ex = V.x - ( ax + dx * t ), ey = V.y - ( ay + dy * t );
                            if ( ex * ex + ey * ey > KTER_WELD * KTER_WELD || fabsf( V.z - ( az + dz * t ) ) > KTER_WELD_Z )
                                continue;
                            bool own = false;               // a point of this very patch: a degenerate border
                            for ( int r = 0; r < V.refN && !own; ++r )
                                own = g.refs[V.ref0 + r].def == def;
                            if ( own || HostChainReaches( a, q ) || HostChainReaches( b, q ) )
                                continue;                   // (a cycle: two edge densities that do not nest)
                            V.host0     = a;
                            V.host1     = b;
                            V.hostT     = t;
                            V.stampable = false;
                            g.deps.push_back( q );
                        }
                    }
            }
        }

        // 4. smoothing neighbours (every copy's 3 x 3 grid neighbourhood) and triangle
        //    neighbours (the other corners of every triangle through the vertex)
        std::vector<std::pair<int, int> > nb, tri;
        for ( size_t pi = 0; pi < P.size(); ++pi )
        {
            const gPatch_t    &p   = P[pi];
            const patchMesh_t *def = p.def;
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                {
                    const int v = vidAt( p, i, j );
                    for ( int di = -1; di <= 1; ++di )
                        for ( int dj = -1; dj <= 1; ++dj )
                        {
                            const int ni = i + di, nj = j + dj;
                            if ( ( di || dj ) && ni >= 0 && nj >= 0 && ni < def->width && nj < def->height && vidAt( p, ni, nj ) != v )
                                nb.push_back( std::make_pair( v, vidAt( p, ni, nj ) ) );
                        }
                    if ( i + 1 >= def->width || j + 1 >= def->height )
                        continue;
                    // the cell's two triangles, split the way the renderer splits them
                    const int  c00 = v, c10 = vidAt( p, i + 1, j ), c01 = vidAt( p, i, j + 1 ), c11 = vidAt( p, i + 1, j + 1 );
                    const bool turned = ( def->ctrl[i][j].turned_edge & 1 ) != 0;
                    const int  t[2][3] = { { c00, c10, turned ? c11 : c01 }, { turned ? c00 : c10, c11, c01 } };
                    for ( int k = 0; k < 2; ++k )
                        for ( int m = 0; m < 3; ++m )
                            for ( int o = 0; o < 3; ++o )
                                if ( t[k][m] != t[k][o] )
                                    tri.push_back( std::make_pair( t[k][m], t[k][o] ) );
                }
        }
        // A dependent moves with its hosts: it and its triangle neighbours must fit too for
        // the free vertices at the root of its host chain to move.
        {
            std::sort( tri.begin(), tri.end() );
            std::vector<std::pair<int, int> > extra;
            std::vector<int> roots;
            for ( size_t d = 0; d < g.deps.size(); ++d )
            {
                const int dv = g.deps[d];
                roots.clear();
                DepRoots( dv, 0, roots );
                for ( size_t r = 0; r < roots.size(); ++r )
                {
                    extra.push_back( std::make_pair( roots[r], dv ) );
                    for ( std::vector<std::pair<int, int> >::const_iterator it =
                              std::lower_bound( tri.begin(), tri.end(), std::make_pair( dv, -1 ) );
                          it != tri.end() && it->first == dv; ++it )
                        if ( it->second != roots[r] )
                            extra.push_back( std::make_pair( roots[r], it->second ) );
                }
            }
            tri.insert( tri.end(), extra.begin(), extra.end() );
        }
        // pairs -> per-vertex lists (sorted by vertex, so each vertex's run is contiguous)
        for ( int list = 0; list < 2; ++list )
        {
            std::vector<std::pair<int, int> > &pairs = list == 0 ? nb : tri;
            std::vector<int>                  &out   = list == 0 ? g.nbs : g.fits;
            std::sort( pairs.begin(), pairs.end() );
            pairs.erase( std::unique( pairs.begin(), pairs.end() ), pairs.end() );
            out.resize( pairs.size() );
            for ( size_t k = 0; k < pairs.size(); ++k )
            {
                kterVert_t &v     = g.verts[pairs[k].first];
                int        &first = list == 0 ? v.nb0 : v.fit0;
                int        &count = list == 0 ? v.nbN : v.fitN;
                if ( count == 0 )
                    first = (int)k;
                ++count;
                out[k] = pairs[k].second;
            }
        }
    }

    // Makes the graph cover the rings of this flush (s_reachCenters), with a radius of room
    // to move before the stroke needs the next rebuild.
    void GraphCover()
    {
        const float r = ( s_shape == KTER_SQUARE ? s_outer * 1.42f : s_outer ) + KTER_WELD;
        float need[4] = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
        for ( int k = 0; k < s_reachCount; ++k )
            for ( int a = 0; a < 2; ++a )
            {
                if ( s_reachCenters[k][a] - r < need[a] )     need[a]     = s_reachCenters[k][a] - r;
                if ( s_reachCenters[k][a] + r > need[a + 2] ) need[a + 2] = s_reachCenters[k][a] + r;
            }
        const kterGraph_t &g = s_graph;
        if ( g.valid && need[0] >= g.box[0] && need[1] >= g.box[1] && need[2] <= g.box[2] && need[3] <= g.box[3] )
            return;
        const float box[4] = { need[0] - s_outer, need[1] - s_outer, need[2] + s_outer, need[3] + s_outer };
        GraphBuild( box );
    }

    // How far from the brush centre the triangles a vertex's move would tilt reach: the
    // farthest of its triangle neighbours, in the brush's own metric.
    float VertReach( const kterVert_t &v, const float *center )
    {
        float reach = 0.0f;
        for ( int k = 0; k < v.fitN; ++k )
        {
            const kterVert_t &u = s_graph.verts[s_graph.fits[v.fit0 + k]];
            const float p[3] = { u.x, u.y, 0.0f };
            const float d = BrushDistance( center, p );
            if ( d > reach )
                reach = d;
        }
        return reach;
    }

    // Dependents follow their host segment whenever an end moved (a chain takes a few passes).
    void GraphResolveDependents()
    {
        kterGraph_t &g = s_graph;
        for ( int pass = 0; pass <= KTER_HOST_DEPTH; ++pass )
        {
            bool any = false;
            for ( size_t d = 0; d < g.deps.size(); ++d )
            {
                kterVert_t &D = g.verts[g.deps[d]];
                const kterVert_t &a = g.verts[D.host0], &b = g.verts[D.host1];
                if ( !a.moved && !b.moved )
                    continue;
                const float z = a.z + ( b.z - a.z ) * D.hostT;
                if ( D.moved && z == D.z )
                    continue;
                D.z     = z;
                D.moved = true;
                any     = true;
            }
            if ( !any )
                break;
        }
    }

    struct kterCand_t
    {
        int   v;
        float w, reach;
    };
    std::vector<kterCand_t> s_cands;
    std::vector<float>      s_smoothSnap;

    // One stamp of a height op over the graph; true when anything moved.  Contained, a vertex
    // moves only when its REACH lies inside the ring.  On a grid too coarse for the ring
    // nothing qualifies, so the limit grows to one radius past the best reach: on an even
    // coarse grid that is every point under the brush (status line says so), beside a FINE
    // patch only the fine side - never a coarse neighbour's corner.
    bool StampHeights( const float *center, kterOp_t op, float sign, float dt )
    {
        kterGraph_t &g = s_graph;
        if ( !g.valid )
            return false;
        const size_t n = g.verts.size();
        const float  r = s_shape == KTER_SQUARE ? s_outer * 1.42f : s_outer;
        s_cands.clear();
        float best = FLT_MAX;
        for ( size_t k = 0; k < n; ++k )
        {
            const kterVert_t &v = g.verts[k];
            if ( !v.stampable || fabsf( v.x - center[0] ) > r || fabsf( v.y - center[1] ) > r )
                continue;
            const float p[3] = { v.x, v.y, 0.0f };
            const float w = Falloff( BrushDistance( center, p ) ) * s_strength;
            if ( w <= 0.0f )
                continue;
            const kterCand_t c = { (int)k, w, s_setHeightContain ? VertReach( v, center ) : 0.0f };
            if ( c.reach < best )
                best = c.reach;
            s_cands.push_back( c );
        }
        if ( s_cands.empty() )
            return false;
        float limit = s_outer;
        if ( s_setHeightContain && best > s_outer )
        {
            limit = best + s_outer;
            s_coarseSpill = true;
        }

        if ( op == OP_SMOOTH )
        {
            s_smoothSnap.resize( n );
            for ( size_t k = 0; k < n; ++k )
                s_smoothSnap[k] = g.verts[k].z;
        }
        const float at = dt * AdditiveRate();
        bool any = false;
        for ( size_t c = 0; c < s_cands.size(); ++c )
        {
            if ( s_cands[c].reach > limit )
                continue;                           // its triangles stick out of the ring
            kterVert_t &v = g.verts[s_cands[c].v];
            const float w = s_cands[c].w;
            float z = v.z;
            switch ( op )
            {
            case OP_RAISE:
                z += sign * w * at;
                break;
            case OP_SETHEIGHT:                      // exactly the target at once; Feather eases instead
                z = s_setHeightFeather ? z + ( s_targetZ - z ) * ( w > 1.0f ? 1.0f : w ) : s_targetZ;
                break;
            case OP_NOISE:
                z += sign * ValueNoise2( v.x * s_noiseFreq, v.y * s_noiseFreq, (int)s_noiseSeed )
                   * w * at * ( s_noiseScale / 64.0f );
                break;
            case OP_SMOOTH:
            {
                float sum = s_smoothSnap[s_cands[c].v];
                for ( int m = 0; m < v.nbN; ++m )
                    sum += s_smoothSnap[g.nbs[v.nb0 + m]];
                z += ( sum / (float)( v.nbN + 1 ) - z ) * LerpStep( w, dt );
                break;
            }
            default:
                break;
            }
            if ( z != v.z )
            {
                v.z     = z;
                v.moved = true;
                any     = true;
            }
        }
        if ( any )
            GraphResolveDependents();
        return any;
    }

    // The new heights of the vertices that moved go to every copy (undo-marked first);
    // nothing else is written.
    void GraphWriteBack()
    {
        kterGraph_t &g = s_graph;
        for ( size_t k = 0; k < g.verts.size(); ++k )
        {
            kterVert_t &v = g.verts[k];
            if ( !v.moved )
                continue;
            v.moved = false;
            for ( int r = 0; r < v.refN; ++r )
            {
                const kterRef_t &ref = g.refs[v.ref0 + r];
                drawVert_t      &cp  = ref.def->ctrl[ref.i][ref.j];
                if ( cp.xyz[2] == v.z )
                    continue;
                if ( ref.target )
                    MarkTouched( ref.def );
                else
                    MarkUndo( ref.def );
                cp.xyz[2] = v.z;
                NoteDirty( ref.def, true );
            }
        }
    }

    // Paint seams: under the brush, the copies of a shared vertex take the MEAN weight of
    // every layer (matched by NAME - slot indices differ between patches) they carry, so a
    // painted transition continues across the chunk edge instead of stepping.
    void StitchWeights()
    {
        const kterGraph_t &g = s_graph;
        if ( !g.valid )
            return;
        for ( size_t k = 0; k < g.verts.size(); ++k )
        {
            const kterVert_t &v = g.verts[k];
            const float p[3] = { v.x, v.y, 0.0f };
            if ( v.refN < 2 || !InStrokeReach( p ) )
                continue;
            bool dirty = false;
            for ( int r = 0; r < v.refN && !dirty; ++r )
                dirty = IsDirtyDef( g.refs[v.ref0 + r].def );
            if ( !dirty )
                continue;
            for ( int r = 0; r < v.refN; ++r )
                for ( int slot = 0; slot < KTER_SLOTS; ++slot )
                {
                    const patchMesh_t *first = g.refs[v.ref0 + r].def;
                    if ( !SlotUsed( first, slot ) )
                        continue;
                    const char *name = first->kiwiLayer[slot];
                    bool seen = false;                  // handled with an earlier copy
                    for ( int e = 0; e < r && !seen; ++e )
                        seen = FindSlotByName( g.refs[v.ref0 + e].def, name ) >= 0;
                    if ( seen )
                        continue;
                    float sum = 0.0f;
                    int   cnt = 0;
                    for ( int e = r; e < v.refN; ++e )
                    {
                        const kterRef_t &ref = g.refs[v.ref0 + e];
                        const int s = FindSlotByName( ref.def, name );
                        if ( s >= 0 )
                        {
                            sum += (float)( (const byte *)&ref.def->ctrl[ref.i][ref.j].vert_color )[s];
                            ++cnt;
                        }
                    }
                    if ( cnt < 2 )
                        continue;
                    const byte mean = (byte)(int)( sum / (float)cnt + 0.5f );
                    for ( int e = r; e < v.refN; ++e )
                    {
                        const kterRef_t &ref = g.refs[v.ref0 + e];
                        const int s = FindSlotByName( ref.def, name );
                        byte *c = (byte *)&ref.def->ctrl[ref.i][ref.j].vert_color;
                        if ( s < 0 || c[s] == mean )
                            continue;
                        MarkUndo( ref.def );
                        c[s] = mean;
                        NoteDirty( ref.def, false );
                    }
                }
        }
    }

    void FlushDirty()
    {
        if ( !s_dirtyDefs.empty() && ( s_tool == KTER_TEXTURE || s_tool == KTER_BLEND ) )
            StitchWeights();
        for ( size_t i = 0; i < s_dirtyDefs.size(); ++i )
            Patch_Rebuild( s_dirtyDefs[i], s_dirtyBounds ? 1 : 0 );
        s_dirtyDefs.clear();
        s_dirtyBounds = false;
    }

    // One PAINT stamp on one patch: Texture paint (OP_TEXTURE), its Shift-smooth
    // (OP_SMOOTH) and Blend.  Weights live per patch (slots differ between patches);
    // StitchWeights carries them across the seams under the brush.
    bool StampWeights( selbrush_t *b, kterOp_t op, const float *center, float sign, float dt )
    {
        patchMesh_t *def = b->patch->def;
        if ( def->width <= 0 || def->height <= 0 || !UnderBrush( b->def, center ) )
            return false;
        // a layer stored under the editor's preview name goes back to the real material first
        for ( int k = 0; k < KTER_SLOTS; ++k )
            if ( SlotUsed( def, k ) && !_strnicmp( def->kiwiLayer[k], "kiwi_blend_", 11 ) )
            {
                MarkTouched( def );
                HealTwinLayers( def );
                NoteDirty( def, false );
                break;
            }
        // Texture paint: which slot on THIS patch the brush material means.
        //   -1        = base / erase every layer (needs at least one layer to erase)
        //   existing  = paint (or erase / smooth) that slot
        //   none yet  = painting IN adds the slot on the first point it reaches
        //               (needAdd); erasing or smoothing a layer the patch lacks is a no-op.
        int  slot    = -1;
        bool needAdd = false;
        if ( op != OP_BLEND )
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
        byte snap[16][16][4];                   // the pre-stamp weights (Smooth, Blend)
        if ( op != OP_TEXTURE )
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                    memcpy( snap[i][j], &def->ctrl[i][j].vert_color, 4 );

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
                // Weights respond three times faster than heights: a paint stroke should
                // reach its ceiling in a fraction of a second.
                const float lt = LerpStep( w * 3.0f, dt );
                MarkTouched( def );
                changed = true;
                byte *col = (byte *)&cp->vert_color;
                if ( op == OP_TEXTURE )
                {
                    if ( needAdd )
                    {
                        // First point reached: the patch gets the brush material as a layer.
                        // With no layers yet the colour bytes were white, not weights.
                        if ( UsedSlotCount( def ) == 0 )
                            SetAllWeights( def, 0u );
                        strncpy( def->kiwiLayer[slot], s_paintMaterial, 63 );
                        def->kiwiLayer[slot][63] = '\0';
                        needAdd = false;
                        ++s_layersAdded;
                    }
                    for ( int k = 0; k < KTER_SLOTS; ++k )     // the slot, or (base) erase them all
                        if ( ( slot < 0 || k == slot ) && SlotUsed( def, k ) )
                            col[k] = LerpByte( col[k], k == slot ? texTarget : 0.0f, lt );
                    continue;
                }
                // Smooth / Blend: toward the mean over the (2R+1)^2 neighbourhood of the
                // pre-stamp weights (Smooth: the brush's layer, or all; Blend: all, wider).
                const int R = op == OP_BLEND ? s_blendRings : 1;
                float sumC[4] = { 0, 0, 0, 0 };
                int   n = 0;
                for ( int ni = i - R; ni <= i + R; ++ni )
                    for ( int nj = j - R; nj <= j + R; ++nj )
                        if ( ni >= 0 && nj >= 0 && ni < def->width && nj < def->height )
                        {
                            for ( int k = 0; k < 4; ++k )
                                sumC[k] += (float)snap[ni][nj][k];
                            ++n;
                        }
                for ( int k = 0; k < KTER_SLOTS; ++k )
                    if ( ( op == OP_BLEND || slot < 0 || k == slot ) && SlotUsed( def, k ) )
                        col[k] = LerpByte( col[k], sumC[k] / (float)n, lt );
            }
        }
        if ( changed )
            NoteDirty( def, false );
        return changed;
    }

    // Texture paint on BRUSHES: every upward face of a visible brush whose centre is inside
    // the ring takes the paint material whole (a face has no weights).  Each brush is saved
    // into the stroke's undo record on first touch.
    int PaintBrushFaces( const float *center )
    {
        if ( !s_paintBrushes || s_paintBase || !s_paintMaterial[0] )
            return 0;
        int painted = 0;
        ForEachNode( true, [&]( selbrush_t *b )
        {
            brush_t *def = b->def;
            if ( !def || def->patch || !def->faces || FilterBrush( b, 0 ) || ( b->brushFlags & 0x20 ) != 0
              || !UnderBrush( def, center ) )
                return;
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
        } );
        return painted;
    }

    // Blend / paint-smooth can only carry a layer into a patch that HAS it: every layer with
    // weight under the ring on one target is added (at weight zero) to the other targets the
    // ring touches, so the seam pass and the blend can spread it across the border.
    void ShareLayersUnderRing( const float *center )
    {
        std::vector<selbrush_t *> under;
        for ( size_t t = 0; t < s_targets.size(); ++t )
            if ( UnderBrush( s_targets[t]->def, center ) )
                under.push_back( s_targets[t] );
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
                    if ( UsedSlotCount( B ) == 0 )
                        SetAllWeights( B, 0u );          // white bytes mean nothing as weights
                    else
                        for ( int i = 0; i < B->width; ++i )
                            for ( int j = 0; j < B->height; ++j )
                                ( (byte *)&B->ctrl[i][j].vert_color )[slot] = 0;
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
        bool any = false;
        if ( IsHeightOp( op ) )
            any = StampHeights( center, op, sign, dt );
        else
        {
            // Blend, and the Texture tool's Shift-smooth (same "nothing to smooth here" wall)
            if ( op != OP_TEXTURE )
                ShareLayersUnderRing( center );
            for ( size_t i = 0; i < s_targets.size(); ++i )
                any |= StampWeights( s_targets[i], op, center, sign, dt );
            if ( op == OP_TEXTURE && sign > 0.0f )
            {
                const int faces = PaintBrushFaces( center );
                if ( faces > 0 )
                {
                    s_facesPainted += faces;
                    any = true;
                }
            }
        }
        if ( any )
        {
            ++s_stamps;
            g_nUpdateBits |= W_CAMERA;
        }
    }

    // ── Carry objects ────────────────────────────────────────────────────────
    // A "rider" is any visible non-terrain brush, patch or entity whose bottom sits within
    // KTER_CARRY_TOL of the terrain under its footprint centre when the stroke begins.  Each
    // flush moves it by (terrain now - terrain at the start) minus what was already applied,
    // but only once a stamp ring has covered it.  Saved into the stroke's undo record.
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

    void MarkRidersReached( const float *center )
    {
        for ( size_t i = 0; i < s_riders.size(); ++i )
            if ( !s_riders[i].reached )
            {
                const float p[3] = { s_riders[i].x, s_riders[i].y, 0.0f };
                s_riders[i].reached = BrushDistance( center, p ) <= s_outer;
            }
    }

    // Terrain height under (x, y) across the stroke's target patches: the highest grid
    // that covers the point.
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
            float z;
            if ( !GridHeightAt( b->patch->def, x, y, &z ) )
                continue;
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
        ForEachNode( true, [&]( selbrush_t *b )
        {
            brush_t *def = b->def;
            if ( !def || FilterBrush( b, 0 ) || ( b->brushFlags & 0x20 ) != 0 )
                return;
            if ( b->patch && PatchEligible( b ) )
                return;                                 // terrain is what moves, never a rider
            const float x = 0.5f * ( def->mins[0] + def->maxs[0] );
            const float y = 0.5f * ( def->mins[1] + def->maxs[1] );
            float z;
            if ( !TerrainZAt( x, y, &z ) || fabsf( def->mins[2] - z ) > KTER_CARRY_TOL )
                return;                                 // off the terrain, floating or buried
            const kterRider_t r = { b, def, x, y, z, 0.0f, false, false };
            s_riders.push_back( r );
        } );
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
                // Brush_Move also moves an entity's ORIGIN, which only an entity clone restores
                // (saving the brush alone left carried models raised after Ctrl+Z).
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

    // The patches a stroke works on: the selection, plus every visible patch under "Affect
    // unselected" or when Texture paint carries its own material.
    bool TargetsAll()
    {
        return s_affectUnselected || PaintAnywhere();
    }

    void BuildTargets()
    {
        s_targets.clear();
        ForEachNode( TargetsAll(), [&]( selbrush_t *b )
        {
            if ( PatchEligible( b ) )
                s_targets.push_back( b );
        } );
        GraphInvalidate();
    }

    bool AnyTargetPatch()
    {
        bool any = false;
        ForEachNode( TargetsAll(), [&]( selbrush_t *b ) { any = any || PatchEligible( b ); } );
        return any;
    }

    // ── chunks (split / expander / trim) ─────────────────────────────────────
    void ClearCursor();

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

    // A new w x h TERRAIN patch (never bezier) modelled on `like`: type, flags, materials
    // and layer slots.  The caller fills the control points.
    patchMesh_t *NewPatchLike( const patchMesh_t *like, int w, int h )
    {
        patchMesh_t *p = MakeNewPatch();
        p->width      = w;
        p->height     = h;
        p->type       = (PATCH_TYPES)( like->type | PATCH_TERRAIN );
        p->contents   = like->contents;
        p->flags      = like->flags;
        p->subDivType = like->subDivType;
        p->texture    = like->texture;
        p->lightmap   = like->lightmap;
        p->smoothing  = like->smoothing;
        memcpy( p->kiwiLayer, like->kiwiLayer, sizeof( p->kiwiLayer ) );
        return p;
    }

    // Puts a built patch into the world: its brush under `owner`, a new ACTIVE node.
    selbrush_t *LinkPatch( patchMesh_t *p, entity_s *owner )
    {
        brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
        selbrush_t *inst = Brush_AddToList( pdef, owner );
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
        return inst;
    }

    // A pointsX x pointsY TERRAIN chunk over the rectangle, modelled on `like`: flat at
    // flatZ, or read off the surface of the SHEET `sampleFrom`.
    selbrush_t *CreateChunkXY( const patchMesh_t *like, entity_s *owner,
                               float minx, float miny, float sizeX, float sizeY,
                               int pointsX, int pointsY,
                               const patchMesh_t *sampleFrom, float flatZ )
    {
        if ( pointsX < 2 )  pointsX = 2;
        if ( pointsX > 16 ) pointsX = 16;
        if ( pointsY < 2 )  pointsY = 2;
        if ( pointsY > 16 ) pointsY = 16;
        patchMesh_t *p = NewPatchLike( like, pointsX, pointsY );
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
        return LinkPatch( p, owner );
    }

    void ForgetDef( patchMesh_t *def )
    {
        if ( s_cursorDef == def )                // by key: the node itself may already be freed
        {
            s_cursorNode = nullptr;
            s_cursorDef  = nullptr;
        }
    }

    // ── one undo record for "replace these patches" (Split, Tessellate, Flatten, Trim,
    // Join, fold duplicates) ─────────────────────────────────────────────────────
    void BeginRecord( const char *label )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( label );
    }

    // Records `victims` for deletion the way Edit>Delete does and leaves them selected for
    // DeleteRecorded.  BEFORE creating anything: Undo_AddEntity_W snapshots a func_group
    // with all its brushes, and a new patch in that snapshot would come back on undo.
    void RecordForDelete( const std::vector<selbrush_t *> &victims )
    {
        Select_Deselect( 1 );
        for ( size_t i = 0; i < victims.size(); ++i )
            Select_Brush( victims[i], 0, 0, 0 );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
    }

    void DeleteRecorded()
    {
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            if ( i->patch )
                ForgetDef( i->patch->def );
        Select_Delete();
        Undo_EndBrushList( &selected_brushes );
        s_targets.clear();
        GraphInvalidate();
    }

    // Patches created inside the open record are stamped with its id (so Ctrl+Z frees them),
    // the record closes, and they end up selected.
    void EndRecordCreated( const std::vector<selbrush_t *> &created )
    {
        for ( size_t i = 0; i < created.size(); ++i )
            Undo_KiwiMarkCreated( created[i]->def );
        Undo_End();
        for ( size_t i = 0; i < created.size(); ++i )
            Select_Brush( created[i], 0, 0, 0 );
        g_nUpdateBits = -1;
    }

    void SplitOversized()
    {
        std::vector<selbrush_t *> originals;
        ForEachNode( false, [&]( selbrush_t *b )
        {
            // sheets only: the split rebuilds from the bounding rectangle
            if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                return;
            const float *mins = b->def->mins, *maxs = b->def->maxs;
            if ( maxs[0] - mins[0] > s_chunkSize + 0.5f || maxs[1] - mins[1] > s_chunkSize + 0.5f )
                originals.push_back( b );
        } );
        if ( originals.empty() )
        {
            Sys_Printf( "Terrain Sculpt: no selected terrain exceeds %.0f units.\n", s_chunkSize );
            return;
        }
        BeginRecord( "split terrain" );
        RecordForDelete( originals );
        std::vector<selbrush_t *> created;
        for ( size_t bi = 0; bi < originals.size(); ++bi )
        {
            selbrush_t        *src = originals[bi];
            const patchMesh_t *def = src->patch->def;
            const float *mins = src->def->mins, *maxs = src->def->maxs;
            const float ex = maxs[0] - mins[0], ey = maxs[1] - mins[1];
            const int   nx = (int)ceilf( ex / s_chunkSize ), ny = (int)ceilf( ey / s_chunkSize );
            const float sx = ex / (float)nx, sy = ey / (float)ny;
            for ( int cy = 0; cy < ny; ++cy )
                for ( int cx = 0; cx < nx; ++cx )
                    created.push_back( CreateChunkXY( def, src->owner, mins[0] + sx * (float)cx, mins[1] + sy * (float)cy,
                                                      sx, sy, PointsFor( sx, CellSizeAxis( def, 0 ) ),
                                                      PointsFor( sy, CellSizeAxis( def, 1 ) ), def, 0.0f ) );
        }
        DeleteRecorded();
        EndRecordCreated( created );
        Sys_Printf( "Terrain Sculpt: split %i patch%s into %i chunk%s (%.0f).\n",
                    (int)originals.size(), originals.size() == 1 ? "" : "es",
                    (int)created.size(), created.size() == 1 ? "" : "s", s_chunkSize );
    }

    // ── Flatten: flat selected terrain -> one brush each ─────────────────────
    // A sheet whose heights all lie within `s_flatTol` becomes one brush `s_flatThick` deep:
    // top = its dominant material, caulk elsewhere.  One undo record; others are counted.
    void FlattenSelectedToBrushes()
    {
        std::vector<selbrush_t *> flat;
        int skipped = 0;
        ForEachNode( false, [&]( selbrush_t *b )
        {
            if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                return;
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
        } );
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
            // dominant: the layer with the highest mean weight if over half, else the base
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

        // Delete the patches inside the record...
        BeginRecord( "flatten terrain to brushes" );
        RecordForDelete( flat );
        DeleteRecorded();

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
        g_nUpdateBits = -1;
        char kept[64] = "";
        if ( skipped )
            _snprintf( kept, sizeof( kept ), " (%i not flat, kept)", skipped );
        kept[sizeof( kept ) - 1] = '\0';
        Sys_Printf( "Terrain Sculpt: %i flat patch%s became %i brush%s%s.\n",
                    (int)flat.size(), flat.size() == 1 ? "" : "es", made, made == 1 ? "" : "es", kept );
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

    // The lattice a stroke lays chunks on.  Beside terrain the chunks CONTINUE its sheet
    // (anchored on its corner, sides a whole number of its cells, its materials and layers),
    // so seams share vertices.  With nothing in reach: world-origin aligned at the chunk
    // size, "Cells per new chunk" dense, the texture browser's current material.
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

    // Off the terrain at (x, y): the height of the nearest point of every terrain border within
    // R, inverse-distance weighted (smooth across a gap), and how many of 8 directions from the
    // point meet a border within R.  False when no terrain is in reach.
    bool GapHeightAround( const float *p, float R, float *outZ, int *outSides )
    {
        static const float DIRS[8][2] = { { 1.0f, 0.0f }, { 0.7071f, 0.7071f }, { 0.0f, 1.0f }, { -0.7071f, 0.7071f },
                                          { -1.0f, 0.0f }, { -0.7071f, -0.7071f }, { 0.0f, -1.0f }, { 0.7071f, -0.7071f } };
        float wSum = 0.0f, zSum = 0.0f;
        unsigned sides = 0;
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( !PatchEligible( b ) || BoundsDistanceXY( b, p ) > R )
                return;
            const patchMesh_t *def = b->patch->def;
            int ii[64], jj[64];
            const int n = BorderRing( def, ii, jj );
            float best = FLT_MAX, bestZ = 0.0f;
            for ( int k = 0; k < n; ++k )
            {
                const float *a = def->ctrl[ii[k]][jj[k]].xyz, *c = def->ctrl[ii[( k + 1 ) % n]][jj[( k + 1 ) % n]].xyz;
                const float dx = c[0] - a[0], dy = c[1] - a[1], l2 = dx * dx + dy * dy;
                const float u  = l2 > 1e-6f ? ClampF( ( ( p[0] - a[0] ) * dx + ( p[1] - a[1] ) * dy ) / l2, 0.0f, 1.0f ) : 0.0f;
                const float ex = a[0] + dx * u - p[0], ey = a[1] + dy * u - p[1];
                if ( ex * ex + ey * ey < best )
                {
                    best  = ex * ex + ey * ey;
                    bestZ = a[2] + ( c[2] - a[2] ) * u;
                }
                const float ax = a[0] - p[0], ay = a[1] - p[1];
                for ( int q = 0; q < 8; ++q )
                {
                    if ( sides & ( 1u << q ) )
                        continue;
                    const float den = DIRS[q][0] * dy - DIRS[q][1] * dx;
                    if ( fabsf( den ) < 1e-6f )
                        continue;
                    const float s = ( ax * dy - ay * dx ) / den, v = ( ax * DIRS[q][1] - ay * DIRS[q][0] ) / den;
                    if ( s > 0.0f && s <= R && v >= 0.0f && v <= 1.0f )
                        sides |= 1u << q;
                }
            }
            if ( best <= R * R )
            {
                const float w = 1.0f / ( best + 1.0f );
                wSum += w;
                zSum += w * bestZ;
            }
        } );
        if ( wSum <= 0.0f )
            return false;
        *outZ = zSum / wSum;
        *outSides = 0;
        for ( int q = 0; q < 8; ++q )
            *outSides += ( sides >> q ) & 1;
        return true;
    }

    // A ray that misses every patch over a GAP (a hole, a model standing in one) used to fall
    // through to whatever lay below - a model's box, a brush under the map, the base plane -
    // and the ring jumped away from the mouse.  When terrain closes the spot in from at least
    // 6 of 8 directions, the cursor is where the ray crosses the height of that terrain.
    bool GapCursor( const ray_t &ray, float outPoint[3] )
    {
        if ( fabsf( ray.dir[2] ) <= 1e-6f )
            return false;
        const float R = s_outer * 4.0f > 2048.0f ? s_outer * 4.0f : 2048.0f;
        float z = s_cursorHave ? s_cursor[2] : s_createZ;
        float p[3];
        int   sides = 0;
        for ( int iter = 0; iter < 6; ++iter )
        {
            const float t = ( z - ray.origin[2] ) / ray.dir[2];
            if ( t <= 0.0f || t >= 131072.0f )
                return false;
            for ( int k = 0; k < 3; ++k )
                p[k] = ray.origin[k] + ray.dir[k] * t;
            float zNext;
            if ( !TerrainHeightNear( p[0], p[1], z, &zNext ) && !GapHeightAround( p, R, &zNext, &sides ) )
                return false;
            const bool settled = fabsf( zNext - z ) < 0.5f;
            z = zNext;
            if ( settled )
                break;
        }
        const float t = ( z - ray.origin[2] ) / ray.dir[2];
        if ( t <= 0.0f || t >= 131072.0f )
            return false;
        for ( int k = 0; k < 3; ++k )
            p[k] = ray.origin[k] + ray.dir[k] * t;
        float zAround;
        if ( !GapHeightAround( p, R, &zAround, &sides ) || sides < 6 )
            return false;                               // open ground past the terrain's edge
        outPoint[0] = p[0];
        outPoint[1] = p[1];
        outPoint[2] = z;
        return true;
    }

    // The closest SHEET within `reach` (XY): a stroke beside terrain continues its lattice.
    selbrush_t *NearestEligiblePatch( const float *p, float reach )
    {
        selbrush_t *best = nullptr;
        float bestD = reach;
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                return;
            const float d = BoundsDistanceXY( b, p );
            if ( d < bestD )
            {
                bestD = d;
                best  = b;
            }
        } );
        return best;
    }

    // Template for a chunk with no terrain in reach: the texture browser's current material
    // and lightmap (Create_Terrain's rule), no layers or flags.  One scratch def.
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
        if ( node && !GridIsSheet( node->patch->def ) )
            node = nullptr;                             // a curved patch is no lattice
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
                // any part of the brush over the cell counts: its nearest point, not its centre
                float near2[3] = { s_cursor[0], s_cursor[1], 0.0f };
                if ( near2[0] < minx ) near2[0] = minx; else if ( near2[0] > minx + SX ) near2[0] = minx + SX;
                if ( near2[1] < miny ) near2[1] = miny; else if ( near2[1] > miny + SY ) near2[1] = miny + SY;
                if ( BrushDistance( s_cursor, near2 ) > r )
                    continue;
                // empty = no terrain SURFACE under it (bounding boxes stacked chunks on curved
                // neighbours); a partly covered cell is the gap filler's job
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

    // ── fill a hole ──────────────────────────────────────────────────────────
    // A gap between curved patches has no lattice cell that fits it.  Creation strokes first
    // look for a GAP under the brush (TraceHoleOutline): terrain coverage is rasterised around
    // it, the empty region is flooded, and every terrain BORDER vertex on that region's edge
    // joins the outline in the order the edge passes it - every neighbour vertex is a fill
    // vertex, so the seam cannot crack.  FillOutline cuts the outline into convex pieces and
    // fills each with a Coons blend of its sides.  Outline points are 7 floats: x y z + the
    // four weight bytes.
    const float KHOLE_TOL = 2.0f;           // terrain grows this much: narrower cracks are closed

    char s_holeWhy[256];                    // why the last gap search failed ("" = no gap there)
    bool s_holeSaid = false;                // a stroke says it once

    void HoleWhy( const char *fmt, ... )
    {
        va_list args;
        va_start( args, fmt );
        _vsnprintf( s_holeWhy, sizeof( s_holeWhy ), fmt, args );
        va_end( args );
        s_holeWhy[sizeof( s_holeWhy ) - 1] = '\0';
    }

    void HoleVert( const drawVert_t &v, float out[7] )
    {
        out[0] = v.xyz[0]; out[1] = v.xyz[1]; out[2] = v.xyz[2];
        const byte *c = (const byte *)&v.vert_color;
        for ( int k = 0; k < 4; ++k )
            out[3 + k] = (float)c[k];
    }

    selbrush_t *CreatePatchFromGrid( const patchMesh_t *like, entity_s *owner, const std::vector<float> &grid,
                                     int W, int i0, int i1, int j0, int j1 )
    {
        const int nx = i1 - i0 + 1, ny = j1 - j0 + 1;
        if ( nx < 2 || ny < 2 || nx > 16 || ny > 16 )
            return nullptr;
        patchMesh_t *p = NewPatchLike( like, nx, ny );
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
        return LinkPatch( p, owner );
    }

    bool  s_holeFailValid = false;          // do not re-search the same spot every frame
    float s_holeFailAt[2] = { 0.0f, 0.0f };

    // ── convex pieces ────────────────────────────────────────────────────────
    // One Coons blend over a NON-CONVEX outline folds: flipped (dark) triangles at the inner
    // corners and a smeared texture.  So every inner corner turning more than ~35 degrees is
    // cut away first - the arriving or the leaving edge extended across the gap, whichever
    // cut is shorter - until the pieces are convex (stair-steps become rectangles).  A
    // piece's corners are its four sharpest turns; opposite sides get equal point counts by
    // splitting their longest segments, and splitting a cut splits it in BOTH its pieces, so
    // the pieces meet vertex to vertex.
    struct kterHFill_t
    {
        std::vector<float>            pts;      // 7 floats a point
        std::vector<std::vector<int>> pieces;   // counter-clockwise cycles of point indices
    };

    float HLen( const kterHFill_t &F, int a, int b )
    {
        const float dx = F.pts[b * 7] - F.pts[a * 7], dy = F.pts[b * 7 + 1] - F.pts[a * 7 + 1];
        return sqrtf( dx * dx + dy * dy );
    }

    int HLerp( kterHFill_t &F, int a, int b, float t )
    {
        float p[7];
        for ( int k = 0; k < 7; ++k )
            p[k] = F.pts[a * 7 + k] + ( F.pts[b * 7 + k] - F.pts[a * 7 + k] ) * t;
        F.pts.insert( F.pts.end(), p, p + 7 );
        return (int)( F.pts.size() / 7 ) - 1;
    }

    // Point m goes between a and b in every piece where they are neighbours.
    void HSplit( kterHFill_t &F, int a, int b, int m )
    {
        for ( std::vector<int> &P : F.pieces )
            for ( size_t k = 0; k < P.size(); ++k )
            {
                const int x = P[k], y = P[( k + 1 ) % P.size()];
                if ( ( x == a && y == b ) || ( x == b && y == a ) )
                {
                    P.insert( P.begin() + k + 1, m );
                    break;
                }
            }
    }

    // Split the longest segment of a chain of points (cyclic: the closing segment too).
    void HSplitLongest( kterHFill_t &F, const std::vector<int> &chain, bool cyclic )
    {
        const int n = (int)chain.size(), segs = cyclic ? n : n - 1;
        int   best = -1;
        float bestLen = -1.0f;
        for ( int k = 0; k < segs; ++k )
        {
            const float l = HLen( F, chain[k], chain[( k + 1 ) % n] );
            if ( l > bestLen )
            {
                bestLen = l;
                best = k;
            }
        }
        if ( best < 0 )
            return;
        const int a = chain[best], b = chain[( best + 1 ) % n];
        HSplit( F, a, b, HLerp( F, a, b, 0.5f ) );
    }

    // Direction of piece P arriving at (step -1) or leaving (step +1) its point k, measured
    // to the first point at least minLen away, so one short segment does not decide it.
    void HDir( const kterHFill_t &F, const std::vector<int> &P, int k, int step, float minLen, float out[2] )
    {
        const int n = (int)P.size();
        const float *c = &F.pts[P[k] * 7], *o = c;
        for ( int s = 1; s < n; ++s )
        {
            o = &F.pts[P[( ( k + step * s ) % n + n ) % n] * 7];
            if ( ( o[0] - c[0] ) * ( o[0] - c[0] ) + ( o[1] - c[1] ) * ( o[1] - c[1] ) >= minLen * minLen )
                break;
        }
        const float dx = ( o[0] - c[0] ) * (float)step, dy = ( o[1] - c[1] ) * (float)step;
        const float len = sqrtf( dx * dx + dy * dy );
        out[0] = len > 1e-6f ? dx / len : 1.0f;
        out[1] = len > 1e-6f ? dy / len : 0.0f;
    }

    // The turn at point k: + left (a convex corner of a counter-clockwise piece), - right.
    float HTurn( const kterHFill_t &F, const std::vector<int> &P, int k, float minLen )
    {
        float a[2], b[2];
        HDir( F, P, k, -1, minLen, a );
        HDir( F, P, k, 1, minLen, b );
        return atan2f( a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1] );
    }

    float HArea( const kterHFill_t &F, const std::vector<int> &P )
    {
        float a = 0.0f;
        for ( size_t i = 0, j = P.size() - 1; i < P.size(); j = i++ )
            a += F.pts[P[j] * 7] * F.pts[P[i] * 7 + 1] - F.pts[P[i] * 7] * F.pts[P[j] * 7 + 1];
        return a * 0.5f;
    }

    bool HInside( const kterHFill_t &F, const std::vector<int> &P, float x, float y )
    {
        bool in = false;
        for ( size_t i = 0, j = P.size() - 1; i < P.size(); j = i++ )
        {
            const float *a = &F.pts[P[i] * 7], *b = &F.pts[P[j] * 7];
            if ( ( a[1] > y ) != ( b[1] > y ) && x < ( b[0] - a[0] ) * ( y - a[1] ) / ( b[1] - a[1] ) + a[0] )
                in = !in;
        }
        return in;
    }

    // Do segments p-q and a-b cross (touching ends do not count)?
    bool HCross( const float *p, const float *q, const float *a, const float *b )
    {
        auto side = []( const float *o, const float *s, const float *t )
        {
            return ( s[0] - o[0] ) * ( t[1] - o[1] ) - ( s[1] - o[1] ) * ( t[0] - o[0] );
        };
        return side( p, q, a ) * side( p, q, b ) < 0.0f && side( a, b, p ) * side( a, b, q ) < 0.0f;
    }

    // Cut piece pi at its inner corner k.  The cut ends ON an existing point near its hit
    // when the straight line there stays inside; its own points are about `spacing` apart.
    bool HCut( kterHFill_t &F, int pi, int k, float spacing )
    {
        const std::vector<int> P = F.pieces[pi];
        const int n = (int)P.size(), rid = P[k];
        float bestT = FLT_MAX, bestU = 0.0f;
        int   bestS = -1;
        for ( int way = 0; way < 2; ++way )
        {
            float d[2];
            HDir( F, P, k, way == 0 ? -1 : 1, spacing * 0.5f, d );
            if ( way == 1 )                             // the leaving edge, extended backwards
            {
                d[0] = -d[0];
                d[1] = -d[1];
            }
            const float *r = &F.pts[rid * 7];
            for ( int s = 0; s < n; ++s )
            {
                if ( s == k || ( s + 1 ) % n == k )
                    continue;                           // the corner's own sides
                if ( HLen( F, rid, P[s] ) < spacing * 0.5f && HLen( F, rid, P[( s + 1 ) % n] ) < spacing * 0.5f )
                    continue;                           // a stub HDir looked past
                const float *a = &F.pts[P[s] * 7], *b = &F.pts[P[( s + 1 ) % n] * 7];
                const float ex = b[0] - a[0], ey = b[1] - a[1];
                const float den = d[0] * ey - d[1] * ex;
                if ( fabsf( den ) < 1e-6f )
                    continue;
                const float ax = a[0] - r[0], ay = a[1] - r[1];
                const float t = ( ax * ey - ay * ex ) / den, u = ( ax * d[1] - ay * d[0] ) / den;
                if ( t > KHOLE_TOL && u >= 0.0f && u <= 1.0f && t < bestT )
                {
                    bestT = t;
                    bestU = u;
                    bestS = s;
                }
            }
        }
        if ( bestS < 0 )
            return false;
        const int a = P[bestS], b = P[( bestS + 1 ) % n];
        const float segLen = HLen( F, a, b );
        int x = -1;
        for ( int e = 0; e < 2 && x < 0; ++e )
        {
            const int   cand  = ( e == 0 ) == ( bestU < 0.5f ) ? a : b;      // the nearer end first
            const float along = ( cand == a ? bestU : 1.0f - bestU ) * segLen;
            if ( along > spacing * 0.35f || cand == P[( k + 1 ) % n] || cand == P[( k + n - 1 ) % n] )
                continue;
            const float *r = &F.pts[rid * 7], *c = &F.pts[cand * 7];
            bool clear = HInside( F, P, ( r[0] + c[0] ) * 0.5f, ( r[1] + c[1] ) * 0.5f );
            for ( int s = 0; s < n && clear; ++s )
                clear = !HCross( r, c, &F.pts[P[s] * 7], &F.pts[P[( s + 1 ) % n] * 7] );
            if ( clear )
                x = cand;
        }
        if ( x < 0 )
        {
            x = HLerp( F, a, b, bestU );
            HSplit( F, a, b, x );                       // (a cut already there splits in both pieces)
        }

        // corner -> along the piece -> x, back along the cut; and x -> along the piece -> corner
        const std::vector<int> Q = F.pieces[pi];
        const int m = (int)Q.size();
        int ir = -1, ix = -1;
        for ( int i = 0; i < m; ++i )
        {
            if ( Q[i] == rid ) ir = i;
            if ( Q[i] == x )   ix = i;
        }
        if ( ir < 0 || ix < 0 || ir == ix )
            return false;
        int segs = (int)( HLen( F, rid, x ) / spacing + 0.5f );
        if ( segs < 1 )
            segs = 1;
        std::vector<int> cut;
        for ( int s = 1; s < segs; ++s )
            cut.push_back( HLerp( F, rid, x, (float)s / (float)segs ) );
        std::vector<int> A, B;
        for ( int i = ir; ; i = ( i + 1 ) % m )
        {
            A.push_back( Q[i] );
            if ( i == ix )
                break;
        }
        A.insert( A.end(), cut.rbegin(), cut.rend() );
        for ( int i = ix; ; i = ( i + 1 ) % m )
        {
            B.push_back( Q[i] );
            if ( i == ir )
                break;
        }
        B.insert( B.end(), cut.begin(), cut.end() );
        if ( A.size() < 3 || B.size() < 3 || HArea( F, A ) <= 0.0f || HArea( F, B ) <= 0.0f )
            return false;
        F.pieces[pi] = A;
        F.pieces.push_back( B );
        return true;
    }

    // Piece P's four corners (point indices, in order): exactly four real corners (~30
    // degrees and up) however close - a thin strip - else its sharpest left turns, kept 8% of
    // the way round apart when that can be done.
    bool HCorners( const kterHFill_t &F, const std::vector<int> &P, float minLen, int corner[4] )
    {
        const int n = (int)P.size();
        if ( n < 4 )
            return false;
        std::vector<float> turn( n ), arc( n + 1, 0.0f );
        for ( int i = 0; i < n; ++i )
        {
            turn[i] = HTurn( F, P, i, minLen );
            arc[i + 1] = arc[i] + HLen( F, P[i], P[( i + 1 ) % n] );
        }
        const float perimeter = arc[n];
        int at[4] = { -1, -1, -1, -1 }, real = 0;
        for ( int i = 0; i < n; ++i )
            if ( turn[i] > 0.5f && real++ < 4 )
                at[real - 1] = i;
        if ( real != 4 )
            at[3] = -1;
        for ( int attempt = 0; attempt < 2 && at[3] < 0; ++attempt )
        {
            const float apart = attempt == 0 ? perimeter * 0.08f : 0.0f;
            for ( int c = 0; c < 4; ++c )
            {
                at[c] = -1;
                float best = -FLT_MAX;
                for ( int i = 0; i < n; ++i )
                {
                    if ( turn[i] <= best )
                        continue;
                    bool crowded = false;               // ("near" is a windef.h macro)
                    for ( int o = 0; o < c && !crowded; ++o )
                    {
                        float d = fabsf( arc[i] - arc[at[o]] );
                        if ( d > perimeter * 0.5f ) d = perimeter - d;
                        crowded = i == at[o] || d < apart;
                    }
                    if ( crowded )
                        continue;
                    best  = turn[i];
                    at[c] = i;
                }
                if ( at[c] < 0 )
                    break;
            }
        }
        if ( at[3] < 0 )
            return false;
        std::sort( at, at + 4 );
        for ( int c = 0; c < 4; ++c )
            corner[c] = P[at[c]];
        return true;
    }

    // Piece P's points from corner a round to corner b, both included.
    void HSide( const std::vector<int> &P, int a, int b, std::vector<int> &out )
    {
        out.clear();
        const size_t n = P.size();
        size_t i = 0;
        while ( i < n && P[i] != a )
            ++i;
        for ( size_t s = 0; s <= n && i < n; ++s )
        {
            out.push_back( P[( i + s ) % n] );
            if ( s > 0 && P[( i + s ) % n] == b )
                break;
        }
    }

    // A closed COUNTER-CLOCKWISE outline around a hole (TraceHoleOutline's result) -> convex
    // pieces -> a Coons grid each -> patches facing up.
    bool FillOutline( const std::vector<float> &loop, const patchMesh_t *like, entity_s *owner )
    {
        const int n = (int)( loop.size() / 7 );
        if ( n < 3 || !like || !owner )
            return false;
        kterHFill_t F;
        F.pts = loop;
        F.pieces.resize( 1 );
        std::vector<float> lens( n );
        for ( int i = 0; i < n; ++i )
        {
            F.pieces[0].push_back( i );
            lens[i] = HLen( F, i, ( i + 1 ) % n );
        }
        std::nth_element( lens.begin(), lens.begin() + n / 2, lens.end() );
        const float spacing = lens[n / 2] > 4.0f ? lens[n / 2] : 4.0f;    // the outline's typical edge
        const float minLen  = spacing * 0.5f;

        // 1. cut the inner corners away, sharpest first
        std::set<int> stuck;
        for ( int cuts = 0; cuts < 32; ++cuts )
        {
            int   bp = -1, bk = -1;
            float worst = -0.6f;                        // a right turn of ~35 degrees or more
            for ( size_t p = 0; p < F.pieces.size(); ++p )
                for ( int k = 0; k < (int)F.pieces[p].size(); ++k )
                {
                    const float t = HTurn( F, F.pieces[p], k, minLen );
                    if ( t < worst && !stuck.count( F.pieces[p][k] ) )
                    {
                        worst = t;
                        bp = (int)p;
                        bk = k;
                    }
                }
            if ( bp < 0 )
                break;
            const int id = F.pieces[bp][bk];
            if ( !HCut( F, bp, bk, spacing ) )
                stuck.insert( id );
        }

        // 2. four corners a piece
        std::vector<int> corners( F.pieces.size() * 4 );
        for ( size_t p = 0; p < F.pieces.size(); ++p )
        {
            while ( F.pieces[p].size() < 4 )
            {
                const std::vector<int> P = F.pieces[p];
                HSplitLongest( F, P, true );
            }
            if ( !HCorners( F, F.pieces[p], minLen, &corners[p * 4] ) )
            {
                HoleWhy( "a piece of the %i-vertex outline has no four usable corners.", n );
                return false;
            }
        }

        // 3. opposite sides the same number of points
        std::vector<int> s0, s1;
        for ( int splits = 0; ; ++splits )
        {
            bool changed = false;
            for ( size_t p = 0; p < F.pieces.size() && !changed; ++p )
                for ( int pair = 0; pair < 2 && !changed; ++pair )
                {
                    const int *c = &corners[p * 4];
                    HSide( F.pieces[p], c[pair], c[pair + 1], s0 );
                    HSide( F.pieces[p], c[pair + 2], c[( pair + 3 ) % 4], s1 );
                    if ( s0.size() == s1.size() )
                        continue;
                    if ( s0.size() > 241 || s1.size() > 241 || splits > 20000 )
                    {
                        HoleWhy( "the gap needs more than 241 points along one side (the limit). Fill part of it with "
                                 "ordinary creation chunks first, then the rest." );
                        return false;
                    }
                    HSplitLongest( F, s0.size() < s1.size() ? s0 : s1, false );
                    changed = true;
                }
            if ( !changed )
                break;
        }
        for ( size_t p = 0; p < F.pieces.size(); ++p )  // (checked before anything is made)
        {
            const int *c = &corners[p * 4];
            HSide( F.pieces[p], c[0], c[1], s0 );
            HSide( F.pieces[p], c[1], c[2], s1 );
            if ( s0.size() > 241 || s1.size() > 241 )
            {
                HoleWhy( "the gap needs more than 241 points along one side (the limit). Fill part of it with "
                         "ordinary creation chunks first, then the rest." );
                return false;
            }
        }

        // 4. each piece a Coons blend of its sides, cut into patches of at most 16 x 16
        int made = 0, tris = 0;
        std::vector<float> grid;
        std::vector<int> side[4];
        for ( size_t p = 0; p < F.pieces.size(); ++p )
        {
            const int *c = &corners[p * 4];
            for ( int s = 0; s < 4; ++s )
                HSide( F.pieces[p], c[s], c[( s + 1 ) % 4], side[s] );
            const int W = (int)side[0].size(), H = (int)side[1].size();
            if ( W < 2 || H < 2 )
                continue;
            grid.assign( (size_t)W * H * 7, 0.0f );
            const float *c0 = &F.pts[c[0] * 7], *c1 = &F.pts[c[1] * 7], *c2 = &F.pts[c[2] * 7], *c3 = &F.pts[c[3] * 7];
            for ( int j = 0; j < H; ++j )
                for ( int i = 0; i < W; ++i )
                {
                    const float u = (float)i / (float)( W - 1 ), v = (float)j / (float)( H - 1 );
                    const float *B = &F.pts[side[0][i] * 7], *T = &F.pts[side[2][W - 1 - i] * 7];
                    const float *R = &F.pts[side[1][j] * 7], *L = &F.pts[side[3][H - 1 - j] * 7];
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
            tris += ( W - 1 ) * ( H - 1 ) * 2;
        }
        if ( !made )
            return false;
        s_holeFailValid = false;
        GraphInvalidate();                              // new terrain: the next flush rebuilds
        // the outline dictates the density (every neighbour vertex is a fill vertex): say what it cost
        const int pieces = (int)F.pieces.size();
        SetStatus( "Filled the gap: %i piece%s, %i patch%s, %i triangles (set by the gap's edge vertices). Ctrl+Z undoes.",
                   pieces, pieces == 1 ? "" : "s", made, made == 1 ? "" : "es", tris );
        Sys_Printf( "Terrain Sculpt: filled a %i-vertex gap - %i convex piece%s, %i patch%s, %i triangles (the density "
                    "is set by the vertices along the gap's edge).\n", n, pieces, pieces == 1 ? "" : "s",
                    made, made == 1 ? "" : "es", tris );
        g_nUpdateBits = -1;
        return true;
    }

    // ── finding the gap ──────────────────────────────────────────────────────
    // Terrain triangles near the cursor are rasterised onto a grid of cells, each grown by
    // KHOLE_TOL so a hairline crack between patches counts as closed; the gap is the empty
    // region flooded from the empty cell nearest the cursor, and a region reaching the grid's
    // edge is open ground, not a gap.  (A tracer walking patch borders edge to edge broke off
    // wherever a corner missed its neighbour or terrain overlapped.)
    struct kterHTri_t
    {
        float e[3][3];                  // edge lines: inside while e[0] * x + e[1] * y + e[2] >= -KHOLE_TOL
        float lo[2], hi[2];             // bounds, grown by KHOLE_TOL
    };

    struct kterHScan_t
    {
        float ox, oy, res;              // cell (i, j) is centred on ox + ( i + 0.5 ) * res, oy + ( j + 0.5 ) * res
        int   n;                        // n x n cells
        std::vector<unsigned char> m;   // 0 empty, 1 terrain, 2 the gap
        std::vector<kterHTri_t>    tris;
        std::vector<selbrush_t *>  nodes;
        std::vector<int>           first;   // nodes[k]'s triangles: first[k] .. first[k + 1] - 1

        int At( int i, int j ) const
        {
            return ( i < 0 || j < 0 || i >= n || j >= n ) ? 1 : m[(size_t)j * n + i];
        }
    };

    bool HTriCovers( const kterHTri_t &t, float x, float y )
    {
        if ( x < t.lo[0] || x > t.hi[0] || y < t.lo[1] || y > t.hi[1] )
            return false;
        for ( int k = 0; k < 3; ++k )
            if ( t.e[k][0] * x + t.e[k][1] * y + t.e[k][2] < -KHOLE_TOL )
                return false;
        return true;
    }

    void HAddTri( kterHScan_t &S, const float *a, const float *b, const float *c )
    {
        const float area2 = ( b[0] - a[0] ) * ( c[1] - a[1] ) - ( b[1] - a[1] ) * ( c[0] - a[0] );
        if ( fabsf( area2 ) < 1e-3f )
            return;                                     // edge-on from above: covers nothing
        if ( area2 < 0.0f )
        {
            const float *t = b;                         // counter-clockwise
            b = c;
            c = t;
        }
        kterHTri_t T;
        const float *v[3] = { a, b, c };
        for ( int k = 0; k < 3; ++k )
        {
            const float *p = v[k], *q = v[( k + 1 ) % 3];
            const float ex = q[0] - p[0], ey = q[1] - p[1];
            const float len = sqrtf( ex * ex + ey * ey );
            T.e[k][0] = -ey / len;
            T.e[k][1] =  ex / len;
            T.e[k][2] = -( T.e[k][0] * p[0] + T.e[k][1] * p[1] );
        }
        for ( int k = 0; k < 2; ++k )
        {
            T.lo[k] = ( a[k] < b[k] ? ( a[k] < c[k] ? a[k] : c[k] ) : ( b[k] < c[k] ? b[k] : c[k] ) ) - KHOLE_TOL;
            T.hi[k] = ( a[k] > b[k] ? ( a[k] > c[k] ? a[k] : c[k] ) : ( b[k] > c[k] ? b[k] : c[k] ) ) + KHOLE_TOL;
        }
        S.tris.push_back( T );
        int i0 = (int)ceilf( ( T.lo[0] - S.ox ) / S.res - 0.5f ), i1 = (int)floorf( ( T.hi[0] - S.ox ) / S.res - 0.5f );
        int j0 = (int)ceilf( ( T.lo[1] - S.oy ) / S.res - 0.5f ), j1 = (int)floorf( ( T.hi[1] - S.oy ) / S.res - 0.5f );
        if ( i0 < 0 ) i0 = 0;
        if ( j0 < 0 ) j0 = 0;
        if ( i1 > S.n - 1 ) i1 = S.n - 1;
        if ( j1 > S.n - 1 ) j1 = S.n - 1;
        for ( int j = j0; j <= j1; ++j )
            for ( int i = i0; i <= i1; ++i )
            {
                unsigned char &cell = S.m[(size_t)j * S.n + i];
                if ( cell == 0 && HTriCovers( T, S.ox + ( (float)i + 0.5f ) * S.res, S.oy + ( (float)j + 0.5f ) * S.res ) )
                    cell = 1;
            }
    }

    // n x n cells over the square of half-side `half` around (cx, cy).
    void HScanBuild( kterHScan_t &S, float cx, float cy, float half, int n )
    {
        S.n   = n;
        S.res = half * 2.0f / (float)n;
        S.ox  = cx - half;
        S.oy  = cy - half;
        S.m.assign( (size_t)n * n, 0 );
        S.tris.clear();
        S.nodes.clear();
        S.first.clear();
        const float box[4] = { S.ox, S.oy, S.ox + half * 2.0f, S.oy + half * 2.0f };
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( !PatchEligible( b ) || !BoundsMeet( b->def->mins, b->def->maxs, box, KHOLE_TOL ) )
                return;
            S.nodes.push_back( b );
            S.first.push_back( (int)S.tris.size() );
            const patchMesh_t *def = b->patch->def;
            for ( int i = 0; i + 1 < def->width; ++i )
                for ( int j = 0; j + 1 < def->height; ++j )
                {
                    const float *v00 = def->ctrl[i][j].xyz,     *v10 = def->ctrl[i + 1][j].xyz;
                    const float *v01 = def->ctrl[i][j + 1].xyz, *v11 = def->ctrl[i + 1][j + 1].xyz;
                    if ( ( def->ctrl[i][j].turned_edge & 1 ) != 0 )
                    {
                        HAddTri( S, v00, v10, v11 );
                        HAddTri( S, v00, v11, v01 );
                    }
                    else
                    {
                        HAddTri( S, v00, v10, v01 );
                        HAddTri( S, v10, v11, v01 );
                    }
                }
        } );
        S.first.push_back( (int)S.tris.size() );
    }

    // Is (x, y) on any terrain (grown by KHOLE_TOL)?  Exact, not the cells.
    bool HCovered( const kterHScan_t &S, float x, float y )
    {
        for ( size_t k = 0; k < S.nodes.size(); ++k )
        {
            const float *mins = S.nodes[k]->def->mins, *maxs = S.nodes[k]->def->maxs;
            if ( x < mins[0] - KHOLE_TOL || x > maxs[0] + KHOLE_TOL || y < mins[1] - KHOLE_TOL || y > maxs[1] + KHOLE_TOL )
                continue;
            for ( int t = S.first[k]; t < S.first[k + 1]; ++t )
                if ( HTriCovers( S.tris[t], x, y ) )
                    return true;
        }
        return false;
    }

    // Is a gap cell within rc cells of (x, y)?
    bool HNearGap( const kterHScan_t &S, float x, float y, int rc )
    {
        const int ci = (int)floorf( ( x - S.ox ) / S.res ), cj = (int)floorf( ( y - S.oy ) / S.res );
        for ( int j = cj - rc; j <= cj + rc; ++j )
            for ( int i = ci - rc; i <= ci + rc; ++i )
                if ( S.At( i, j ) == 2 )
                    return true;
        return false;
    }

    // Label the empty region around cell (si, sj) as the gap; false when it reaches the edge.
    bool HFlood( kterHScan_t &S, int si, int sj )
    {
        std::vector<int> stack( 1, sj * S.n + si );
        S.m[(size_t)sj * S.n + si] = 2;
        while ( !stack.empty() )
        {
            const int c = stack.back();
            stack.pop_back();
            const int i = c % S.n, j = c / S.n;
            if ( i == 0 || j == 0 || i == S.n - 1 || j == S.n - 1 )
                return false;
            const int nb[4] = { c - 1, c + 1, c - S.n, c + S.n };
            for ( int k = 0; k < 4; ++k )
                if ( S.m[nb[k]] == 0 )
                {
                    S.m[nb[k]] = 2;
                    stack.push_back( nb[k] );
                }
        }
        return true;
    }

    // The gap's outer edge along the cell borders, counter-clockwise (the gap on the left),
    // as the corners where it turns.
    void HContour( const kterHScan_t &S, std::vector<float> &out )
    {
        out.clear();
        int si = -1, sj = -1;
        for ( int j = 0; j < S.n && si < 0; ++j )
            for ( int i = 0; i < S.n; ++i )
                if ( S.m[(size_t)j * S.n + i] == 2 )
                {
                    si = i;
                    sj = j;
                    break;
                }
        if ( si < 0 )
            return;
        // start on the bottom side of the lowest gap cell, heading +x (0 +x, 1 +y, 2 -x, 3 -y)
        static const int DX[4] = { 1, 0, -1, 0 }, DY[4] = { 0, 1, 0, -1 };
        int ci = si, cj = sj, d = 0;
        for ( int guard = 0; guard < 4 * S.n * S.n + 8; ++guard )
        {
            ci += DX[d];
            cj += DY[d];
            // the cells ahead-left and ahead-right of the corner just reached
            const int lx = DX[d] - DY[d], ly = DY[d] + DX[d], rx = DX[d] + DY[d], ry = DY[d] - DX[d];
            const bool L = S.At( ci + ( lx - 1 ) / 2, cj + ( ly - 1 ) / 2 ) == 2;
            const bool R = S.At( ci + ( rx - 1 ) / 2, cj + ( ry - 1 ) / 2 ) == 2;
            const int nd = !L ? ( d + 1 ) & 3 : ( R ? ( d + 3 ) & 3 : d );
            if ( nd != d )
            {
                out.push_back( S.ox + (float)ci * S.res );
                out.push_back( S.oy + (float)cj * S.res );
            }
            d = nd;
            if ( ci == si && cj == sj && d == 0 )
                break;
        }
    }

    // The outline of the gap under the brush: every terrain border vertex with an empty point
    // of the gap right beside it, ordered by where the gap's traced edge passes it.
    bool TraceHoleOutline( std::vector<float> &loop, selbrush_t **likeNode )
    {
        loop.clear();
        kterHScan_t S;

        // anything empty under the brush at all?  Its own box first: most strokes are on ground.
        const float half0 = s_outer + 8.0f;
        int n0 = (int)( half0 * 0.5f ) + 1;             // 4-unit cells
        n0 = n0 < 16 ? 16 : ( n0 > 1024 ? 1024 : n0 );
        HScanBuild( S, s_cursor[0], s_cursor[1], half0, n0 );
        float start[2] = { 0.0f, 0.0f }, best = FLT_MAX;
        for ( int j = 0; j < S.n; ++j )
            for ( int i = 0; i < S.n; ++i )
            {
                if ( S.m[(size_t)j * S.n + i] != 0 )
                    continue;
                const float p[3] = { S.ox + ( (float)i + 0.5f ) * S.res, S.oy + ( (float)j + 0.5f ) * S.res, s_cursor[2] };
                const float d = ( p[0] - s_cursor[0] ) * ( p[0] - s_cursor[0] ) + ( p[1] - s_cursor[1] ) * ( p[1] - s_cursor[1] );
                if ( d < best && BrushDistance( s_cursor, p ) <= s_outer )
                {
                    best = d;
                    start[0] = p[0];
                    start[1] = p[1];
                }
            }
        if ( best == FLT_MAX )
            return false;                               // terrain everywhere under the brush

        // flood it: 4-unit cells within 2048, else 16-unit cells within 8192 (which also
        // closes cracks of up to ~16 units that leak out of the fine scan)
        bool closed = false;
        for ( int pass = 0; pass < 2 && !closed; ++pass )
        {
            HScanBuild( S, start[0], start[1], pass == 0 ? 2048.0f : 8192.0f, 1024 );
            const int ci = (int)floorf( ( start[0] - S.ox ) / S.res ), cj = (int)floorf( ( start[1] - S.oy ) / S.res );
            int   si = -1, sj = -1;
            float bd = FLT_MAX;
            for ( int j = cj - 2; j <= cj + 2; ++j )
                for ( int i = ci - 2; i <= ci + 2; ++i )
                {
                    const float dx = S.ox + ( (float)i + 0.5f ) * S.res - start[0];
                    const float dy = S.oy + ( (float)j + 0.5f ) * S.res - start[1];
                    if ( S.At( i, j ) == 0 && dx * dx + dy * dy < bd )
                    {
                        bd = dx * dx + dy * dy;
                        si = i;
                        sj = j;
                    }
                }
            closed = si >= 0 && HFlood( S, si, sj );
        }
        if ( !closed )
        {
            HoleWhy( "the empty ground under the brush is not closed in by terrain within 8192 units (open ground, or a "
                     "crack wider than 16 units leads out of it)." );
            return false;
        }

        std::vector<float> edge;                        // x y pairs, counter-clockwise
        HContour( S, edge );
        const int en = (int)( edge.size() / 2 );
        if ( en < 4 )
            return false;
        std::vector<float> arc( en + 1, 0.0f );
        float box[4] = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
        for ( int k = 0; k < en; ++k )
        {
            const float *a = &edge[k * 2], *b = &edge[( ( k + 1 ) % en ) * 2];
            arc[k + 1] = arc[k] + sqrtf( ( b[0] - a[0] ) * ( b[0] - a[0] ) + ( b[1] - a[1] ) * ( b[1] - a[1] ) );
            box[0] = a[0] < box[0] ? a[0] : box[0];
            box[1] = a[1] < box[1] ? a[1] : box[1];
            box[2] = a[0] > box[2] ? a[0] : box[2];
            box[3] = a[1] > box[3] ? a[1] : box[3];
        }

        // every terrain border vertex with an empty point of THIS gap beside it (probes 3 x
        // KHOLE_TOL out: one hidden under a neighbour, or a seam running up to the gap, has none)
        struct kterHV_t
        {
            float       p[7];
            float       s;                              // where the gap's edge passes it
            selbrush_t *node;
        };
        std::vector<kterHV_t> verts;
        const float probe = KHOLE_TOL * 3.0f;
        for ( size_t k = 0; k < S.nodes.size(); ++k )
        {
            selbrush_t *b = S.nodes[k];
            if ( !BoundsMeet( b->def->mins, b->def->maxs, box, S.res * 2.0f ) )
                continue;
            const patchMesh_t *def = b->patch->def;
            int ii[64], jj[64];
            const int rn = BorderRing( def, ii, jj );
            for ( int r = 0; r < rn; ++r )
            {
                const drawVert_t &v = def->ctrl[ii[r]][jj[r]];
                if ( !HNearGap( S, v.xyz[0], v.xyz[1], 4 ) )
                    continue;
                bool beside = false;
                for ( int a = 0; a < 16 && !beside; ++a )
                {
                    const float x = v.xyz[0] + probe * cosf( (float)a * ( KTER_PI / 8.0f ) );
                    const float y = v.xyz[1] + probe * sinf( (float)a * ( KTER_PI / 8.0f ) );
                    beside = HNearGap( S, x, y, 2 ) && !HCovered( S, x, y );
                }
                if ( !beside )
                    continue;
                kterHV_t h;
                HoleVert( v, h.p );
                h.node = b;
                h.s    = 0.0f;
                float bd = FLT_MAX;
                for ( int e = 0; e < en; ++e )
                {
                    const float *a = &edge[e * 2], *c = &edge[( ( e + 1 ) % en ) * 2];
                    const float dx = c[0] - a[0], dy = c[1] - a[1];
                    const float l2 = dx * dx + dy * dy;
                    const float t  = ClampF( l2 > 1e-6f ? ( ( h.p[0] - a[0] ) * dx + ( h.p[1] - a[1] ) * dy ) / l2 : 0.0f, 0.0f, 1.0f );
                    const float ex = h.p[0] - a[0] - dx * t, ey = h.p[1] - a[1] - dy * t;
                    if ( ex * ex + ey * ey < bd )
                    {
                        bd  = ex * ex + ey * ey;
                        h.s = arc[e] + ( arc[e + 1] - arc[e] ) * t;
                    }
                }
                verts.push_back( h );
            }
        }
        std::sort( verts.begin(), verts.end(), []( const kterHV_t &a, const kterHV_t &b ) { return a.s < b.s; } );

        // the coincident corners of neighbouring patches are ONE outline point
        std::vector<kterHV_t> ring;
        for ( const kterHV_t &v : verts )
        {
            bool dup = false;
            for ( size_t r = 0; r < ring.size() && !dup; ++r )
                dup = ( ring[r].p[0] - v.p[0] ) * ( ring[r].p[0] - v.p[0] )
                    + ( ring[r].p[1] - v.p[1] ) * ( ring[r].p[1] - v.p[1] ) <= KHOLE_TOL * KHOLE_TOL;
            if ( !dup )
                ring.push_back( v );
        }
        const int n = (int)ring.size();
        if ( n < 3 )
        {
            HoleWhy( "only %i terrain vertices border the gap at (%.0f %.0f).", n, start[0], start[1] );
            return false;
        }
        // points closer together than two cells can project out of order: there the outline's
        // own direction decides
        for ( int pass = 0; pass < 4 && n >= 4; ++pass )
        {
            bool swapped = false;
            for ( int i = 0; i < n; ++i )
            {
                kterHV_t &a = ring[i], &b = ring[( i + 1 ) % n];
                const float dx = b.p[0] - a.p[0], dy = b.p[1] - a.p[1];
                if ( dx * dx + dy * dy > 4.0f * S.res * S.res )
                    continue;
                const kterHV_t &p = ring[( i + n - 1 ) % n], &q = ring[( i + 2 ) % n];
                if ( dx * ( q.p[0] - p.p[0] ) + dy * ( q.p[1] - p.p[1] ) < 0.0f )
                {
                    std::swap( a, b );
                    swapped = true;
                }
            }
            if ( !swapped )
                break;
        }
        float area = 0.0f;
        for ( int i = 0, j = n - 1; i < n; j = i++ )
            area += ring[j].p[0] * ring[i].p[1] - ring[i].p[0] * ring[j].p[1];
        if ( area <= 0.0f )
        {
            HoleWhy( "the gap's outline at (%.0f %.0f) came out inside-out.", start[0], start[1] );
            return false;
        }
        // the fill copies the patch nearest the cursor (material, layers, owner)
        float nearest = FLT_MAX;
        *likeNode = nullptr;
        for ( const kterHV_t &v : ring )
        {
            loop.insert( loop.end(), v.p, v.p + 7 );
            const float d = ( v.p[0] - s_cursor[0] ) * ( v.p[0] - s_cursor[0] ) + ( v.p[1] - s_cursor[1] ) * ( v.p[1] - s_cursor[1] );
            if ( d < nearest )
            {
                nearest = d;
                *likeNode = v.node;
            }
        }
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
        std::vector<float> loop;
        selbrush_t *likeNode = nullptr;
        return TraceHoleOutline( loop, &likeNode ) && likeNode
            && FillOutline( loop, likeNode->patch->def, likeNode->owner );
    }

    void ExpandUnderBrush()
    {
        // A gap between existing patches is filled along its own outline first; the square
        // lattice below only ever lays chunks on ground that has NO terrain at all.
        s_holeWhy[0] = '\0';
        if ( FillHoleUnderCursor() )
            return;
        kterLattice_t L;
        float cells[64][5];
        const int n = ResolveLattice( &L ) ? EmptyCellsUnderBrush( cells, L ) : 0;
        if ( !n )
        {
            if ( s_holeWhy[0] && !s_holeSaid )          // nothing to lay either: say why the gap stays open
            {
                Sys_Printf( "Terrain Sculpt: gap fill - %s\n", s_holeWhy );
                s_holeSaid = true;
            }
            return;
        }
        const patchMesh_t *like = L.like ? L.like : FreshTemplate();
        for ( int c = 0; c < n; ++c )
        {
            selbrush_t *node = CreateChunkXY( like, L.owner, cells[c][0], cells[c][1],
                                              cells[c][2], cells[c][3],
                                              PointsFor( cells[c][2], L.cellX ), PointsFor( cells[c][3], L.cellY ),
                                              nullptr, cells[c][4] );
            // seams: every border point takes the height of the terrain already there
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
                // born in the stroke's record: undo frees it, and no stamp saves a copy of it
                def->xx22b = 1;
                Undo_KiwiMarkCreated( node->def );
            }
            Select_Brush( node, 0, 0, 0 );             // so the next stroke moves it with its neighbours
            s_targets.push_back( node );
            ++s_created;
        }
        GraphInvalidate();                              // new terrain: the next flush rebuilds
        g_nUpdateBits = -1;
    }

    // ── exact subdivision (Tessellate on curved / irregular grids) ──────────
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

    // Source cells [i0,i1) x [j0,j1), each split k x k (SampleCellTri: the new points lie on
    // the old triangles and carry the old mapping).  k = 1 is a verbatim sub-grid.
    selbrush_t *CreatePiece( const patchMesh_t *src, entity_s *owner, int i0, int i1, int j0, int j1, int k )
    {
        const int nx = ( i1 - i0 ) * k + 1, ny = ( j1 - j0 ) * k + 1;
        if ( nx < 2 || ny < 2 || nx > 16 || ny > 16 )
            return nullptr;
        patchMesh_t *p = NewPatchLike( src, nx, ny );
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
        // not Patch_KiwiTextureAndBuild (it re-projects): the mapping state rides along instead
        p->bDirty = src->bDirty;
        *(float *)&p->size_of_struct_0x504C = *(const float *)&src->size_of_struct_0x504C;
        Patch_Rebuild( p, 0 );                   // tessellate only (no re-project, no bounds: no brush yet)
        return LinkPatch( p, owner );
    }

    void TrimUnderBrush()
    {
        if ( !s_cursorHave )
            return;
        std::vector<selbrush_t *> victims;
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( !PatchEligible( b ) )
                return;
            const float *mins = b->def->mins, *maxs = b->def->maxs;
            const float c[3] = { ( mins[0] + maxs[0] ) * 0.5f, ( mins[1] + maxs[1] ) * 0.5f, 0.0f };
            if ( BrushDistance( s_cursor, c ) <= s_outer )
                victims.push_back( b );
        } );
        if ( victims.empty() )
            return;
        BeginRecord( "trim terrain" );
        RecordForDelete( victims );
        DeleteRecorded();
        Undo_End();
        ClearCursor();
        SetStatus( "Armed. Trimmed %i patch%s.", (int)victims.size(), victims.size() == 1 ? "" : "es" );
    }

    // Tessellate takes an ABSOLUTE cell size: every selected patch is rebuilt with cells that
    // big per axis (extent / cell rounded to whole cells); more than 15 cells on an axis splits
    // it into chunks (16 points is the CoD4 cap).  Every chunk reads the ORIGINAL surface.
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
        // Sheets re-grid from their rectangle.  Any other grid's bounding box is not its shape,
        // so it is subdivided EXACTLY instead (CreatePiece, k x k per cell) - finer only.
        std::vector<selbrush_t *> nodes;
        std::vector<int>          exactK;           // 0 = regular sheet, else the k of CreatePiece
        int alreadyFine = 0;
        ForEachNode( false, [&]( selbrush_t *b )
        {
            if ( !PatchEligible( b ) || b->patch->def->width < 2 || b->patch->def->height < 2 )
                return;
            int k = 0;
            if ( !GridIsSheet( b->patch->def ) )
            {
                const int fx = RefineFactor( CellSpan( b->patch->def, 0 ), s_tessCell );
                const int fy = RefineFactor( CellSpan( b->patch->def, 1 ), s_tessCell );
                k = fx > fy ? fx : fy;
                if ( k < 2 )
                {
                    ++alreadyFine;
                    return;
                }
            }
            nodes.push_back( b );
            exactK.push_back( k );
        } );
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
        BeginRecord( "tessellate terrain" );
        RecordForDelete( nodes );
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
        DeleteRecorded();
        EndRecordCreated( created );
        Sys_Printf( "Terrain Sculpt: %i patch%s tessellated to %.0f-unit cells (actual %.1f..%.1f) as %i chunk%s, %i triangles.\n",
                    (int)nodes.size(), nodes.size() == 1 ? "" : "es", s_tessCell, cellMin, cellMax,
                    (int)created.size(), created.size() == 1 ? "" : "s", triangles );
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
        ForEachNode( false, [&]( selbrush_t *b )
        {
            if ( PatchEligible( b ) && GridIsSheet( b->patch->def ) )
                out.push_back( b );
        } );
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

    bool UpdateCursor( int imgX, int imgY )
    {
        float hit[3];
        if ( !PickCursor( imgX, imgY, hit ) )
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
        // The stamp centres of this flush, spaced along the path since the last one.
        for ( s_reachCount = 0; s_reachCount < n; ++s_reachCount )
        {
            const float f = (float)( s_reachCount + 1 ) / (float)n;
            for ( int a = 0; a < 3; ++a )
                s_reachCenters[s_reachCount][a] = s_haveLastCenter
                    ? s_lastCenter[a] + ( s_cursor[a] - s_lastCenter[a] ) * f : s_cursor[a];
        }
        GraphCover();
        const kterOp_t op = OpForStroke();
        for ( int k = 0; k < s_reachCount; ++k )
        {
            Stamp( s_reachCenters[k], op, sign, s_accumDt / (float)n );
            MarkRidersReached( s_reachCenters[k] );
        }
        GraphWriteBack();
        memcpy( s_lastCenter, s_cursor, sizeof( s_cursor ) );
        s_haveLastCenter = true;
        s_accumDt = 0.0f;
        if ( CreationAllowed() && !s_modShift && !s_modCtrl )
        {
            FlushDirty();                        // new chunks probe the terrain's REBUILT triangles for their seams
            ExpandUnderBrush();
        }
        FlushDirty();
        s_reachCount = 0;
        CarryRiders();                       // after the rebuild: riders read final heights
    }

    // ── height colours ("heatmap") and the weight view ──────────────────────
    // Height tools: every patch's base run is re-uploaded blue -> red by height over a flat
    // material.  Armed Texture paint: the brush material's layer draws red at its weight.
    bool WeightViewActive()
    {
        return s_armed && s_weightView && s_tool == KTER_TEXTURE;
    }

    bool HeatmapActive()
    {
        const bool heightTool = s_tool == KTER_RAISE || s_tool == KTER_SETHEIGHT || s_tool == KTER_SMOOTH
                             || s_tool == KTER_NOISE || s_tool == KTER_TRIM;
        // "while NOT armed too" still yields to an ARMED paint / grass tool (they need the real look)
        if ( s_heatAlways && !( s_armed && !heightTool ) )
            return true;
        return s_armed && s_heatmap && heightTool;
    }

    // Min/max control-point Z over the eligible patches; true when the range moved.
    bool ComputeHeatRange()
    {
        float lo = FLT_MAX, hi = -FLT_MAX;
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( !PatchEligible( b ) )
                return;
            const patchMesh_t *def = b->patch->def;
            for ( int i = 0; i < def->width; ++i )
                for ( int j = 0; j < def->height; ++j )
                {
                    const float z = def->ctrl[i][j].xyz[2];
                    if ( z < lo ) lo = z;
                    if ( z > hi ) hi = z;
                }
        } );
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
        // hysteresis: a re-tint re-uploads EVERY patch, so only 5 % of the span or 32 units counts
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

    // Re-tinting: a patch rebuilds its visuals lazily at draw when its def version changed, so
    // `++def->version` is enough (Patch_Rebuild would re-tessellate too).  Every frame with ANY
    // re-upload also rebuilds the world's mesh runs (~50-100 ms while textured), so a MODE
    // change bumps everything AT ONCE; only a range change with the heat view staying on
    // (runs cheap under one material) is spread as a wave from the camera out.
    std::set<const patchMesh_t *> s_retintDone;     // raw def pointers as keys only, never dereferenced
    bool s_retintPending = false;
    const int KTER_RETINT_PER_FRAME = 64;           // wave: patches re-tinted per frame (~20 frames on powerplant)

    void RebuildAllPatchVisuals()
    {
        s_retintPending = false;
        s_retintDone.clear();
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( NodeIsPatch( b ) )
                ++b->patch->def->version;
        } );
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
        // always-on view, tool put away: no stroke end refreshes the range, so poll it ~1/s
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
        ForEachNode( true, [&]( selbrush_t *b )
        {
            if ( !NodeIsPatch( b ) || s_retintDone.count( b->patch->def ) )
                return;
            float d2 = 0.0f;
            for ( int k = 0; k < 3; ++k )
            {
                const float mid = ( b->def->mins[k] + b->def->maxs[k] ) * 0.5f - c->origin[k];
                d2 += mid * mid;
            }
            todo.push_back( std::make_pair( d2, b ) );
        } );
        if ( todo.empty() )
        {
            s_retintPending = false;
            s_retintDone.clear();
            return;
        }
        std::sort( todo.begin(), todo.end() );

        int done = 0;                           // the cost lands in the draw: budget a count, not time
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
        if ( s_coarseSpill )                    // said once per stroke: the effect looks like a bug
        {
            SetStatus( "Armed. The terrain under the ring is too coarse for it: moved points drag their whole cells, so "
                       "ground up to one cell past the ring leaned. Tessellate that patch finer to confine strokes." );
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

    void RadiusStep( float factor )
    {
        s_outer = ClampF( s_outer * factor, 4.0f, 12288.0f );
        s_inner = ClampF( s_inner * factor, 0.0f, s_outer );
        Save();
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
                SetPaintMaterial( dropped );
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
                    SetPaintMaterial( q->name );
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
                SetPaintMaterial( def->kiwiLayer[k] );
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

        changed |= ImGui::SliderFloat( "Paint weight", &s_blendWeight, 0.0f, 1.0f, "%.2f" );
        if ( ImGui::Checkbox( "Preview blending in the camera", &s_previewBlend ) )
        {
            changed = true;
            RebuildAllPatchVisuals();
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
    // Weight view: ONLY the brush material's own layer draws red (packed B | G<<8 | R<<16)
    // at its weight; every other layer keeps its blended texture.
    const bool red  = WeightViewActive() && !s_paintBase && !_stricmp( def->kiwiLayer[slot], s_paintMaterial );
    Material  *flat = red ? WeightMaterial() : nullptr;
    for ( int i = 0; i < vertCount; ++i )
    {
        const unsigned a = ( (const byte *)&verts[i].vert_color )[slot];
        color[i] = ( flat ? 0xFF0000u : ( color[i] & 0x00FFFFFFu ) ) | ( a << 24 );
    }
    Material *use = flat ? flat : BlendTwin( def->kiwiLayer[slot] );
    if ( use && material )
        *material = use;
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

        // Two columns: LEFT = the tool (brush + its settings), RIGHT = everything else.  Fixed
        // widths keep the auto-resized window from oscillating.
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
                    ImGui::SetTooltip( "Every height tool. A terrain point moves only if ALL the triangles it tilts -\n"
                                       "in every patch that shares it - lie inside the ring, so the ramp between old\n"
                                       "and new ground stays inside it: no ground, seam or carried object outside the\n"
                                       "ring ever changes.  Where the grid is too coarse for the ring to contain any\n"
                                       "point, the points under it move anyway and the status line says so.\n"
                                       "Off: every point inside the ring moves and its cells lean past the ring." );
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
        // Smooth is not listed (Shift+LMB smooths with every tool); the slot stays for the test DSL.
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
            SetArmed( !s_armed );
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
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Keeps the height gradient (and its legend) on with the tool put away.\n"
                                   "An armed Texture / Blend / Grass tool still shows the real look." );
            if ( ImGui::Checkbox( "Paint never draws over other geometry", &s_layerDepthEqual ) )
            {
                Save();
                RestripTwins();
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "On (default): a paint layer only lands where its OWN ground is the visible\n"
                                   "surface (depth EQUAL), so props flush with the ground stay on top.\n"
                                   "Turn it off only if paint flickers or vanishes." );

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
            changed |= UnitInputWorld( "Cell size (absolute)", &s_tessCell, 160.0f );
            s_tessCell = ClampF( s_tessCell, 4.0f, 4096.0f );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "/2" ) ) { s_tessCell = ClampF( s_tessCell * 0.5f, 4.0f, 4096.0f ); changed = true; }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x2" ) ) { s_tessCell = ClampF( s_tessCell * 2.0f, 4.0f, 4096.0f ); changed = true; }
            {
                // what the selected sheets would become, so the number is never a guess
                int patches = 0, chunks = 0, tris = 0, nowTris = 0;
                ForEachNode( false, [&]( selbrush_t *b )
                {
                    if ( !PatchEligible( b ) || !GridIsSheet( b->patch->def ) )
                        return;
                    const kterTessPlan_t p = TessPlan( b->def->maxs[0] - b->def->mins[0],
                                                       b->def->maxs[1] - b->def->mins[1], s_tessCell );
                    ++patches;
                    chunks  += p.kx * p.ky;
                    tris    += p.cellsX * p.cellsY * 2;
                    nowTris += ( b->patch->def->width - 1 ) * ( b->patch->def->height - 1 ) * 2;
                } );
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
        }

        ImGui::PopItemWidth();
        ImGui::PopTextWrapPos();
        if ( table )
            ImGui::EndTable();

        if ( changed )
        {
            Save();
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
static bool BeginStroke( bool shift, bool ctrl )
{
    s_modShift = shift;
    s_modCtrl  = ctrl;

    // never paint the editor's own preview copy (a saved profile can still hold one)
    if ( !_strnicmp( s_paintMaterial, "kiwi_blend_", 11 ) )
    {
        const char *real = UnwrapTwinName( s_paintMaterial );
        Sys_Printf( "Terrain Sculpt: '%s' is the editor's preview copy - painting '%s' instead.\n", s_paintMaterial, real );
        memmove( s_paintMaterial, real, strlen( real ) + 1 );
        Save();
    }

    if ( ctrl && !shift && s_tool == KTER_SETHEIGHT )
    {
        s_targetZ = s_cursor[2];
        Save();
        SetStatus( "Armed. Target height picked: %.1f", s_targetZ );
        return false;
    }

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
    Patch_Paint( &active_brushes );          // shared seam points may move unselected neighbours
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
    s_holeSaid      = false;
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

// The armed tool's keys for the camera's hint strip (Set height also shows its live target).
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

    if ( !UpdateCursor( imgX, imgY ) )
    {
        SetStatus( "Armed. No patch under the cursor." );
        return false;
    }
    return BeginStroke( shift, ctrl );
}

void KiwiTerrain_HandleDrag( int imgX, int imgY )
{
    if ( s_armed && s_tool == KTER_GRASS )
    {
        KiwiGrass_HandleDrag( imgX, imgY );
        return;
    }
    if ( !s_stroke || !UpdateCursor( imgX, imgY ) )
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
    selbrush_t *node = nullptr;
    if ( PickPatches( ray.origin, ray.dir, true, hit, nullptr, &node ) && node )
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
        SetPaintMaterial( slot >= 0 ? def->kiwiLayer[slot] : BaseMaterialName( def ) );
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
            SetPaintMaterial( q->name );
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
    // Alt+wheel: Set height's target, every other tool's strength.
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
    UpdateCursor( imgX, imgY );
    s_expandCellCount = PreviewCells();
}

// Reference images draw after a depth clear, over everything, and hid the red weight run.
// While the weight view is on and pictures exist, the red runs are re-emitted after them
// (KiwiHover_DrawWorld calls this right after KiwiRefImage_DrawWorld).
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
    ForEachNode( true, [&]( selbrush_t *b )
    {
        if ( !PatchEligible( b ) )
            return;
        const patch_t *inst = b->patch;
        if ( !inst->visArray || inst->vertCount <= 0 || !inst->indicesFront )
            return;
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
    } );
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
    const float box[4] = { s_cursor[0] - pad, s_cursor[1] - pad, s_cursor[0] + pad, s_cursor[1] + pad };
    if ( !BoundsMeet( b->def->mins, b->def->maxs, box, 0.0f ) )
        return false;
    const curvePatchDef_t *mesh = b->patch->def->curveDef;
    return mesh && mesh->width > 1 && mesh->height > 1 && mesh->verts;
}

// The armed-tool wireframe: the patches' render grids, only within outer radius x "Wire
// reach" of the cursor; stroke targets white, the rest grey.  Over the line budget (16k
// segments) the whole grid coarsens uniformly (every 2nd / 4th / 8th line) - never a hole.
static void DrawWireframeAoE()
{
    if ( !s_armed || s_tool == KTER_GRASS || s_hideWire || !s_cursorHave )
        return;
    const float reach  = s_outer * s_wireReach;
    const float pad    = s_shape == KTER_SQUARE ? reach * 1.42f : reach;
    const int   budget = 16384;

    // Pass 1: how many segments would the full grid take (cells inside the reach square)?
    int estimate = 0;
    ForEachNode( true, [&]( selbrush_t *b )
    {
        if ( !WirePatchInReach( b, pad ) )
            return;
        const curvePatchDef_t *mesh = b->patch->def->curveDef;
        const float *mins = b->def->mins, *maxs = b->def->maxs;
        float frac[2];                                  // fraction of the patch inside the square, per axis
        for ( int a = 0; a < 2; ++a )
        {
            const float lo = mins[a] > s_cursor[a] - pad ? mins[a] : s_cursor[a] - pad;
            const float hi = maxs[a] < s_cursor[a] + pad ? maxs[a] : s_cursor[a] + pad;
            frac[a] = maxs[a] - mins[a] > 1.0f ? ClampF( ( hi - lo ) / ( maxs[a] - mins[a] ), 0.0f, 1.0f ) : 1.0f;
        }
        const float cells = (float)( ( mesh->width - 1 ) * ( mesh->height - 1 ) ) * frac[0] * frac[1];
        estimate += (int)( cells * 3.0f ) + mesh->width + mesh->height;
    } );
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

    // Set height's target: a magenta line, marker on the RIGHT (the cursor's is on the left);
    // outside the range it pins to that end and says which way it lies.
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
        PatchEditBegin( "join terrain" );
        PatchEditMark( a );
        MergeInto( a, nb->patch->def, axis );
        RecordForDelete( std::vector<selbrush_t *>( 1, nb ) );
        DeleteRecorded();
        PatchEditEnd( true );

        sheets.erase( sheets.begin() + ib );
        for ( size_t i = 0; i < sheets.size(); ++i )
            Select_Brush( sheets[i], 0, 0, 0 );
        ++joins;
    }
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
    else if ( !_stricmp( key, "layerequal" ) )  { s_layerDepthEqual = value != 0.0f; RestripTwins(); }
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
    ForEachNode( true, [&]( selbrush_t *b ) { n += b->patch ? 1 : 0; } );
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
        s_paintBase = true;
    else
        SetPaintMaterial( name );
    Save();
    return true;
}

void KiwiTerrain_TestArm( bool armed )
{
    Load();
    SetArmed( armed );
}

bool KiwiTerrain_TestHeightAt( float x, float y, float *outZ )
{
    Load();
    const float org[3] = { x, y, 65536.0f };
    const float dir[3] = { 0.0f, 0.0f, -1.0f };
    float hit[3];
    if ( !PickPatches( org, dir, true, hit, nullptr ) )
        return false;
    *outZ = hit[2];
    return true;
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
    if ( !ResolveCursor( ray, hit ) )
    {
        ClearCursor();
        SetStatus( "Armed. No patch under the cursor." );
        return false;
    }
    s_cursorHave = true;
    memcpy( s_cursor, hit, sizeof( hit ) );
    RebuildRing();
    if ( !BeginStroke( shift, ctrl ) )
        return false;
    if ( s_stroke && s_tool != KTER_TRIM && seconds > 0.0f )
    {
        s_accumDt = seconds;
        ApplyStroke();
    }
    EndStroke();
    return true;
}
