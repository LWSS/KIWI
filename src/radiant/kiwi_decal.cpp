#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Decals — see kiwi_decal.h.  Placement is a click on a brush face while the tool is
// armed; editing works on the selected 3x3 planar patch.  Every mutation goes through
// the legacy undo bracket so Ctrl+Z restores the patch like any other edit.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

// Same include order as imgui_panel_lyrmtl.cpp — the colormap thumbnail walks a
// Material's textureTable down to its GfxImage and hands the D3D texture to ImGui.
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_gfx.h>
#include <d3d9.h>

#include "kiwi_command.h"
#include "kiwi_decal.h"
#include "kiwi_fmt.h"
#include "kiwi_import.h"        // KiwiImport_BrowseDecals (the decal importer)
#include "kiwi_lines.h"
#include "kiwi_material.h"
#include "kiwi_matwriter.h"     // KiwiMat_ReadSource (decals-only list, unloaded materials)
#include "kiwi_pick.h"
#include "kiwi_windows.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

extern int          Sys_Printf( const char *fmt, ... );
extern int          g_nUpdateBits;
extern entity_s    *world_entity;
extern selbrush_t   selected_brushes;
extern selbrush_t   active_brushes;
extern void         Select_Deselect( int deselectFaces );
extern patchMesh_t *MakeNewPatch();                                            // pmesh.cpp 0x438210
extern void         SetMaterial( const char *name, patchMesh_material *out );  // materialdef.cpp:101
extern curvePatchDef_t *Patch_GenericMesh2( patchMesh_t *p, int layer, int *colMapArg, int *rowMapArg ); // pmesh.cpp:721
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );   // pmesh.cpp:841
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );          // brush.cpp:670
extern void         Brush_AddToList2( selbrush_t *b );                         // brush.cpp:928
extern void         Patch_Rebuild( patchMesh_t *p, char doBounds );            // pmesh.cpp
extern void         Patch_PaintMarkUndo( patchMesh_t *def );                   // pmesh.cpp (tail)
extern void         Undo_ClearRedo();
extern void         Undo_GeneralStart( const char *operation );
extern void         Undo_End();
extern void         Undo_EndBrushList( selbrush_t *brushlist );                // undo.cpp:576
extern void         Face_MoveTexture( const float *surfDef, const float *normal, float *outVecs,
                                      const float *uvBase, float rotate, float crossterm ); // texturevecs.cpp
extern void         Patch_KiwiEnsureLmapCoords( patchMesh_t *p );              // pmesh.cpp: layer-1 pass the patch creators share
namespace LayerMat  { int GetCurrentLayer( MaterialDef *def ); }               // materialdef.cpp 0x431B30
extern bool         Cam_MaterialIsOverlay( Material *handle );                 // camwnd.cpp (decal badge)
extern int          TexWnd_MaterialCount();                                    // texwnd.cpp (tail)
extern qtexture_s  *TexWnd_MaterialAt( int idx );
extern qtexture_s  *Texture_GetHandle( const char *name );                     // texwnd.cpp:315
extern ImGuiID      ImGuiShell_DockRoot();                                     // imgui_shell.cpp
extern void         ImGuiShell_FocusTab( const char *title );                  // imgui_shell.cpp:210
extern bool         ImGuiShell_ScreenToImGui( int screenX, int screenY, float *outX, float *outY ); // imgui_shell.cpp

namespace
{
    const char *KDEC_PROFILE = "KiwiDecal";
    const float KDEC_PI      = 3.14159265358979323846f;
    const float KDEC_LAYER_STEP = 0.125f;     // extra normal offset per layer
    enum { KDEC_NAME_CHARS = 128, KDEC_MAX_LAYER = 15 };

    // ── settings (persisted) ────────────────────────────────────────────────
    char  s_material[KDEC_NAME_CHARS] = "";
    char  s_filter[64] = "";
    bool  s_showAll  = false;    // false = the list shows decal materials only (the orange badge)
    float s_width    = 64.0f;
    float s_height   = 64.0f;
    bool  s_keepAspect = true;
    float s_rot      = 0.0f;
    float s_offset   = 0.5f;
    int   s_layer    = 0;
    float s_opacity  = 1.0f;
    float s_fade     = 0.0f;
    bool  s_flipU    = false;
    bool  s_flipV    = false;
    bool  s_randomRot = false;
    float s_sizeJitter = 0.0f;   // percent
    int   s_projection = 1;      // 0 = Flat (3x3 planar), 1 = Wrap over the patch under the cursor (default)

    // ── session ──────────────────────────────────────────────────────────────
    bool  s_loaded = false;
    bool  s_armed  = false;
    bool  s_hoverHave = false;
    float s_hoverPoint[3]  = { 0, 0, 0 };
    float s_hoverNormal[3] = { 0, 0, 1 };
    patchMesh_t *s_hoverPatch = nullptr;  // the patch under the cursor (wrap projection)
    ImVec2 s_winMin( 0, 0 ), s_winMax( 0, 0 );   // the window's rect last frame it was shown
    int    s_winFrame = -1;                       // (file drops onto it import decal stamps)
    bool  s_previewDirty = true;          // rebuild the wrap preview outline
    std::vector<float> s_previewOutline;  // xyz triples; empty = draw the flat preview
    int   s_placed = 0;
    char  s_status[160] = "Disarmed.";

    struct decalRec_t
    {
        float surface[3];   // the picked surface point (offset 0)
        float normal[3];
        float u0[3], v0[3]; // unrotated in-plane axes (v0 = normal x u0)
        float width, height, rot, offset;
        int   layer;
        float opacity, fade;
        bool  flipU, flipV;
        // KIWI (2026-09-24, user: "decals need a projection mode so I can wrap them around
        // cylinders and such (patches)"): a WRAPPED decal is laid over the curve/terrain patch
        // it was placed on (see WRAP PROJECTION below).  Its control grid follows the surface,
        // so it is not a 3x3 planar patch and Adopt() cannot rebuild the record after a reload.
        bool  wrap = false;
        patchMesh_t *target = nullptr;    // the patch it was laid on
        float anchor[3] = { 0, 0, 0 };    // its centre control point after the last layout
        bool  offSurface = false;         // part of it ran past every patch edge (laid flat)
        std::vector<float> outline;       // border samples, xyz triples (camera outline)
    };
    std::map<patchMesh_t *, decalRec_t> s_decals;

    // Live edit bracket over one selected decal.
    patchMesh_t *s_editDef  = nullptr;
    bool         s_editOpen = false;

    // ── helpers ──────────────────────────────────────────────────────────────
    float ClampF( float v, float lo, float hi )
    {
        if ( !_finite( v ) ) return lo;
        return v < lo ? lo : ( v > hi ? hi : v );
    }
    void  Cross( const float *a, const float *b, float *o )
    {
        o[0] = a[1] * b[2] - a[2] * b[1];
        o[1] = a[2] * b[0] - a[0] * b[2];
        o[2] = a[0] * b[1] - a[1] * b[0];
    }
    float Dot( const float *a, const float *b ) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float Normalize( float *v )
    {
        const float l = sqrtf( Dot( v, v ) );
        if ( l > 1e-6f ) { v[0] /= l; v[1] /= l; v[2] /= l; }
        return l;
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
        const std::string text = Radiant_ProfileGetString( KDEC_PROFILE, entry, "" );
        if ( text.empty() ) return def;
        char *end = 0;
        const double v = strtod( text.c_str(), &end );
        return ( end == text.c_str() || !_finite( v ) ) ? def : (float)v;
    }
    void WriteFloat( const char *entry, float v )
    {
        char text[64];
        KiwiFmt_Num( text, sizeof( text ), v, 6 );
        Radiant_ProfileSetString( KDEC_PROFILE, entry, text );
    }
    void CopyString( char *dst, int size, const char *src )
    {
        strncpy( dst, src ? src : "", (size_t)size - 1 );
        dst[size - 1] = '\0';
    }

    void Sanitize()
    {
        s_width   = ClampF( s_width,  1.0f, 4096.0f );
        s_height  = ClampF( s_height, 1.0f, 4096.0f );
        s_rot     = ClampF( s_rot, -360.0f, 360.0f );
        s_offset  = ClampF( s_offset, 0.0f, 64.0f );
        if ( s_layer < 0 ) s_layer = 0;
        if ( s_layer > KDEC_MAX_LAYER ) s_layer = KDEC_MAX_LAYER;
        s_opacity = ClampF( s_opacity, 0.0f, 1.0f );
        s_fade    = ClampF( s_fade, 0.0f, 1.0f );
        s_sizeJitter = ClampF( s_sizeJitter, 0.0f, 90.0f );
    }

    void Load()
    {
        if ( s_loaded ) return;
        s_loaded = true;
        CopyString( s_material, sizeof( s_material ),
                    Radiant_ProfileGetString( KDEC_PROFILE, "Material", "" ).c_str() );
        CopyString( s_filter, sizeof( s_filter ),
                    Radiant_ProfileGetString( KDEC_PROFILE, "Filter", "" ).c_str() );
        s_width   = ReadFloat( "Width", 64.0f );
        s_height  = ReadFloat( "Height", 64.0f );
        s_keepAspect = Radiant_ProfileGetInt( KDEC_PROFILE, "KeepAspect", 1 ) != 0;
        s_rot     = ReadFloat( "Rotation", 0.0f );
        s_offset  = ReadFloat( "Offset", 0.5f );
        s_layer   = Radiant_ProfileGetInt( KDEC_PROFILE, "Layer", 0 );
        s_opacity = ReadFloat( "Opacity", 1.0f );
        s_fade    = ReadFloat( "Fade", 0.0f );
        s_flipU   = Radiant_ProfileGetInt( KDEC_PROFILE, "FlipU", 0 ) != 0;
        s_flipV   = Radiant_ProfileGetInt( KDEC_PROFILE, "FlipV", 0 ) != 0;
        s_randomRot = Radiant_ProfileGetInt( KDEC_PROFILE, "RandomRot", 0 ) != 0;
        s_sizeJitter = ReadFloat( "SizeJitter", 0.0f );
        s_projection = Radiant_ProfileGetInt( KDEC_PROFILE, "Projection", 1 ) ? 1 : 0;
        s_showAll    = Radiant_ProfileGetInt( KDEC_PROFILE, "ShowAll", 0 ) != 0;
        // KIWI (2026-09-24, user: "wrap to surface should be the default" / "show only decals
        // by default"): every earlier Save() stored Projection 0 and the old "decal" name
        // filter, so profiles written before this get the new defaults once.  The name filter
        // goes because the list is now filtered by the material itself - "decal" in the name
        // would hide the *_burnt overlays and every decal named otherwise.
        if ( Radiant_ProfileGetInt( KDEC_PROFILE, "SettingsVersion", 1 ) < 2 )
        {
            s_projection = 1;
            if ( !_stricmp( s_filter, "decal" ) )
                s_filter[0] = '\0';
            Radiant_ProfileSetInt( KDEC_PROFILE, "Projection", 1 );
            Radiant_ProfileSetString( KDEC_PROFILE, "Filter", s_filter );
            Radiant_ProfileSetInt( KDEC_PROFILE, "SettingsVersion", 2 );
        }
        Sanitize();
    }

    void Save()
    {
        Sanitize();
        Radiant_ProfileSetString( KDEC_PROFILE, "Material", s_material );
        Radiant_ProfileSetString( KDEC_PROFILE, "Filter", s_filter );
        WriteFloat( "Width", s_width );
        WriteFloat( "Height", s_height );
        Radiant_ProfileSetInt( KDEC_PROFILE, "KeepAspect", s_keepAspect ? 1 : 0 );
        WriteFloat( "Rotation", s_rot );
        WriteFloat( "Offset", s_offset );
        Radiant_ProfileSetInt( KDEC_PROFILE, "Layer", s_layer );
        WriteFloat( "Opacity", s_opacity );
        WriteFloat( "Fade", s_fade );
        Radiant_ProfileSetInt( KDEC_PROFILE, "FlipU", s_flipU ? 1 : 0 );
        Radiant_ProfileSetInt( KDEC_PROFILE, "FlipV", s_flipV ? 1 : 0 );
        Radiant_ProfileSetInt( KDEC_PROFILE, "RandomRot", s_randomRot ? 1 : 0 );
        WriteFloat( "SizeJitter", s_sizeJitter );
        Radiant_ProfileSetInt( KDEC_PROFILE, "Projection", s_projection );
        Radiant_ProfileSetInt( KDEC_PROFILE, "ShowAll", s_showAll ? 1 : 0 );
        Radiant_ProfileSetInt( KDEC_PROFILE, "SettingsVersion", 2 );
        s_previewDirty = true;              // the wrap preview outline follows the settings
    }

    // ── material helpers ─────────────────────────────────────────────────────
    // The material's colormap as a D3D texture, or null (unregistered / not 2D / not
    // yet loaded).  Mirrors imgui_panel_lyrmtl.cpp's LayerColorMap minus its lazy
    // registration: the palette registers only on request, never per frame.
    IDirect3DTexture9 *ColorMap( qtexture_s *q )
    {
        if ( !q || !q->next )
            return nullptr;
        Material *mtl = q->next;
        if ( !mtl->textureTable )
            return nullptr;
        for ( int i = 0; i < (int)mtl->textureCount; ++i )
        {
            if ( mtl->textureTable[i].semantic != 2 )        // TS_COLOR_MAP
                continue;
            GfxImage *img = mtl->textureTable[i].u.image;
            if ( !img || img->delayLoadPixels )
                continue;
            if ( img->mapType == MAPTYPE_2D && img->texture.map )
                return img->texture.map;
        }
        return nullptr;
    }

    bool MaterialAspect( const char *name, float *outAspect )
    {
        if ( !name || !name[0] )
            return false;
        qtexture_s *q = Texture_GetHandle( name );
        if ( !q || q->width <= 0 || q->height <= 0 )
            return false;
        *outAspect = (float)q->height / (float)q->width;
        return true;
    }

    bool NameMatchesFilter( const char *name )
    {
        if ( !name ) return false;
        if ( !s_filter[0] ) return true;
        // case-insensitive substring
        const size_t fl = strlen( s_filter );
        for ( const char *p = name; *p; ++p )
            if ( _strnicmp( p, s_filter, fl ) == 0 )
                return true;
        return false;
    }

    // ── geometry ─────────────────────────────────────────────────────────────
    void BaseAxesFor( const float *n, float *u0, float *v0 )
    {
        float refv[3];
        if ( fabsf( n[2] ) < 0.9f ) { refv[0] = 0; refv[1] = 0; refv[2] = 1; }   // wall: u along the horizon
        else                        { refv[0] = 0; refv[1] = 1; refv[2] = 0; }   // floor/ceiling: u along +X
        Cross( refv, n, u0 );
        Normalize( u0 );
        Cross( n, u0, v0 );
        Normalize( v0 );
    }

    void Axes( const decalRec_t &r, float *U, float *V )
    {
        const float a = r.rot * KDEC_PI / 180.0f;
        const float c = cosf( a ), s = sinf( a );
        for ( int k = 0; k < 3; ++k )
        {
            U[k] =  r.u0[k] * c + r.v0[k] * s;
            V[k] = -r.u0[k] * s + r.v0[k] * c;
        }
    }

    void Corner( const decalRec_t &r, int i, int j, float *out )
    {
        float U[3], V[3];
        Axes( r, U, V );
        const float fu = ( (float)i - 1.0f ) * 0.5f * r.width;
        const float fv = ( (float)j - 1.0f ) * 0.5f * r.height;
        const float off = r.offset + (float)r.layer * KDEC_LAYER_STEP;
        for ( int k = 0; k < 3; ++k )
            out[k] = r.surface[k] + r.normal[k] * off + U[k] * fu + V[k] * fv;
    }

    // ── WRAP PROJECTION ──────────────────────────────────────────────────────
    // A wrapped decal is UNROLLED onto the surface instead of projected: from the picked
    // point, the centre column is walked along the decal's V axis over the surface, then each
    // row is walked along the transported U axis, so every sample sits at its true arc-length
    // distance.  On a cylinder, cone or any other developable patch that is exact - no stretch,
    // any rotation, all the way round.  The walk runs in the patch's own parameter space
    // (the quadratic Bezier segments the tessellator subdivides; terrain meshes bilinear per
    // cell), wraps across a closed seam, and hops onto a neighbouring patch whose edge it
    // reaches; past every patch it continues flat in the last tangent plane.
    // The samples become a (2k+1)x(2m+1) curve patch whose surface passes through all of
    // them (odd control points = 2*mid - (prev+next)/2), k/m from how far the surface turns
    // (one segment per ~20 degrees, 7 max = the 15-point cap on odd curve grids).
    const float KDEC_WRAP_SEG_DEG = 20.0f;
    const int   KDEC_WRAP_MAX_SEG = 7;
    const float KDEC_WRAP_MIN_OFFSET = 0.3f;   // > the ~0.25 chord sag at subDivType 1 (FillWrap)

    struct surfPos_t
    {
        const patchMesh_t *p = nullptr;
        float s = 0.0f, t = 0.0f;       // control-index units: [0, width-1] x [0, height-1]
        float sign = 1.0f;              // orients Ps x Pt to the decal's side
    };

    struct wrapCtx_t
    {
        std::vector<const patchMesh_t *> hops;   // patches the walk may continue onto
        std::vector<float> hopBounds;             // 6 per hop: mins, maxs (control hull)
        float step = 1.0f;                        // integration step (world units)
        bool  offSurface = false;
    };

    bool DefLive( patchMesh_t *def, selbrush_t **outNode );   // below

    struct walkState_t
    {
        surfPos_t pos;
        bool  flat = false;             // ran past every patch: straight in the last tangent plane
        float P[3] = { 0, 0, 0 };
        float N[3] = { 0, 0, 1 };
    };

    bool PatchIsCurve( const patchMesh_t *p )
    {
        return ( p->type & PATCH_TERRAIN ) == 0 && ( p->width & 1 ) && ( p->height & 1 );
    }

    bool PatchUsable( const patchMesh_t *p )
    {
        return p && p->width >= 2 && p->height >= 2
            && p->width <= KIWI_PATCH_MAX_DIM && p->height <= KIWI_PATCH_MAX_DIM;
    }

    void PatchEval( const patchMesh_t *p, float s, float t, float P[3], float Ps[3], float Pt[3] )
    {
        for ( int k = 0; k < 3; ++k )
            P[k] = Ps[k] = Pt[k] = 0.0f;
        if ( PatchIsCurve( p ) && p->width >= 3 && p->height >= 3 )
        {
            const int segS = ( p->width - 1 ) / 2, segT = ( p->height - 1 ) / 2;
            int is = (int)floorf( s * 0.5f ), it = (int)floorf( t * 0.5f );
            if ( is < 0 ) is = 0;
            if ( is > segS - 1 ) is = segS - 1;
            if ( it < 0 ) it = 0;
            if ( it > segT - 1 ) it = segT - 1;
            const float u = ( s - 2.0f * (float)is ) * 0.5f, v = ( t - 2.0f * (float)it ) * 0.5f;
            const float bu[3] = { ( 1 - u ) * ( 1 - u ), 2 * u * ( 1 - u ), u * u };
            const float bv[3] = { ( 1 - v ) * ( 1 - v ), 2 * v * ( 1 - v ), v * v };
            const float du[3] = { -( 1 - u ), 1 - 2 * u, u };    // d/ds = (d/du) / 2
            const float dv[3] = { -( 1 - v ), 1 - 2 * v, v };
            for ( int a = 0; a < 3; ++a )
                for ( int b = 0; b < 3; ++b )
                {
                    const float *c = p->ctrl[2 * is + a][2 * it + b].xyz;
                    for ( int k = 0; k < 3; ++k )
                    {
                        P[k]  += bu[a] * bv[b] * c[k];
                        Ps[k] += du[a] * bv[b] * c[k];
                        Pt[k] += bu[a] * dv[b] * c[k];
                    }
                }
            return;
        }
        int i = (int)floorf( s ), j = (int)floorf( t );
        if ( i < 0 ) i = 0;
        if ( i > p->width - 2 ) i = p->width - 2;
        if ( j < 0 ) j = 0;
        if ( j > p->height - 2 ) j = p->height - 2;
        const float u = s - (float)i, v = t - (float)j;
        const float *c00 = p->ctrl[i][j].xyz,     *c10 = p->ctrl[i + 1][j].xyz;
        const float *c01 = p->ctrl[i][j + 1].xyz, *c11 = p->ctrl[i + 1][j + 1].xyz;
        for ( int k = 0; k < 3; ++k )
        {
            P[k]  = ( 1 - u ) * ( 1 - v ) * c00[k] + u * ( 1 - v ) * c10[k]
                  + ( 1 - u ) * v * c01[k] + u * v * c11[k];
            Ps[k] = ( 1 - v ) * ( c10[k] - c00[k] ) + v * ( c11[k] - c01[k] );
            Pt[k] = ( 1 - u ) * ( c01[k] - c00[k] ) + u * ( c11[k] - c10[k] );
        }
    }

    // Oriented unit normal at a surface position; false where the patch is degenerate (poles).
    bool PosNormal( const surfPos_t &pos, float P[3], float N[3], float Ps[3] = nullptr, float Pt[3] = nullptr )
    {
        float ps[3], pt[3];
        PatchEval( pos.p, pos.s, pos.t, P, ps, pt );
        if ( Ps ) memcpy( Ps, ps, sizeof( ps ) );
        if ( Pt ) memcpy( Pt, pt, sizeof( pt ) );
        Cross( ps, pt, N );
        if ( Normalize( N ) < 1e-6f )
            return false;
        for ( int k = 0; k < 3; ++k )
            N[k] *= pos.sign;
        return true;
    }

    bool ClosedS( const patchMesh_t *p )
    {
        for ( int j = 0; j < p->height; ++j )
        {
            const float *a = p->ctrl[0][j].xyz, *b = p->ctrl[p->width - 1][j].xyz;
            if ( fabsf( a[0] - b[0] ) > 0.5f || fabsf( a[1] - b[1] ) > 0.5f || fabsf( a[2] - b[2] ) > 0.5f )
                return false;
        }
        return true;
    }
    bool ClosedT( const patchMesh_t *p )
    {
        for ( int i = 0; i < p->width; ++i )
        {
            const float *a = p->ctrl[i][0].xyz, *b = p->ctrl[i][p->height - 1].xyz;
            if ( fabsf( a[0] - b[0] ) > 0.5f || fabsf( a[1] - b[1] ) > 0.5f || fabsf( a[2] - b[2] ) > 0.5f )
                return false;
        }
        return true;
    }

    // Nearest surface parameter to X: a coarse grid (whole patch, or its four edges only),
    // then Newton steps on the tangent plane.  Returns the distance left.
    float FindST( const patchMesh_t *p, const float X[3], bool edgesOnly, float *outS, float *outT )
    {
        const float smax = (float)( p->width - 1 ), tmax = (float)( p->height - 1 );
        const int   per  = edgesOnly ? 8 : 4;             // samples per control step
        const int   ns   = p->width * per, nt = p->height * per;
        float bestD = FLT_MAX, bs = 0.0f, bt = 0.0f;
        for ( int a = 0; a <= ns; ++a )
        {
            const float s = smax * (float)a / (float)ns;
            for ( int b = 0; b <= nt; ++b )
            {
                if ( edgesOnly && a != 0 && a != ns && b != 0 && b != nt )
                    continue;
                const float t = tmax * (float)b / (float)nt;
                float P[3], Ps[3], Pt[3];
                PatchEval( p, s, t, P, Ps, Pt );
                const float d[3] = { X[0] - P[0], X[1] - P[1], X[2] - P[2] };
                const float dd = Dot( d, d );
                if ( dd < bestD ) { bestD = dd; bs = s; bt = t; }
            }
        }
        for ( int it = 0; it < 8; ++it )
        {
            float P[3], Ps[3], Pt[3];
            PatchEval( p, bs, bt, P, Ps, Pt );
            const float r[3] = { X[0] - P[0], X[1] - P[1], X[2] - P[2] };
            const float a = Dot( Ps, Ps ), b = Dot( Ps, Pt ), c = Dot( Pt, Pt );
            const float det = a * c - b * b;
            if ( fabsf( det ) < 1e-9f )
                break;
            const float r1 = Dot( Ps, r ), r2 = Dot( Pt, r );
            bs = ClampF( bs + ( c * r1 - b * r2 ) / det, 0.0f, smax );
            bt = ClampF( bt + ( a * r2 - b * r1 ) / det, 0.0f, tmax );
        }
        float P[3], Ps[3], Pt[3];
        PatchEval( p, bs, bt, P, Ps, Pt );
        const float d[3] = { X[0] - P[0], X[1] - P[1], X[2] - P[2] };
        *outS = bs;
        *outT = bt;
        return sqrtf( Dot( d, d ) );
    }

    // The walk reached X on the edge of its patch: continue on another patch whose edge
    // passes within 1 unit, keeping the normal on the same side.
    bool Hop( walkState_t &w, const float X[3], const float N[3], wrapCtx_t &ctx )
    {
        for ( size_t i = 0; i < ctx.hops.size(); ++i )
        {
            const patchMesh_t *q = ctx.hops[i];
            if ( q == w.pos.p )
                continue;
            const float *bb = &ctx.hopBounds[i * 6];
            if ( X[0] < bb[0] - 1.0f || X[1] < bb[1] - 1.0f || X[2] < bb[2] - 1.0f
              || X[0] > bb[3] + 1.0f || X[1] > bb[4] + 1.0f || X[2] > bb[5] + 1.0f )
                continue;
            float s, t;
            if ( FindST( q, X, true, &s, &t ) > 1.0f )
                continue;
            surfPos_t np;
            np.p = q; np.s = s; np.t = t; np.sign = 1.0f;
            float P[3], N2[3];
            if ( !PosNormal( np, P, N2 ) )
                continue;
            if ( Dot( N2, N ) < 0.0f )
                np.sign = -1.0f;
            w.pos = np;
            return true;
        }
        return false;
    }

    // Advance by arc length `dist` in the tangent direction `dir` (kept tangent in place).
    void Advance( walkState_t &w, float dir[3], float dist, wrapCtx_t &ctx )
    {
        float left = dist;
        int guard = 0, stuck = 0;
        while ( left > 1e-4f && ++guard < 20000 )
        {
            // Two edge stops in a row without moving = the hop led nowhere (a neighbour the
            // direction leaves at once): finish the distance flat instead of ping-ponging.
            if ( stuck > 2 && !w.flat )
            {
                w.flat = true;
                ctx.offSurface = true;
            }
            const float d = left < ctx.step ? left : ctx.step;
            if ( w.flat )
            {
                for ( int k = 0; k < 3; ++k ) w.P[k] += dir[k] * d;
                left -= d;
                continue;
            }
            float P[3], N[3], Ps[3], Pt[3];
            if ( !PosNormal( w.pos, P, N, Ps, Pt ) )
            {
                w.flat = true;                  // a pole: continue flat from here
                ctx.offSurface = true;
                memcpy( w.P, P, sizeof( P ) );
                continue;
            }
            memcpy( w.P, P, sizeof( P ) );
            memcpy( w.N, N, sizeof( N ) );
            const float dn = Dot( dir, N );
            float td[3] = { dir[0] - N[0] * dn, dir[1] - N[1] * dn, dir[2] - N[2] * dn };
            if ( Normalize( td ) > 1e-6f )
                memcpy( dir, td, sizeof( td ) );

            const float a = Dot( Ps, Ps ), b = Dot( Ps, Pt ), c = Dot( Pt, Pt );
            const float det = a * c - b * b;
            if ( fabsf( det ) < 1e-9f )
            {
                w.flat = true;
                ctx.offSurface = true;
                continue;
            }
            const float r1 = Dot( Ps, dir ) * d, r2 = Dot( Pt, dir ) * d;
            const float ds = ( c * r1 - b * r2 ) / det, dt = ( a * r2 - b * r1 ) / det;
            const patchMesh_t *p = w.pos.p;
            const float smax = (float)( p->width - 1 ), tmax = (float)( p->height - 1 );
            float ns = w.pos.s + ds, nt = w.pos.t + dt;
            if ( ClosedS( p ) && smax > 0.0f )
                while ( ns < 0.0f || ns > smax ) ns += ns < 0.0f ? smax : -smax;
            if ( ClosedT( p ) && tmax > 0.0f )
                while ( nt < 0.0f || nt > tmax ) nt += nt < 0.0f ? tmax : -tmax;
            if ( ns >= 0.0f && ns <= smax && nt >= 0.0f && nt <= tmax )
            {
                w.pos.s = ns;
                w.pos.t = nt;
                left -= d;
                stuck = 0;
                continue;
            }
            // Leaving the patch: stop on its edge, then hop or go flat.
            float f = 1.0f;
            if ( ns < 0.0f && ds < 0.0f )  f = (std::min)( f, -w.pos.s / ds );
            if ( ns > smax && ds > 0.0f )  f = (std::min)( f, ( smax - w.pos.s ) / ds );
            if ( nt < 0.0f && dt < 0.0f )  f = (std::min)( f, -w.pos.t / dt );
            if ( nt > tmax && dt > 0.0f )  f = (std::min)( f, ( tmax - w.pos.t ) / dt );
            f = ClampF( f, 0.0f, 1.0f );
            w.pos.s = ClampF( w.pos.s + ds * f, 0.0f, smax );
            w.pos.t = ClampF( w.pos.t + dt * f, 0.0f, tmax );
            left -= d * f;
            stuck = ( d * f < 1e-3f ) ? stuck + 1 : 0;
            float X[3], NX[3];
            if ( !PosNormal( w.pos, X, NX ) )
                memcpy( NX, N, sizeof( N ) );
            if ( Hop( w, X, NX, ctx ) )
                continue;
            w.flat = true;
            ctx.offSurface = true;
            memcpy( w.P, X, sizeof( X ) );
            memcpy( w.N, NX, sizeof( NX ) );
        }
        if ( !w.flat )
        {
            float P[3], N[3];
            if ( PosNormal( w.pos, P, N ) )
            {
                memcpy( w.P, P, sizeof( P ) );
                memcpy( w.N, N, sizeof( N ) );
            }
        }
    }

    // Keep `a` in the tangent plane and perpendicular to `walk`.
    void Transport( float a[3], const float N[3], const float walk[3] )
    {
        const float dn = Dot( a, N );
        for ( int k = 0; k < 3; ++k ) a[k] -= N[k] * dn;
        const float dw = Dot( a, walk );
        for ( int k = 0; k < 3; ++k ) a[k] -= walk[k] * dw;
        Normalize( a );
    }

    void PatchHull( const patchMesh_t *p, float lo[3], float hi[3] )
    {
        for ( int k = 0; k < 3; ++k ) { lo[k] = FLT_MAX; hi[k] = -FLT_MAX; }
        for ( int i = 0; i < p->width; ++i )
            for ( int j = 0; j < p->height; ++j )
                for ( int k = 0; k < 3; ++k )
                {
                    const float v = p->ctrl[i][j].xyz[k];
                    if ( v < lo[k] ) lo[k] = v;
                    if ( v > hi[k] ) hi[k] = v;
                }
    }

    void AddHop( wrapCtx_t &ctx, const patchMesh_t *p, const float lo[3], const float hi[3] )
    {
        ctx.hops.push_back( p );
        ctx.hopBounds.insert( ctx.hopBounds.end(), lo, lo + 3 );
        ctx.hopBounds.insert( ctx.hopBounds.end(), hi, hi + 3 );
    }

    // The start patch, then every visible patch near the decal the walk may step onto.
    // Decal/overlay patches are left out: a wrap never climbs onto another decal.
    void GatherHops( const decalRec_t &r, const patchMesh_t *self, wrapCtx_t &ctx )
    {
        float lo[3], hi[3];
        PatchHull( r.target, lo, hi );
        AddHop( ctx, r.target, lo, hi );
        const float reach = 0.5f * sqrtf( r.width * r.width + r.height * r.height ) + 16.0f;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &active_brushes : &selected_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !b->patch || !b->patch->def || b->patch->def == self || b->patch->def == r.target )
                    continue;
                const patchMesh_t *p = b->patch->def;
                if ( !PatchUsable( p ) || ( b->owner && b->owner->prefab ) || !Pick_BrushPickable( b ) )
                    continue;
                if ( p->texture.radMtl && p->texture.radMtl->next
                  && Cam_MaterialIsOverlay( p->texture.radMtl->next ) )
                    continue;
                PatchHull( p, lo, hi );
                bool near_ = true;
                for ( int k = 0; k < 3 && near_; ++k )
                    near_ = r.surface[k] >= lo[k] - reach && r.surface[k] <= hi[k] + reach;
                if ( near_ )
                    AddHop( ctx, p, lo, hi );
            }
        }
    }

    // Walk the full sample grid (cols x rows, both odd).  xyz/nrm: cols*rows triples,
    // index (i + j * cols); points ON the surface (offset not applied).
    bool WalkGrid( const decalRec_t &r, int cols, int rows, wrapCtx_t &ctx,
                   std::vector<float> &xyz, std::vector<float> &nrm )
    {
        walkState_t start;
        start.pos.p = r.target;
        FindST( r.target, r.surface, false, &start.pos.s, &start.pos.t );
        float P0[3], N0[3];
        if ( !PosNormal( start.pos, P0, N0 ) )
            return false;
        if ( Dot( N0, r.normal ) < 0.0f )
        {
            start.pos.sign = -1.0f;
            for ( int k = 0; k < 3; ++k ) N0[k] = -N0[k];
        }
        memcpy( start.P, P0, sizeof( P0 ) );
        memcpy( start.N, N0, sizeof( N0 ) );

        float U[3], V[3];
        Axes( r, U, V );
        {
            const float un = Dot( U, N0 );
            for ( int k = 0; k < 3; ++k ) U[k] -= N0[k] * un;
            if ( Normalize( U ) < 1e-6f )
                return false;
            Cross( N0, U, V );
            Normalize( V );
        }

        xyz.assign( (size_t)cols * rows * 3, 0.0f );
        nrm.assign( (size_t)cols * rows * 3, 0.0f );
        const int midC = cols / 2, midR = rows / 2;
        const float du = r.width / (float)( cols - 1 ), dv = r.height / (float)( rows - 1 );

        std::vector<walkState_t> colState( (size_t)rows );
        std::vector<float>       colU( (size_t)rows * 3 );
        colState[midR] = start;
        memcpy( &colU[midR * 3], U, sizeof( U ) );
        for ( int dirSign = -1; dirSign <= 1; dirSign += 2 )
        {
            walkState_t w = start;
            float walk[3] = { V[0] * dirSign, V[1] * dirSign, V[2] * dirSign };
            float u[3] = { U[0], U[1], U[2] };
            for ( int j = midR + dirSign; j >= 0 && j < rows; j += dirSign )
            {
                Advance( w, walk, dv, ctx );
                Transport( u, w.N, walk );
                colState[j] = w;
                memcpy( &colU[j * 3], u, sizeof( u ) );
            }
        }

        for ( int j = 0; j < rows; ++j )
        {
            const walkState_t &c = colState[j];
            float *o = &xyz[( midC + j * cols ) * 3];
            float *n = &nrm[( midC + j * cols ) * 3];
            memcpy( o, c.P, sizeof( c.P ) );
            memcpy( n, c.N, sizeof( c.N ) );
            for ( int dirSign = -1; dirSign <= 1; dirSign += 2 )
            {
                walkState_t w = c;
                float walk[3] = { colU[j * 3] * dirSign, colU[j * 3 + 1] * dirSign, colU[j * 3 + 2] * dirSign };
                for ( int i = midC + dirSign; i >= 0 && i < cols; i += dirSign )
                {
                    Advance( w, walk, du, ctx );
                    memcpy( &xyz[( i + j * cols ) * 3], w.P, sizeof( w.P ) );
                    memcpy( &nrm[( i + j * cols ) * 3], w.N, sizeof( w.N ) );
                }
            }
        }
        return true;
    }

    // Total turning of the surface normal along the centre row / column of a fine grid.
    void MeasureTurn( const std::vector<float> &nrm, int cols, int rows, float *turnU, float *turnV )
    {
        *turnU = *turnV = 0.0f;
        const int midC = cols / 2, midR = rows / 2;
        for ( int i = 1; i < cols; ++i )
        {
            const float d = Dot( &nrm[( i - 1 + midR * cols ) * 3], &nrm[( i + midR * cols ) * 3] );
            *turnU += acosf( ClampF( d, -1.0f, 1.0f ) );
        }
        for ( int j = 1; j < rows; ++j )
        {
            const float d = Dot( &nrm[( midC + ( j - 1 ) * cols ) * 3], &nrm[( midC + j * cols ) * 3] );
            *turnV += acosf( ClampF( d, -1.0f, 1.0f ) );
        }
    }

    // Lay the wrapped decal's control grid into `p` (dims, xyz, texcoords, alpha) and the
    // camera outline into `r`.  False = no usable surface (the caller falls back to flat).
    bool FillWrap( patchMesh_t *p, decalRec_t &r, const patchMesh_t *self )
    {
        if ( !r.target || !PatchUsable( r.target ) )
            return false;
        wrapCtx_t ctx;
        ctx.step = ClampF( (std::min)( r.width, r.height ) / 48.0f, 0.25f, 4.0f );
        GatherHops( r, self, ctx );

        // Pass 1: a fine grid measures how far the surface turns; pass 2 lays the real one.
        std::vector<float> xyz, nrm;
        if ( !WalkGrid( r, 17, 17, ctx, xyz, nrm ) )
            return false;
        float turnU, turnV;
        MeasureTurn( nrm, 17, 17, &turnU, &turnV );
        const float segRad = KDEC_WRAP_SEG_DEG * KDEC_PI / 180.0f;
        int k = (int)ceilf( turnU / segRad ), m = (int)ceilf( turnV / segRad );
        k = k < 1 ? 1 : ( k > KDEC_WRAP_MAX_SEG ? KDEC_WRAP_MAX_SEG : k );
        m = m < 1 ? 1 : ( m > KDEC_WRAP_MAX_SEG ? KDEC_WRAP_MAX_SEG : m );
        const int cols = 2 * k + 1, rows = 2 * m + 1;
        ctx.offSurface = false;
        if ( !WalkGrid( r, cols, rows, ctx, xyz, nrm ) )
            return false;
        r.offSurface = ctx.offSurface;

        // KIWI (2026-09-24, user: "when projecting decals over a curved surface (cylinder), it
        // has some nasty gaps"): the gaps were TESSELLATION, not the walk.  A curve patch is
        // subdivided until each segment's flatness (the curve's distance from its chord) is
        // under subDivType world units - Curve_SubdivideCols/Rows in the editor, SubdivideMesh
        // with the same patch value in cod4map - and MakeNewPatch seeds 8.  A 20-degree
        // segment on a 256-unit cylinder is only 3.9 off its chord, so it stayed two straight
        // chords sagging ~1 unit: deeper than the 0.5 offset, and the cylinder's own vertex
        // columns (which sit ON the surface) poked through as evenly spaced stripes.  At the
        // minimum tolerance of 1 a rendered chord sags at most ~0.25, so the decal stays above
        // the true surface - and the target's rendered facets lie inside it - as long as it
        // is lifted a little more than that: wrapped decals sit at least 0.3 off.
        const float off = (std::max)( r.offset, KDEC_WRAP_MIN_OFFSET ) + (float)r.layer * KDEC_LAYER_STEP;
        p->subDivType = 1;
        std::vector<float> ctl( xyz.size() );
        for ( size_t n = 0; n < xyz.size(); ++n )
            ctl[n] = xyz[n] + nrm[n] * off;

        // Outline: the border samples, in order round the grid.
        r.outline.clear();
        for ( int i = 0; i < cols; ++i )         r.outline.insert( r.outline.end(), &ctl[( i ) * 3], &ctl[( i ) * 3] + 3 );
        for ( int j = 1; j < rows; ++j )         r.outline.insert( r.outline.end(), &ctl[( cols - 1 + j * cols ) * 3], &ctl[( cols - 1 + j * cols ) * 3] + 3 );
        for ( int i = cols - 2; i >= 0; --i )    r.outline.insert( r.outline.end(), &ctl[( i + ( rows - 1 ) * cols ) * 3], &ctl[( i + ( rows - 1 ) * cols ) * 3] + 3 );
        for ( int j = rows - 2; j >= 0; --j )    r.outline.insert( r.outline.end(), &ctl[( j * cols ) * 3], &ctl[( j * cols ) * 3] + 3 );

        // Samples -> Bezier control points, both directions (B^-1 per 3-point segment).
        std::vector<float> tmp = ctl;
        for ( int j = 0; j < rows; ++j )
            for ( int i = 1; i < cols; i += 2 )
                for ( int c = 0; c < 3; ++c )
                    tmp[( i + j * cols ) * 3 + c] = 2.0f * ctl[( i + j * cols ) * 3 + c]
                        - 0.5f * ( ctl[( i - 1 + j * cols ) * 3 + c] + ctl[( i + 1 + j * cols ) * 3 + c] );
        ctl = tmp;
        for ( int j = 1; j < rows; j += 2 )
            for ( int i = 0; i < cols; ++i )
                for ( int c = 0; c < 3; ++c )
                    ctl[( i + j * cols ) * 3 + c] = 2.0f * tmp[( i + j * cols ) * 3 + c]
                        - 0.5f * ( tmp[( i + ( j - 1 ) * cols ) * 3 + c] + tmp[( i + ( j + 1 ) * cols ) * 3 + c] );

        // Alpha: full at the centre, (1 - fade) on the border, through the same B^-1.
        std::vector<float> alpha( (size_t)cols * rows ), atmp;
        for ( int j = 0; j < rows; ++j )
            for ( int i = 0; i < cols; ++i )
            {
                const float eu = fabsf( 2.0f * (float)i / (float)( cols - 1 ) - 1.0f );
                const float ev = fabsf( 2.0f * (float)j / (float)( rows - 1 ) - 1.0f );
                alpha[i + j * cols] = r.opacity * 255.0f * ( 1.0f - r.fade * (std::max)( eu, ev ) );
            }
        atmp = alpha;
        for ( int j = 0; j < rows; ++j )
            for ( int i = 1; i < cols; i += 2 )
                atmp[i + j * cols] = 2.0f * alpha[i + j * cols] - 0.5f * ( alpha[i - 1 + j * cols] + alpha[i + 1 + j * cols] );
        alpha = atmp;
        for ( int j = 1; j < rows; j += 2 )
            for ( int i = 0; i < cols; ++i )
                alpha[i + j * cols] = 2.0f * atmp[i + j * cols] - 0.5f * ( atmp[i + ( j - 1 ) * cols] + atmp[i + ( j + 1 ) * cols] );

        p->width  = cols;
        p->height = rows;
        for ( int i = 0; i < cols; ++i )
            for ( int j = 0; j < rows; ++j )
            {
                drawVert_t *cp = &p->ctrl[i][j];
                memcpy( cp->xyz, &ctl[( i + j * cols ) * 3], sizeof( float ) * 3 );
                const float fs = (float)i / (float)( cols - 1 ), ft = (float)j / (float)( rows - 1 );
                const float s = r.flipU ? 1.0f - fs : fs;
                const float t = r.flipV ? 1.0f - ft : ft;
                cp->texCoord.st[0]       = s;
                cp->texCoord.st[1]       = -t;
                cp->texCoord.lightmap[0] = s;
                cp->texCoord.lightmap[1] = -t;
                cp->savedTexCoord        = cp->texCoord;
                *(unsigned int *)&cp->vert_color = 0xFFFFFFFFu;   // grid slots past the old dims were never set
                ( (byte *)&cp->vert_color )[3] = (byte)(int)( ClampF( alpha[i + j * cols], 0.0f, 255.0f ) + 0.5f );
            }
        memcpy( r.anchor, p->ctrl[cols / 2][rows / 2].xyz, sizeof( r.anchor ) );
        return true;
    }

    // The 3x3 planar grid of a flat decal.
    void FillFlat( patchMesh_t *p, const decalRec_t &r )
    {
        p->width = p->height = 3;
        for ( int i = 0; i < 3; ++i )
        {
            for ( int j = 0; j < 3; ++j )
            {
                drawVert_t *cp = &p->ctrl[i][j];
                Corner( r, i, j, cp->xyz );
                const float s = r.flipU ? 1.0f - (float)i * 0.5f : (float)i * 0.5f;
                const float t = r.flipV ? 1.0f - (float)j * 0.5f : (float)j * 0.5f;
                cp->texCoord.st[0]        = s;
                cp->texCoord.st[1]        = -t;          // naturalize's sign convention
                cp->texCoord.lightmap[0]  = s;
                cp->texCoord.lightmap[1]  = -t;
                cp->savedTexCoord         = cp->texCoord;
                const bool edge = ( i == 0 || i == 2 || j == 0 || j == 2 );
                const float a = r.opacity * 255.0f * ( edge ? ( 1.0f - r.fade ) : 1.0f );
                byte *c = (byte *)&cp->vert_color;
                c[3] = (byte)(int)( ClampF( a, 0.0f, 255.0f ) + 0.5f );
            }
        }
    }

    // Write the control points, texcoords and alphas from `r`.
    void Layout( patchMesh_t *def, decalRec_t &r )
    {
        if ( r.wrap )
        {
            if ( !DefLive( r.target, nullptr ) || !FillWrap( def, r, def ) )
            {
                SetStatus( "The surface this decal is wrapped on is gone; the edit was not applied." );
                return;
            }
        }
        else
            FillFlat( def, r );
        def->bDirty = false;             // keep our ST: Patch_Rebuild re-projects dirty layer 1
        Patch_Rebuild( def, 1 );
        ++def->version;
        g_nUpdateBits = -1;
    }

    // Derive a record from an existing 3x3 planar patch (rotation folded into u0).
    bool Adopt( patchMesh_t *def, decalRec_t *out )
    {
        if ( !def || def->width != 3 || def->height != 3 )
            return false;
        const float *p00 = def->ctrl[0][0].xyz, *p20 = def->ctrl[2][0].xyz;
        const float *p02 = def->ctrl[0][2].xyz, *p11 = def->ctrl[1][1].xyz;
        float u[3], v[3], n[3];
        for ( int k = 0; k < 3; ++k ) { u[k] = p20[k] - p00[k]; v[k] = p02[k] - p00[k]; }
        Cross( u, v, n );
        if ( Normalize( n ) < 1e-4f )
            return false;
        const float d = Dot( n, p11 );
        for ( int i = 0; i < 3; ++i )
            for ( int j = 0; j < 3; ++j )
                if ( fabsf( Dot( n, def->ctrl[i][j].xyz ) - d ) > 0.5f )
                    return false;                      // not planar
        decalRec_t r;
        memcpy( r.normal, n, sizeof( n ) );
        r.width  = Normalize( u );
        r.height = Normalize( v );
        if ( r.width < 0.01f || r.height < 0.01f )
            return false;
        memcpy( r.u0, u, sizeof( u ) );
        Cross( n, u, r.v0 );
        Normalize( r.v0 );
        r.rot    = 0.0f;
        r.offset = s_offset;
        r.layer  = 0;
        for ( int k = 0; k < 3; ++k )
            r.surface[k] = p11[k] - n[k] * r.offset;
        const byte centreA = ( (const byte *)&def->ctrl[1][1].vert_color )[3];
        const byte edgeA   = ( (const byte *)&def->ctrl[0][0].vert_color )[3];
        r.opacity = (float)centreA / 255.0f;
        r.fade    = centreA ? 1.0f - (float)edgeA / (float)centreA : 0.0f;
        r.fade    = ClampF( r.fade, 0.0f, 1.0f );
        // Flip flags from the stored texcoords.
        r.flipU = def->ctrl[2][0].texCoord.st[0] < def->ctrl[0][0].texCoord.st[0];
        r.flipV = def->ctrl[0][2].texCoord.st[1] > def->ctrl[0][0].texCoord.st[1];
        *out = r;
        return true;
    }

    bool DefLive( patchMesh_t *def, selbrush_t **outNode )
    {
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( b->patch && b->patch->def == def )
                {
                    if ( outNode ) *outNode = b;
                    return true;
                }
        }
        return false;
    }

    // The selected decal (single selected 3x3 planar patch), adopting it on first sight
    // and re-syncing the record when the patch was moved by another tool.
    patchMesh_t *SelectedDecal( decalRec_t **outRec )
    {
        // Exactly one selected node, tested directly: QE_SingleBrush() prints
        // "you must have a single brush selected" on every miss, and this runs per frame.
        selbrush_t *b = selected_brushes.next;
        if ( !b || b == &selected_brushes || b->next != &selected_brushes )
            return nullptr;
        if ( !b->patch || !b->patch->def )
            return nullptr;
        patchMesh_t *def = b->patch->def;

        std::map<patchMesh_t *, decalRec_t>::iterator it = s_decals.find( def );
        if ( it != s_decals.end() && it->second.wrap )
        {
            // A wrapped decal cannot be re-derived from its grid: moved by another tool, it
            // is just a patch from then on.
            const float *actual = def->ctrl[def->width / 2][def->height / 2].xyz;
            const float *expect = it->second.anchor;
            if ( fabsf( expect[0] - actual[0] ) > 0.05f || fabsf( expect[1] - actual[1] ) > 0.05f
              || fabsf( expect[2] - actual[2] ) > 0.05f )
            {
                s_decals.erase( it );
                return nullptr;
            }
            if ( outRec ) *outRec = &it->second;
            return def;
        }
        if ( def->width != 3 || def->height != 3 )
            return nullptr;
        if ( it != s_decals.end() )
        {
            float expect[3];
            Corner( it->second, 1, 1, expect );
            const float *actual = def->ctrl[1][1].xyz;
            if ( fabsf( expect[0] - actual[0] ) > 0.05f || fabsf( expect[1] - actual[1] ) > 0.05f
              || fabsf( expect[2] - actual[2] ) > 0.05f )
            {
                // Moved/rotated externally: re-derive but keep the offset/layer split.
                decalRec_t fresh;
                if ( Adopt( def, &fresh ) )
                {
                    fresh.offset = it->second.offset;
                    fresh.layer  = it->second.layer;
                    const float off = fresh.offset + (float)fresh.layer * KDEC_LAYER_STEP;
                    for ( int k = 0; k < 3; ++k )
                        fresh.surface[k] = def->ctrl[1][1].xyz[k] - fresh.normal[k] * off;
                    it->second = fresh;
                }
                else
                {
                    s_decals.erase( it );
                    return nullptr;
                }
            }
            if ( outRec ) *outRec = &it->second;
            return def;
        }
        decalRec_t r;
        if ( !Adopt( def, &r ) )
            return nullptr;
        s_decals[def] = r;
        if ( outRec ) *outRec = &s_decals[def];
        return def;
    }

    void PruneDead()
    {
        for ( std::map<patchMesh_t *, decalRec_t>::iterator it = s_decals.begin(); it != s_decals.end(); )
        {
            if ( !DefLive( it->first, nullptr ) )
                it = s_decals.erase( it );
            else
                ++it;
        }
    }

    // ── undo bracket for live edits ──────────────────────────────────────────
    void EditBegin( patchMesh_t *def )
    {
        if ( s_editOpen )
            return;
        Undo_ClearRedo();
        Undo_GeneralStart( "edit decal" );
        Patch_PaintMarkUndo( def );
        s_editOpen = true;
        s_editDef  = def;
    }
    void EditEnd()
    {
        if ( !s_editOpen )
            return;
        Undo_End();
        s_editOpen = false;
        s_editDef  = nullptr;
    }

    // ── placement ────────────────────────────────────────────────────────────
    bool PickFace( int imgX, int imgY, float outPoint[3], float outNormal[3],
                   patchMesh_t **outPatch = nullptr )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;
        pick_result_t r = Pick( ray, SEL_MASK_FACE | SEL_MASK_OBJECT, PICKF_NONE );
        if ( !r.valid || !r.haveNormal )
            return false;
        memcpy( outPoint, r.point, sizeof( r.point ) );
        memcpy( outNormal, r.normal, sizeof( r.normal ) );
        if ( outPatch )
        {
            // The wrap target: a patch the ray hit directly (prefab contents draw through a
            // placement transform the walk does not apply).
            selbrush_t *hb = r.item.brush;
            *outPatch = ( hb && hb->patch && hb->patch->def && !( hb->owner && hb->owner->prefab )
                       && PatchUsable( hb->patch->def ) ) ? hb->patch->def : nullptr;
        }
        // Face the camera side: a back-face hit still wants the decal on the visible side.
        if ( Dot( outNormal, ray.dir ) > 0.0f )
            for ( int k = 0; k < 3; ++k ) outNormal[k] = -outNormal[k];
        return true;
    }

    float UnitRandom() { return (float)rand() / ( (float)RAND_MAX + 1.0f ); }

    void FillRecFromSettings( decalRec_t &r, const float *point, const float *normal )
    {
        memcpy( r.surface, point, sizeof( float ) * 3 );
        memcpy( r.normal, normal, sizeof( float ) * 3 );
        BaseAxesFor( normal, r.u0, r.v0 );
        float scale = 1.0f;
        if ( s_sizeJitter > 0.0f )
            scale = 1.0f + ( UnitRandom() * 2.0f - 1.0f ) * s_sizeJitter * 0.01f;
        r.width   = s_width * scale;
        r.height  = s_height * scale;
        r.rot     = s_randomRot ? UnitRandom() * 360.0f : s_rot;
        r.offset  = s_offset;
        r.layer   = s_layer;
        r.opacity = s_opacity;
        r.fade    = s_fade;
        r.flipU   = s_flipU;
        r.flipV   = s_flipV;
    }

    bool Place( const float *point, const float *normal, patchMesh_t *surfPatch )
    {
        if ( !s_material[0] )
        {
            SetStatus( "Armed, but no material is chosen." );
            return false;
        }
        if ( !world_entity )
            return false;

        Undo_ClearRedo();
        Undo_GeneralStart( "place decal" );
        Select_Deselect( 1 );

        patchMesh_t *p = MakeNewPatch();
        p->width = p->height = 3;
        p->type  = (PATCH_TYPES)0;
        p->contents = 0;
        p->flags    = 0;
        SetMaterial( s_material, &p->texture );

        decalRec_t r;
        FillRecFromSettings( r, point, normal );
        // Layout() rebuilds; the def has no symbiot yet, so do the geometry half by hand.
        const char *wrapNote = "";
        if ( s_projection == 1 )
        {
            r.wrap   = surfPatch != nullptr;
            r.target = surfPatch;
            if ( !surfPatch )
                wrapNote = " (flat: no curve or terrain patch under the cursor)";
            else if ( !FillWrap( p, r, p ) )
            {
                r.wrap   = false;
                r.target = nullptr;
                wrapNote = " (flat: the patch could not be walked)";
            }
            else if ( r.offSurface )
                wrapNote = " (part of it runs past the patch edge and lies flat)";
        }
        if ( !r.wrap )
            FillFlat( p, r );
        *(float *)&p->size_of_struct_0x504C = 16.0f;
        p->bDirty = false;
        p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
        ++p->version;

        brush_t    *pdef = AddBrushForPatch( p, (entity_s *)world_entity->def );
        selbrush_t *inst = Brush_AddToList( pdef, world_entity );
        Brush_AddToList2( inst );                   // selected, so the sliders edit it at once
        Undo_EndBrushList( &selected_brushes );     // stamp the new patch into the record, or Ctrl+Z has nothing to remove
        Undo_End();

        s_decals[p] = r;
        ++s_placed;
        g_nUpdateBits = -1;
        SetStatus( "Armed. Placed %i decal%s this session%s.", s_placed, s_placed == 1 ? "" : "s", wrapNote );
        return true;
    }

    // ── overlay: one patch per selected face, sharing the face's own mapping ──
    // A decal stamps one repeat of its material.  An overlay instead copies the FACE's
    // texture mapping (the texMat Face_BuildLayerGeom draws it with, brush.cpp:2598), so a
    // multiply twin such as ch_brick_wall_03_burnt lands texel-for-texel on the wall under
    // it.  A 4-corner face is covered exactly, a triangle becomes a quad with a collapsed
    // corner, and any other outline gets its bounding rectangle in the face plane.
    struct overlaySrc_t
    {
        float corner[4][3];
        float normal[3];
        float texMat[8];
        bool  exact;
    };

    bool OverlayFromFace( const face_t *f, overlaySrc_t *out )
    {
        const float *n = f->plane.normal;
        float pts[64][3];
        int   np = 0;
        const int wn = f->w->numpoints;
        for ( int k = 0; k < wn && np < 64; ++k )   // drop collinear winding points
        {
            const float *a = f->w->p[( k + wn - 1 ) % wn];
            const float *b = f->w->p[k];
            const float *c = f->w->p[( k + 1 ) % wn];
            float e0[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            float e1[3] = { c[0] - b[0], c[1] - b[1], c[2] - b[2] };
            float cr[3];
            Cross( e0, e1, cr );
            if ( sqrtf( Dot( cr, cr ) ) <= 0.001f * sqrtf( Dot( e0, e0 ) * Dot( e1, e1 ) ) )
                continue;
            memcpy( pts[np++], b, sizeof( float ) * 3 );
        }
        if ( np < 3 )
            return false;

        out->exact = ( np <= 4 );
        if ( np == 4 || np == 3 )
        {
            for ( int k = 0; k < 4; ++k )
                memcpy( out->corner[k], pts[k < np ? k : np - 1], sizeof( float ) * 3 );
        }
        else
        {
            float u[3], v[3];
            BaseAxesFor( n, u, v );
            float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
            for ( int k = 0; k < np; ++k )
            {
                float d[3] = { pts[k][0] - pts[0][0], pts[k][1] - pts[0][1], pts[k][2] - pts[0][2] };
                const float a = Dot( d, u ), b = Dot( d, v );
                if ( a < lo[0] ) lo[0] = a;
                if ( a > hi[0] ) hi[0] = a;
                if ( b < lo[1] ) lo[1] = b;
                if ( b > hi[1] ) hi[1] = b;
            }
            const float ab[4][2] = { { lo[0], lo[1] }, { hi[0], lo[1] }, { hi[0], hi[1] }, { lo[0], hi[1] } };
            for ( int c = 0; c < 4; ++c )
                for ( int k = 0; k < 3; ++k )
                    out->corner[c][k] = pts[0][k] + u[k] * ab[c][0] + v[k] * ab[c][1];
        }

        // The patch's front is d/di x d/dj (see Corner); make it the face's outward side.
        float ei[3], ej[3], cr[3];
        for ( int k = 0; k < 3; ++k )
        {
            ei[k] = out->corner[1][k] - out->corner[0][k];
            ej[k] = out->corner[3][k] - out->corner[0][k];
        }
        Cross( ei, ej, cr );
        if ( Dot( cr, n ) < 0.0f )
        {
            float t[3];
            memcpy( t, out->corner[1], sizeof( t ) );
            memcpy( out->corner[1], out->corner[3], sizeof( t ) );
            memcpy( out->corner[3], t, sizeof( t ) );
        }
        memcpy( out->normal, n, sizeof( float ) * 3 );
        return true;
    }

    // Front normal at control point (col,row): d/dcol x d/drow, the same side Corner()
    // builds a decal's front on.  Collapsed rows/columns (cylinder poles, cones) step
    // further along the grid; if that still fails the patch's average normal is used.
    bool PatchCtrlNormal( const patchMesh_t *p, int col, int row, float *out )
    {
        for ( int reach = 1; reach < 16; ++reach )
        {
            const int c0 = col - reach < 0 ? 0 : col - reach, c1 = col + reach >= p->width  ? p->width  - 1 : col + reach;
            const int r0 = row - reach < 0 ? 0 : row - reach, r1 = row + reach >= p->height ? p->height - 1 : row + reach;
            float dc[3], dr[3];
            for ( int k = 0; k < 3; ++k )
            {
                dc[k] = p->ctrl[c1][row].xyz[k] - p->ctrl[c0][row].xyz[k];
                dr[k] = p->ctrl[col][r1].xyz[k] - p->ctrl[col][r0].xyz[k];
            }
            Cross( dc, dr, out );
            if ( Normalize( out ) > 1e-4f )
                return true;
            if ( c0 == 0 && c1 == p->width - 1 && r0 == 0 && r1 == p->height - 1 )
                break;
        }
        return false;
    }

    // One overlay patch from a selected curve/terrain patch: the same control grid, type
    // and subdivision (so it tessellates the same), the source's texture coords untouched
    // (so the overlay lines up texel-for-texel), every control point pushed out along the
    // surface normal.  Returns the new patch, not yet linked.
    patchMesh_t *OverlayFromPatch( const patchMesh_t *srcP, float off )
    {
        float avg[3] = { 0.0f, 0.0f, 0.0f };
        for ( int i = 0; i < srcP->width; ++i )
            for ( int j = 0; j < srcP->height; ++j )
            {
                float n[3];
                if ( PatchCtrlNormal( srcP, i, j, n ) )
                    for ( int k = 0; k < 3; ++k ) avg[k] += n[k];
            }
        Normalize( avg );

        patchMesh_t *p = MakeNewPatch();
        p->width      = srcP->width;
        p->height     = srcP->height;
        p->type       = srcP->type;
        p->subDivType = srcP->subDivType;
        p->contents   = 0;
        p->flags      = 0;
        SetMaterial( s_material, &p->texture );
        p->lightmap = srcP->lightmap;
        memset( p->kiwiLayer, 0, sizeof( p->kiwiLayer ) );    // terrain paint layers stay on the source
        memcpy( p->ctrl, srcP->ctrl, sizeof( p->ctrl ) );
        for ( int i = 0; i < srcP->width; ++i )
            for ( int j = 0; j < srcP->height; ++j )
            {
                float n[3];
                if ( !PatchCtrlNormal( srcP, i, j, n ) )
                    memcpy( n, avg, sizeof( n ) );
                drawVert_t *cp = &p->ctrl[i][j];
                for ( int k = 0; k < 3; ++k )
                    cp->xyz[k] += n[k] * off;
                cp->savedTexCoord = cp->texCoord;
                *(unsigned int *)&cp->vert_color = 0xFFFFFFFFu;
                ( (byte *)&cp->vert_color )[3] = (byte)(int)( ClampF( s_opacity * 255.0f, 0.0f, 255.0f ) + 0.5f );
            }
        *(int *)&p->size_of_struct_0x504C = *(const int *)&srcP->size_of_struct_0x504C;   // same lightmap sample size
        p->bDirty = false;                          // keep the copied lightmap coords
        p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
        ++p->version;
        return p;
    }

    bool OverlaySelectedFaces()
    {
        if ( !s_material[0] )
        {
            SetStatus( "Overlay: choose a material first." );
            return false;
        }
        if ( !world_entity )
            return false;

        std::vector<overlaySrc_t> src;
        int skipped = 0;
        const int nf = g_SelectedFaces.GetSize();
        for ( int i = 0; i < nf; ++i )
        {
            selface_t  &sf   = g_SelectedFaces.GetAt( i );
            selbrush_t *node = sf.brush;
            brush_t    *def  = node ? node->def : nullptr;
            // Prefab contents draw through a placement transform this world-space patch lacks.
            if ( !def || !def->faces || node->patch || sf.index < 0 || sf.index >= def->faceCount
              || ( node->owner && node->owner->prefab ) )
            {
                ++skipped;
                continue;
            }
            face_t      *f  = &def->faces[sf.index];
            MaterialDef *md = &f->mtldef[0];                 // the material layer, whatever the edit layer
            overlaySrc_t s;
            if ( !f->w || ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 || !OverlayFromFace( f, &s ) )
            {
                ++skipped;
                continue;
            }
            texdef_sub_t *td = &md->mat_texDef + LayerMat::GetCurrentLayer( md );
            Face_MoveTexture( td->size, f->plane.normal, s.texMat, td->shift, td->rotate, td->crossterm );
            src.push_back( s );
        }

        // Whole selected patches (curves, terrain): overlay the entire surface.
        std::vector<const patchMesh_t *> patches;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !b->patch || !b->patch->def )
                continue;
            if ( b->owner && b->owner->prefab )
            {
                ++skipped;
                continue;
            }
            patches.push_back( b->patch->def );
        }

        if ( src.empty() && patches.empty() )
        {
            SetStatus( ( nf || skipped ) ? "Overlay: none of the selected faces or curves can take one."
                                         : "Overlay: select brush faces (Ctrl+Shift+click) or curves first." );
            return false;
        }

        Undo_ClearRedo();
        Undo_GeneralStart( "overlay faces" );
        Select_Deselect( 1 );

        const float off = s_offset + (float)s_layer * KDEC_LAYER_STEP;
        int inexact = 0;
        for ( size_t n = 0; n < src.size(); ++n )
        {
            const overlaySrc_t &s = src[n];
            if ( !s.exact )
                ++inexact;
            patchMesh_t *p = MakeNewPatch();
            p->width = p->height = 3;
            p->type  = (PATCH_TYPES)0;
            p->contents = 0;
            p->flags    = 0;
            SetMaterial( s_material, &p->texture );
            for ( int i = 0; i < 3; ++i )
                for ( int j = 0; j < 3; ++j )
                {
                    const float u = (float)i * 0.5f, v = (float)j * 0.5f;
                    const float w0 = ( 1.0f - u ) * ( 1.0f - v ), w1 = u * ( 1.0f - v ),
                                w2 = u * v,                       w3 = ( 1.0f - u ) * v;
                    float on[3];
                    for ( int k = 0; k < 3; ++k )
                        on[k] = s.corner[0][k] * w0 + s.corner[1][k] * w1 + s.corner[2][k] * w2 + s.corner[3][k] * w3;
                    drawVert_t *cp = &p->ctrl[i][j];
                    for ( int k = 0; k < 3; ++k )
                        cp->xyz[k] = on[k] + s.normal[k] * off;
                    // ST from the point ON the face, so the offset cannot shift the mapping.
                    cp->texCoord.st[0] = s.texMat[0] * on[0] + s.texMat[1] * on[1] + s.texMat[2] * on[2] + s.texMat[3];
                    cp->texCoord.st[1] = s.texMat[4] * on[0] + s.texMat[5] * on[1] + s.texMat[6] * on[2] + s.texMat[7];
                    cp->texCoord.lightmap[0] = u;
                    cp->texCoord.lightmap[1] = v;
                    cp->savedTexCoord = cp->texCoord;
                    *(unsigned int *)&cp->vert_color = 0xFFFFFFFFu;
                    ( (byte *)&cp->vert_color )[3] = (byte)(int)( ClampF( s_opacity * 255.0f, 0.0f, 255.0f ) + 0.5f );
                }
            Patch_KiwiEnsureLmapCoords( p );                // real lightmap coords at sample 16
            p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
            ++p->version;

            brush_t    *pdef = AddBrushForPatch( p, (entity_s *)world_entity->def );
            selbrush_t *inst = Brush_AddToList( pdef, world_entity );
            Brush_AddToList2( inst );
        }
        for ( size_t n = 0; n < patches.size(); ++n )
        {
            patchMesh_t *p    = OverlayFromPatch( patches[n], off );
            brush_t     *pdef = AddBrushForPatch( p, (entity_s *)world_entity->def );
            selbrush_t  *inst = Brush_AddToList( pdef, world_entity );
            Brush_AddToList2( inst );
        }
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
        g_nUpdateBits = -1;

        SetStatus( "Overlay: %i from face%s, %i from curve%s%s%s.",
                   (int)src.size(), src.size() == 1 ? "" : "s",
                   (int)patches.size(), patches.size() == 1 ? "" : "s",
                   inexact ? " (faces that are not 4-sided are covered by their bounding rectangle)" : "",
                   skipped ? ", some of the selection skipped" : "" );
        return true;
    }

    void ClearHover()
    {
        if ( s_hoverHave )
        {
            s_hoverHave = false;
            g_nUpdateBits |= W_CAMERA;
        }
    }

    void SetArmed( bool armed )
    {
        if ( s_armed == armed )
            return;
        s_armed = armed;
        ClearHover();
        SetStatus( armed ? "Armed. Click a brush face in the 3D camera to place; Esc disarms."
                         : "Disarmed." );
        g_nUpdateBits |= W_CAMERA;
    }

    // Draw one decal outline (KiwiLines must be open).
    void OutlineRec( const decalRec_t &r, float rr, float gg, float bb )
    {
        float c[4][3];
        Corner( r, 0, 0, c[0] );
        Corner( r, 2, 0, c[1] );
        Corner( r, 2, 2, c[2] );
        Corner( r, 0, 2, c[3] );
        KiwiLines_Color( rr, gg, bb );
        for ( int k = 0; k < 4; ++k )
            KiwiLines_Add( c[k], c[( k + 1 ) % 4] );
        // "Up" tick along +V so rotation reads at a glance.
        float mid[3], top[3];
        Corner( r, 1, 1, mid );
        Corner( r, 1, 2, top );
        KiwiLines_Add( mid, top );
    }

    // A closed polyline (xyz triples) - a wrapped decal's border.
    void OutlineLoop( const std::vector<float> &pts, float rr, float gg, float bb )
    {
        KiwiLines_Color( rr, gg, bb );
        const int n = (int)pts.size() / 3;
        for ( int k = 0; k < n; ++k )
            KiwiLines_Add( &pts[k * 3], &pts[( ( k + 1 ) % n ) * 3] );
    }

    // 1 = a decal / overlay material (the texture browser's orange badge), 0 = not, -1 = not
    // classified yet.  A loaded material answers through Cam_MaterialIsOverlay itself; an
    // unloaded one from its file header - the same three facts (a world techset, sorted after
    // the opaque class, no depth write; shipped decals are sortKey 12, refStateBits[1] 0x1C) -
    // at most `*budget` header reads per frame.
    std::map<std::string, bool> s_decalClass;
    int DecalClass( qtexture_s *q, int *budget )
    {
        if ( q->next )
            return Cam_MaterialIsOverlay( q->next ) ? 1 : 0;
        std::map<std::string, bool>::iterator it = s_decalClass.find( q->name );
        if ( it != s_decalClass.end() )
            return it->second ? 1 : 0;
        if ( *budget <= 0 )
            return -1;
        --*budget;
        kiwiMatSource_t src;
        char err[256];
        bool decal = false;
        if ( KiwiMat_ReadSource( q->name, &src, err, sizeof( err ) ) )
        {
            const bool editorSet = !strcmp( src.techSet, "2d" ) || !strcmp( src.techSet, "tools" );
            decal = !editorSet && src.sortKey > 4 && ( src.refStateBits[1] & 1u ) == 0;   // GFXS1_DEPTHWRITE
        }
        s_decalClass[q->name] = decal;
        return decal ? 1 : 0;
    }

    // ── palette UI ───────────────────────────────────────────────────────────
    void DrawPalette()
    {
        ImGui::SeparatorText( "Material" );
        ImGui::SetNextItemWidth( 260.0f );
        if ( ImGui::InputText( "##mat", s_material, sizeof( s_material ) ) )
            Save();
        if ( ImGui::BeginDragDropTarget() )            // drag a thumbnail from the Textures tab
        {
            if ( const ImGuiPayload *pl = ImGui::AcceptDragDropPayload( KMTL_PAYLOAD ) )
            {
                CopyString( s_material, sizeof( s_material ), (const char *)pl->Data );
                float aspect;
                if ( s_keepAspect && MaterialAspect( s_material, &aspect ) )
                    s_height = s_width * aspect;
                Save();
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Use current" ) )
        {
            qtexture_s *q = g_qeglobals.random_texture_stuff[0].mtl.radMtl;
            if ( q && q->name )
            {
                CopyString( s_material, sizeof( s_material ), q->name );
                Save();
            }
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Take the texture browser's current material." );

        ImGui::SetNextItemWidth( 160.0f );
        if ( ImGui::InputText( "Filter", s_filter, sizeof( s_filter ) ) )
            Save();
        ImGui::SameLine();
        if ( ImGui::Checkbox( "Show all", &s_showAll ) )
            Save();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Off: only decal / overlay materials - the ones with the orange corner\n"
                               "badge (blended, no depth write, made to sit on a surface).\n"
                               "On: every material the texture browser knows." );
        ImGui::SameLine();
        if ( ImGui::Button( "Import decal..." ) )
            KiwiImport_BrowseDecals();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Make a decal material from a PNG (or TGA/DDS) with transparency.\n"
                               "The alpha channel is kept; the material is an alpha-blended\n"
                               "decal and becomes the chosen material here.  You can also drop\n"
                               "image files from Explorer onto this window." );

        // Thumbnails register on their own: only tiles on screen, a few per frame so a long
        // list never hitches, and a name that fails to register is not retried every frame.
        static std::set<std::string> s_thumbTried;
        int thumbBudget = 4;

        // The rows this frame: name filter, then (unless Show all) decals only.  Unloaded
        // materials are classified from their file header a few per frame.
        const int count = TexWnd_MaterialCount();
        int classifyBudget = 48, pending = 0;
        std::vector<int> rows;
        rows.reserve( (size_t)count );
        for ( int i = 0; i < count; ++i )
        {
            qtexture_s *q = TexWnd_MaterialAt( i );
            if ( !q || !q->name || !NameMatchesFilter( q->name ) )
                continue;
            if ( !s_showAll )
            {
                const int c = DecalClass( q, &classifyBudget );
                if ( c < 0 )
                    ++pending;
                if ( c != 1 )
                    continue;
            }
            rows.push_back( i );
        }
        if ( pending )
            g_nUpdateBits |= W_CAMERA;               // keep frames coming until the scan is done

        if ( ImGui::BeginChild( "##decalmats", ImVec2( 340.0f, 220.0f ), ImGuiChildFlags_Borders ) )
        {
            const float tile = 40.0f;
            ImGuiListClipper clipper;
            clipper.Begin( (int)rows.size(), tile + ImGui::GetStyle().ItemSpacing.y );
            while ( clipper.Step() )
            for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row )
            {
                const int i = rows[row];
                qtexture_s *q = TexWnd_MaterialAt( i );
                ImGui::PushID( i );
                IDirect3DTexture9 *tex = ColorMap( q );
                const bool selected = strcmp( s_material, q->name ) == 0;
                if ( tex )
                {
                    ImGui::Image( (ImTextureID)(intptr_t)tex, ImVec2( tile, tile ) );
                    // Same decal badge as the texture browser (camwnd.cpp Cam_MaterialIsOverlay).
                    if ( q->next && Cam_MaterialIsOverlay( q->next ) )
                    {
                        const ImVec2 mx = ImGui::GetItemRectMax();
                        const ImVec2 mn = ImGui::GetItemRectMin();
                        ImDrawList *dl = ImGui::GetWindowDrawList();
                        const ImVec2 p0( mx.x - 10.0f, mn.y + 2.0f ), p1( mx.x - 2.0f, mn.y + 10.0f );
                        dl->AddRectFilled( p0, p1, IM_COL32( 255, 140, 20, 255 ) );
                        dl->AddRect( ImVec2( p0.x - 1.0f, p0.y - 1.0f ), ImVec2( p1.x + 1.0f, p1.y + 1.0f ),
                                     IM_COL32( 20, 20, 20, 255 ) );
                        if ( ImGui::IsItemHovered() )
                            ImGui::SetTooltip( "Decal / overlay material: blended, no depth write.\n"
                                               "Put it on top of a surface, not on its own." );
                    }
                }
                else
                {
                    ImGui::Dummy( ImVec2( tile, tile ) );
                    if ( !q->next && thumbBudget > 0 && ImGui::IsItemVisible()
                      && s_thumbTried.insert( q->name ).second )
                    {
                        Texture_GetHandle( q->name );   // shows from the next frame
                        --thumbBudget;
                    }
                }
                ImGui::SameLine();
                if ( ImGui::Selectable( q->name, selected, 0, ImVec2( 0.0f, tile ) ) )
                {
                    CopyString( s_material, sizeof( s_material ), q->name );
                    if ( s_keepAspect )
                    {
                        float aspect;
                        if ( MaterialAspect( s_material, &aspect ) )
                            s_height = s_width * aspect;
                    }
                    Save();
                }
                ImGui::PopID();
            }
            if ( rows.empty() )
                ImGui::TextDisabled( pending ? "Looking for decal materials..."
                                   : s_showAll ? "No material matches \"%s\"."
                                               : "No decal material matches \"%s\" (Show all lists everything).",
                                     s_filter );
        }
        ImGui::EndChild();
        if ( pending && !rows.empty() )
            ImGui::TextDisabled( "%i decals so far, %i materials left to check...", (int)rows.size(), pending );
    }

    // Shared parameter rows; `rec` non-null edits the selected decal live.
    void DrawParams( patchMesh_t *def, decalRec_t *rec )
    {
        float *width   = rec ? &rec->width   : &s_width;
        float *height  = rec ? &rec->height  : &s_height;
        float *rot     = rec ? &rec->rot     : &s_rot;
        float *offset  = rec ? &rec->offset  : &s_offset;
        int   *layer   = rec ? &rec->layer   : &s_layer;
        float *opacity = rec ? &rec->opacity : &s_opacity;
        float *fade    = rec ? &rec->fade    : &s_fade;
        bool  *flipU   = rec ? &rec->flipU   : &s_flipU;
        bool  *flipV   = rec ? &rec->flipV   : &s_flipV;

        bool edited = false, active = false, released = false;
        #define KDEC_ROW( expr ) do { if ( expr ) edited = true; \
            active |= ImGui::IsItemActive(); released |= ImGui::IsItemDeactivatedAfterEdit(); } while ( 0 )

        KDEC_ROW( ImGui::SliderFloat( "Width",  width,  1.0f, 1024.0f, "%.1f", ImGuiSliderFlags_Logarithmic ) );
        if ( edited && s_keepAspect )
        {
            float aspect;
            if ( MaterialAspect( s_material, &aspect ) )
                *height = *width * aspect;
        }
        KDEC_ROW( ImGui::SliderFloat( "Height", height, 1.0f, 1024.0f, "%.1f", ImGuiSliderFlags_Logarithmic ) );
        ImGui::SameLine();
        if ( ImGui::Checkbox( "Keep aspect", &s_keepAspect ) )
            Save();
        KDEC_ROW( ImGui::SliderFloat( "Rotation", rot, -180.0f, 180.0f, "%.0f deg" ) );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "+90" ) ) { *rot += 90.0f; if ( *rot > 180.0f ) *rot -= 360.0f; edited = released = true; }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "-90" ) ) { *rot -= 90.0f; if ( *rot < -180.0f ) *rot += 360.0f; edited = released = true; }
        KDEC_ROW( ImGui::SliderFloat( "Surface offset", offset, 0.0f, 8.0f, "%.3f" ) );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Distance above the face. Keep it small; z-fighting starts at 0.\n"
                               "Wrapped decals sit at least %.1f off: their curve is drawn as\n"
                               "short chords that dip up to ~0.25 toward the surface.",
                               KDEC_WRAP_MIN_OFFSET );
        KDEC_ROW( ImGui::SliderInt( "Layer", layer, 0, KDEC_MAX_LAYER ) );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Back" ) && *layer > 0 )               { --*layer; edited = released = true; }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Front" ) && *layer < KDEC_MAX_LAYER ) { ++*layer; edited = released = true; }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Stacking order: each layer sits %.3f further off the face.", KDEC_LAYER_STEP );
        KDEC_ROW( ImGui::SliderFloat( "Opacity", opacity, 0.0f, 1.0f, "%.2f" ) );
        KDEC_ROW( ImGui::SliderFloat( "Edge fade", fade, 0.0f, 1.0f, "%.2f" ) );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Vertex alpha falls from the centre to the border.\n"
                               "Needs a material whose technique multiplies vertex alpha." );
        if ( ImGui::Checkbox( "Flip U", flipU ) ) edited = released = true;
        ImGui::SameLine();
        if ( ImGui::Checkbox( "Flip V", flipV ) ) edited = released = true;
        #undef KDEC_ROW

        if ( rec )
        {
            if ( edited )
            {
                if ( !s_editOpen || s_editDef != def )
                {
                    EditEnd();
                    EditBegin( def );
                }
                rec->width  = ClampF( rec->width, 1.0f, 4096.0f );
                rec->height = ClampF( rec->height, 1.0f, 4096.0f );
                rec->offset = ClampF( rec->offset, 0.0f, 64.0f );
                Layout( def, *rec );
            }
            if ( released || ( s_editOpen && !active && !ImGui::IsAnyItemActive() ) )
                EditEnd();
        }
        else if ( edited || released )
        {
            Save();
            g_nUpdateBits |= W_CAMERA;
        }
    }
}

// ── dock window ──────────────────────────────────────────────────────────────
void KiwiDecal_Draw()
{
    Load();
    PruneDead();

    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_DECALS );
    if ( !open || !*open )
    {
        EditEnd();
        return;
    }
    if ( KiwiWindows_JustOpened( KIWI_WIN_DECALS ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_DECALS ), open ) )
    {
        s_winMin   = ImGui::GetWindowPos();
        s_winMax   = ImVec2( s_winMin.x + ImGui::GetWindowSize().x, s_winMin.y + ImGui::GetWindowSize().y );
        s_winFrame = ImGui::GetFrameCount();
        DrawPalette();

        decalRec_t  *rec = nullptr;
        patchMesh_t *def = SelectedDecal( &rec );
        if ( def && rec )
        {
            ImGui::SeparatorText( "Selected decal" );
            const char *mat = def->texture.radMtl && def->texture.radMtl->name
                            ? def->texture.radMtl->name : "(none)";
            ImGui::TextDisabled( "%s", mat );
            if ( rec->wrap )
                ImGui::TextDisabled( "Wrapped over a patch, %ix%i control grid%s", def->width, def->height,
                                     rec->offSurface ? " - part runs past the patch edge (flat)" : "" );
            if ( ImGui::SmallButton( "Apply chosen material" ) && s_material[0] )
            {
                EditBegin( def );
                SetMaterial( s_material, &def->texture );
                Layout( def, *rec );
                EditEnd();
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Copy settings from it" ) )
            {
                s_width = rec->width; s_height = rec->height; s_rot = rec->rot;
                s_offset = rec->offset; s_layer = rec->layer; s_opacity = rec->opacity;
                s_fade = rec->fade; s_flipU = rec->flipU; s_flipV = rec->flipV;
                s_projection = rec->wrap ? 1 : 0;
                if ( def->texture.radMtl && def->texture.radMtl->name )
                    CopyString( s_material, sizeof( s_material ), def->texture.radMtl->name );
                Save();
            }
            DrawParams( def, rec );
        }
        else
        {
            EditEnd();
            ImGui::SeparatorText( "Placement" );
            {
                static const char *const kProj[] = { "Flat", "Wrap to surface" };
                ImGui::SetNextItemWidth( 160.0f );
                if ( ImGui::Combo( "Projection", &s_projection, kProj, IM_ARRAYSIZE( kProj ) ) )
                {
                    Save();
                    g_nUpdateBits |= W_CAMERA;
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Flat: one flat 3x3 patch on the face you click.\n"
                                       "Wrap to surface: the decal is unrolled over the curve or terrain\n"
                                       "patch you click - round cylinders, pipes, arches, bumpy ground -\n"
                                       "without stretching, at any rotation, and on across neighbouring\n"
                                       "patches that share an edge.  Brush faces stay flat." );
            }
            DrawParams( nullptr, nullptr );
            if ( ImGui::Checkbox( "Random rotation", &s_randomRot ) ) Save();
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 120.0f );
            if ( ImGui::SliderFloat( "Size jitter", &s_sizeJitter, 0.0f, 50.0f, "%.0f %%" ) ) Save();
            ImGui::TextDisabled( "Select a single decal (3x3 patch, or one wrapped this session) to edit it here." );
        }

        ImGui::Separator();
        if ( ImGui::Button( s_armed ? "Disarm (Esc)" : "Place decals (click faces)" ) )
            SetArmed( !s_armed );
        ImGui::SameLine();
        ImGui::TextDisabled( "Ctrl+wheel size   Shift+wheel rotate" );
        if ( ImGui::Button( "Overlay selection" ) )
            OverlaySelectedFaces();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "One overlay patch per selected brush face (Ctrl+Shift+click) or\n"
                               "selected curve, covering it and using its own texture alignment,\n"
                               "pushed out by Surface offset + Layer.  For multiply overlays like\n"
                               "*_burnt on their base.  Uses the chosen material and Opacity;\n"
                               "Ctrl+Z removes them." );
        if ( s_armed )
            ImGui::TextColored( ImVec4( 0.42f, 0.92f, 0.48f, 1.0f ), "%s", s_status );
        else
            ImGui::TextDisabled( "%s", s_status );
    }
    ImGui::End();

    if ( !*open )
    {
        SetArmed( false );
        EditEnd();
    }
}

// ── decal importer bridge (kiwi_import.cpp) ──────────────────────────────────
// A freshly imported stamp becomes the chosen material, sized to its aspect.
void KiwiDecal_UseImported( const char *name )
{
    if ( !name || !name[0] )
        return;
    Load();
    CopyString( s_material, sizeof( s_material ), name );
    float aspect;
    if ( s_keepAspect && MaterialAspect( s_material, &aspect ) )
        s_height = s_width * aspect;
    s_decalClass[name] = true;              // listed at once, loaded or not
    Save();
    KiwiWindows_Set( KIWI_WIN_DECALS, true );
    ImGuiShell_FocusTab( KiwiWindows_Title( KIWI_WIN_DECALS ) );
    SetStatus( "Imported '%s' - it is the chosen material; place it with the button below.", name );
}

// Is this screen point over the Decals window?  Only if it was drawn in the latest ImGui
// frame (a docked tab that is not the selected one is not drawn).
bool KiwiDecal_ScreenOverWindow( int screenX, int screenY )
{
    if ( s_winFrame < 0 || !ImGui::GetCurrentContext() || ImGui::GetFrameCount() != s_winFrame )
        return false;
    float x, y;
    if ( !ImGuiShell_ScreenToImGui( screenX, screenY, &x, &y ) )
        return false;
    return x >= s_winMin.x && y >= s_winMin.y && x < s_winMax.x && y < s_winMax.y;
}

// ── viewport bridge ──────────────────────────────────────────────────────────
bool KiwiDecal_IsArmed()
{
    return s_armed;
}

bool KiwiDecal_HandleDown( int imgX, int imgY )
{
    Load();
    if ( !s_armed )
        return false;
    if ( KiwiEditorCommand *live = KiwiCmd_Active() )
    {
        if ( live->PreemptIdle() )
            KiwiCmd_Cancel();
        else
        {
            Sys_Printf( "Decals: finish or cancel \"%s\" before placing.\n", live->Name() );
            return false;
        }
    }
    float point[3], normal[3];
    patchMesh_t *surf = nullptr;
    if ( !PickFace( imgX, imgY, point, normal, &surf ) )
    {
        SetStatus( "Armed. No face under the cursor." );
        return false;
    }
    s_hoverHave = true;
    memcpy( s_hoverPoint, point, sizeof( point ) );
    memcpy( s_hoverNormal, normal, sizeof( normal ) );
    s_hoverPatch = surf;
    s_previewDirty = true;
    return Place( point, normal, surf );
}

void KiwiDecal_HandleUp()   {}
void KiwiDecal_HandleAbort() {}

bool KiwiDecal_HandleEscape()
{
    if ( !s_armed )
        return false;
    SetArmed( false );
    return true;
}

bool KiwiDecal_HandleWheel( float steps, bool shift, bool ctrl )
{
    if ( !s_armed || steps == 0.0f )
        return false;
    if ( ctrl )
    {
        const float f = steps > 0.0f ? 1.1f : 1.0f / 1.1f;
        s_width  = ClampF( s_width * f, 1.0f, 4096.0f );
        s_height = ClampF( s_height * f, 1.0f, 4096.0f );
        Save();
        g_nUpdateBits |= W_CAMERA;
        return true;
    }
    if ( shift )
    {
        s_rot += steps > 0.0f ? 15.0f : -15.0f;
        if ( s_rot > 180.0f ) s_rot -= 360.0f;
        if ( s_rot < -180.0f ) s_rot += 360.0f;
        Save();
        g_nUpdateBits |= W_CAMERA;
        return true;
    }
    return false;
}

void KiwiDecal_Hover( int imgX, int imgY, bool over )
{
    if ( !s_armed || !over )
    {
        ClearHover();
        return;
    }
    float point[3], normal[3];
    patchMesh_t *surf = nullptr;
    if ( !PickFace( imgX, imgY, point, normal, &surf ) )
    {
        ClearHover();
        return;
    }
    const bool moved = !s_hoverHave || surf != s_hoverPatch
                    || fabsf( point[0] - s_hoverPoint[0] ) > 0.01f
                    || fabsf( point[1] - s_hoverPoint[1] ) > 0.01f
                    || fabsf( point[2] - s_hoverPoint[2] ) > 0.01f;
    s_hoverHave = true;
    memcpy( s_hoverPoint, point, sizeof( point ) );
    memcpy( s_hoverNormal, normal, sizeof( normal ) );
    s_hoverPatch = surf;
    if ( moved )
    {
        s_previewDirty = true;
        g_nUpdateBits |= W_CAMERA;
    }
}

void KiwiDecal_DrawWorld()
{
    // Selected decal: outline + up tick so rotation/size edits read in the camera.
    decalRec_t  *rec = nullptr;
    patchMesh_t *def = SelectedDecal( &rec );
    const bool preview = s_armed && s_hoverHave;
    if ( !def && !preview )
        return;

    // The wrap preview is walked only when the cursor or a setting changed.
    if ( preview && s_previewDirty )
    {
        s_previewDirty = false;
        s_previewOutline.clear();
        if ( s_projection == 1 && s_hoverPatch && DefLive( s_hoverPatch, nullptr ) )
        {
            decalRec_t r;
            FillRecFromSettings( r, s_hoverPoint, s_hoverNormal );
            r.rot = s_rot;
            r.width = s_width; r.height = s_height;
            r.wrap = true;
            r.target = s_hoverPatch;
            static patchMesh_t s_scratch;           // 20 KB: a grid to lay into, never linked
            if ( FillWrap( &s_scratch, r, nullptr ) )
                s_previewOutline.swap( r.outline );
        }
    }

    int segs = 12;
    if ( def && rec && rec->wrap )
        segs += (int)rec->outline.size() / 3;
    if ( preview )
        segs += (int)s_previewOutline.size() / 3;
    KiwiLines_Begin( segs, 2 );
    if ( def && rec )
    {
        if ( rec->wrap && rec->outline.size() >= 6 )
            OutlineLoop( rec->outline, 1.0f, 0.75f, 0.2f );
        else
            OutlineRec( *rec, 1.0f, 0.75f, 0.2f );
    }
    if ( preview )
    {
        if ( s_previewOutline.size() >= 6 )
            OutlineLoop( s_previewOutline, 0.3f, 0.9f, 1.0f );
        else
        {
            decalRec_t r;
            FillRecFromSettings( r, s_hoverPoint, s_hoverNormal );
            r.rot = s_rot;                          // no jitter/random in the preview
            r.width = s_width; r.height = s_height;
            OutlineRec( r, 0.3f, 0.9f, 1.0f );
        }
    }
    KiwiLines_Flush();
}
