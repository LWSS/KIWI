#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Material and texdef writes stay in the browser-apply and Surface Inspector funnels.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_caulk.h"
#include "kiwi_command.h"
#include "kiwi_material.h"  // KiwiMtl_FaceIsInheritable / PickSourceFace / SeedFaceFrom
#include "kiwi_ported.h"
#include "kiwi_uv.h"        // shared availability and gesture handoff
#include "kiwi_selection.h" // mixed-selection split
#include "winding.h"        // Winding_Area

#include <math.h>
#include <string.h>
#include <vector>

// Cross-file entry points.
// texwnd_s is TU-local, so browser materials are exposed through these accessors.
extern int         TexWnd_MaterialCount();                                   // texwnd.cpp
extern qtexture_s *TexWnd_MaterialAt( int idx );                             // texwnd.cpp
extern void        TexWnd_ApplyMaterialAtIndex( int idx );                   // texwnd.cpp
extern void        Brush_AutoCaulk();                                        // csg.cpp (0x47E0F0)
extern void        Brush_SetFaceTexdefSize( const patchMesh_material *material, face_t *f,
                                            brush_t *b );                    // brush.cpp (0x476740)

namespace
{
    // Matches Brush_AutoCaulkFace (0x47E080) and Brush_AutoCaulk (0x47E0F0).
    const char *const KCAULK_NAME = "caulk";

    // Registered names may be bare or folder-qualified.
    const char *LeafName( const char *name )
    {
        const char *slash = strrchr( name, '/' );
        if ( slash )
            return slash + 1;
        const char *back = strrchr( name, '\\' );
        return back ? back + 1 : name;
    }

    // Register a first reference before scanning; pointer identity avoids ambiguous names.
    int CaulkIndex()
    {
        qtexture_s *q = Texture_GetHandle( KCAULK_NAME );
        if ( !q )
            return -1;
        const int n = TexWnd_MaterialCount();
        for ( int i = 0; i < n; ++i )
            if ( TexWnd_MaterialAt( i ) == q )
                return i;
        return -1;
    }
}

bool KiwiCaulk_IsCaulkName( const char *name )
{
    if ( !name || !name[0] )
        return false;
    return _stricmp( LeafName( name ), KCAULK_NAME ) == 0;
}

bool KiwiCaulk_CanExecute()
{
    return KiwiUv_CanEdit();
}

namespace
{
    // The face's current-layer material is caulk.  Materialdef_GetName asserts exactly
    // one active material representation, so check that first.
    bool IsCaulkFace( face_t *f )
    {
        MaterialDef *md = &f->mtldef[g_qeglobals.current_edit_layer];
        if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
            return false;
        return KiwiCaulk_IsCaulkName( (const char *)Materialdef_GetName( md ) );
    }

    bool IsSolid( const selbrush_t *node )
    {
        const brush_t  *def   = node ? node->def : nullptr;
        const entity_s *owner = def ? def->owner : nullptr;
        return def && def->faces && !def->patch
            && !( owner && owner->eclass && owner->eclass->fixedsize );
    }

    // Brush_AutoCaulk skips fixed-size entities and patches.
    int SelectedBrushCount( int *outSolids )
    {
        int n = 0, solids = 0;
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            ++n;
            brush_t  *def   = b->def;
            entity_s *owner = def ? def->owner : nullptr;
            const bool fixed = owner && owner->eclass && owner->eclass->fixedsize;
            if ( def && !def->patch && !fixed )
                ++solids;
        }
        if ( outSolids )
            *outSolids = solids;
        return n;
    }

    // Brush_AutoCaulk reports its count but returns void, so measure around the call.
    int NonCaulkFaceCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            brush_t *def = b->def;
            if ( !def || !def->faces || def->patch )
                continue;
            for ( int fi = 0; fi < def->faceCount; ++fi )
                if ( !IsCaulkFace( &def->faces[fi] ) )
                    ++n;
        }
        return n;
    }

    // The live selected brush faces (patch faces excluded), from the typed selection.
    std::vector<sel_item_t> SelectedBrushFaces( int *outPatchFaces )
    {
        std::vector<sel_item_t> out;
        int patchFaces = 0;
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( it.kind != SEL_FACE || !Sel_ItemValid( it ) || !Sel_BrushLive( it.brush ) )
                continue;
            if ( !IsSolid( it.brush ) )
            {
                ++patchFaces;
                continue;
            }
            if ( it.faceIndex >= 0 && it.faceIndex < it.brush->def->faceCount )
                out.push_back( it );
        }
        if ( outPatchFaces )
            *outPatchFaces = patchFaces;
        return out;
    }

    // Bounds of a winding, grown by `pad`.
    void WindingBounds( const winding_t *w, float pad, float mins[3], float maxs[3] )
    {
        for ( int k = 0; k < 3; ++k )
        {
            mins[k] =  1.0e30f;
            maxs[k] = -1.0e30f;
        }
        for ( int i = 0; i < w->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
            {
                if ( w->p[i][k] < mins[k] ) mins[k] = w->p[i][k];
                if ( w->p[i][k] > maxs[k] ) maxs[k] = w->p[i][k];
            }
        for ( int k = 0; k < 3; ++k )
        {
            mins[k] -= pad;
            maxs[k] += pad;
        }
    }

    bool BoundsTouch( const float *amins, const float *amaxs, const float *bmins, const float *bmaxs )
    {
        for ( int k = 0; k < 3; ++k )
            if ( amins[k] > bmaxs[k] || bmins[k] > amaxs[k] )
                return false;
        return true;
    }

    bool UsableWinding( const winding_t *w )
    {
        return w && w->numpoints >= 3 && w->numpoints <= MAX_POINTS_ON_WINDING;
    }

    // A visible face on ANOTHER brush in the same plane, facing the same way, touching
    // this face: copying it continues the surface seamlessly (CoD4 projects textures from
    // the world axes).  The largest such face wins.
    bool FindFlushNeighbour( const brush_t *def, int fi, const brush_t **outDef, int *outFace )
    {
        const face_t *f = &def->faces[fi];
        if ( !UsableWinding( f->w ) )
            return false;
        float fmins[3], fmaxs[3];
        WindingBounds( f->w, 1.0f, fmins, fmaxs );

        float bestArea = 0.0f;
        selbrush_t *const lists[2] = { &active_brushes, &selected_brushes };
        for ( int l = 0; l < 2; ++l )
            for ( selbrush_t *b = lists[l]->next; b && b != lists[l]; b = b->next )
            {
                const brush_t *g = b->def;
                if ( !g || g == def || !IsSolid( b ) || !BoundsTouch( fmins, fmaxs, g->mins, g->maxs ) )
                    continue;
                for ( int gi = 0; gi < g->faceCount; ++gi )
                {
                    const face_t *gf = &g->faces[gi];
                    const float   d  = gf->plane.normal[0] * f->plane.normal[0]
                                     + gf->plane.normal[1] * f->plane.normal[1]
                                     + gf->plane.normal[2] * f->plane.normal[2];
                    if ( d < 0.9999f || fabsf( gf->plane.dist - f->plane.dist ) > 0.25f )
                        continue;
                    if ( !UsableWinding( gf->w ) || !KiwiMtl_FaceIsInheritable( gf ) )
                        continue;
                    float gmins[3], gmaxs[3];
                    WindingBounds( gf->w, 0.0f, gmins, gmaxs );
                    if ( !BoundsTouch( fmins, fmaxs, gmins, gmaxs ) )
                        continue;
                    const float area = Winding_Area( const_cast<winding_t *>( gf->w ) );
                    if ( area > bestArea )
                    {
                        bestArea = area;
                        *outDef  = g;
                        *outFace = gi;
                    }
                }
            }
        return bestArea > 0.0f;
    }
}

static bool KiwiCaulk_ApplyToSelection()
{
    if ( !KiwiCaulk_CanExecute() )
    {
        Sys_Printf( "Caulk: nothing selected - select brushes or faces first.\n" );
        return true;
    }

    int       nSolids  = 0;
    const int nBrushes = SelectedBrushCount( &nSolids );
    int       nPatchFaces = 0;
    const std::vector<sel_item_t> faces = SelectedBrushFaces( &nPatchFaces );

    const int idx = CaulkIndex();
    if ( idx < 0 )
    {
        Sys_Printf( "Caulk: no \"%s\" material in the registry - this asset set is "
                    "missing main/materials/%s.\n", KCAULK_NAME, KCAULK_NAME );
        return true;
    }

    // Patches, fixed-size entities and curve faces have no brush-face write here: the
    // browser funnel caulks them (one undo record).
    if ( nSolids == 0 && faces.empty() )
    {
        TexWnd_ApplyMaterialAtIndex( idx );
        Sys_Printf( "Caulk: caulked the selected curves / entities (%d item%s).\n",
                    nBrushes + nPatchFaces, nBrushes + nPatchFaces == 1 ? "" : "s" );
        g_nUpdateBits = -1;
        return true;
    }

    int toCaulk = 0;
    for ( size_t i = 0; i < faces.size(); ++i )
        if ( !IsCaulkFace( &faces[i].brush->def->faces[faces[i].faceIndex] ) )
            ++toCaulk;
    if ( nSolids == 0 && toCaulk == 0 )
    {
        Sys_Printf( "Caulk: the selected faces are already caulk.\n" );
        return true;
    }

    // KIWI (2026-10-10, user: "the caulk command requires 2 undo's for some reason"): ONE
    // record - auto-caulk the selected solids' hidden faces and caulk the selected faces
    // inside one bracket (it was AutoCaulk's record, the browser apply's and the caulk fit's).
    // Material only: no fit (user: "you dont need to texture fit the caulking").
    if ( !KiwiUv_EndGestureBeforeApply( "Caulk" ) )
        return true;

    patchMesh_material caulk{};
    SetMaterial( KCAULK_NAME, &caulk );
    const int before = NonCaulkFaceCount();

    KiwiCmd_UndoBegin( "caulk" );           // covers selected_brushes (the solids)
    for ( size_t i = 0; i < faces.size(); ++i )
        KiwiCmd_UndoCoverBrush( faces[i].brush );   // face selections are not in that list

    // Brush_AutoCaulk walks selected_brushes only, so the selected faces' brushes are
    // neither caulked nor counted as occluders by it - the same as the old split.
    if ( nSolids > 0 )
        Brush_AutoCaulk();
    const int autoDid = before - NonCaulkFaceCount();

    int faceDid = 0;
    for ( size_t i = 0; i < faces.size(); ++i )
    {
        brush_t *def = faces[i].brush->def;
        face_t  *f   = &def->faces[faces[i].faceIndex];
        if ( IsCaulkFace( f ) )
            continue;
        Brush_SetFaceTexdefSize( &caulk, f, def );     // material pointers only; bumps version
        ++faceDid;
    }
    KiwiCmd_UndoCommit();

    // Re-sync the face selection's visuals with the bumped brush versions.
    for ( size_t i = 0; i < faces.size(); ++i )
        Brush_CheckBuildFaceVis( faces[i].brush, (const float *)world_orient_matrix );
    if ( autoDid > 0 || faceDid > 0 )
        MarkMapModified();
    KiwiUv_RestoreGestureAfterApply();

    Sys_Printf( "Caulk: %d selected face%s caulked; %d hidden face%s auto-caulked on %d "
                "solid%s.\n", faceDid, faceDid == 1 ? "" : "s", autoDid > 0 ? autoDid : 0,
                autoDid == 1 ? "" : "s", nSolids, nSolids == 1 ? "" : "s" );
    g_nUpdateBits = -1;
    return true;
}

// KIWI (2026-10-10, user: "make an uncaulk on shift-end"): every caulk face of the selected
// solids (or each selected caulk face) takes the material of a flush neighbouring face in the
// same plane - a seamless continuation - else its own brush's largest visible face
// (kiwi_material.h R2, area only).  One undo record.
static bool KiwiCaulk_UncaulkSelection()
{
    if ( !KiwiCaulk_CanExecute() )
    {
        Sys_Printf( "Uncaulk: nothing selected - select brushes or faces first.\n" );
        return true;
    }

    struct target_t
    {
        selbrush_t    *node;
        int            face;
        const brush_t *srcDef;
        int            srcFace;
        bool           flush;
    };
    std::vector<target_t> targets;
    auto addTarget = [&]( selbrush_t *node, int fi )
    {
        for ( size_t i = 0; i < targets.size(); ++i )
            if ( targets[i].node == node && targets[i].face == fi )
                return;
        target_t t = { node, fi, nullptr, -1, false };
        targets.push_back( t );
    };
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( !Sel_ItemValid( it ) || !Sel_BrushLive( it.brush ) || !IsSolid( it.brush ) )
                continue;
            brush_t *def = it.brush->def;
            if ( it.kind == SEL_OBJECT )
            {
                for ( int fi = 0; fi < def->faceCount; ++fi )
                    if ( IsCaulkFace( &def->faces[fi] ) )
                        addTarget( it.brush, fi );
            }
            else if ( it.kind == SEL_FACE && it.faceIndex >= 0 && it.faceIndex < def->faceCount
                   && IsCaulkFace( &def->faces[it.faceIndex] ) )
            {
                addTarget( it.brush, it.faceIndex );
            }
        }
    }
    if ( targets.empty() )
    {
        Sys_Printf( "Uncaulk: no caulk faces in the selection.\n" );
        return true;
    }

    // Resolve every source before writing, so one uncaulked face never feeds another.
    int nFlush = 0, nOwn = 0, nNone = 0;
    for ( size_t i = 0; i < targets.size(); ++i )
    {
        target_t &t = targets[i];
        const brush_t *def = t.node->def;
        if ( FindFlushNeighbour( def, t.face, &t.srcDef, &t.srcFace ) )
        {
            t.flush = true;
            ++nFlush;
            continue;
        }
        const int own = KiwiMtl_PickSourceFace( def, nullptr, -1, -1 );
        if ( own >= 0 )
        {
            t.srcDef  = def;
            t.srcFace = own;
            ++nOwn;
        }
        else
            ++nNone;
    }
    if ( nFlush + nOwn == 0 )
    {
        Sys_Printf( "Uncaulk: %d caulk face%s, but no visible face to copy from - no flush "
                    "neighbour in the same plane and no textured face on the brush.  Apply a "
                    "texture from the browser instead.\n", nNone, nNone == 1 ? "" : "s" );
        return true;
    }

    if ( !KiwiUv_EndGestureBeforeApply( "Uncaulk" ) )
        return true;

    KiwiCmd_UndoBegin( "uncaulk" );
    for ( size_t i = 0; i < targets.size(); ++i )
        if ( targets[i].srcDef )
            KiwiCmd_UndoCoverBrush( targets[i].node );   // self-deduplicating
    for ( size_t i = 0; i < targets.size(); ++i )
    {
        const target_t &t = targets[i];
        if ( !t.srcDef )
            continue;
        brush_t *def = t.node->def;
        KiwiMtl_SeedFaceFrom( &def->faces[t.face], t.srcDef, t.srcFace );   // material + texdef
        ++def->version;
    }
    KiwiCmd_UndoCommit();

    for ( size_t i = 0; i < targets.size(); ++i )
        if ( targets[i].srcDef )
            Brush_CheckBuildFaceVis( targets[i].node, (const float *)world_orient_matrix );
    MarkMapModified();
    KiwiUv_RestoreGestureAfterApply();

    Sys_Printf( "Uncaulk: %d face%s textured - %d from a flush neighbour, %d from %s own "
                "brush%s.\n", nFlush + nOwn, nFlush + nOwn == 1 ? "" : "s", nFlush, nOwn,
                nOwn == 1 ? "its" : "their", nNone
                    ? " (some had nothing to copy from and stay caulk)" : "" );
    g_nUpdateBits = -1;
    return true;
}

void KiwiCaulk_RegisterCommands()
{
    // Modern: End caulks, Shift+End uncaulks; View->Center moves to Ctrl+End.
    // Classic stays unchanged.
    Radiant_RegisterCommand( "KiwiCaulkSelection",   0, 0, KIWI_CMD_CAULK_FACES );
    Radiant_RegisterCommand( "KiwiUncaulkSelection", 0, 0, KIWI_CMD_UNCAULK_FACES );
}

bool KiwiCaulk_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == (unsigned int)KIWI_CMD_CAULK_FACES )
        return KiwiCaulk_ApplyToSelection();
    if ( cmdId == (unsigned int)KIWI_CMD_UNCAULK_FACES )
        return KiwiCaulk_UncaulkSelection();
    return false;
}
