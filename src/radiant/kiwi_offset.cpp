#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Offset Curve works only on the editor construction store, never map data.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_offset.h"
#include "kiwi_pick.h"
#include "kiwi_region.h"
#include "kiwi_units.h"
#include "kiwi_vec.h"     // Dot3/Sub3/...

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <vector>

//   engine_stubs.cpp:773   int  g_nUpdateBits = 0;   // 0x25d5a74
extern int  Sys_Printf( const char *fmt, ... );
extern int  g_nUpdateBits;
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // Shared construction preview palette.
    #define KOFF_COL_OK  KCON_PREVIEW_OK
    #define KOFF_COL_BAD KCON_PREVIEW_BAD

    // Numeric field copies retain label pointers, so labels need static storage.
    const kiwiNumField_t KOFF_FIELDS[1] = { { "offset", KNUM_LENGTH, false } };


    inline float Cross2( float ax, float ay, float bx, float by ) { return ax*by - ay*bx; }

    // Selection sub-items resolve to their owning construction object.
    int SelectedObject()
    {
        const int n = KiwiConSel_Count();
        for ( int i = 0; i < n; ++i )
        {
            const kconSelItem_t *it = KiwiConSel_At( i );
            if ( it && it->object >= 0 && KiwiCon_At( it->object ) )
                return it->object;
        }
        return -1;
    }

    bool Offsettable( const kconObject_t &o )
    {
        if ( KiwiCon_HasSmooth( o ) )                    // spline spans: layout curves only
            return false;
        if ( KiwiCon_IsParametric( o ) )                 // CIRCLE / ARC
            return o.radius > KOFF_MIN_RADIUS;
        return (int)( o.pts.size() / 3 ) >= 2;           // LINE / POLYLINE / RECT
    }

    // Returns false with `why` set when there is no sideways to offset along.
    bool PlaneFor( const kconObject_t &o, kconPlane_t *out, const char **why )
    {
        if ( KiwiCon_ObjectPlane( o, out ) )
            return true;

        // A straight chain has no fitted plane, so the active-plane normal supplies
        // sideways and is orthogonalised to make the resulting plane contain it.
        const int n = (int)( o.pts.size() / 3 );
        if ( n < 2 )
        {
            *why = "the object has no points";
            return false;
        }
        float dir[3];
        Sub3( &o.pts[( (size_t)n - 1 ) * 3], &o.pts[0], dir );
        float len = Len3( dir );
        if ( len < 1.0e-4f )
        {
            *why = "the chain has no length";
            return false;
        }
        for ( int k = 0; k < 3; ++k )
            dir[k] /= len;

        const kconPlane_t &act = KiwiCon_ActivePlane();
        float nrm[3];
        const float along = Dot3( act.normal, dir );
        for ( int k = 0; k < 3; ++k )
            nrm[k] = act.normal[k] - dir[k] * along;     // orthogonalise against the chain
        const float nlen = Len3( nrm );
        if ( nlen < 1.0e-3f )
        {
            *why = "the line runs along the construction plane's normal — "
                   "change the construction plane";
            return false;
        }
        for ( int k = 0; k < 3; ++k )
            nrm[k] /= nlen;

        if ( !KiwiCon_MakePlane( &o.pts[0], nrm, dir, out ) )
        {
            *why = "could not build a plane through the line";
            return false;
        }
        return true;
    }

    bool ToPlaneChain( const kconObject_t &o, const kconPlane_t &plane,
                       std::vector<float> *out )
    {
        out->clear();
        const int n = KiwiCon_VertCount( o );
        for ( int i = 0; i < n; ++i )
        {
            float w[3], uv[2];
            if ( !KiwiCon_VertWorld( o, i, w ) )
                return false;
            KiwiCon_WorldToPlane( plane, w, uv );
            const int have = (int)( out->size() / 2 );
            if ( have > 0 )
            {
                const float dx = uv[0] - (*out)[( (size_t)have - 1 ) * 2 + 0];
                const float dy = uv[1] - (*out)[( (size_t)have - 1 ) * 2 + 1];
                if ( dx * dx + dy * dy < KOFF_WELD_2D * KOFF_WELD_2D )
                    continue;
            }
            out->push_back( uv[0] );
            out->push_back( uv[1] );
        }
        // Closed plane-space rings omit a repeated first/last point.
        const int have = (int)( out->size() / 2 );
        if ( o.closed && have >= 2 )
        {
            const float dx = (*out)[0] - (*out)[( (size_t)have - 1 ) * 2 + 0];
            const float dy = (*out)[1] - (*out)[( (size_t)have - 1 ) * 2 + 1];
            if ( dx * dx + dy * dy < KOFF_WELD_2D * KOFF_WELD_2D )
                out->resize( ( (size_t)have - 1 ) * 2 );
        }
        return (int)( out->size() / 2 ) >= 2;
    }

    // Open chains need a non-ring crossing test; KiwiRegion_SelfIntersects closes input.
    bool SegCross2D( const float *a0, const float *a1, const float *b0, const float *b1 )
    {
        const float d1x = a1[0]-a0[0], d1y = a1[1]-a0[1];
        const float d2x = b1[0]-b0[0], d2y = b1[1]-b0[1];
        const float den = Cross2( d1x, d1y, d2x, d2y );
        if ( fabsf( den ) < 1.0e-9f )
            return false;                                // parallel or collinear
        const float sx = b0[0]-a0[0], sy = b0[1]-a0[1];
        const float t = Cross2( sx, sy, d2x, d2y ) / den;
        const float u = Cross2( sx, sy, d1x, d1y ) / den;
        return t > 1.0e-4f && t < 1.0f - 1.0e-4f
            && u > 1.0e-4f && u < 1.0f - 1.0e-4f;
    }

    bool OpenChainSelfIntersects( const std::vector<float> &pts )
    {
        const int n = (int)( pts.size() / 2 );
        for ( int i = 0; i + 1 < n; ++i )
            for ( int j = i + 2; j + 1 < n; ++j )
                if ( SegCross2D( &pts[(size_t)i*2], &pts[( (size_t)i+1 )*2],
                                 &pts[(size_t)j*2], &pts[( (size_t)j+1 )*2] ) )
                    return true;
        return false;
    }

    // Plane-space points are uv pairs; closed chains omit a repeated endpoint.
    // Positive `d` follows EdgeNormal; refusals set `why`.
    void EdgeNormal( const float *p0, const float *p1, bool flip, float *out )
    {
        float ex = p1[0] - p0[0], ey = p1[1] - p0[1];
        const float len = sqrtf( ex*ex + ey*ey );
        if ( len > 1.0e-9f ) { ex /= len; ey /= len; }
        // With u x v == normal, a CCW edge's outward normal is (e.y, -e.x).
        out[0] = flip ? -ey :  ey;
        out[1] = flip ?  ex : -ex;
    }

    bool OffsetChain2D( const std::vector<float> &src, bool closed, float d,
                        std::vector<float> *out, const char **why )
    {
        out->clear();
        const int n = (int)( src.size() / 2 );
        if ( n < 2 )
        {
            *why = "not enough points";
            return false;
        }

        // Closed winding decides outward; open positive distance is right of travel.
        bool flip = false;
        float area0 = 0.0f;
        if ( closed )
        {
            area0 = KiwiRegion_SignedArea( src );
            // A zero-area closed chain has no outward side; refuse before winding tests.
            if ( area0 == 0.0f )
            {
                *why = "the loop encloses no area";
                return false;
            }
            flip = ( area0 < 0.0f );                     // CW loop: mirror the normal
        }

        const int edges = closed ? n : ( n - 1 );
        std::vector<float> nrm;                          // uv normal per edge
        nrm.resize( (size_t)edges * 2 );
        for ( int i = 0; i < edges; ++i )
            EdgeNormal( &src[(size_t)i*2], &src[( (size_t)( i + 1 ) % n )*2],
                        flip, &nrm[(size_t)i*2] );

        const float miterMax = KOFF_MITER_LIMIT * fabsf( d );

        // Emit the prev/next join at each vertex.
        const int firstJoint = closed ? 0 : 1;
        const int lastJoint  = closed ? n : ( n - 1 );

        if ( !closed )
        {
            out->push_back( src[0] + nrm[0] * d );
            out->push_back( src[1] + nrm[1] * d );
        }

        for ( int j = firstJoint; j < lastJoint; ++j )
        {
            const int prev = ( j + edges - 1 ) % edges;
            const int nxt  = j % edges;
            const float *v = &src[(size_t)( ( j % n ) * 2 )];
            const float *np = &nrm[(size_t)prev*2];
            const float *nn = &nrm[(size_t)nxt*2];

            // Intersect the two shifted lines; `cross` is the sine of their turn.
            const float cross = Cross2( np[0], np[1], nn[0], nn[1] );
            const float dot   = np[0]*nn[0] + np[1]*nn[1];

            if ( fabsf( cross ) < 1.0e-6f && dot > 0.0f )
            {
                out->push_back( v[0] + nn[0] * d );
                out->push_back( v[1] + nn[1] * d );
                continue;
            }

            bool bevel = ( fabsf( cross ) < 1.0e-6f );   // a 180-degree spike
            float mx = 0.0f, my = 0.0f;
            if ( !bevel )
            {
                // The unit normal sum is the miter direction; |np + nn| / 2 is cosHalf.
                float sx = np[0] + nn[0], sy = np[1] + nn[1];
                const float slen = sqrtf( sx*sx + sy*sy );
                if ( slen < 1.0e-6f )
                {
                    bevel = true;
                }
                else
                {
                    sx /= slen;  sy /= slen;
                    const float cosHalf = slen * 0.5f;   // == cos(half turn angle)
                    const float mlen    = d / ( cosHalf > 1.0e-4f ? cosHalf : 1.0e-4f );
                    if ( fabsf( mlen ) > miterMax && miterMax > 0.0f )
                        bevel = true;
                    else
                    {
                        mx = v[0] + sx * mlen;
                        my = v[1] + sy * mlen;
                    }
                }
            }

            if ( bevel )
            {
                out->push_back( v[0] + np[0] * d );
                out->push_back( v[1] + np[1] * d );
                out->push_back( v[0] + nn[0] * d );
                out->push_back( v[1] + nn[1] * d );
            }
            else
            {
                out->push_back( mx );
                out->push_back( my );
            }
        }

        if ( !closed )
        {
            const int e = edges - 1;
            out->push_back( src[( (size_t)n - 1 )*2 + 0] + nrm[(size_t)e*2 + 0] * d );
            out->push_back( src[( (size_t)n - 1 )*2 + 1] + nrm[(size_t)e*2 + 1] * d );
        }

        const int m = (int)( out->size() / 2 );
        if ( m < 2 )
        {
            *why = "the offset collapsed";
            return false;
        }
        if ( m > KCON_MAX_POINTS )
        {
            *why = "too many points";
            return false;
        }
        if ( closed )
        {
            const float area1 = KiwiRegion_SignedArea( *out );
            if ( area1 == 0.0f || ( area1 > 0.0f ) != ( area0 > 0.0f ) )
            {
                *why = "the loop turned inside out";
                return false;
            }
            if ( KiwiRegion_SelfIntersects( *out ) )
            {
                *why = "the offset loop crosses itself";
                return false;
            }
        }
        else if ( OpenChainSelfIntersects( *out ) )
        {
            *why = "the offset chain crosses itself";
            return false;
        }
        return true;
    }

    // Drag sign uses the same nearest-edge normals as the offset.
    float SignedDistance2D( const std::vector<float> &src, bool closed,
                            float px, float py )
    {
        const int n     = (int)( src.size() / 2 );
        const int edges = closed ? n : ( n - 1 );
        float best      = 0.0f;
        float bestSign  = 1.0f;
        bool  have      = false;

        for ( int i = 0; i < edges; ++i )
        {
            const float *a = &src[(size_t)i*2];
            const float *b = &src[(size_t)( ( i + 1 ) % n )*2];
            const float ex = b[0]-a[0], ey = b[1]-a[1];
            const float len2 = ex*ex + ey*ey;
            float t = 0.0f;
            if ( len2 > 1.0e-9f )
            {
                t = ( ( px - a[0] ) * ex + ( py - a[1] ) * ey ) / len2;
                if ( t < 0.0f ) t = 0.0f;
                if ( t > 1.0f ) t = 1.0f;
            }
            const float dx = px - ( a[0] + ex * t );
            const float dy = py - ( a[1] + ey * t );
            const float dist = sqrtf( dx*dx + dy*dy );
            if ( have && dist >= best )
                continue;

            float nr[2];
            EdgeNormal( a, b, false, nr );
            best     = dist;
            bestSign = ( dx*nr[0] + dy*nr[1] >= 0.0f ) ? 1.0f : -1.0f;
            have     = true;
        }
        if ( !have )
            return 0.0f;

        // OffsetChain2D mirrors CW normals, so mirror drag sign from the same area test.
        // Nearest-edge sign can flip past a concave loop's medial axis; typing stays exact.
        if ( closed )
        {
            const float area = KiwiRegion_SignedArea( src );
            if ( area < 0.0f )
                bestSign = -bestSign;
        }
        return best * bestSign;
    }

    class KiwiOffsetCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Offset Curve"; }
        bool CanExecute() override        { return KiwiOffset_CanOffset(); }

        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool HudInvalid() const override      { return m_invalid; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KOFF_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out || !m_have )
                return false;
            *out = m_dist;
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !m_have || m_preview.empty() )
                return false;
            // Anchor the value to the middle preview vertex.
            const int n = (int)( m_preview.size() / 3 );
            const int k = n / 2;
            out3[0] = m_preview[(size_t)k*3 + 0];
            out3[1] = m_preview[(size_t)k*3 + 1];
            out3[2] = m_preview[(size_t)k*3 + 2];
            return true;
        }

        bool Begin() override
        {
            Reset();
            m_object = SelectedObject();
            const kconObject_t *o = ( m_object >= 0 ) ? KiwiCon_At( m_object ) : 0;
            if ( !o || !Offsettable( *o ) )
            {
                Sys_Printf( "Offset: select a construction line, polyline, rectangle, "
                            "circle or arc first.\n" );
                return false;
            }
            m_parametric = KiwiCon_IsParametric( *o );
            m_closed     = o->closed;
            m_radius0    = o->radius;

            const char *why = 0;
            if ( !PlaneFor( *o, &m_plane, &why ) )
            {
                Sys_Printf( "Offset: %s.\n", why ? why : "no plane" );
                return false;
            }
            if ( m_parametric )
            {
                m_srcPlane = o->plane;                   // authoritative for CIRCLE/ARC
            }
            else if ( !ToPlaneChain( *o, m_plane, &m_src ) )
            {
                Sys_Printf( "Offset: the chain has fewer than two distinct points.\n" );
                return false;
            }

            m_have = true;
            LatchStart();
            Recompute();
            Sys_Printf( "Offset: drag to the side you want the copy on, or type a "
                        "distance.  RMB/Enter commits, Esc cancels.\n" );
            return true;
        }

        void Rebase() override
        {
            // Re-latch on PAUSED -> HOT; bias the delta origin by the current value
            // so resuming neither jumps nor resets the offset.
            const float keep = m_dist;
            m_haveStart = false;
            LatchStart();
            if ( m_haveStart )
                m_start -= keep;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;  (void)snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
            g_nUpdateBits |= 1;
        }

        void Commit() override
        {
            if ( !m_have || m_invalid || fabsf( m_dist ) < KOFF_MIN_DIST )
            {
                Sys_Printf( "Offset: cancelled — %s.\n",
                            m_why ? m_why
                                  : ( fabsf( m_dist ) < KOFF_MIN_DIST ? "distance too small"
                                                                      : "nothing to offset" ) );
                Reset();
                g_nUpdateBits |= 1;
                return;
            }

            // Build from the offset-validated preview; push construction undo before add.
            kconObject_t o;
            if ( m_parametric )
            {
                const kconObject_t *src = KiwiCon_At( m_object );
                if ( !src )
                {
                    Sys_Printf( "Offset: the object went away.\n" );
                    Reset();
                    g_nUpdateBits |= 1;       // redraw after Reset clears the preview
                    return;
                }
                o        = *src;                          // centre / plane / sweep kept
                o.radius = m_radius0 + m_dist;
            }
            else
            {
                o.type   = ( (int)( m_preview.size() / 3 ) == 2 ) ? KCON_LINE : KCON_POLYLINE;
                o.plane  = m_plane;                       // a SEED; KiwiCon_Add refits
                o.pts    = m_preview;
                o.closed = m_closed;
            }

            KiwiCon_UndoPush();
            const int added = KiwiCon_Add( o );
            if ( added < 0 )
            {
                // Store validation failed after the undo snapshot was pushed.
                Sys_Printf( "Offset: the store refused the new object.\n" );
            }
            else
            {
                char b[32];
                KiwiUnits_Format( b, sizeof( b ), fabsf( m_dist ) );
                Sys_Printf( "Offset: new %s at %s.\n",
                            m_parametric ? "circle/arc" : ( m_closed ? "loop" : "chain" ), b );
            }
            Reset();
            g_nUpdateBits |= 1;
        }

        void Cancel() override
        {
            Reset();
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( !m_have || m_preview.empty() )
                return;
            const float *col = m_invalid ? KOFF_COL_BAD : KOFF_COL_OK;
            KiwiLines_Color( col[0], col[1], col[2] );
            const int n = (int)( m_preview.size() / 3 );
            for ( int i = 0; i + 1 < n; ++i )
                if ( !KiwiLines_Add( &m_preview[(size_t)i*3], &m_preview[( (size_t)i+1 )*3] ) )
                    return;
            if ( m_closed && n >= 3 )
                KiwiLines_Add( &m_preview[( (size_t)n-1 )*3], &m_preview[0] );
        }

    private:
        void Reset()
        {
            m_object     = -1;
            m_have       = false;
            m_haveStart  = false;
            m_parametric = false;
            m_closed     = false;
            m_invalid    = false;
            m_hasNum     = false;
            m_numWorld   = 0.0f;
            m_dist       = 0.0f;
            m_start      = 0.0f;
            m_radius0    = 0.0f;
            m_why        = 0;
            m_hud[0]     = '\0';
            m_src.clear();
            m_preview.clear();
        }

        // Chain scalars are signed in-plane distance; parametric scalars change radius.
        bool RawScalar( float *out ) const
        {
            int cx = 0, cy = 0;
            ray_t ray;
            float w[3], uv[2];
            if ( !KiwiCmd_LastCursor( &cx, &cy ) )
                return false;
            if ( !Pick_RayFromImagePos( cx, cy, &ray ) )
                return false;

            if ( m_parametric )
            {
                if ( !KiwiCon_RayPlaneBounded( m_srcPlane, ray, w ) )
                    return false;
                KiwiCon_WorldToPlane( m_srcPlane, w, uv );
                const float dx = uv[0] - SrcCentre()[0];
                const float dy = uv[1] - SrcCentre()[1];
                *out = sqrtf( dx*dx + dy*dy ) - m_radius0;
                return true;
            }

            if ( !KiwiCon_RayPlaneBounded( m_plane, ray, w ) )
                return false;
            KiwiCon_WorldToPlane( m_plane, w, uv );
            *out = SignedDistance2D( m_src, m_closed, uv[0], uv[1] );
            return true;
        }

        // Read the plane-space centre live; a missing source falls back to the origin.
        const float *SrcCentre() const
        {
            static const float zero[2] = { 0.0f, 0.0f };
            const kconObject_t *o = ( m_object >= 0 ) ? KiwiCon_At( m_object ) : 0;
            return o ? o->centre : zero;
        }

        void LatchStart()
        {
            float raw = 0.0f;
            if ( !RawScalar( &raw ) )
                return;
            m_start     = raw;
            m_haveStart = true;
        }

        // Quantise the world-unit scalar, never the cursor point.
        float SnapScalar( float d ) const
        {
            const float g = KiwiUnits_GridSpacingWorld();
            if ( g > 0.0f )
                return floorf( d / g + 0.5f ) * g;
            return d;
        }

        void Recompute()
        {
            if ( !m_have )
                return;
            const kconObject_t *o = ( m_object >= 0 ) ? KiwiCon_At( m_object ) : 0;
            if ( !o )
            {
                Sys_Printf( "Offset: the source went away — cancelled.\n" );
                KiwiCmd_Cancel();
                return;
            }

            if ( m_hasNum )
            {
                m_dist = m_numWorld;
            }
            else
            {
                if ( !m_haveStart )
                    LatchStart();
                float raw = 0.0f;
                if ( m_haveStart && RawScalar( &raw ) )
                    m_dist = SnapScalar( raw - m_start );
            }

            m_invalid = false;
            m_why     = 0;
            m_preview.clear();

            if ( fabsf( m_dist ) < KOFF_MIN_DIST )
            {
                m_invalid = true;
                m_why     = "distance too small";
                UpdateHud();
                return;
            }

            if ( m_parametric )
            {
                const float r = m_radius0 + m_dist;
                if ( r <= KOFF_MIN_RADIUS )
                {
                    m_invalid = true;
                    m_why     = "radius would collapse";
                    UpdateHud();
                    return;
                }
                BuildParametricPreview( *o, r );
                UpdateHud();
                return;
            }

            std::vector<float> out2;
            const char *why = 0;
            if ( !OffsetChain2D( m_src, m_closed, m_dist, &out2, &why ) )
            {
                m_invalid = true;
                m_why     = why;
                UpdateHud();
                return;
            }
            const int n = (int)( out2.size() / 2 );
            m_preview.reserve( (size_t)n * 3 );
            for ( int i = 0; i < n; ++i )
            {
                float w[3];
                KiwiCon_PlaneToWorld( m_plane, &out2[(size_t)i*2], w );
                m_preview.push_back( w[0] );
                m_preview.push_back( w[1] );
                m_preview.push_back( w[2] );
            }
            UpdateHud();
        }

        // Use store tessellation so preview and committed parametric geometry match.
        void BuildParametricPreview( const kconObject_t &src, float radius )
        {
            kconObject_t tmp = src;
            tmp.radius = radius;
            const int n = KiwiCon_VertCount( tmp );
            m_preview.reserve( (size_t)n * 3 );
            for ( int i = 0; i < n; ++i )
            {
                float w[3];
                if ( !KiwiCon_VertWorld( tmp, i, w ) )
                    break;
                m_preview.push_back( w[0] );
                m_preview.push_back( w[1] );
                m_preview.push_back( w[2] );
            }
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), fabsf( m_dist ) );
            if ( m_invalid )
                SetHud( "offset %s  ·  REFUSED: %s", b, m_why ? m_why : "invalid" );
            else
                SetHud( "offset %s  %s  ·  RMB/Enter: commit  ·  Esc: cancel",
                        b, m_parametric ? "(radius)" : ( m_closed ? "(loop)" : "(chain)" ) );
        }

        void SetHud( const char *fmt, ... )
        {
            va_list ap;
            va_start( ap, fmt );
            _vsnprintf( m_hud, sizeof( m_hud ), fmt, ap );
            va_end( ap );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        int                m_object     = -1;
        bool               m_have       = false;
        bool               m_haveStart  = false;
        bool               m_parametric = false;
        bool               m_closed     = false;
        bool               m_invalid    = false;
        bool               m_hasNum     = false;
        float              m_numWorld   = 0.0f;
        float              m_dist       = 0.0f;
        float              m_start      = 0.0f;
        float              m_radius0    = 0.0f;
        const char        *m_why        = 0;
        kconPlane_t        m_plane;                  // the OFFSET plane (chains)
        kconPlane_t        m_srcPlane;               // the source's own (CIRCLE/ARC)
        std::vector<float> m_src;                    // plane-space source chain
        std::vector<float> m_preview;                // world-space result
        char               m_hud[192] = { 0 };
    };

    KiwiOffsetCommand s_offset;
}

bool KiwiOffset_CanOffset()
{
    const int idx = SelectedObject();
    if ( idx < 0 )
        return false;
    const kconObject_t *o = KiwiCon_At( idx );
    return o && Offsettable( *o );
}

void KiwiOffset_RegisterCommands()
{
    // Classic leaves this unbound; the modern keymap assigns Plasticity's O and
    // displaces ViewConsole (kiwi_keymap.h).
    Radiant_RegisterCommand( "KiwiOffsetCurve", 0, 0, KIWI_CMD_OFFSET_CURVE );
}

KiwiEditorCommand *KiwiOffset_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_OFFSET_CURVE ) ? &s_offset : 0;
}
