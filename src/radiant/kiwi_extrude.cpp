#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Region and face extrusion build ordinary brushes through the ported creation
// sequence; this file owns profile math, cursor mapping, preview, and validation.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
// Filled previews use the same triangle-command path as region and split overlays.
#include <gfx_d3d/r_gfx.h>          // GfxColor, Byte4PackPixelColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT,
                                    // R_AddRenderCmdDrawTris, R_AddCmdSetMaterialColor

#include "kiwi_extrude.h"
#include "kiwi_boxselect.h"             // KiwiBox_ClickSelectAt
#include "kiwi_camera.h"                // KiwiCam_AxisPortrayable
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_grid.h"
#include "kiwi_hover.h"                 // hovered-face arm
#include "kiwi_lines.h"
#include "kiwi_lollipop.h"              // KiwiLollipop_FaceSide (the handle's display side)
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_region.h"
#include "kiwi_selection.h"             // selected-face arm
#include "kiwi_snap.h"
#include "kiwi_transform.h"             // KiwiXform_PushFaceOnce (negative E)
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // Dot3/Sub3/...

#include <math.h>
#include <stdarg.h>
#include <stdint.h>     // uint16_t index buffers
#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points; addresses identify mirrored ordering and ABI contracts.
extern int         Sys_Printf( const char *fmt, ... );                     // win_qe3.cpp
extern camera_s   *Ed_Camera();                                            // camwnd.cpp
extern int         g_nUpdateBits;                                          // 0x25D5A74 (mainfrm.cpp)

// Poll the physical Ctrl key like the ported drag paths; command state cannot
// safely latch a key whose messages are handled elsewhere.
bool KiwiExt_AbsoluteHeld()
{
    return ( ::GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) != 0;
}

// One-axis snap ladder: aimed named geometry, face-plane magnet, then hard lattice.
// Geometry must precede the lattice, whose nearest answer is always within half a cell.
float KiwiExt_LadderDepth( const snap_result_t &snap, const float *ref,
                           const float *axis, float rawAbs, bool *outMajor )
{
    if ( outMajor )
        *outMajor = false;
    if ( !ref || !axis )
        return rawAbs;

    // Resolve geometry onto this gesture's axis and reject source-plane depth.
    float sd = 0.0f;
    const bool haveGeom = snap.valid && KiwiSnap_IsGeometry( snap.type )
                       && KiwiSnap_AxisDepth( snap, ref, axis, &sd )
                       && fabsf( sd ) >= KEXT_SELF_SNAP_BAND;
    if ( haveGeom )
    {
        if ( snap.type != SNAP_FACE )
            return sd;                     // a place the user aimed at: never rounded

        float at[3];
        for ( int k = 0; k < 3; ++k )
            at[k] = ref[k] + axis[k] * rawAbs;
        // Area hits are magnets near the cursor depth, not remote teleports.
        if ( KiwiSnap_AreaMagnet( rawAbs, sd, at ) == sd )
            return sd;
    }

    // Final rung: absolute world-axis lattice with major-line preference.
    bool major = false;
    const float ld = KiwiSnap_LatticeAxis( rawAbs, ref, axis, true, &major );
    if ( outMajor )
        *outMajor = major;
    return ld;
}

extern brush_t    *Brush_Alloc( const void *materialDefSrc, eclass_t *ecls ); // brush.cpp 0x4751e0
extern selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner );          // brush.cpp 0x475980
extern void        Brush_AddToList2( selbrush_t *b );                         // brush.cpp 0x4765a0
extern void        Brush_Free_R( brush_t *def );                              // brush.cpp 0x475af0
extern void        Entity_LinkBrush( brush_t *b, entity_s *world_ent );       // entity.cpp 0x484fc0
extern void        Select_Deselect( int bAlsoFreeFaces );                     // select.cpp 0x48E800 (int, NOT char — mangling)
extern entity_s   *world_entity;                                              // entity.cpp 0x25D5B30

// brush.cpp/xywnd.cpp forwarders used by the ported creation sequence.
extern void Ed_BrushSetFaceCount( brush_t *def, int faceCount );
extern void Ed_EnsureCurrentMaterial_Kiwi();

// These declarations must match the other translucent-overlay users exactly;
// signature drift is a link failure.
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    // Static because the numeric layer copies the field but not its label string.
    const kiwiNumField_t KEXT_FIELDS[1] = { { "length", KNUM_LENGTH, false } };

    const float KEXT_COL_PREVIEW[3] = { 0.55f, 0.85f, 1.00f };   // the prism outline
    const float KEXT_COL_BAD[3]     = { 1.00f, 0.30f, 0.25f };
    // Carve is an ordinary reshape; doomed uses the same warning red as push-through delete.
    const float KEXT_COL_CARVE[3]   = { 1.00f, 0.72f, 0.35f };
    const float KEXT_COL_DOOMED[3]  = { 0.60f, 0.13f, 0.13f };

    // The preview and commit must compare the same exported push-through epsilon.
    const float KEXT_PUSH_EPS = KXPUSH_EPS;

    enum kiwiExtrudeFaceMode_t
    {
        KEXTF_IDLE = 0,   // |distance| too small to mean anything
        KEXTF_GROW,       // OUT: a new body off the face winding (the original verb)
        KEXTF_CARVE,      // IN : push the SOURCE face inward — the brush shrinks
        KEXTF_DESTROY,    // IN, all the way through: the brush is removed
    };



    // `loop` is CCW plane-space data; offsets are world units along plane.normal.
    // Planepts are wound so Face_MakePlane's cross product points outward.
    // Sorting signed endpoints into lo/hi keeps one side-face winding rule.
    struct prismBuild_t
    {
        kconPlane_t plane;
        float       lo, hi;              // offsets along plane.normal
    };

    void PrismVertex( const prismBuild_t &pb, const float uv[2], bool high, float out[3] )
    {
        float base[3];
        KiwiCon_PlaneToWorld( pb.plane, uv, base );
        Mad3( base, pb.plane.normal, high ? pb.hi : pb.lo, out );
    }

    // Filled previews draw translucent side quads and the moving cap before their wireframe.
    // kiwi_lines forces opaque alpha, so surfaces use the render-command triangle path.
    // `idx` is mutable because every triangle is eye-oriented against backface culling.
    void EmitFilledTris( float *xyzw, int vertCount,
                         uint16_t *idx, int idxCount, const float rgba[4] )
    {
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        const camera_s *cam = Ed_Camera();
        if ( !cam || vertCount < 3 || idxCount < 3 )
            return;

        enum { KEXT_PREVIEW_MAX_VERTS = 4 * ( KEXT_MAX_PROFILE + 1 ) };
        if ( vertCount > KEXT_PREVIEW_MAX_VERTS )
            return;

        static float nrm[KEXT_PREVIEW_MAX_VERTS][3];
        static float st [KEXT_PREVIEW_MAX_VERTS][2];
        static float col[KEXT_PREVIEW_MAX_VERTS];

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        for ( int i = 0; i < vertCount; ++i )
        {
            // Use a constant world normal: a camera vector would make brightness view-dependent.
            KiwiTris_FillNormal( nrm[i] );
            st[i][0]  = 0.0f;
            st[i][1]  = 0.0f;
            col[i]    = packedAsFloat;
        }

        // Eye-orient each triangle so both halves of the closed prism survive backface culling.
        KiwiTris_OrientToEye( xyzw, 4, idx, idxCount, cam->origin );

        R_AddCmdSetMaterialColor( s_neutral );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)idxCount, idx, (short)vertCount,
                                (float (*)[4])xyzw, nrm, col, st );
        R_AddCmdSetMaterialColor( s_white );
    }

    // `loopUV` is in pb.plane coordinates; capTris indexes that profile.
    // Cap and sides use separate draws so the moving cap can be more prominent.
    // The white material backface-culls, so EmitFilledTris eye-orients each triangle.
    void EmitPrismSolid( const prismBuild_t &pb, const std::vector<float> &loopUV,
                         const std::vector<int> &capTris, const float rgb[3] )
    {
        const int n = (int)( loopUV.size() / 2 );
        if ( n < 3 || n > KEXT_MAX_PROFILE )
            return;
        if ( !( pb.hi - pb.lo > 1.0e-4f ) )
            return;                          // no depth: there is no solid to show

        // The sign chooses the moving cap; MakeBuild places the source at the other endpoint.
        const bool capHigh = !( pb.lo < 0.0f );
        {
            static float xyzw[KEXT_MAX_PROFILE][4];
            static uint16_t idx[( KEXT_MAX_PROFILE - 2 ) * 3];
            if ( (int)capTris.size() >= 3
              && (int)capTris.size() <= ( KEXT_MAX_PROFILE - 2 ) * 3 )
            {
                for ( int i = 0; i < n; ++i )
                {
                    float w[3];
                    PrismVertex( pb, &loopUV[(size_t)i * 2], capHigh, w );
                    xyzw[i][0] = w[0]; xyzw[i][1] = w[1]; xyzw[i][2] = w[2];
                    xyzw[i][3] = 1.0f;
                }
                for ( size_t k = 0; k < capTris.size(); ++k )
                    idx[k] = (uint16_t)capTris[k];
                const float capRgba[4] = { rgb[0], rgb[1], rgb[2], 0.34f };
                EmitFilledTris( &xyzw[0][0], n, idx, (int)capTris.size(), capRgba );
            }
        }

        // Batch all side quads to avoid reopening material-color state per edge.
        {
            static float    xyzw[4 * KEXT_MAX_PROFILE][4];
            static uint16_t idx [6 * KEXT_MAX_PROFILE];
            int v = 0, t = 0;
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float p[4][3];
                PrismVertex( pb, &loopUV[(size_t)i * 2], false, p[0] );
                PrismVertex( pb, &loopUV[(size_t)j * 2], false, p[1] );
                PrismVertex( pb, &loopUV[(size_t)j * 2], true,  p[2] );
                PrismVertex( pb, &loopUV[(size_t)i * 2], true,  p[3] );
                const int base = v;
                for ( int k = 0; k < 4; ++k, ++v )
                {
                    xyzw[v][0] = p[k][0]; xyzw[v][1] = p[k][1]; xyzw[v][2] = p[k][2];
                    xyzw[v][3] = 1.0f;
                }
                idx[t++] = (uint16_t)( base + 0 );
                idx[t++] = (uint16_t)( base + 1 );
                idx[t++] = (uint16_t)( base + 2 );
                idx[t++] = (uint16_t)( base + 0 );
                idx[t++] = (uint16_t)( base + 2 );
                idx[t++] = (uint16_t)( base + 3 );
            }
            const float sideRgba[4] = { rgb[0], rgb[1], rgb[2], 0.20f };
            EmitFilledTris( &xyzw[0][0], v, idx, t, sideRgba );
        }
    }

    // Wire helper for a complete prism: both cap rings and every vertical.
    void EmitPrismWire( const prismBuild_t &pb, const std::vector<float> &loopUV )
    {
        const int n = (int)( loopUV.size() / 2 );
        for ( int i = 0; i < n; ++i )
        {
            const int j = ( i + 1 ) % n;
            float a[3], b[3];
            PrismVertex( pb, &loopUV[(size_t)i * 2], true,  a );
            PrismVertex( pb, &loopUV[(size_t)j * 2], true,  b );
            if ( !KiwiLines_Add( a, b ) )
                return;
            PrismVertex( pb, &loopUV[(size_t)i * 2], false, a );
            PrismVertex( pb, &loopUV[(size_t)j * 2], false, b );
            if ( !KiwiLines_Add( a, b ) )
                return;
            PrismVertex( pb, &loopUV[(size_t)i * 2], false, a );
            PrismVertex( pb, &loopUV[(size_t)i * 2], true,  b );
            if ( !KiwiLines_Add( a, b ) )
                return;
        }
    }

    inline float Cross2( const float *o, const float *a, const float *b )
    {
        return ( a[0] - o[0] ) * ( b[1] - o[1] ) - ( a[1] - o[1] ) * ( b[0] - o[0] );
    }

    // Pick well-spread cap planepts in ascending ring order. Ascending order preserves
    // CCW winding; spread avoids near-collinear triples on dense profiles.
    bool CapTriple( const std::vector<float> &loopUV, const std::vector<int> &ring,
                    int *o0, int *o1, int *o2 )
    {
        const int n = (int)ring.size();
        if ( n < 3 )
            return false;
        const float *p0 = &loopUV[(size_t)ring[0] * 2];

        int   i1 = -1;
        float best = 0.0f;
        for ( int k = 1; k < n; ++k )
        {
            const float *p = &loopUV[(size_t)ring[k] * 2];
            const float d = ( p[0] - p0[0] ) * ( p[0] - p0[0] )
                          + ( p[1] - p0[1] ) * ( p[1] - p0[1] );
            if ( i1 < 0 || d > best ) { best = d; i1 = k; }
        }
        if ( i1 < 0 || best < 1.0e-6f )
            return false;

        int   i2 = -1;
        float bestArea = 0.0f;
        const float *p1 = &loopUV[(size_t)ring[i1] * 2];
        for ( int k = 1; k < n; ++k )
        {
            if ( k == i1 )
                continue;
            const float a = fabsf( Cross2( p0, p1, &loopUV[(size_t)ring[k] * 2] ) );
            if ( i2 < 0 || a > bestArea ) { bestArea = a; i2 = k; }
        }
        if ( i2 < 0 || bestArea < 1.0e-4f )
            return false;                    // every vertex is collinear — no plane

        int t[3] = { 0, i1, i2 };
        for ( int a = 0; a < 2; ++a )
            for ( int b = a + 1; b < 3; ++b )
                if ( t[b] < t[a] ) { const int s = t[a]; t[a] = t[b]; t[b] = s; }
        *o0 = t[0]; *o1 = t[1]; *o2 = t[2];
        return true;
    }

    // Remove zero-length and same-direction collinear edges before writing side planes;
    // otherwise they produce degenerate or duplicate planes. Use the validity gate's
    // tolerance and iterate to a fixed point because removal can expose another pair.
    // This also protects non-region callers of the exported prism builder.
    void CleanRing( const std::vector<float> &loopUV, std::vector<int> *ring )
    {
        bool changed = true;
        while ( changed )
        {
            changed = false;
            const int n = (int)ring->size();
            if ( n < 4 )
                return;
            for ( int i = 0; i < n; ++i )
            {
                const float *a = &loopUV[(size_t)( *ring )[( i + n - 1 ) % n] * 2];
                const float *b = &loopUV[(size_t)( *ring )[i] * 2];
                const float *c = &loopUV[(size_t)( *ring )[( i + 1 ) % n] * 2];
                const float e0[2] = { b[0] - a[0], b[1] - a[1] };
                const float e1[2] = { c[0] - b[0], c[1] - b[1] };
                const float l0 = sqrtf( e0[0] * e0[0] + e0[1] * e0[1] );
                const float l1 = sqrtf( e1[0] * e1[0] + e1[1] * e1[1] );
                bool drop = false;
                if ( !( l1 > 1.0e-4f ) )
                    drop = true;             // b == c: a zero-length side face
                else if ( l0 > 1.0e-4f )
                    drop = ( ( e0[0] * e1[0] + e0[1] * e1[1] ) / ( l0 * l1 ) )
                           > KVALID_PLANE_DOT;
                if ( !drop )
                    continue;
                ring->erase( ring->begin() + i );
                changed = true;
                break;
            }
        }
    }

    // Build one convex piece as an unlinked def; callers land it only after all gates pass.
    brush_t *BuildPieceDef( const prismBuild_t &pb, const std::vector<float> &loopUV,
                            const std::vector<int> &ringIn, const char **why )
    {
        // Clean a copy so the caller's decomposition still matches its preview.
        std::vector<int> ring = ringIn;
        CleanRing( loopUV, &ring );
        const int n = (int)ring.size();
        if ( n < 3 )
        {
            *why = "piece has fewer than 3 vertices";
            return 0;
        }
        if ( n > KEXT_MAX_PROFILE )
        {
            *why = "piece over the profile cap";
            return 0;
        }

        int i0, i1, i2;
        if ( !CapTriple( loopUV, ring, &i0, &i1, &i2 ) )
        {
            *why = "piece is degenerate (collinear)";
            return 0;
        }

        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, 0 );
        if ( !def )
        {
            *why = "brush allocation failed";
            return 0;
        }
        Ed_BrushSetFaceCount( def, n + 2 );
        if ( !def->faces || def->faceCount != n + 2 )
        {
            Brush_Free_R( def );              // still unlinked, refCount 0
            *why = "face array allocation failed";
            return 0;
        }

        // Reverse the CCW triple so the high-cap normal is +plane.normal.
        {
            face_t *f = &def->faces[0];
            PrismVertex( pb, &loopUV[(size_t)ring[i2] * 2], true, f->planepts[0] );
            PrismVertex( pb, &loopUV[(size_t)ring[i1] * 2], true, f->planepts[1] );
            PrismVertex( pb, &loopUV[(size_t)ring[i0] * 2], true, f->planepts[2] );
        }
        // The CCW triple gives the low cap a -plane.normal normal.
        {
            face_t *f = &def->faces[1];
            PrismVertex( pb, &loopUV[(size_t)ring[i0] * 2], false, f->planepts[0] );
            PrismVertex( pb, &loopUV[(size_t)ring[i1] * 2], false, f->planepts[1] );
            PrismVertex( pb, &loopUV[(size_t)ring[i2] * 2], false, f->planepts[2] );
        }

        // Each side uses the full edge and extrusion spans to keep planepts well separated.
        // For a CCW ring in a right-handed (u,v,normal) basis, (b_lo,a_lo,a_hi)
        // makes Face_MakePlane point outward as u*dv - v*du.
        for ( int e = 0; e < n; ++e )
        {
            const float *a = &loopUV[(size_t)ring[e] * 2];
            const float *b = &loopUV[(size_t)ring[( e + 1 ) % n] * 2];
            face_t *f = &def->faces[2 + e];
            PrismVertex( pb, b, false, f->planepts[0] );
            PrismVertex( pb, a, false, f->planepts[1] );
            PrismVertex( pb, a, true,  f->planepts[2] );
        }
        return def;
    }

    // Ported Ed_NewBrushDrag tail order (xywnd.cpp 0x467fa0).
    selbrush_t *LandDef( brush_t *def )
    {
        Entity_LinkBrush( def, (entity_s *)world_entity->def );
        selbrush_t *inst = Brush_AddToList( def, world_entity );
        Brush_AddToList2( inst );                 // → selected_brushes
        return inst;
    }

    // Region extrusion command.
    class KiwiExtrudeCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Extrude Region"; }
        bool CanExecute() override { return KiwiExtrude_CanExecute(); }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KEXT_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_dist;                     // SIGNED: which way the prism grew
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || m_region < 0 )
                return false;
            // Profile centroid lifted to the moving cap.
            for ( int k = 0; k < 3; ++k )
                out3[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            return true;
        }

        void Rebase() override
        {
            // LatchStart owns the rebase correction; applying it here too would double it.
            LatchStart();
        }

        // Anchor the lollipop on the moving cap and fold direction by distance sign.
        // At zero distance the camera chooses the visible side; geometry convention is unchanged.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_region < 0 || !outAnchor || !outDir )
                return false;
            for ( int k = 0; k < 3; ++k )
                outAnchor[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            const float side = KiwiLollipop_FaceSide( outAnchor, m_plane.normal, m_dist );
            for ( int k = 0; k < 3; ++k )
                outDir[k] = m_plane.normal[k] * side;
            return true;
        }

        bool Begin() override
        {
            m_region  = -1;
            m_dist    = 0.0f;
            m_hasNum  = false;
            m_invalid = false;
            m_hud[0]  = '\0';

            const std::vector<kregion_t> &regions = KiwiRegion_All();
            if ( regions.empty() )
            {
                Sys_Printf( "Extrude: no closed construction region.\n" );
                // Explain failed loop detection; force output because this refusal is user-visible.
                KiwiRegion_ReportGaps( true );
                return false;
            }

            // A deliberate region selection outranks cursor picking.
            m_region = KiwiRegion_SelectedIndex();
            m_extra.clear();

            ray_t ray;
            if ( m_region < 0 && Pick_RayFromCursor( &ray ) )
                m_region = KiwiRegion_PickAt( ray );
            if ( m_region < 0 && regions.size() == 1 )
                m_region = 0;                     // the documented "only one" shortcut
            if ( m_region < 0 )
            {
                Sys_Printf( "Extrude: put the cursor over a region first "
                            "(%i available).\n", (int)regions.size() );
                return false;
            }

            // Latch all selected regions once. The primary region drives axis, reference,
            // and numeric depth; each source later extrudes along its own normal.
            {
                const int selN = KiwiRegion_SelectedCount();
                for ( int s = 0; s < selN; ++s )
                {
                    const int idx = KiwiRegion_SelectedAt( s );
                    if ( idx >= 0 && idx != m_region )
                        m_extra.push_back( idx );
                }
            }

            const kregion_t &reg = regions[m_region];
            if ( (int)( reg.pts.size() / 2 ) > KEXT_MAX_PROFILE )
            {
                Sys_Printf( "Extrude: profile has %i vertices, the v1 limit is %i.\n",
                            (int)( reg.pts.size() / 2 ), KEXT_MAX_PROFILE );
                return false;
            }

            // Plane-space profile centroid lifted onto the region plane.
            m_plane = reg.plane;
            float c[2] = { 0.0f, 0.0f };
            const int n = (int)( reg.pts.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                c[0] += reg.pts[i * 2 + 0];
                c[1] += reg.pts[i * 2 + 1];
            }
            c[0] /= (float)n;
            c[1] /= (float)n;
            KiwiCon_PlaneToWorld( m_plane, c, m_ref );

            m_dist  = 0.0f;
            m_axisBlocked = false;  // recomputed on the first move
            LatchStart();
            UpdateHud();
            // Region extrusion always lands a prism; subtraction remains the explicit Q verb.
            Sys_Printf( "Extrude: drag along the region normal, or type a distance.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        // Idle preemption is safe only before any distance or numeric input applies.
        // Region extrusion mutates and opens undo only inside commit, so this state is disposable.
        bool PreemptIdle() const override
        {
            return m_region >= 0 && !m_hasNum && !( fabsf( m_dist ) >= KEXT_MIN_DIST );
        }

        // Transform snap context is raw until Ctrl; no per-command override is needed.

        bool IdlePressReselect( int imgX, int imgY, bool shift ) override
        {
            if ( !PreemptIdle() )
                return false;
            KiwiCmd_Cancel();
            KiwiBox_ClickSelectAt( imgX, imgY, shift, false );
            return true;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        void Commit() override
        {
            if ( fabsf( m_dist ) < KEXT_MIN_DIST )
            {
                Sys_Printf( "Extrude: zero distance — nothing created.\n" );
                return;
            }
            // Region extrusion deliberately never auto-carves overlapping solids; boolean
            // subtraction is explicit Q. Negative face extrusion below is a separate one-face push.
            Extrude();
            // Clear source-region selection; newly landed brushes stay selected and source lines remain.
            KiwiRegion_ClearSelection();
        }

        void Cancel() override
        {
            m_region = -1;
            m_extra.clear();            // selected sources are per gesture
            g_nUpdateBits |= 1;
        }

        // Three wire segments per profile vertex per source, plus snap-marker headroom.
        int LineBudget() const override
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            int segs = 0;
            for ( size_t i = 0; i < srcs.size(); ++i )
                segs += 3 * (int)( regions[(size_t)srcs[i]].pts.size() / 2 );
            return segs + 96;          // …plus the snap marker's own headroom
        }

        void DrawWorld() override
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            for ( size_t si = 0; si < srcs.size(); ++si )
            {
            const kregion_t &reg = regions[(size_t)srcs[si]];

            prismBuild_t pb;
            MakeBuildFor( reg, &pb );

            const float *col = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;

            // Draw translucent solid before wireframe; zero depth is rejected by EmitPrismSolid.
            {
                std::vector<int> capTris;
                KiwiRegion_Triangulate( reg.pts, &capTris );
                EmitPrismSolid( pb, reg.pts, capTris, col );
            }

            KiwiLines_Color( col[0], col[1], col[2] );

            const int n = (int)( reg.pts.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float a[3], b[3];
                PrismVertex( pb, &reg.pts[(size_t)i * 2], true, a );
                PrismVertex( pb, &reg.pts[(size_t)j * 2], true, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                // Include the source ring so the preview reads as a closed prism.
                PrismVertex( pb, &reg.pts[(size_t)i * 2], false, a );
                PrismVertex( pb, &reg.pts[(size_t)j * 2], false, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                PrismVertex( pb, &reg.pts[(size_t)i * 2], false, a );
                PrismVertex( pb, &reg.pts[(size_t)i * 2], true,  b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
            }   // end per-source loop
        }

    private:
        // Every consumer builds from its current source plane; sharing the primary plane
        // would push sources on other walls sideways.

        // One gesture applies one signed distance to every source, but each source keeps
        // its own plane and normal. The primary source drives cursor and numeric mapping.
        void MakeBuildFor( const kregion_t &reg, prismBuild_t *pb ) const
        {
            pb->plane = reg.plane;
            pb->lo    = ( m_dist >= 0.0f ) ? 0.0f  : m_dist;
            pb->hi    = ( m_dist >= 0.0f ) ? m_dist : 0.0f;
        }

        // Emit the primary first and bounds-check latched indices against the current region list.
        void EachSource( std::vector<int> *out ) const
        {
            out->clear();
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            if ( m_region >= 0 && m_region < (int)regions.size() )
                out->push_back( m_region );
            for ( size_t i = 0; i < m_extra.size(); ++i )
            {
                const int idx = m_extra[i];
                if ( idx < 0 || idx >= (int)regions.size() || idx == m_region )
                    continue;
                out->push_back( idx );
            }
        }

        // Append one region's unlinked, validated defs. On failure the caller frees
        // the complete accumulated list so a multi-source gesture remains all-or-nothing.
        bool BuildRegionDefs( const kregion_t &reg, std::vector<brush_t *> *out,
                              const char **why ) const
        {
            const int n = (int)( reg.pts.size() / 2 );
            if ( n < 3 || n > KEXT_MAX_PROFILE )
            {
                *why = "profile out of range";
                return false;
            }

            std::vector< std::vector<int> > pieces;
            if ( KiwiRegion_IsConvex( reg.pts ) )
            {
                std::vector<int> whole;
                for ( int i = 0; i < n; ++i )
                    whole.push_back( i );
                pieces.push_back( whole );
            }
            else if ( !KiwiRegion_ConvexPieces( reg.pts, &pieces ) )
            {
                *why = "could not decompose the profile (self-intersecting or degenerate)";
                return false;
            }
            if ( pieces.empty() || (int)pieces.size() > KEXT_MAX_PIECES )
            {
                *why = "the decomposition produced too many pieces";
                return false;
            }

            prismBuild_t pb;
            MakeBuildFor( reg, &pb );
            for ( size_t p = 0; p < pieces.size(); ++p )
            {
                brush_t *def = BuildPieceDef( pb, reg.pts, pieces[p], why );
                if ( !def )
                    return false;
                out->push_back( def );
                KiwiValid_Rebuild( def );
                if ( !KiwiValid_CheckBrush( def, why ) )
                    return false;
            }
            return true;
        }

        // A world-aligned end reports its absolute axis coordinate; a slanted end
        // reports signed distance from the source plane to avoid naming a false axis.
        void AbsoluteLabel( char *buf, int bufSize ) const
        {
            int worldAxis = -1;
            for ( int k = 0; k < 3; ++k )
                if ( fabsf( m_plane.normal[k] ) > 0.999f )
                    worldAxis = k;
            char v[32];
            if ( worldAxis >= 0 )
            {
                const float pos = m_ref[worldAxis] + m_plane.normal[worldAxis] * m_dist;
                KiwiUnits_Format( v, sizeof( v ), pos );
                _snprintf( buf, bufSize, "%c %s", "XYZ"[worldAxis], v );
            }
            else
            {
                KiwiUnits_Format( v, sizeof( v ), m_dist );
                _snprintf( buf, bufSize, "out %s", v );
            }
            buf[bufSize - 1] = '\0';
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        // Bias the latched cursor coordinate by current distance so relatching is a rebase.
        void LatchStart()
        {
            m_haveStart = false;
            if ( !KiwiCam_AxisPortrayable( m_plane.normal ) )
                return;
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                return;
            float rel[3];
            Sub3( p, m_ref, rel );
            m_start     = Dot3( rel, m_plane.normal ) - m_dist;
            m_haveStart = true;
        }

        void Recompute()
        {
            float d = m_dist;
            m_gridMajor = false;               // one frame's answer

            // Hold depth while the camera cannot portray this axis, then rebase when it can.
            // Numeric input and snap resolution do not depend on the cursor-axis solve.
            const bool canAxis = KiwiCam_AxisPortrayable( m_plane.normal );
            if ( !canAxis )
                m_haveStart = false;
            else if ( !m_haveStart )
                LatchStart();
            m_axisBlocked = !canAxis;

            // Ctrl-up rebases relative mode at the current depth; Ctrl-down deliberately
            // switches directly to absolute cursor depth.
            const bool absNow = KiwiExt_AbsoluteHeld();
            if ( !absNow && m_absPrev )
                LatchStart();
            m_absPrev  = absNow;
            m_absolute = absNow && canAxis && m_haveStart;

            ray_t ray;
            float p[3];
            float rawAbs  = d;                 // the cursor's own axis position
            bool  haveRaw = false;
            if ( canAxis && m_haveStart && CursorRay( &ray )
              && KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
            {
                float rel[3];
                Sub3( p, m_ref, rel );
                rawAbs  = Dot3( rel, m_plane.normal );
                haveRaw = true;
                d = absNow ? rawAbs : ( rawAbs - m_start );
            }

            if ( m_hasNum )
            {
                d = m_numWorld;
            }
            else if ( m_absolute && haveRaw )
            {
                // Shared absolute ladder: geometry, face magnet, then major-aware lattice.
                d = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, rawAbs,
                                         &m_gridMajor );
            }
            else if ( m_snap.valid && m_snap.type != SNAP_NONE )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // Resolve face snaps as planes on the gesture axis so every point on
                    // one face yields one depth; edge-on faces provide no answer.
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_plane.normal, &sd ) )
                    {
                        // Refuse source-plane and near-zero geometry snaps.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            d = sd;
                    }
                }
                else
                {
                    // Quantize the scalar depth, not the cursor point. The hard lattice uses
                    // absolute world coordinates and can prefer nearby major lines; this arm
                    // is reached only when the transform snap context is engaged.
                    bool major = false;
                    d = KiwiSnap_LatticeAxis( d, m_ref, m_plane.normal, true, &major );
                    m_gridMajor = major;
                }
            }
            // Without an engaged snap answer, preserve the raw mapped depth.

            m_dist    = d;
            m_invalid = ( fabsf( d ) < KEXT_MIN_DIST );
            UpdateHud();
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_dist );
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            const int verts = ( m_region >= 0 && m_region < (int)regions.size() )
                            ? (int)( regions[m_region].pts.size() / 2 ) : 0;
            // Show source count because some selected regions may be off screen.
            char many[24];
            many[0] = '\0';
            if ( !m_extra.empty() )
                _snprintf( many, sizeof( many ), " x%i", (int)m_extra.size() + 1 );
            many[sizeof( many ) - 1] = '\0';

            // The view-gate remedy takes precedence over the derivative TOO THIN state.
            if ( m_axisBlocked && !m_hasNum )
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude  %i-gon%s  %s  ·  ORBIT to pull — this view looks "
                           "straight along the extrude axis  ·  or type a distance",
                           verts, many, b );
            else if ( m_invalid )
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s  TOO THIN",
                           verts, many, b );
            // Report major locks, and in absolute mode show the aimed coordinate before
            // the familiar signed delta.
            else if ( m_absolute )
            {
                char abso[32];
                AbsoluteLabel( abso, sizeof( abso ) );
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s  (%s)%s  CTRL",
                           verts, many, abso, b, m_gridMajor ? "  [MAJOR]" : "" );
            }
            else
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s%s",
                           verts, many, b, m_gridMajor ? "  [MAJOR]" : "" );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }


        // Commit.
        void Extrude()
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            if ( srcs.empty() )
                return;

            // Build and validate every def before linking any. Rejection lands no brush
            // and opens no undo bracket; multiple sources commit all-or-nothing.
            std::vector<brush_t *> defs;
            const char *why = "unknown";
            bool  ok    = true;
            int   verts = 0;
            for ( size_t s = 0; s < srcs.size() && ok; ++s )
            {
                const kregion_t &reg = regions[(size_t)srcs[s]];
                verts += (int)( reg.pts.size() / 2 );
                ok = BuildRegionDefs( reg, &defs, &why );
            }

            if ( !ok )
            {
                // Unlinked defs satisfy Brush_Free_R's refCount/owner-chain precondition.
                for ( size_t i = 0; i < defs.size(); ++i )
                    Brush_Free_R( defs[i] );
                Sys_Printf( "Extrude: rejected — %s.\n", why ? why : "invalid geometry" );
                return;
            }

            // Deselect before opening one undo bracket so only the new selected brushes
            // are stamped as creations.
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "extrude region" );
            for ( size_t i = 0; i < defs.size(); ++i )
                LandDef( defs[i] );

            Sys_Printf( "Extruded %i region(s): %i brush(es) from %i profile vertices.\n",
                        (int)srcs.size(), (int)defs.size(), verts );
            g_nUpdateBits = -1;
        }

        int              m_region  = -1;
        // Other selected regions latched for this gesture; empty is single-source.
        std::vector<int> m_extra;
        kconPlane_t   m_plane;
        float         m_ref[3]  = { 0.0f, 0.0f, 0.0f };
        float         m_start   = 0.0f;
        bool          m_haveStart = false;
        float         m_dist    = 0.0f;
        bool          m_axisBlocked = false;  // view gate
        bool          m_hasNum  = false;
        float         m_numWorld = 0.0f;
        bool          m_invalid = false;
        // True only for the current frame when a major grid line supplied the depth.
        bool          m_gridMajor = false;
        // Ctrl edge detector and current absolute-mode state.
        bool          m_absPrev   = false;
        bool          m_absolute  = false;
        snap_result_t m_snap;
        char          m_hud[192] = { 0 };
    };

    // Face extrusion grows a separate prism and leaves the source brush untouched.
    // Project the convex face winding into a right-handed plane basis and normalize it
    // to CCW before using the shared prism writer. Negative depth delegates to the
    // existing one-face push/destroy path instead of building an inward body.
    class KiwiExtrudeFaceCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Extrude Face"; }
        // Begin performs the refusal so it can print a useful message.
        bool CanExecute() override { return true; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KEXT_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_dist;
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || !m_have )
                return false;
            for ( int k = 0; k < 3; ++k )
                out3[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            return true;
        }

        // Face extrusion is a transform: raw depth until Ctrl engages snapping.

        // Three wire segments per source vertex, plus snap-marker headroom.
        int LineBudget() const override
        {
            int segs = 3 * (int)( m_loop.size() / 2 );
            for ( size_t i = 0; i < m_extraFaces.size(); ++i )
                segs += 3 * (int)( m_extraFaces[i].loop.size() / 2 );
            return segs + 96;
        }

        // Never pick or snap the selected geometry this transform grows from.
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        void Rebase() override
        {
            // LatchStart owns distance-preserving rebase.
            LatchStart();
        }

        // Keep the lollipop on the moving face and fold direction by depth sign.
        // At rest the camera chooses the visible side after orbiting.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( !m_have || !outAnchor || !outDir )
                return false;
            for ( int k = 0; k < 3; ++k )
                outAnchor[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            const float side = KiwiLollipop_FaceSide( outAnchor, m_plane.normal, m_dist );
            for ( int k = 0; k < 3; ++k )
                outDir[k] = m_plane.normal[k] * side;
            return true;
        }

        bool Begin() override
        {
            m_have      = false;
            m_dist      = 0.0f;
            m_axisBlocked = false;      // view gate
            m_hasNum    = false;
            m_invalid   = true;
            m_mode      = KEXTF_IDLE;
            m_node      = 0;
            m_faceIndex = -1;
            m_depth     = 0.0f;
            m_haveDepth = false;
            m_hud[0]    = '\0';
            m_loop.clear();
            m_extraFaces.clear();       // selected sources are per gesture

            selbrush_t *node = 0;
            int         face = -1;
            if ( !KiwiExtrudeFace_Pick( &node, &face ) )
            {
                Sys_Printf( "Extrude: select (or hover) a brush face, or put the "
                            "cursor over a construction region.\n" );
                return false;
            }
            if ( !BuildProfile( node, face ) )
                return false;

            // Latch the source face and its inward destroy depth from untouched geometry.
            m_node      = node;
            m_faceIndex = face;
            m_haveDepth = ( KiwiXform_FacePushDepth( node, face, &m_depth ) != 0 );

            // Profile every other selected face once, preserving its own plane.
            // The active face drives the gesture. Extras that cannot form a convex profile
            // are omitted and reported by count instead of rejecting the drive face.
            {
                const selection_t &sel = KiwiSel();
                int dropped = 0;
                for ( size_t i = 0; i < sel.items.size(); ++i )
                {
                    const sel_item_t &it = sel.items[i];
                    // Exclude only the drive face; another face on the same brush is a valid source.
                    if ( it.kind != SEL_FACE || !it.brush )
                        continue;
                    if ( it.brush == m_node && it.faceIndex == m_faceIndex )
                        continue;
                    if ( !Sel_BrushLive( it.brush ) || it.brush->patch )
                        continue;
                    const brush_t *d = it.brush->def;
                    if ( !d || !d->faces || it.faceIndex < 0 || it.faceIndex >= d->faceCount )
                        continue;

                    faceSrc_t fs;
                    fs.node = it.brush;
                    fs.face = it.faceIndex;
                    if ( !BuildProfileInto( it.brush, it.faceIndex, false,
                                            &fs.plane, &fs.loop, 0 ) )
                    {
                        ++dropped;
                        continue;
                    }
                    m_extraFaces.push_back( fs );
                }
                if ( dropped > 0 )
                    Sys_Printf( "Extrude Face: %i selected face(s) could not be "
                                "profiled and are not in this extrusion.\n", dropped );
            }

            m_have = true;
            LatchStart();
            UpdateHud();
            Sys_Printf( "Extrude Face: drag OUT along the face normal for a new "
                        "body, or IN to carve this brush (all the way through "
                        "destroys it).  RMB / Enter confirms.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        void Commit() override
        {
            if ( !m_have || m_mode == KEXTF_IDLE )
            {
                Sys_Printf( "Extrude Face: zero distance — nothing created.\n" );
                return;
            }
            if ( m_mode == KEXTF_GROW )
            {
                Grow();
                return;
            }
            // Negative depth stays single-source: each brush has an independent destroy
            // threshold, so mixing several carve/delete outcomes would be ambiguous.
            if ( !m_extraFaces.empty() )
                Sys_Printf( "Extrude Face: carving IN applies to the ACTIVE face only "
                            "— the other %i selected face(s) are left alone.  Drag OUT "
                            "to grow all of them at once.\n", (int)m_extraFaces.size() );
            Carve();
        }

        void Cancel() override
        {
            m_have = false;
            m_extraFaces.clear();       // selected sources are per gesture
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( !m_have )
                return;

            // The gesture mutates nothing before commit. Preview carve at the translated
            // source winding, or outline the whole doomed brush in delete red.
            if ( m_mode == KEXTF_CARVE || m_mode == KEXTF_DESTROY )
            {
                DrawCarve();
                return;
            }

            // Draw extras first so the drive-face preview remains on top.
            for ( size_t i = 0; i < m_extraFaces.size(); ++i )
            {
                const faceSrc_t &fs = m_extraFaces[i];
                if ( fs.loop.size() < 6 )
                    continue;
                prismBuild_t xb;
                xb.plane = fs.plane;
                xb.lo    = 0.0f;
                xb.hi    = ( m_dist > 0.0f ) ? m_dist : 0.0f;
                const float *xc = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;
                std::vector<int> xtris;
                KiwiRegion_Triangulate( fs.loop, &xtris );
                EmitPrismSolid( xb, fs.loop, xtris, xc );
                KiwiLines_Color( xc[0], xc[1], xc[2] );
                EmitPrismWire( xb, fs.loop );
            }

            prismBuild_t pb;
            pb.plane = m_plane;
            pb.lo    = 0.0f;
            pb.hi    = ( m_dist > 0.0f ) ? m_dist : 0.0f;

            const float *col = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;
            KiwiLines_Color( col[0], col[1], col[2] );

            // Suppress sub-threshold previews; a collapsed prism would only retrace the source face.
            if ( fabsf( m_dist ) < KEXT_MIN_DIST )
                return;

            // Draw the translucent solid before its wireframe.
            {
                std::vector<int> capTris;
                KiwiRegion_Triangulate( m_loop, &capTris );
                EmitPrismSolid( pb, m_loop, capTris, col );
                KiwiLines_Color( col[0], col[1], col[2] );
            }

            const int n = (int)( m_loop.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float a[3], b[3];
                PrismVertex( pb, &m_loop[(size_t)i * 2], true, a );
                PrismVertex( pb, &m_loop[(size_t)j * 2], true, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                // Source ring.
                PrismVertex( pb, &m_loop[(size_t)i * 2], false, a );
                PrismVertex( pb, &m_loop[(size_t)j * 2], false, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                PrismVertex( pb, &m_loop[(size_t)i * 2], false, a );
                PrismVertex( pb, &m_loop[(size_t)i * 2], true,  b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

    private:
        // Carve previews the translated source winding; destroy outlines every brush winding.
        void DrawCarve()
        {
            if ( !m_node || !Sel_BrushLive( m_node ) )
                return;
            const brush_t *def = m_node->def;
            if ( !def || !def->faces )
                return;

            if ( m_mode == KEXTF_DESTROY )
            {
                KiwiLines_Color( KEXT_COL_DOOMED[0], KEXT_COL_DOOMED[1],
                                 KEXT_COL_DOOMED[2] );
                for ( int f = 0; f < def->faceCount; ++f )
                {
                    const winding_t *w = def->faces[f].w;
                    if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                        continue;
                    for ( int p = 0; p < w->numpoints; ++p )
                        if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                            return;
                }
                return;
            }

            if ( m_faceIndex < 0 || m_faceIndex >= def->faceCount )
                return;
            const winding_t *w = def->faces[m_faceIndex].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                return;

            KiwiLines_Color( KEXT_COL_CARVE[0], KEXT_COL_CARVE[1], KEXT_COL_CARVE[2] );
            for ( int p = 0; p < w->numpoints; ++p )
            {
                const int q = ( p + 1 ) % w->numpoints;
                float a[3], b[3];
                for ( int k = 0; k < 3; ++k )
                {
                    a[k] = w->p[p][k] + m_plane.normal[k] * m_dist;   // m_dist < 0
                    b[k] = w->p[q][k] + m_plane.normal[k] * m_dist;
                }
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

        // Delegate negative depth to the interactive push helper so deletion threshold,
        // texture locking, validity, and removal ordering stay in one implementation.
        // Its undo bracket remains open for the framework to commit; rejection cancels it.
        void Carve()
        {
            const char *why = 0;
            const int r = KiwiXform_PushFaceOnce( m_node, m_faceIndex, m_dist,
                                                  "un-extrude face", &why );
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), -m_dist );

            if ( r == KXPUSH_DELETED )
            {
                Sel_Clear( KiwiSel() );
                Sel_RebuildFromLegacy();
                Sys_Printf( "Un-extrude: pushed through — the brush was destroyed.\n" );
            }
            else if ( r == KXPUSH_PUSHED )
            {
                // Rebuild typed selection after windings are re-solved; face count is unchanged.
                Sel_RebuildFromLegacy();
                Sys_Printf( "Un-extrude: carved %s into the brush.\n", b );
            }
            else
            {
                Sys_Printf( "Un-extrude: rejected — %s.\n", why ? why : "invalid geometry" );
            }
            g_nUpdateBits = -1;
        }

        // Write profile results through parameters so drive and extra faces share one path.
        bool BuildProfileInto( selbrush_t *node, int faceIndex, bool verbose,
                               kconPlane_t *outPlane, std::vector<float> *outLoop,
                               float *outRef ) const
        {
            const brush_t *def = node->def;
            const face_t  *f   = &def->faces[faceIndex];
            const winding_t *w = f->w;
            if ( !w || w->numpoints < 3 || w->numpoints > KEXT_MAX_PROFILE )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that face has %i vertices (the v1 limit "
                                "is %i).\n", w ? w->numpoints : 0, KEXT_MAX_PROFILE );
                return false;
            }

            // Seed the plane basis with the face's longest edge.
            float c[3] = { 0.0f, 0.0f, 0.0f };
            for ( int i = 0; i < w->numpoints; ++i )
                for ( int k = 0; k < 3; ++k )
                    c[k] += w->p[i][k];
            const float inv = 1.0f / (float)w->numpoints;
            for ( int k = 0; k < 3; ++k )
                c[k] *= inv;

            float hint[3] = { 0.0f, 0.0f, 0.0f };
            float bestLen = 0.0f;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                float e[3];
                Sub3( w->p[( i + 1 ) % w->numpoints], w->p[i], e );
                const float l = sqrtf( Dot3( e, e ) );
                if ( l > bestLen ) { bestLen = l; Copy3( e, hint ); }
            }

            if ( !KiwiCon_MakePlane( c, f->plane.normal, ( bestLen > 1.0e-3f ) ? hint : 0,
                                     outPlane ) )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that face has a degenerate plane.\n" );
                return false;
            }
            if ( outRef )
                Copy3( c, outRef );

            std::vector<float> &loop = *outLoop;
            loop.resize( (size_t)w->numpoints * 2 );
            for ( int i = 0; i < w->numpoints; ++i )
                KiwiCon_WorldToPlane( *outPlane, w->p[i], &loop[(size_t)i * 2] );

            // The prism writer requires CCW plane-space data. Brush winding sense is not
            // guaranteed, so reverse a negative signed area before the convexity check.
            float area2 = 0.0f;
            const int n = (int)( loop.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                area2 += loop[(size_t)i * 2 + 0] * loop[(size_t)j * 2 + 1]
                       - loop[(size_t)j * 2 + 0] * loop[(size_t)i * 2 + 1];
            }
            if ( area2 < 0.0f )
            {
                for ( int i = 0; i < n / 2; ++i )
                {
                    const int j = n - 1 - i;
                    float t0 = loop[(size_t)i * 2 + 0], t1 = loop[(size_t)i * 2 + 1];
                    loop[(size_t)i * 2 + 0] = loop[(size_t)j * 2 + 0];
                    loop[(size_t)i * 2 + 1] = loop[(size_t)j * 2 + 1];
                    loop[(size_t)j * 2 + 0] = t0;
                    loop[(size_t)j * 2 + 1] = t1;
                }
            }
            if ( !KiwiRegion_IsConvex( loop ) )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that winding is not convex — it cannot "
                                "be one brush.\n" );
                return false;
            }
            return true;
        }

        // Drive-face wrapper; only the drive face reports detailed profile errors.
        bool BuildProfile( selbrush_t *node, int faceIndex )
        {
            return BuildProfileInto( node, faceIndex, true, &m_plane, &m_loop, m_ref );
        }

        // A world-aligned end reports its absolute axis coordinate; a slanted end
        // reports signed distance from the source plane.
        void AbsoluteLabel( char *buf, int bufSize ) const
        {
            int worldAxis = -1;
            for ( int k = 0; k < 3; ++k )
                if ( fabsf( m_plane.normal[k] ) > 0.999f )
                    worldAxis = k;
            char v[32];
            if ( worldAxis >= 0 )
            {
                const float pos = m_ref[worldAxis] + m_plane.normal[worldAxis] * m_dist;
                KiwiUnits_Format( v, sizeof( v ), pos );
                _snprintf( buf, bufSize, "%c %s", "XYZ"[worldAxis], v );
            }
            else
            {
                KiwiUnits_Format( v, sizeof( v ), m_dist );
                _snprintf( buf, bufSize, "out %s", v );
            }
            buf[bufSize - 1] = '\0';
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        // Relatch as a distance-preserving rebase.
        void LatchStart()
        {
            m_haveStart = false;
            if ( !KiwiCam_AxisPortrayable( m_plane.normal ) )
                return;
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                return;
            float rel[3];
            Sub3( p, m_ref, rel );
            m_start     = Dot3( rel, m_plane.normal ) - m_dist;
            m_haveStart = true;
        }

        void Recompute()
        {
            float d = m_dist;
            m_gridMajor = false;               // one frame's answer

            // Hold and later rebase when the camera looks along the face normal and cannot
            // portray motion on that axis.
            const bool canAxis = KiwiCam_AxisPortrayable( m_plane.normal );
            if ( !canAxis )
                m_haveStart = false;
            else if ( !m_haveStart )
                LatchStart();
            m_axisBlocked = !canAxis;

            // Ctrl-up rebases relative mode at the current depth; Ctrl-down switches
            // directly to absolute cursor depth.
            const bool absNow = KiwiExt_AbsoluteHeld();
            if ( !absNow && m_absPrev )
                LatchStart();
            m_absPrev  = absNow;
            m_absolute = absNow && canAxis && m_haveStart;

            ray_t ray;
            float p[3];
            float rawAbs  = d;                 // the cursor's own axis position
            bool  haveRaw = false;
            if ( canAxis && m_haveStart && CursorRay( &ray )
              && KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
            {
                float rel[3];
                Sub3( p, m_ref, rel );
                rawAbs  = Dot3( rel, m_plane.normal );
                haveRaw = true;
                d = absNow ? rawAbs : ( rawAbs - m_start );
            }

            if ( m_hasNum )
            {
                d = m_numWorld;
            }
            else if ( m_absolute && haveRaw )
            {
                // Shared absolute ladder: geometry, face magnet, then major-aware lattice.
                d = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, rawAbs,
                                         &m_gridMajor );
            }
            else if ( m_snap.valid && m_snap.type != SNAP_NONE )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // Resolve planar snaps consistently with the region path.
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_plane.normal, &sd ) )
                    {
                        // Refuse the source-plane band. Pick flags cover selected brush geometry,
                        // while this scalar rule also covers construction and guide candidates.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            d = sd;
                    }
                }
                else
                {
                    // Hard absolute lattice with major-line preference.
                    bool major = false;
                    d = KiwiSnap_LatticeAxis( d, m_ref, m_plane.normal, true, &major );
                    m_gridMajor = major;
                }
            }
            // Without an engaged snap answer, preserve raw mapped depth.

            m_dist = d;

            // Recompute grow, carve, or destroy from signed depth every frame.
            // Use the helper's exported epsilon so HUD and commit agree at the threshold.
            m_mode = KEXTF_IDLE;
            if ( d >= KEXT_MIN_DIST )
                m_mode = KEXTF_GROW;
            else if ( d <= -KEXT_MIN_DIST )
                m_mode = ( m_haveDepth && m_depth > KEXT_PUSH_EPS
                        && d <= -m_depth + KEXT_PUSH_EPS )
                       ? KEXTF_DESTROY : KEXTF_CARVE;

            // Only sub-threshold depth is invalid; negative depth is a valid un-extrude.
            m_invalid = ( m_mode == KEXTF_IDLE );
            UpdateHud();
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), ( m_dist < 0.0f ) ? -m_dist : m_dist );
            const int verts = (int)( m_loop.size() / 2 );

            // The view gate outranks action modes because it holds depth unchanged.
            if ( m_axisBlocked && !m_hasNum )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude face  %i-gon  %s  ·  ORBIT to pull — this view "
                           "looks straight along the face normal  ·  or type a distance",
                           verts, b );
                m_hud[sizeof( m_hud ) - 1] = '\0';
                return;
            }

            // State the exact commit outcome for the current signed depth.
            switch ( m_mode )
            {
            case KEXTF_GROW:
                // Include source count for selected faces that may be off screen.
                if ( !m_extraFaces.empty() )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "extrude face  %i-gon x%i  %s  ->  %i NEW BODIES",
                               verts, (int)m_extraFaces.size() + 1, b,
                               (int)m_extraFaces.size() + 1 );
                else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "extrude face  %i-gon  %s  ->  NEW BODY", verts, b );
                break;
            case KEXTF_CARVE:
                _snprintf( m_hud, sizeof( m_hud ),
                           "un-extrude  %s IN  ->  CARVES the brush  (%s to destroy)",
                           b, m_haveDepth ? "keep pulling" : "no depth" );
                break;
            case KEXTF_DESTROY:
                _snprintf( m_hud, sizeof( m_hud ),
                           "un-extrude  %s IN  ->  PUSHED THROUGH, confirm DESTROYS "
                           "this brush", b );
                break;
            default:
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude face  %i-gon  %s  (pull OUT for a new body, "
                           "IN to carve)", verts, b );
                break;
            }
            // Append the major-lock badge once because it describes every action mode.
            if ( m_gridMajor )
            {
                const size_t n = strlen( m_hud );
                if ( n + 9 < sizeof( m_hud ) )
                    _snprintf( m_hud + n, sizeof( m_hud ) - n, "  [MAJOR]" );
            }
            // Append the current absolute coordinate once for every action mode.
            if ( m_absolute )
            {
                char abso[32];
                AbsoluteLabel( abso, sizeof( abso ) );
                const size_t n = strlen( m_hud );
                if ( n + 24 < sizeof( m_hud ) )
                    _snprintf( m_hud + n, sizeof( m_hud ) - n, "  CTRL @ %s", abso );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // Build and validate all unlinked defs before landing them with one undo bracket.
        void Grow()
        {
            // Every selected face grows by one depth along its own normal.
            // Build and gate all unlinked prisms before landing any so one rejection
            // cannot leave a partial multi-face edit.
            const char *why = "unknown";
            std::vector<brush_t *> defs;
            bool ok = true;
            const int n = (int)( m_loop.size() / 2 );
            {
                brush_t *def = KiwiExtrude_BuildPrismDef( m_plane, &m_loop[0], n,
                                                          0.0f, m_dist, &why );
                if ( !def )
                {
                    ok = false;
                }
                else
                {
                    defs.push_back( def );
                    KiwiValid_Rebuild( def );
                    if ( !KiwiValid_CheckBrush( def, &why ) )
                        ok = false;
                }
            }
            for ( size_t i = 0; i < m_extraFaces.size() && ok; ++i )
            {
                const faceSrc_t &fs = m_extraFaces[i];
                const int fn = (int)( fs.loop.size() / 2 );
                if ( fn < 3 )
                    continue;
                brush_t *def = KiwiExtrude_BuildPrismDef( fs.plane, &fs.loop[0], fn,
                                                          0.0f, m_dist, &why );
                if ( !def )
                {
                    ok = false;
                    break;
                }
                defs.push_back( def );
                KiwiValid_Rebuild( def );
                if ( !KiwiValid_CheckBrush( def, &why ) )
                    ok = false;
            }
            if ( !ok )
            {
                for ( size_t i = 0; i < defs.size(); ++i )
                    Brush_Free_R( defs[i] );   // never linked: refCount 0, no owner chain
                Sys_Printf( "Extrude Face: rejected — %s.\n",
                            why ? why : "invalid geometry" );
                return;
            }

            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "extrude face" );
            for ( size_t i = 0; i < defs.size(); ++i )
                LandDef( defs[i] );            // lands SELECTED

            // Source brushes remain untouched; rebuild typed selection from newly landed brushes.
            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();

            Sys_Printf( "Extrude Face: %i new brush(es) from %i face(s) (%i-gon drive).\n",
                        (int)defs.size(), (int)m_extraFaces.size() + 1, n );
            g_nUpdateBits = -1;

            // Match paste/clone handoff: start a paused Move for the selected new bodies.
            KiwiCmd_StartDeferred( KIWI_CMD_MOVE, true );
        }

        // Extra source with its own plane and CCW plane-space profile.
        struct faceSrc_t
        {
            selbrush_t        *node;
            int                face;
            kconPlane_t        plane;
            std::vector<float> loop;
        };
        std::vector<faceSrc_t> m_extraFaces;

        kconPlane_t        m_plane;
        std::vector<float> m_loop;                       // CCW, plane space
        float              m_ref[3]  = { 0.0f, 0.0f, 0.0f };
        float              m_start   = 0.0f;
        bool               m_haveStart = false;
        bool               m_have    = false;
        float              m_dist    = 0.0f;
        bool               m_axisBlocked = false;   // view gate
        bool               m_hasNum  = false;
        float              m_numWorld = 0.0f;
        bool               m_invalid = true;
        // True only for the current frame when a major grid line supplied the depth.
        bool               m_gridMajor = false;
        // Ctrl edge detector and current absolute-mode state.
        bool               m_absPrev   = false;
        bool               m_absolute  = false;
        snap_result_t      m_snap;
        char               m_hud[192] = { 0 };

        // Source face and untouched inward depth used by negative extrusion.
        selbrush_t        *m_node      = 0;
        int                m_faceIndex = -1;
        float              m_depth     = 0.0f;   // thickness along the face normal
        bool               m_haveDepth = false;
        int                m_mode      = KEXTF_IDLE;
    };

    KiwiExtrudeCommand     s_extrude;
    KiwiExtrudeFaceCommand s_extrudeFace;
}

// Pick active selected face, then another selected face, then hovered face.
// The active source drives multi-face extrusion.
bool KiwiExtrudeFace_Pick( selbrush_t **outNode, int *outFace )
{
    const selection_t &sel = KiwiSel();
    const sel_item_t  &act = sel.active;

    const sel_item_t *chosen = 0;
    if ( act.kind == SEL_FACE && Sel_BrushLive( act.brush ) && !act.brush->patch )
        chosen = &act;
    for ( size_t i = 0; i < sel.items.size() && !chosen; ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( it.kind == SEL_FACE && Sel_BrushLive( it.brush ) && !it.brush->patch )
            chosen = &sel.items[i];
    }

    pick_result_t hov;
    if ( !chosen )
    {
        hov = KiwiHover_Get();
        if ( hov.valid && hov.item.kind == SEL_FACE && Sel_BrushLive( hov.item.brush )
          && !hov.item.brush->patch )
            chosen = &hov.item;
    }
    if ( !chosen )
        return false;

    const brush_t *def = chosen->brush->def;
    if ( !def || !def->faces || chosen->faceIndex < 0 || chosen->faceIndex >= def->faceCount )
        return false;
    if ( outNode ) *outNode = chosen->brush;
    if ( outFace ) *outFace = chosen->faceIndex;
    return true;
}

bool KiwiExtrudeFace_CanExecute()
{
    selbrush_t *node = 0;
    int         face = -1;
    return KiwiExtrudeFace_Pick( &node, &face );
}

// Exported prism writer; see the header contract.
brush_t *KiwiExtrude_BuildPrismDef( const kconPlane_t &plane, const float *loopUV, int n,
                                    float lo, float hi, const char **why )
{
    const char *localWhy = "unknown";
    if ( !why )
        why = &localWhy;
    if ( !loopUV || n < 3 || n > KEXT_MAX_PROFILE )
    {
        *why = "profile out of range";
        return 0;
    }

    prismBuild_t pb;
    pb.plane = plane;
    pb.lo    = lo;
    pb.hi    = hi;

    std::vector<float> uv( loopUV, loopUV + (size_t)n * 2 );
    std::vector<int>   ring;
    ring.reserve( (size_t)n );
    for ( int i = 0; i < n; ++i )
        ring.push_back( i );

    return BuildPieceDef( pb, uv, ring, why );
}

selbrush_t *KiwiExtrude_LandDef( brush_t *def )
{
    return LandDef( def );
}

bool KiwiExtrude_CanExecute()
{
    return !KiwiRegion_All().empty();
}

void KiwiExtrude_RegisterCommands()
{
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    Radiant_RegisterCommand( "KiwiExtrudeRegion", 0, 0, KIWI_CMD_EXTRUDE_REGION );
    Radiant_RegisterCommand( "KiwiExtrudeFace",   0, 0, KIWI_CMD_EXTRUDE_FACE );
}

// E dispatches to face extrusion when a face is selected or hovered, otherwise
// to region extrusion. If neither exists, choose the face command so Begin can
// print the refusal instead of failing silently in CanExecute.
KiwiEditorCommand *KiwiExtrude_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_EXTRUDE_REGION )
        return &s_extrude;
    if ( commandId == KIWI_CMD_EXTRUDE_FACE )
    {
        if ( KiwiExtrudeFace_CanExecute() )
            return &s_extrudeFace;
        if ( KiwiExtrude_CanExecute() )
            return &s_extrude;
        return &s_extrudeFace;
    }
    return 0;
}
