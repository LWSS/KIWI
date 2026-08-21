#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_material.cpp — ROUND T implementation.  See kiwi_material.h for the six
// rules, the two user reports this one file answers, and the exact broken link
// (R6) that made the round-Q patch fillets invisible.
//
// NEW code over the ported cores.  It computes no material and registers no
// asset: every read goes through the ported MaterialDef accessors and the only
// write is a struct copy plus the ported Materialdef_Realize.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "winding.h"

#include "kiwi_material.h"
#include "kiwi_pick.h"                 // ROUND Y: Pick / Pick_RayFromCursor / ray_t
#include "kiwi_selection.h"            // ROUND Y: sel_item_t, SEL_MASK_FACE, Sel_BrushLive
#include "kiwi_str.h"                // KIWI-UX (CLEANUP, C-66): KiwiStr_ContainsNoCase

#include <math.h>
#include <string.h>

// ── ported entry points (each verified against its DEFINITION) ──────────────
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );          // materialdef.cpp:159 (0x431640)
extern bool              Materialdef_Realize( MaterialDef *md );              // materialdef.cpp:125
extern float             Winding_Area( winding_t *w );                        // winding.cpp (winding.h:78)
extern int               Sys_Printf( const char *fmt, ... );                  // win_qe3.cpp

// xywnd.cpp:2233 — the CLIPPER's caulk / nodraw_decal synthesis, the // KIWI-UX
// forwarder shakeout G hoisted out of Ed_ProduceSplitLists.  R3's fallback.
extern void              Ed_BuildClipFaceMaterial_Kiwi( face_t *out, const brush_t *src );

namespace
{
    // ── R1: the TOOL family, by name ────────────────────────────────────────
    // Substrings, case-insensitive, matched anywhere in the material name — the
    // same shape `MtlDef_IsFaceFiltered` uses for the texture filter
    // (mayaexport.cpp:112, `strstr(name, key)` over the whole map), so a
    // "tools/caulk" path and a bare "caulk" classify identically without this
    // file needing to know CoD4's naming layout.
    //
    // `$default` is NOT here on purpose: it is what an untextured brush wears,
    // it draws (round O substituted the 3D variant for exactly that reason), and
    // refusing to inherit it would send every fresh box's cut faces to caulk.
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

    // KIWI-UX (CLEANUP, C-66): was a third ContainsNoCase, differing from
    // kiwi_entbrowser.cpp's by answering FALSE for an empty needle.  Unreachable
    // either way here — KMTL_TOOL_NAMES are non-empty literals and IsToolName
    // guards its haystack — so it folds into KiwiStr_ContainsNoCase (kiwi_str.h)
    // with no behaviour change.  _strnicmp is locale-sensitive; the shared body
    // folds ASCII only, which is what a material path wants.

    // The channel-0 MaterialDef of a face, as the ported accessors want it
    // (non-const: Materialdef_GetName's own signature is non-const, and it is a
    // pure read — the cast is the call convention, not a mutation).
    MaterialDef *Channel0( const face_t *f )
    {
        return f ? const_cast<MaterialDef *>( &f->mtldef[0] ) : 0;
    }

    // MtlDef_IsValid's invariant, restated locally so this file can ASK rather
    // than assert: exactly one of the two handles is set.  A half-built face
    // (both NULL) would trip Materialdef_GetName's own iassert, and this file
    // runs on paths where the honest answer is "no material" rather than a stop.
    bool HasMaterial( const MaterialDef *m )
    {
        return m && ( ( m->lyrMtl != 0 ) + ( m->radMtl != 0 ) == 1 );
    }

    // ── R1, and the accessor it reads through ───────────────────────────────
    // KIWI-UX (CLEANUP, C-69): both of these were EXPORTED (kiwi_material.h) and
    // nothing outside this file ever called either — `KiwiMtl_FaceIsInheritable`
    // below is the entry point every caller actually uses.  They are file-local
    // now; the R1 rule they implement is still documented on that function.
    //
    // Is `name` a TOOL material (caulk / nodraw / clip / hint / skip / portal /
    // origin / trigger / lightgrid / anything under a "tools" path)?  NULL and
    // the empty string answer true — an unnamed material is not something to
    // inherit.
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

// ═════════════════════════════════════════════════════════════════════════════
//  R1
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiMtl_FaceIsInheritable( const face_t *f )
{
    const char *name = FaceMaterialName( f );
    return name && !IsToolName( name );
}

// ═════════════════════════════════════════════════════════════════════════════
//  R2 / R5 — which face to inherit from
// ═════════════════════════════════════════════════════════════════════════════
int KiwiMtl_PickSourceFace( const brush_t *def, const float cutNormal[3],
                            int prefer0, int prefer1 )
{
    if ( !def || !def->faces || def->faceCount <= 0 )
        return -1;

    // R5: the caller's preferences first, IN ORDER.  A chamfer wants its own
    // corner's material when that corner has one, and only falls through to the
    // brush-wide scan when neither adjacent face is inheritable.
    const int prefer[2] = { prefer0, prefer1 };
    for ( int i = 0; i < 2; ++i )
    {
        const int fi = prefer[i];
        if ( fi < 0 || fi >= def->faceCount )
            continue;
        if ( KiwiMtl_FaceIsInheritable( &def->faces[fi] ) )
            return fi;
    }

    // R2: score = area * (1 - |n_f · n_cut|), maximised over inheritable faces.
    // With no cut normal the perpendicularity term is 1 for every face and the
    // scan degenerates to "the largest inheritable face", which is the right
    // reading of "the brush's dominant material".
    int   best      = -1;
    float bestScore = -1.0f;
    for ( int f = 0; f < def->faceCount; ++f )
    {
        const face_t *fc = &def->faces[f];
        if ( !KiwiMtl_FaceIsInheritable( fc ) )
            continue;

        // Area, from the winding when there is one.  A face whose winding was
        // clipped away entirely still carries a material and still counts — at
        // the floor area below, so it loses every tie to a face with a surface.
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
            // A face exactly PARALLEL to the cut contributes nothing under the
            // formula and would be unreachable even when it is the only
            // inheritable face on the brush.  Floor it so "some material" always
            // beats "no material"; the ordering between real candidates is
            // untouched because the floor is far below any real perp term.
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
    return best;                            // -1 = R3, the brush is all tool faces
}

// ═════════════════════════════════════════════════════════════════════════════
//  the copy + R6
// ═════════════════════════════════════════════════════════════════════════════
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
    // The three channels are contiguous {lyrMtl, radMtl} pairs at +0x18 / +0x20 /
    // +0x28 (qe3.h patchMesh_t), which is exactly how the ported draw walks them
    // (`&inst->def->texture + layer`, pmesh.cpp:9896).  Same walk here.
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
        out->mtldef[c] = src->mtldef[c];    // the pointer pair AND the texdef
    KiwiMtl_RealizeFace( out );
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  the cut/split entry point (R2 + R3)
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiMtl_SeedClipFace( face_t *out, const brush_t *def, const float cutNormal[3] )
{
    if ( !out || !def )
        return false;

    const int src = KiwiMtl_PickSourceFace( def, cutNormal, -1, -1 );
    if ( src >= 0 && KiwiMtl_SeedFaceFrom( out, def, src ) )
        return true;

    // R3 — an all-tool brush stays a tool brush.  The classic synthesis, byte for
    // byte, through the same forwarder the clipper itself calls.
    Ed_BuildClipFaceMaterial_Kiwi( out, def );
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND Y, ITEM 1 — "KiwiMatInfo", the permanent material-state readout
// ═════════════════════════════════════════════════════════════════════════════
// See kiwi_material.h for the report this answers and for the field list.  The
// DECODE is camwnd.cpp's (KiwiMtl_Diagnose) because that file owns the
// substitution predicate; this side only picks the subjects and formats.
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

    // 1. THE TEMPLATE.  random_texture_stuff[layer] is what every new brush face
    //    is memcpy'd from (brush.cpp:7664, :3426, :3662 ...), so it is the single
    //    most useful thing to know when new geometry draws wrong.
    {
        const int layer = g_qeglobals.current_edit_layer;
        const MaterialDef *md = ( layer >= 0 && layer < 3 )
                              ? &g_qeglobals.random_texture_stuff[layer].mtl : 0;
        Material *h = ( md && md->radMtl ) ? md->radMtl->handle : 0;
        if ( md && md->radMtl && md->radMtl->name )
            Sys_Printf( "  template name      \"%s\"\n", md->radMtl->name );
        PrintOne( "template", h );
    }

    // 2. THE FACE UNDER THE CURSOR.  SEL_MASK_FACE alone, exactly as the snap
    //    accents do (kiwi_snap.cpp KiwiSnap_DrawFaceAccents), so the answer is
    //    about the surface the user is looking at rather than about the mode.
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
