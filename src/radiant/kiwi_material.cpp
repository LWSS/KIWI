#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI material inheritance and material-layer repair helpers.
// Material reads and realization stay behind the ported MaterialDef entry points.

#include "stdafx.h"
#include "qe3.h"
#include "winding.h"

#include "kiwi_material.h"
#include "kiwi_pick.h"                 // Pick / Pick_RayFromCursor / ray_t
#include "kiwi_selection.h"            // sel_item_t, SEL_MASK_FACE, Sel_BrushLive
#include "kiwi_str.h"                // KiwiStr_ContainsNoCase

#include <math.h>
#include <string.h>

// Ported entry points.
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );          // materialdef.cpp:159 (0x431640)
extern bool              Materialdef_Realize( MaterialDef *md );              // materialdef.cpp:125
extern float             Winding_Area( winding_t *w );                        // winding.cpp (winding.h:78)
extern int               Sys_Printf( const char *fmt, ... );                  // win_qe3.cpp

// xywnd.cpp:2233 — the clipper's caulk/nodraw_decal synthesis and KIWI fallback.
extern void              Ed_BuildClipFaceMaterial_Kiwi( face_t *out, const brush_t *src );

namespace
{
    // Tool-family material names.
    // Match substrings like MtlDef_IsFaceFiltered (mayaexport.cpp:112), independent
    // of whether a material uses a tools path. `$default` stays inheritable so a
    // fresh brush's cut faces do not fall back to caulk.
    const char *const KMTL_TOOL_NAMES[] = {
        "caulk",
        "nodraw",
        "clip",          // clip, player_clip, ai_clip, clipfoliage
        "hint",
        "skip",
        "portal",
        "origin",
        "trigger",
        "lightgrid",
        "tools/",
        "tools\\",
    };

    // Material paths use the shared ASCII-only, case-insensitive matcher.

    // Materialdef_GetName is non-const even though this call is a pure read.
    MaterialDef *Channel0( const face_t *f )
    {
        return f ? const_cast<MaterialDef *>( &f->mtldef[0] ) : 0;
    }

    // MtlDef_IsValid requires exactly one handle; check it here instead of
    // tripping Materialdef_GetName on a half-built face.
    bool HasMaterial( const MaterialDef *m )
    {
        return m && ( ( m->lyrMtl != 0 ) + ( m->radMtl != 0 ) == 1 );
    }

    // Name classification and access.
    // Empty names and tool-family materials are not inheritable.
    bool IsToolName( const char *name )
    {
        if ( !name || !*name )
            return true;                    // unnamed is not something to inherit
        for ( size_t i = 0; i < sizeof( KMTL_TOOL_NAMES ) / sizeof( KMTL_TOOL_NAMES[0] ); ++i )
            if ( KiwiStr_ContainsNoCase( name, KMTL_TOOL_NAMES[i] ) )
                return true;
        return false;
    }

    // The channel-0 material name of a face, or NULL when it has none.
    const char *FaceMaterialName( const face_t *f )
    {
        MaterialDef *md = Channel0( f );
        if ( !HasMaterial( md ) )
            return 0;
        return (const char *)Materialdef_GetName( md );
    }
}

// Material inheritance.
bool KiwiMtl_FaceIsInheritable( const face_t *f )
{
    const char *name = FaceMaterialName( f );
    return name && !IsToolName( name );
}

// Source-face selection.
int KiwiMtl_PickSourceFace( const brush_t *def, const float cutNormal[3],
                            int prefer0, int prefer1 )
{
    if ( !def || !def->faces || def->faceCount <= 0 )
        return -1;

    // Preferred adjacent faces win in caller order before brush-wide scoring.
    const int prefer[2] = { prefer0, prefer1 };
    for ( int i = 0; i < 2; ++i )
    {
        const int fi = prefer[i];
        if ( fi < 0 || fi >= def->faceCount )
            continue;
        if ( KiwiMtl_FaceIsInheritable( &def->faces[fi] ) )
            return fi;
    }

    // Score = area * (1 - |n_f · n_cut|); without a normal, area alone wins.
    int   best      = -1;
    float bestScore = -1.0f;
    for ( int f = 0; f < def->faceCount; ++f )
    {
        const face_t *fc = &def->faces[f];
        if ( !KiwiMtl_FaceIsInheritable( fc ) )
            continue;

        // A clipped-away winding keeps a floor area, preserving its material but
        // losing every tie to a face with a surface.
        float area = 1.0f;
        if ( fc->w && fc->w->numpoints >= 3 && fc->w->numpoints <= MAX_POINTS_ON_WINDING )
        {
            area = Winding_Area( const_cast<winding_t *>( fc->w ) );
            if ( !( area > 1.0f ) )
                area = 1.0f;
        }

        float perp = 1.0f;
        if ( cutNormal )
        {
            const float d = fc->plane.normal[0] * cutNormal[0]
                          + fc->plane.normal[1] * cutNormal[1]
                          + fc->plane.normal[2] * cutNormal[2];
            perp = 1.0f - fabsf( d );
            // Floor parallel faces so the only inheritable material beats none.
            if ( perp < 1.0e-3f )
                perp = 1.0e-3f;
        }

        const float score = area * perp;
        if ( score > bestScore )
        {
            bestScore = score;
            best      = f;
        }
    }
    return best;                            // -1 means every face is a tool material
}

// Material realization and copying.
void KiwiMtl_RealizeFace( face_t *f )
{
    if ( !f )
        return;
    for ( int c = 0; c < 4; ++c )
        if ( HasMaterial( &f->mtldef[c] ) )
            Materialdef_Realize( &f->mtldef[c] );
}

void KiwiMtl_RealizePatch( patchMesh_t *p )
{
    if ( !p )
        return;
    // Material pairs are contiguous at +0x18/+0x20/+0x28, matching the ported
    // pmesh.cpp:9896 draw walk.
    for ( int c = 0; c < 3; ++c )
    {
        MaterialDef *md = (MaterialDef *)( &p->texture + c );
        if ( HasMaterial( md ) )
            Materialdef_Realize( md );
    }
}

bool KiwiMtl_SeedFaceFrom( face_t *out, const brush_t *def, int srcFace )
{
    if ( !out || !def || !def->faces )
        return false;
    if ( srcFace < 0 || srcFace >= def->faceCount )
        return false;

    const face_t *src = &def->faces[srcFace];
    for ( int c = 0; c < 4; ++c )
        out->mtldef[c] = src->mtldef[c];    // pointer pair and texdef
    KiwiMtl_RealizeFace( out );
    return true;
}

// Cut/split seeding.
bool KiwiMtl_SeedClipFace( face_t *out, const brush_t *def, const float cutNormal[3] )
{
    if ( !out || !def )
        return false;

    const int src = KiwiMtl_PickSourceFace( def, cutNormal, -1, -1 );
    if ( src >= 0 && KiwiMtl_SeedFaceFrom( out, def, src ) )
        return true;

    // All-tool brushes retain the clipper's caulk/nodraw_decal synthesis.
    Ed_BuildClipFaceMaterial_Kiwi( out, def );
    return false;
}

// Material-state diagnostic.
// camwnd.cpp owns camera substitution decoding; this command only selects
// subjects and formats the result.
namespace
{
    const char *YesNo( bool b ) { return b ? "YES" : "no"; }

    void PrintOne( const char *what, Material *handle )
    {
        kiwiMatDiag_t d;
        if ( !handle || !KiwiMtl_Diagnose( handle, &d ) )
        {
            Sys_Printf( "  %-18s (no material)\n", what );
            return;
        }
        Sys_Printf( "  %-18s \"%s\"\n", what, d.name ? d.name : "?" );
        Sys_Printf( "      techniqueSet   \"%s\"   (base \"%s\")\n",
                    d.techSet ? d.techSet : "?", d.techSetBase ? d.techSetBase : "?" );
        Sys_Printf( "      sortKey        %i%s\n", d.sortKey,
                    ( d.sortKey >= 24 ) ? "  (>= SORTKEY_DECAL 24 - not opaque)" : "  (opaque class)" );
        if ( !d.techPresent )
        {
            Sys_Printf( "      camera tech    %i  ** ABSENT on this material **\n", d.cameraTech );
        }
        else
        {
            Sys_Printf( "      camera tech    %i   loadBits 0x%08X / 0x%08X\n",
                        d.cameraTech, d.loadBits0, d.loadBits1 );
            Sys_Printf( "      depthTest      %-4s   depthWrite %s%s\n",
                        YesNo( d.depthTest ), YesNo( d.depthWrite ),
                        d.depthWrite ? "" : "   <== cannot participate in z-order" );
        }
        if ( d.substituted )
            Sys_Printf( "      SUBSTITUTED    the camera draws this face as \"%s\"\n",
                        d.substituteName ? d.substituteName : "?" );
    }
}

void KiwiMtl_InfoCommand()
{
    Sys_Printf( "--- KiwiMatInfo (edit layer %i) ---\n", g_qeglobals.current_edit_layer );

    // New faces copy random_texture_stuff[layer] (brush.cpp:7664 et al.).
    {
        const int layer = g_qeglobals.current_edit_layer;
        const MaterialDef *md = ( layer >= 0 && layer < 3 )
                              ? &g_qeglobals.random_texture_stuff[layer].mtl : 0;
        Material *h = ( md && md->radMtl ) ? md->radMtl->handle : 0;
        if ( md && md->radMtl && md->radMtl->name )
            Sys_Printf( "  template name      \"%s\"\n", md->radMtl->name );
        PrintOne( "template", h );
    }

    // Pick only the face under the cursor, independent of selection mode.
    ray_t ray;
    if ( !Pick_RayFromCursor( &ray ) )
    {
        Sys_Printf( "  (no cursor ray - move the mouse into the 3D view first)\n" );
        return;
    }
    const pick_result_t hit = Pick( ray, SEL_MASK_FACE );
    if ( !hit.valid || hit.item.kind != SEL_FACE || !Sel_BrushLive( hit.item.brush ) )
    {
        Sys_Printf( "  (no face under the cursor)\n" );
        return;
    }
    brush_t *def = hit.item.brush->def;
    if ( !def || !def->faces || hit.item.faceIndex < 0 || hit.item.faceIndex >= def->faceCount )
    {
        Sys_Printf( "  (face index out of range)\n" );
        return;
    }
    const face_t *f = &def->faces[hit.item.faceIndex];
    const int     layer = g_qeglobals.current_edit_layer;
    const MaterialDef *md = ( layer >= 0 && layer < 4 ) ? &f->mtldef[layer] : &f->mtldef[0];
    if ( md->radMtl && md->radMtl->name )
        Sys_Printf( "  face %i name        \"%s\"\n", hit.item.faceIndex, md->radMtl->name );
    PrintOne( "face under cursor", md->radMtl ? md->radMtl->handle : 0 );
}

// Material-layer integrity.
// Repairs use the ported Face_InitMaterialChannel and SetMaterial helpers.

extern int   Face_InitMaterialChannel( unsigned int textureChannel, face_t *faceDef,
                                       MaterialDef *src );                     // brush.cpp:417  (0x472C90)
extern void  SetMaterial( const char *tex_name, patchMesh_material *mtlDef );  // materialdef.cpp:101 (0x4315C0)
extern qtexture_s *MaterialDef_GetLayeredMaterial( MaterialDef *mtlDef );      // materialdef.cpp:168 (0x4314A0)
namespace LayerMat { int GetCurrentLayer( MaterialDef *def ); }                // materialdef.cpp:252 (0x431B30)
extern void  MarkMapModified( void );                                          // win_qe3.cpp:195 (0x499BB0)
extern entity_s entities;                                                      // entity.cpp:295 (0x23F17A0)
// Same layer-1 texcoord pass used by patch creators.
extern void  Patch_KiwiEnsureLmapCoords( patchMesh_t *p );                     // pmesh.cpp:10405

namespace
{
    // Ported defaults: face samples are 0.25/16/0.25 (mainfrm.cpp:742), while
    // patches use $default/lightmap_gray/smoothing_smooth (pmesh.cpp:145).
    const char *const KMTL_PATCH_CHANNEL[3] = { "$default", "lightmap_gray", "smoothing_smooth" };
    const float       KMTL_CHANNEL_SAMPLE[3] = { 0.25f, 16.0f, 0.25f };

    bool IsFiniteNonZero( float v )
    {
        // Zero is compiler-invalid; pmesh.cpp:1096 also rejects INF/NAN exponents.
        if ( v == 0.0f )
            return false;
        return ( *(const unsigned int *)&v & 0x7F800000u ) != 0x7F800000u;
    }

    // A valid channel is named and has finite, non-zero scales on its active layer.
    bool FaceChannelIsValid( face_t *f, int channel )
    {
        MaterialDef *md = &f->mtldef[channel];
        if ( !HasMaterial( md ) )
            return false;
        const char *name = (const char *)Materialdef_GetName( md );
        if ( !name || !name[0] )
            return false;
        const float *size = &md->mat_texDef.size[ 7 * LayerMat::GetCurrentLayer( md ) ];
        return IsFiniteNonZero( size[0] ) && IsFiniteNonZero( size[1] );
    }

    // Zero-layer materials receive no scales; match Init_MaterialLayer's 512
    // dimension fallback and Ed_BuildClipFaceMaterial_Kiwi's width*sample values
    // (materialdef.cpp:351; xywnd.cpp:2293).
    void ForceChannelScale( face_t *f, int channel )
    {
        MaterialDef *md = &f->mtldef[channel];
        qtexture_s  *t  = MaterialDef_GetLayeredMaterial( md );
        const int    w  = t ? t->width  : 512;
        const int    h  = t ? t->height : 512;
        float       *size = &md->mat_texDef.size[ 7 * LayerMat::GetCurrentLayer( md ) ];
        if ( !IsFiniteNonZero( size[0] ) ) size[0] = (float)w * KMTL_CHANNEL_SAMPLE[channel];
        if ( !IsFiniteNonZero( size[1] ) ) size[1] = (float)h * KMTL_CHANNEL_SAMPLE[channel];
    }

    bool PatchChannelIsValid( patchMesh_t *p, int channel )
    {
        MaterialDef *md = (MaterialDef *)( &p->texture + channel );
        if ( !HasMaterial( md ) )
            return false;
        const char *name = (const char *)Materialdef_GetName( md );
        return name && name[0];
    }

    // Lightmap coordinates are texCoord [2],[3]. Repair only a non-finite grid or
    // one where both axes are constant; one constant axis can be a valid thin patch.
    bool PatchLmapCoordsDegenerate( const patchMesh_t *p )
    {
        if ( p->width < 2 || p->height < 2 )
            return false;                     // nothing to be degenerate ABOUT
        const float *first = &p->ctrl[0][0].texCoord.st[2];
        bool varies = false;
        for ( int col = 0; col < p->width; ++col )
        {
            for ( int row = 0; row < p->height; ++row )
            {
                const float *lm = &p->ctrl[col][row].texCoord.st[2];
                if ( ( *(const unsigned int *)&lm[0] & 0x7F800000u ) == 0x7F800000u ||
                     ( *(const unsigned int *)&lm[1] & 0x7F800000u ) == 0x7F800000u )
                    return true;              // INF/NAN — worse than flat
                if ( lm[0] != first[0] || lm[1] != first[1] )
                    varies = true;
            }
        }
        return !varies;
    }
}

bool KiwiMtl_FaceLayersAreValid( face_t *f )
{
    if ( !f )
        return true;
    for ( int c = 0; c < 3; ++c )
        if ( !FaceChannelIsValid( f, c ) )
            return false;
    return true;
}

bool KiwiMtl_EnsureFaceLayers( face_t *f )
{
    if ( !f )
        return false;
    // The common valid path performs no realization work.
    if ( KiwiMtl_FaceLayersAreValid( f ) )
        return false;

    bool repaired = false;

    // Reinitialize invalid channels independently, preserving the other channels.
    // Samples come from matching current-texture templates, with boot defaults for
    // unseeded values. Face_InitMaterialChannel consumes the float bit pattern
    // through its MaterialDef* parameter (materialdef.cpp:348).
    for ( int c = 0; c < 3; ++c )
    {
        if ( FaceChannelIsValid( f, c ) )
            continue;
        float sample = g_qeglobals.random_texture_stuff[c].sampleSize;
        if ( !IsFiniteNonZero( sample ) )
            sample = KMTL_CHANNEL_SAMPLE[c];
        MaterialDef *sampleBits = 0;
        memcpy( &sampleBits, &sample, sizeof( sample ) );
        Face_InitMaterialChannel( (unsigned int)c, f, sampleBits );
        repaired = true;
    }

    // Degenerate material resolution can leave zero scales after initialization.
    for ( int c = 0; c < 3; ++c )
    {
        MaterialDef *md   = &f->mtldef[c];
        const float *size = &md->mat_texDef.size[ 7 * LayerMat::GetCurrentLayer( md ) ];
        if ( !IsFiniteNonZero( size[0] ) || !IsFiniteNonZero( size[1] ) )
        {
            ForceChannelScale( f, c );
            repaired = true;
        }
    }

    if ( repaired )
        KiwiMtl_RealizeFace( f );
    return repaired;
}

bool KiwiMtl_EnsurePatchChannels( patchMesh_t *p )
{
    if ( !p )
        return false;

    bool repaired = false;
    for ( int c = 0; c < 3; ++c )
    {
        if ( PatchChannelIsValid( p, c ) )
            continue;
        SetMaterial( KMTL_PATCH_CHANNEL[c], &p->texture + c );
        repaired = true;
    }

    if ( repaired )
        KiwiMtl_RealizePatch( p );
    return repaired;
}

bool KiwiMtl_EnsurePatchLayers( patchMesh_t *p )
{
    if ( !p )
        return false;

    bool repaired = KiwiMtl_EnsurePatchChannels( p );

    // Material presence and lightmap parametrization are independent. Count the
    // texcoord repair only if the ported pass makes the grid non-degenerate, or a
    // legitimately flat patch would mark the map modified on every load.
    if ( PatchLmapCoordsDegenerate( p ) )
    {
        Patch_KiwiEnsureLmapCoords( p );
        if ( !PatchLmapCoordsDegenerate( p ) )
            repaired = true;
    }
    return repaired;
}

bool KiwiMtl_EnsureBrushLayers( brush_t *def )
{
    if ( !def )
        return false;
    if ( def->patch )
        return KiwiMtl_EnsurePatchLayers( def->patch );

    bool repaired = false;
    for ( int i = 0; def->faces && i < def->faceCount; ++i )
        if ( KiwiMtl_EnsureFaceLayers( &def->faces[i] ) )
            repaired = true;
    return repaired;
}

// Patch tessellation-cap diagnostic.
// Indices and counts narrow to uint16_t in pmesh.cpp:9885 and r_ed_scene.cpp:131,
// so values above 0xFFFF wrap into unrelated vertices. CURVE_GRID_DIM permits
// 511x511; report the patch so it can be split or assigned a lower subdivision.
static void KiwiMtl_ReportPatchTessellationCap( const patchMesh_t *p )
{
    const curvePatchDef_t *cd = p ? p->curveDef : 0;
    if ( !cd )
        return;
    const int verts = cd->width * cd->height;
    const int indices = ( cd->height - 1 ) * ( 6 * cd->width - 6 );   // indexCount, pmesh.cpp:9905
    if ( verts <= 0xFFFF && indices <= 0xFFFF )
        return;
    const char *name = "?";
    MaterialDef *md = (MaterialDef *)&p->texture;
    if ( HasMaterial( md ) )
        name = (const char *)Materialdef_GetName( md );
    Sys_Printf( "Patch tessellation over the editor's 16-bit mesh cap: \"%s\" %ix%i control "
                "grid, subdiv %i -> %ix%i tessellated (%i verts, %i indices).  It will draw "
                "stray geometry; lower its subdivision level or split it.\n",
                name, p->width, p->height, p->subDivType,
                cd->width, cd->height, verts, indices );
}

int KiwiMtl_HealMapLayers( int *outBrushes, int *outPatches )
{
    int brushes = 0;
    int patches = 0;

    // Per-entity DEF lists run from brushes.prev to the &def sentinel via onext
    // (MapFile_WriteEntity, map.cpp:1592).
    for ( entity_s *e = entities.next; e != &entities; e = e->next )
    {
        brush_t *sentinel = (brush_t *)&e->def;
        for ( brush_t *b = (brush_t *)e->brushes.prev; b && b != sentinel; b = b->onext )
        {
            const bool didRepair = KiwiMtl_EnsureBrushLayers( b );
            if ( b->patch )
                KiwiMtl_ReportPatchTessellationCap( b->patch );
            if ( !didRepair )
                continue;
            if ( b->patch ) ++patches;
            else            ++brushes;
        }
    }

    if ( outBrushes ) *outBrushes = brushes;
    if ( outPatches ) *outPatches = patches;
    return brushes + patches;
}

void KiwiMtl_HealMapLayersOnLoad()
{
    int       brushes = 0, patches = 0;
    const int total = KiwiMtl_HealMapLayers( &brushes, &patches );
    if ( !total )
        return;
    Sys_Printf( "Material layers: repaired %i brush(es) and %i patch(es) carrying a missing or "
                "zero-scale lightmap/smoothing layer.  Save to keep the repair.\n",
                brushes, patches );
    MarkMapModified();
}
