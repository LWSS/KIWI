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
#include "kiwi_lines.h"
#include "kiwi_material.h"
#include "kiwi_pick.h"
#include "kiwi_windows.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <map>
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
extern curvePatchDef_t *Patch_GenericMesh2( patchMesh_t *p, int layer, int colMapArg, int rowMapArg ); // pmesh.cpp:721
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );   // pmesh.cpp:841
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );          // brush.cpp:670
extern void         Brush_AddToList2( selbrush_t *b );                         // brush.cpp:928
extern void         Patch_Rebuild( patchMesh_t *p, char doBounds );            // pmesh.cpp
extern void         Patch_PaintMarkUndo( patchMesh_t *def );                   // pmesh.cpp (tail)
extern void         Undo_ClearRedo();
extern void         Undo_GeneralStart( const char *operation );
extern void         Undo_End();
extern int          TexWnd_MaterialCount();                                    // texwnd.cpp (tail)
extern qtexture_s  *TexWnd_MaterialAt( int idx );
extern qtexture_s  *Texture_GetHandle( const char *name );                     // texwnd.cpp:315
extern ImGuiID      ImGuiShell_DockRoot();                                     // imgui_shell.cpp

namespace
{
    const char *KDEC_PROFILE = "KiwiDecal";
    const float KDEC_PI      = 3.14159265358979323846f;
    const float KDEC_LAYER_STEP = 0.125f;     // extra normal offset per layer
    enum { KDEC_NAME_CHARS = 128, KDEC_MAX_LAYER = 15 };

    // ── settings (persisted) ────────────────────────────────────────────────
    char  s_material[KDEC_NAME_CHARS] = "";
    char  s_filter[64] = "decal";
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

    // ── session ──────────────────────────────────────────────────────────────
    bool  s_loaded = false;
    bool  s_armed  = false;
    bool  s_hoverHave = false;
    float s_hoverPoint[3]  = { 0, 0, 0 };
    float s_hoverNormal[3] = { 0, 0, 1 };
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
                    Radiant_ProfileGetString( KDEC_PROFILE, "Filter", "decal" ).c_str() );
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

    // Write the 9 control points, texcoords and alphas from `r`.
    void Layout( patchMesh_t *def, const decalRec_t &r )
    {
        for ( int i = 0; i < 3; ++i )
        {
            for ( int j = 0; j < 3; ++j )
            {
                drawVert_t *cp = &def->ctrl[i][j];
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
        if ( def->width != 3 || def->height != 3 )
            return nullptr;

        std::map<patchMesh_t *, decalRec_t>::iterator it = s_decals.find( def );
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
    bool PickFace( int imgX, int imgY, float outPoint[3], float outNormal[3] )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;
        pick_result_t r = Pick( ray, SEL_MASK_FACE | SEL_MASK_OBJECT, PICKF_NONE );
        if ( !r.valid || !r.haveNormal )
            return false;
        memcpy( outPoint, r.point, sizeof( r.point ) );
        memcpy( outNormal, r.normal, sizeof( r.normal ) );
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

    bool Place( const float *point, const float *normal )
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
        for ( int i = 0; i < 3; ++i )
            for ( int j = 0; j < 3; ++j )
            {
                drawVert_t *cp = &p->ctrl[i][j];
                Corner( r, i, j, cp->xyz );
                const float s = r.flipU ? 1.0f - (float)i * 0.5f : (float)i * 0.5f;
                const float t = r.flipV ? 1.0f - (float)j * 0.5f : (float)j * 0.5f;
                cp->texCoord.st[0] = s;  cp->texCoord.st[1] = -t;
                cp->texCoord.lightmap[0] = s;  cp->texCoord.lightmap[1] = -t;
                cp->savedTexCoord = cp->texCoord;
                const bool edge = ( i == 0 || i == 2 || j == 0 || j == 2 );
                const float a = r.opacity * 255.0f * ( edge ? ( 1.0f - r.fade ) : 1.0f );
                ( (byte *)&cp->vert_color )[3] = (byte)(int)( ClampF( a, 0.0f, 255.0f ) + 0.5f );
            }
        *(float *)&p->size_of_struct_0x504C = 16.0f;
        p->bDirty = false;
        p->curveDef = Patch_GenericMesh2( p, g_qeglobals.current_edit_layer, 0, 0 );
        ++p->version;

        brush_t    *pdef = AddBrushForPatch( p, (entity_s *)world_entity->def );
        selbrush_t *inst = Brush_AddToList( pdef, world_entity );
        Brush_AddToList2( inst );                   // selected, so the sliders edit it at once
        Undo_End();

        s_decals[p] = r;
        ++s_placed;
        g_nUpdateBits = -1;
        SetStatus( "Armed. Placed %i decal%s this session.", s_placed, s_placed == 1 ? "" : "s" );
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
        const bool loadThumbs = ImGui::Button( "Load thumbnails" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Register every listed material so its colormap shows here.\n"
                               "Materials the texture browser already shows are ready." );

        const int count = TexWnd_MaterialCount();
        int listed = 0;
        if ( ImGui::BeginChild( "##decalmats", ImVec2( 340.0f, 220.0f ), ImGuiChildFlags_Borders ) )
        {
            const float tile = 40.0f;
            for ( int i = 0; i < count; ++i )
            {
                qtexture_s *q = TexWnd_MaterialAt( i );
                if ( !q || !q->name || !NameMatchesFilter( q->name ) )
                    continue;
                if ( ++listed > 400 )
                    break;
                if ( loadThumbs && !q->next )
                    Texture_GetHandle( q->name );
                ImGui::PushID( i );
                IDirect3DTexture9 *tex = ColorMap( q );
                const bool selected = strcmp( s_material, q->name ) == 0;
                if ( tex )
                    ImGui::Image( (ImTextureID)(intptr_t)tex, ImVec2( tile, tile ) );
                else
                    ImGui::Dummy( ImVec2( tile, tile ) );
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
            if ( !listed )
                ImGui::TextDisabled( "No material matches \"%s\".", s_filter );
        }
        ImGui::EndChild();
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
            ImGui::SetTooltip( "Distance above the face. Keep it small; z-fighting starts at 0." );
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
        DrawPalette();

        decalRec_t  *rec = nullptr;
        patchMesh_t *def = SelectedDecal( &rec );
        if ( def && rec )
        {
            ImGui::SeparatorText( "Selected decal" );
            const char *mat = def->texture.radMtl && def->texture.radMtl->name
                            ? def->texture.radMtl->name : "(none)";
            ImGui::TextDisabled( "%s", mat );
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
            DrawParams( nullptr, nullptr );
            if ( ImGui::Checkbox( "Random rotation", &s_randomRot ) ) Save();
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 120.0f );
            if ( ImGui::SliderFloat( "Size jitter", &s_sizeJitter, 0.0f, 50.0f, "%.0f %%" ) ) Save();
            ImGui::TextDisabled( "Select a single decal (3x3 patch) to edit it here." );
        }

        ImGui::Separator();
        if ( ImGui::Button( s_armed ? "Disarm (Esc)" : "Place decals (click faces)" ) )
            SetArmed( !s_armed );
        ImGui::SameLine();
        ImGui::TextDisabled( "Ctrl+wheel size   Shift+wheel rotate" );
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
    if ( !PickFace( imgX, imgY, point, normal ) )
    {
        SetStatus( "Armed. No face under the cursor." );
        return false;
    }
    s_hoverHave = true;
    memcpy( s_hoverPoint, point, sizeof( point ) );
    memcpy( s_hoverNormal, normal, sizeof( normal ) );
    return Place( point, normal );
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
    if ( !PickFace( imgX, imgY, point, normal ) )
    {
        ClearHover();
        return;
    }
    const bool moved = !s_hoverHave
                    || fabsf( point[0] - s_hoverPoint[0] ) > 0.01f
                    || fabsf( point[1] - s_hoverPoint[1] ) > 0.01f
                    || fabsf( point[2] - s_hoverPoint[2] ) > 0.01f;
    s_hoverHave = true;
    memcpy( s_hoverPoint, point, sizeof( point ) );
    memcpy( s_hoverNormal, normal, sizeof( normal ) );
    if ( moved )
        g_nUpdateBits |= W_CAMERA;
}

void KiwiDecal_DrawWorld()
{
    // Selected decal: outline + up tick so rotation/size edits read in the camera.
    decalRec_t  *rec = nullptr;
    patchMesh_t *def = SelectedDecal( &rec );
    const bool preview = s_armed && s_hoverHave;
    if ( !def && !preview )
        return;

    KiwiLines_Begin( 12, 2 );
    if ( def && rec )
        OutlineRec( *rec, 1.0f, 0.75f, 0.2f );
    if ( preview )
    {
        decalRec_t r;
        FillRecFromSettings( r, s_hoverPoint, s_hoverNormal );
        r.rot = s_rot;                              // no jitter/random in the preview
        r.width = s_width; r.height = s_height;
        OutlineRec( r, 0.3f, 0.9f, 1.0f );
    }
    KiwiLines_Flush();
}
