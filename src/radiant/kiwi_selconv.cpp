#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Ctrl+1..4 convert typed selection without mutating geometry.
// Results cross into the legacy stores only through Sel_SyncToLegacy; the header
// documents KIWI's deliberate differences from Plasticity.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_selconv.h"
#include "kiwi_command.h"
#include "kiwi_selection.h"

#include <math.h>
#include <vector>

extern int  Sys_Printf( const char *fmt, ... );   // win_qe3.cpp
extern int  g_nUpdateBits;                        // 0x25D5A74 (mainfrm.cpp)

extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // Match the ported FindPoint/SetupVertexSelection tolerance so winding
    // references to one world-space corner deduplicate to one selection item.
    const float KSC_TOL = 0.1f;

    inline bool PointNear( const float *a, const float *b )
    {
        return fabsf( a[0] - b[0] ) <= KSC_TOL
            && fabsf( a[1] - b[1] ) <= KSC_TOL
            && fabsf( a[2] - b[2] ) <= KSC_TOL;
    }

    winding_t *WindingOf( const selbrush_t *b, int faceIndex )
    {
        if ( !b || !b->def || !b->def->faces )
            return 0;
        if ( faceIndex < 0 || faceIndex >= b->def->faceCount )
            return 0;
        winding_t *w = b->def->faces[faceIndex].w;
        if ( w && ( w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING ) )
            return 0;
        return w;
    }

    patchMesh_t *PatchOf( const selbrush_t *b )
    {
        if ( !b || !b->patch || !b->def )
            return 0;
        patchMesh_t *pm = b->def->patch;
        if ( !Patch_DimsSane( pm ) )
            return 0;
        return pm;
    }

    // The outer conversion validates each source item before dereference; items
    // built during the walk can skip repeated liveness scans in geometry helpers.

    // Deduplicated conversion output.
    struct outSet_t
    {
        std::vector<sel_item_t> items;

        void AddObject( selbrush_t *b )
        {
            if ( !b )
                return;
            for ( size_t i = 0; i < items.size(); ++i )
                if ( items[i].brush == b )
                    return;
            items.push_back( Sel_MakeObject( b ) );
        }

        void AddFace( selbrush_t *b, int faceIndex )
        {
            for ( size_t i = 0; i < items.size(); ++i )
                if ( items[i].brush == b && items[i].faceIndex == faceIndex )
                    return;
            items.push_back( Sel_MakeFace( b, faceIndex ) );
        }

        // Adjacent face windings reverse a shared edge, so compare unordered endpoints.
        void AddEdge( selbrush_t *b, int faceIndex, int edgeIndex,
                      const float *a, const float *c )
        {
            for ( size_t i = 0; i < items.size(); ++i )
            {
                if ( !items[i].brush || items[i].brush->def != b->def )
                    continue;
                float ea[3], eb[3];
                if ( !Sel_EdgeEnds( items[i], ea, eb, false ) )
                    continue;
                if ( ( PointNear( ea, a ) && PointNear( eb, c ) )
                  || ( PointNear( ea, c ) && PointNear( eb, a ) ) )
                    return;
            }
            items.push_back( Sel_MakeEdge( b, faceIndex, edgeIndex ) );
        }

        // Winding faces repeat physical corners; deduplicate by definition and position.
        void AddVertex( selbrush_t *b, int faceIndex, int vertIndex, const float *p )
        {
            for ( size_t i = 0; i < items.size(); ++i )
            {
                if ( !items[i].brush || items[i].brush->def != b->def )
                    continue;
                float q[3];
                if ( !Sel_ItemWorldPos( items[i], q, false ) )
                    continue;
                if ( PointNear( p, q ) )
                    return;
            }
            items.push_back( Sel_MakeVertex( b, faceIndex, vertIndex ) );
        }
    };

    // Object conversions.
    void ObjectToPoints( selbrush_t *b, outSet_t &out )
    {
        if ( patchMesh_t *pm = PatchOf( b ) )
        {
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                {
                    const int idx = col * pm->height + row;
                    sel_item_t it = Sel_MakePatchPoint( b, idx );
                    float p[3];
                    if ( Sel_ItemWorldPos( it, p, false ) )
                        out.AddVertex( b, -1, idx, p );
                }
            return;
        }
        if ( !b->def || !b->def->faces )
            return;
        for ( int f = 0; f < b->def->faceCount; ++f )
        {
            winding_t *w = WindingOf( b, f );
            if ( !w )
                continue;
            for ( int i = 0; i < w->numpoints; ++i )
                out.AddVertex( b, f, i, w->p[i] );
        }
    }

    void ObjectToEdges( selbrush_t *b, outSet_t &out )
    {
        if ( b->patch || !b->def || !b->def->faces )
            return;                       // a patch has no winding edges here
        for ( int f = 0; f < b->def->faceCount; ++f )
        {
            winding_t *w = WindingOf( b, f );
            if ( !w || w->numpoints < 2 )
                continue;
            for ( int e = 0; e < w->numpoints; ++e )
                out.AddEdge( b, f, e, w->p[e], w->p[( e + 1 ) % w->numpoints] );
        }
    }

    void ObjectToFaces( selbrush_t *b, outSet_t &out )
    {
        if ( b->patch || !b->def || !b->def->faces )
            return;                       // a patch has no plane faces
        for ( int f = 0; f < b->def->faceCount; ++f )
            if ( WindingOf( b, f ) )
                out.AddFace( b, f );
    }

    // Face conversions.
    void FaceToPoints( const sel_item_t &it, outSet_t &out )
    {
        winding_t *w = WindingOf( it.brush, it.faceIndex );
        if ( !w )
            return;
        for ( int i = 0; i < w->numpoints; ++i )
            out.AddVertex( it.brush, it.faceIndex, i, w->p[i] );
    }

    // Plasticity's face2edge uses the outer loop; for a brush that is every winding edge.
    void FaceToEdges( const sel_item_t &it, outSet_t &out )
    {
        winding_t *w = WindingOf( it.brush, it.faceIndex );
        if ( !w || w->numpoints < 2 )
            return;
        for ( int e = 0; e < w->numpoints; ++e )
            out.AddEdge( it.brush, it.faceIndex, e, w->p[e], w->p[( e + 1 ) % w->numpoints] );
    }

    // Edge and vertex conversions.
    void EdgeToPoints( const sel_item_t &it, outSet_t &out )
    {
        winding_t *w = WindingOf( it.brush, it.faceIndex );
        if ( !w || it.edgeIndex < 0 || it.edgeIndex >= w->numpoints )
            return;
        const int j = ( it.edgeIndex + 1 ) % w->numpoints;
        out.AddVertex( it.brush, it.faceIndex, it.edgeIndex, w->p[it.edgeIndex] );
        out.AddVertex( it.brush, it.faceIndex, j,            w->p[j] );
    }

    // Plasticity uses adjacency links unavailable here; match both endpoints
    // against each winding at the shared 0.1-world-unit tolerance instead.
    void EdgeToFaces( const sel_item_t &it, outSet_t &out )
    {
        float a[3], b[3];
        if ( !Sel_EdgeEnds( it, a, b, false ) )
            return;
        brush_t *def = it.brush->def;
        if ( !def || !def->faces )
            return;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            winding_t *w = WindingOf( it.brush, f );
            if ( !w )
                continue;
            bool has0 = false, has1 = false;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                if ( PointNear( w->p[i], a ) ) has0 = true;
                if ( PointNear( w->p[i], b ) ) has1 = true;
            }
            if ( has0 && has1 )
                out.AddFace( it.brush, f );
        }
    }

    void VertexToFaces( const sel_item_t &it, outSet_t &out )
    {
        float p[3];
        if ( !Sel_ItemWorldPos( it, p, false ) || it.brush->patch )
            return;
        brush_t *def = it.brush->def;
        if ( !def || !def->faces )
            return;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            winding_t *w = WindingOf( it.brush, f );
            if ( !w )
                continue;
            for ( int i = 0; i < w->numpoints; ++i )
                if ( PointNear( w->p[i], p ) )
                {
                    out.AddFace( it.brush, f );
                    break;
                }
        }
    }

    // KIWI adds the reciprocal vertex-to-edge conversion.
    void VertexToEdges( const sel_item_t &it, outSet_t &out )
    {
        float p[3];
        if ( !Sel_ItemWorldPos( it, p, false ) || it.brush->patch )
            return;
        brush_t *def = it.brush->def;
        if ( !def || !def->faces )
            return;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            winding_t *w = WindingOf( it.brush, f );
            if ( !w || w->numpoints < 2 )
                continue;
            for ( int e = 0; e < w->numpoints; ++e )
            {
                const int j = ( e + 1 ) % w->numpoints;
                if ( PointNear( w->p[e], p ) || PointNear( w->p[j], p ) )
                    out.AddEdge( it.brush, f, e, w->p[e], w->p[j] );
            }
        }
    }

    // Conversion dispatch.
    const char *KindWord( sel_kind_t k )
    {
        return ( k == SEL_VERTEX ) ? "points"
             : ( k == SEL_EDGE   ) ? "edges"
             : ( k == SEL_FACE   ) ? "faces" : "objects";
    }

    void Convert( sel_kind_t to )
    {
        selection_t &sel = KiwiSel();
        if ( sel.items.empty() )
        {
            Sys_Printf( "Convert selection: nothing is selected.\n" );
            return;
        }

        outSet_t out;
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( !Sel_BrushLive( it.brush ) )
                continue;                     // never deref a freed node

            switch ( to )
            {
            case SEL_VERTEX:
                if      ( it.kind == SEL_OBJECT ) ObjectToPoints( it.brush, out );
                else if ( it.kind == SEL_FACE   ) FaceToPoints( it, out );
                else if ( it.kind == SEL_EDGE   ) EdgeToPoints( it, out );
                else
                {
                    float p[3];
                    if ( Sel_ItemWorldPos( it, p, false ) )
                        out.AddVertex( it.brush, it.faceIndex, it.vertIndex, p );
                }
                break;

            case SEL_EDGE:
                if      ( it.kind == SEL_OBJECT ) ObjectToEdges( it.brush, out );
                else if ( it.kind == SEL_FACE   ) FaceToEdges( it, out );
                else if ( it.kind == SEL_VERTEX ) VertexToEdges( it, out );
                else
                {
                    float a[3], b[3];
                    if ( Sel_EdgeEnds( it, a, b, false ) )
                        out.AddEdge( it.brush, it.faceIndex, it.edgeIndex, a, b );
                }
                break;

            case SEL_FACE:
                if      ( it.kind == SEL_OBJECT ) ObjectToFaces( it.brush, out );
                else if ( it.kind == SEL_EDGE   ) EdgeToFaces( it, out );
                else if ( it.kind == SEL_VERTEX ) VertexToFaces( it, out );
                else if ( WindingOf( it.brush, it.faceIndex ) )
                    out.AddFace( it.brush, it.faceIndex );
                break;

            default:                          // SEL_OBJECT
                out.AddObject( it.brush );
                break;
            }
        }

        if ( out.items.empty() )
        {
            // Patches have no winding edges or plane faces. Keep an empty conversion
            // because selection changes have no undo record.
            Sys_Printf( "Convert selection: nothing in the selection converts to %s "
                        "(patches have no faces or edges) — selection kept.\n",
                        KindWord( to ) );
            return;
        }

        const int before = (int)sel.items.size();
        Sel_Clear( sel );
        for ( size_t i = 0; i < out.items.size(); ++i )
            Sel_Add( sel, out.items[i] );
        // Sel_Add leaves the last item active; preserve the first source-derived
        // item as the operation anchor.
        sel.active = out.items[0];

        // Unlike Plasticity, KIWI makes the picker mode follow a successful
        // conversion so the next click does not discard the converted kind.
        KiwiSel_SetModeMask( SEL_KIND_BIT( to ) );

        Sel_SyncToLegacy();
        g_nUpdateBits |= ( W_CAMERA | W_XY | W_Z );
        Sys_Printf( "Convert selection: %d item%s -> %d %s\n",
                    before, ( before == 1 ) ? "" : "s",
                    (int)out.items.size(), KindWord( to ) );
    }
}

bool KiwiSelConv_CanConvert()
{
    return !KiwiSel().items.empty();
}

// Classic registration is unbound; the modern keymap supplies Ctrl+1..4.
void KiwiSelConv_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiConvertToPoints",  0, 0, KIWI_CMD_SELCONV_POINT );
    Radiant_RegisterCommand( "KiwiConvertToEdges",   0, 0, KIWI_CMD_SELCONV_EDGE );
    Radiant_RegisterCommand( "KiwiConvertToFaces",   0, 0, KIWI_CMD_SELCONV_FACE );
    Radiant_RegisterCommand( "KiwiConvertToObjects", 0, 0, KIWI_CMD_SELCONV_OBJECT );
}

bool KiwiSelConv_DispatchInstant( unsigned int commandId )
{
    switch ( commandId )
    {
    case KIWI_CMD_SELCONV_POINT:  Convert( SEL_VERTEX ); return true;
    case KIWI_CMD_SELCONV_EDGE:   Convert( SEL_EDGE );   return true;
    case KIWI_CMD_SELCONV_FACE:   Convert( SEL_FACE );   return true;
    case KIWI_CMD_SELCONV_OBJECT: Convert( SEL_OBJECT ); return true;
    default:                      return false;
    }
}
