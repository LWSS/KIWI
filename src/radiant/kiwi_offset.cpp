#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_offset.cpp — ROUND J: OFFSET CURVE (O).  See kiwi_offset.h for the
// Plasticity source, the plane rules, the miter/bevel joins and the refusal list.
//
// NEW code.  It touches NO map data: the whole file works on the construction
// store, which is editor-only scaffolding.
// ─────────────────────────────────────────────────────────────────────────────

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

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <vector>

// ── ported entry points (each verified against its DEFINITION) ─────────────
//   win_qe3.cpp:112        int  Sys_Printf( const char *fmt, ... )
//   engine_stubs.cpp:693   int  g_nUpdateBits = 0;   // 0x25d5a74
//   mainfrm.cpp:1251       bool Radiant_RegisterCommand( const char *name, byte vk,
//                                                        byte mods, int commandId )
extern int  Sys_Printf( const char *fmt, ... );
extern int  g_nUpdateBits;
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // §18 palette: the live offset preview is construction ROSE like the geometry
    // it parallels, and turns RED the moment the result is refused — the same two
    // colours kiwi_bevel.cpp uses for exactly the same "this will / will not
    // commit" message.
    const float KOFF_COL_OK [3] = { 1.00f, 0.55f, 0.72f };
    const float KOFF_COL_BAD[3] = { 1.00f, 0.22f, 0.18f };

    // ONE named LENGTH field (shakeout E).  Static storage: the numeric layer
    // copies the structs but never the label string (kiwi_numeric.h).
    const kiwiNumField_t KOFF_FIELDS[1] = { { "offset", KNUM_LENGTH, false } };

    inline void  Sub3 ( const float *a, const float *b, float *o )
    { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
    inline float Dot3 ( const float *a, const float *b )
    { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    inline float Len3 ( const float *a ) { return sqrtf( Dot3( a, a ) ); }

    inline float Cross2( float ax, float ay, float bx, float by ) { return ax*by - ay*bx; }

    // ── the source, resolved from the construction selection ────────────────
    // The FIRST selected object, at any granularity (kiwi_offset.h).  -1 = none.
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
        if ( KiwiCon_IsParametric( o ) )                 // CIRCLE / ARC
            return o.radius > KOFF_MIN_RADIUS;
        return (int)( o.pts.size() / 3 ) >= 2;           // LINE / POLYLINE / RECT
    }

    // ── THE PLANE (kiwi_offset.h "THE PLANE IT OFFSETS IN") ─────────────────
    // Returns false with `why` set when there is no sideways to offset along.
    bool PlaneFor( const kconObject_t &o, kconPlane_t *out, const char **why )
    {
        if ( KiwiCon_ObjectPlane( o, out ) )
            return true;

        // A straight chain.  Newell refused it, correctly — it determines no plane
        // of its own — so the ACTIVE construction plane's normal decides, exactly
        // as Plasticity feeds its factory the viewport's construction plane
        // (OffsetCurveCommand.ts:15).
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

    // ── plane-space chain, duplicates culled ────────────────────────────────
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
        // A closed chain must not repeat its first point (the §8 toolkit's own
        // convention — kiwi_region.h kregion_t::pts "first point NOT repeated").
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

    // Do two NON-ADJACENT segments of an OPEN chain cross?  The closed case uses
    // KiwiRegion_SelfIntersects instead (kiwi_offset.h REFUSALS): that helper reads
    // its input as a RING, which an open chain is not.
    bool SegCross2D( const float *a0, const float *a1, const float *b0, const float *b1 )
    {
        const float d1x = a1[0]-a0[0], d1y = a1[1]-a0[1];
        const float d2x = b1[0]-b0[0], d2y = b1[1]-b0[1];
        const float den = Cross2( d1x, d1y, d2x, d2y );
        if ( fabsf( den ) < 1.0e-9f )
            return false;                                // parallel: never "crosses"
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

    // ── THE 2D OFFSET ───────────────────────────────────────────────────────
    // `src` is a plane-space chain (2 floats per point, first point NOT repeated
    // when closed).  Positive `d` is the direction of each edge's `Normal` below.
    // Returns false with `why` set on a refusal.
    void EdgeNormal( const float *p0, const float *p1, bool flip, float *out )
    {
        float ex = p1[0] - p0[0], ey = p1[1] - p0[1];
        const float len = sqrtf( ex*ex + ey*ey );
        if ( len > 1.0e-9f ) { ex /= len; ey /= len; }
        // For a CCW loop (signed area > 0) the OUTWARD normal of an edge walked in
        // winding order is ( e.y, -e.x ) — check it on the unit square:
        // (0,0)->(1,0) gives (0,-1), which points away from the interior.
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

        // SIGN.  A closed loop's own winding decides which side "outward" is; an
        // open chain has no outward, so positive is the right of travel and the
        // flip is never applied (kiwi_offset.h SIGN).
        bool flip = false;
        float area0 = 0.0f;
        if ( closed )
        {
            area0 = KiwiRegion_SignedArea( src );
            // A "closed" chain with NO AREA is collinear (or under three points):
            // KiwiRegion_SignedArea returns exactly 0 for both (kiwi_region.cpp:
            // 884-886).  It has no inside, so it has no outward, and the
            // winding-flip refusal below cannot fire on it either — `area0 > 0` is
            // false and a negative result would compare equal.  Refuse here, where
            // the reason is still knowable.
            if ( area0 == 0.0f )
            {
                *why = "the loop encloses no area";
                return false;
            }
            flip = ( area0 < 0.0f );                     // CW loop: mirror the normal
        }

        const int edges = closed ? n : ( n - 1 );
        std::vector<float> nrm;                          // 2 floats per EDGE
        nrm.resize( (size_t)edges * 2 );
        for ( int i = 0; i < edges; ++i )
            EdgeNormal( &src[(size_t)i*2], &src[( (size_t)( i + 1 ) % n )*2],
                        flip, &nrm[(size_t)i*2] );

        const float miterMax = KOFF_MITER_LIMIT * fabsf( d );

        // Emit the joint between edge `prev` and edge `next` at vertex `v`.
        // Written as a lambda-free helper loop so the open/closed cases share it.
        const int firstJoint = closed ? 0 : 1;
        const int lastJoint  = closed ? n : ( n - 1 );

        if ( !closed )
        {
            // The free start: the first edge's shifted origin.
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

            // The two shifted lines: through v+np*d with direction perp(np), and
            // through v+nn*d with direction perp(nn).  Their intersection is the
            // miter point; `cross` is the sine of the turn.
            const float cross = Cross2( np[0], np[1], nn[0], nn[1] );
            const float dot   = np[0]*nn[0] + np[1]*nn[1];

            if ( fabsf( cross ) < 1.0e-6f && dot > 0.0f )
            {
                // Collinear continuation — one point, shifted.
                out->push_back( v[0] + nn[0] * d );
                out->push_back( v[1] + nn[1] * d );
                continue;
            }

            bool bevel = ( fabsf( cross ) < 1.0e-6f );   // a 180-degree spike
            float mx = 0.0f, my = 0.0f;
            if ( !bevel )
            {
                // Miter direction is the normalised sum of the two normals; its
                // length along that sum is |d| / cos(half-angle), and
                // cos(half-angle)^2 == (1 + dot)/2.
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
            // The free end: the last edge's shifted terminus.
            const int e = edges - 1;
            out->push_back( src[( (size_t)n - 1 )*2 + 0] + nrm[(size_t)e*2 + 0] * d );
            out->push_back( src[( (size_t)n - 1 )*2 + 1] + nrm[(size_t)e*2 + 1] * d );
        }

        // ── the refusals (kiwi_offset.h) ────────────────────────────────────
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

    // ── the SIGNED distance from a plane-space point to the chain ───────────
    // This is what maps the DRAG: the offset goes where the cursor is, so the user
    // never has to know the sign convention.  The sign uses the SAME per-edge
    // normal the offset itself uses, so the two can never disagree.
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

        // A CW loop's normals are mirrored by OffsetChain2D's `flip`, so the sign
        // above — computed with the UNFLIPPED normal — has to be mirrored too, or
        // dragging outward on a CW loop would offset inward.  One global flip from
        // the same KiwiRegion_SignedArea the offset itself reads, so the two cannot
        // disagree about which side is which.
        //
        // WHAT THIS DOES NOT DO: it is still a NEAREST-EDGE sign, not a
        // point-in-polygon test, so past the loop's medial axis (deep inside a
        // notch, further from every edge than the notch is wide) the nearest edge
        // can be one whose normal points the other way.  The cursor has to be a
        // long way inside a concave shape for that, and the consequence is a
        // sign flip in the DRAG only — the offset itself is unaffected, and the
        // typed field is always exact.
        if ( closed )
        {
            const float area = KiwiRegion_SignedArea( src );
            if ( area < 0.0f )
                bestSign = -bestSign;
        }
        return best * bestSign;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  The command.
    // ═══════════════════════════════════════════════════════════════════════
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
            // The middle of the offset chain — the geometry the value describes,
            // which is what §13b asks for.
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
            // A PAUSED -> HOT edge: re-latch the raw scalar under the cursor so
            // resuming does not jump the offset by however far the cursor wandered.
            //
            // THE `keep` IS NOT OPTIONAL.  The mapping is a DELTA from `m_start`
            // (Recompute below), so re-latching `m_start = raw` alone makes the very
            // next Recompute produce ZERO — resume a 32-unit offset and it collapses
            // to "distance too small" the instant you press LMB.  Biasing the new
            // origin by the current value is what makes the cursor reproduce it, and
            // it is the same three lines kiwi_extrude.cpp:291-299 uses.
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

            // Build the object from the ALREADY-VALIDATED preview.  Nothing below
            // can decline, which is why the snapshot is pushed here and not before
            // (kiwi_offset.h UNDO; the lesson is kiwi_trim.cpp's).
            kconObject_t o;
            if ( m_parametric )
            {
                const kconObject_t *src = KiwiCon_At( m_object );
                if ( !src )
                {
                    Sys_Printf( "Offset: the object went away.\n" );
                    Reset();
                    g_nUpdateBits |= 1;       // …and drop the dead preview with it
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
                // KiwiCon_Add's own rejection (the store's point cap).  The push is
                // already spent; leaving it is the honest outcome — one Ctrl+Z
                // restores a store that did not change, which is strictly better
                // than stranding a journal ticket (kiwi_trim.cpp's note).
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
            // The gesture stored nothing, so there is nothing to roll back.
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

        // The cursor's raw scalar in the offset's own units: for a chain the signed
        // in-plane distance to it, for a circle/arc the signed change in radius.
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

        // The parametric source's centre (plane space), read live so a store edit
        // cannot leave a stale copy behind.  Returns the origin when it has gone.
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

        // §6 numeric hygiene: quantise the SCALAR, never the cursor point.
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

        // The circle/arc preview is drawn from the SAME tessellation rule the store
        // uses, by building a throwaway object and asking it — so the preview and
        // the committed object can never be different shapes.
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

// ─── the public surface ──────────────────────────────────────────────────────
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
    // Unbound here — the CLASSIC-profile row.  kiwi_keymap.cpp puts Offset on the
    // bare O in the modern profile (Plasticity's own key,
    // default-keymap.ts:268), with the ViewConsole displacement; the audit is in
    // kiwi_keymap.h.
    Radiant_RegisterCommand( "KiwiOffsetCurve", 0, 0, KIWI_CMD_OFFSET_CURVE );
}

KiwiEditorCommand *KiwiOffset_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_OFFSET_CURVE ) ? &s_offset : 0;
}
