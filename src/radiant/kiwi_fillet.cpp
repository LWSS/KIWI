#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_fillet.cpp — ROUND J: FILLET CORNERS (B).  See kiwi_fillet.h for the
// Plasticity source (ContourFilletFactory + the fillet-all gizmo), the per-corner
// clamp, and why the result is a tessellated polyline.
//
// NEW code.  It touches NO map data: the whole file works on the construction
// store, which is editor-only scaffolding.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_fillet.h"
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
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
    // §18: the same two colours kiwi_offset.cpp uses, for the same reason — one
    // "this will commit" and one "this is refused".
    const float KFIL_COL_OK [3] = { 1.00f, 0.55f, 0.72f };
    const float KFIL_COL_BAD[3] = { 1.00f, 0.22f, 0.18f };

    const kiwiNumField_t KFIL_FIELDS[1] = { { "radius", KNUM_LENGTH, false } };

    const float KFIL_PI = 3.14159265358979323846f;

    // Two plane-space points closer than this are the same point.
    const float KFIL_WELD_2D = 0.01f;

    // ── the source, resolved from the construction selection ────────────────
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

    // kiwi_fillet.h: straight-segment chains only.  A circle has no corners and a
    // parametric arc is already round.
    bool Filletable( const kconObject_t &o )
    {
        if ( KiwiCon_IsParametric( o ) )
            return false;
        // Three points is the floor either way: an open chain needs one interior
        // vertex, a closed one is not a loop below three.
        return (int)( o.pts.size() / 3 ) >= 3;
    }

    // ── the corner SET (ContourFilletFactory.ts:44-47 / :53-72) ─────────────
    // `chosen` is indexed by CHAIN VERTEX and says whether that vertex may round.
    // Closed: every vertex.  Open: the interior ones.  Narrowed to the SELECTED
    // anchors when the construction selection names any point on this object.
    void ChooseCorners( int object, int n, bool closed, std::vector<char> *chosen,
                        bool *outNarrowed )
    {
        chosen->assign( (size_t)n, 0 );
        *outNarrowed = false;

        // Which anchors did the user actually name?  (KCONSEL_POINT items index
        // the object's ANCHORS, which for LINE/POLYLINE/RECT are its defining
        // points — kiwi_construct.h "snap ANCHORS".)
        std::vector<char> named;
        named.assign( (size_t)n, 0 );
        int namedCount = 0;
        const int selCount = KiwiConSel_Count();
        for ( int i = 0; i < selCount; ++i )
        {
            const kconSelItem_t *it = KiwiConSel_At( i );
            if ( !it || it->object != object || it->kind != KCONSEL_POINT )
                continue;
            if ( it->index >= 0 && it->index < n )
            {
                if ( !named[(size_t)it->index] )
                    ++namedCount;
                named[(size_t)it->index] = 1;
            }
        }

        for ( int i = 0; i < n; ++i )
        {
            const bool fillable = closed ? true : ( i > 0 && i < n - 1 );
            if ( !fillable )
                continue;
            if ( namedCount > 0 && !named[(size_t)i] )
                continue;
            (*chosen)[(size_t)i] = 1;
        }
        *outNarrowed = ( namedCount > 0 );
    }

    // ── plane-space chain, duplicates culled ────────────────────────────────
    bool ToPlaneChain( const kconObject_t &o, const kconPlane_t &plane,
                       std::vector<float> *out, std::vector<int> *outSrcIndex )
    {
        out->clear();
        outSrcIndex->clear();
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
                if ( dx * dx + dy * dy < KFIL_WELD_2D * KFIL_WELD_2D )
                    continue;
            }
            out->push_back( uv[0] );
            out->push_back( uv[1] );
            outSrcIndex->push_back( i );
        }
        const int have = (int)( out->size() / 2 );
        if ( o.closed && have >= 2 )
        {
            const float dx = (*out)[0] - (*out)[( (size_t)have - 1 ) * 2 + 0];
            const float dy = (*out)[1] - (*out)[( (size_t)have - 1 ) * 2 + 1];
            if ( dx * dx + dy * dy < KFIL_WELD_2D * KFIL_WELD_2D )
            {
                out->resize( ( (size_t)have - 1 ) * 2 );
                outSrcIndex->pop_back();
            }
        }
        return (int)( out->size() / 2 ) >= 3;
    }

    struct filletStats_t
    {
        int rounded = 0;        // corners that produced an arc
        int clamped = 0;        // …of which were limited by their own edges
        int skipped = 0;        // chosen but too flat / too sharp
    };

    // ── THE FILLET (ContourFilletFactory's job, in plane space) ─────────────
    // `src` is a plane-space chain (2 floats per point, first NOT repeated when
    // closed).  `chosen` is per-vertex.  Writes the rounded chain into `out`.
    bool FilletChain2D( const std::vector<float> &src, bool closed,
                        const std::vector<char> &chosen, float radius,
                        std::vector<float> *out, filletStats_t *stats,
                        const char **why )
    {
        out->clear();
        *stats = filletStats_t();
        const int n = (int)( src.size() / 2 );
        if ( n < 3 )
        {
            *why = "not enough points";
            return false;
        }

        for ( int i = 0; i < n; ++i )
        {
            const float *B = &src[(size_t)i * 2];

            if ( !chosen[(size_t)i] )
            {
                out->push_back( B[0] );
                out->push_back( B[1] );
                continue;
            }

            const int ia = ( i - 1 + n ) % n;
            const int ic = ( i + 1 ) % n;
            const float *A = &src[(size_t)ia * 2];
            const float *C = &src[(size_t)ic * 2];

            float ux = A[0]-B[0], uy = A[1]-B[1];
            float wx = C[0]-B[0], wy = C[1]-B[1];
            const float ulen = sqrtf( ux*ux + uy*uy );
            const float wlen = sqrtf( wx*wx + wy*wy );
            if ( ulen < 1.0e-4f || wlen < 1.0e-4f )
            {
                ++stats->skipped;
                out->push_back( B[0] );  out->push_back( B[1] );
                continue;
            }
            ux /= ulen;  uy /= ulen;
            wx /= wlen;  wy /= wlen;

            float cosT = ux*wx + uy*wy;
            if ( cosT >  1.0f ) cosT =  1.0f;
            if ( cosT < -1.0f ) cosT = -1.0f;
            // cosT == -1 is a STRAIGHT continuation (the two edges point opposite
            // ways from B), cosT == +1 is a spike doubling back on itself.  Both
            // are outside what a fillet means.
            if ( cosT <= -KFIL_FLAT_DOT || cosT >= KFIL_FLAT_DOT )
            {
                ++stats->skipped;
                out->push_back( B[0] );  out->push_back( B[1] );
                continue;
            }

            const float theta   = acosf( cosT );          // interior angle at B
            const float tanHalf = tanf( theta * 0.5f );
            const float sinHalf = sinf( theta * 0.5f );
            if ( tanHalf < 1.0e-4f || sinHalf < 1.0e-4f )
            {
                ++stats->skipped;
                out->push_back( B[0] );  out->push_back( B[1] );
                continue;
            }

            // THE CLAMP (kiwi_fillet.h): half the shorter adjacent edge is what
            // this corner may spend, so two corners sharing an edge can never
            // overlap.
            const float tAvail = 0.5f * ( ( ulen < wlen ) ? ulen : wlen );
            float r = radius;
            const float rMax = tAvail * tanHalf;
            if ( r > rMax )
            {
                r = rMax;
                ++stats->clamped;
            }
            if ( r < KFIL_MIN_RADIUS )
            {
                ++stats->skipped;
                out->push_back( B[0] );  out->push_back( B[1] );
                continue;
            }

            const float t = r / tanHalf;                  // tangent length
            const float p0x = B[0] + ux * t, p0y = B[1] + uy * t;
            const float p1x = B[0] + wx * t, p1y = B[1] + wy * t;

            // Centre: along the interior bisector at r / sin(theta/2).
            float bx = ux + wx, by = uy + wy;
            const float blen = sqrtf( bx*bx + by*by );
            if ( blen < 1.0e-5f )
            {
                ++stats->skipped;
                out->push_back( B[0] );  out->push_back( B[1] );
                continue;
            }
            bx /= blen;  by /= blen;
            const float cx = B[0] + bx * ( r / sinHalf );
            const float cy = B[1] + by * ( r / sinHalf );

            // The sweep is pi - theta, taken the SHORT way from p0 to p1.
            const float a0 = atan2f( p0y - cy, p0x - cx );
            const float a1 = atan2f( p1y - cy, p1x - cx );
            float sweep = a1 - a0;
            while ( sweep >  KFIL_PI ) sweep -= 2.0f * KFIL_PI;
            while ( sweep < -KFIL_PI ) sweep += 2.0f * KFIL_PI;

            // Density: the STORE'S own rule (KCON_SEGS_PER_UNIT), pro-rata over the
            // sweep, so a filleted corner is exactly as smooth as a drawn arc of
            // the same radius.
            const float arcLen = r * fabsf( sweep );
            int segs = (int)floorf( arcLen * KCON_SEGS_PER_UNIT + 0.5f );
            if ( segs < KFIL_SEGS_MIN ) segs = KFIL_SEGS_MIN;
            if ( segs > KFIL_SEGS_MAX ) segs = KFIL_SEGS_MAX;

            for ( int s = 0; s <= segs; ++s )
            {
                const float a = a0 + sweep * ( (float)s / (float)segs );
                out->push_back( cx + cosf( a ) * r );
                out->push_back( cy + sinf( a ) * r );
            }
            ++stats->rounded;
        }

        if ( stats->rounded == 0 )
        {
            *why = "no corner is large enough to round";
            return false;
        }
        if ( (int)( out->size() / 2 ) > KCON_MAX_POINTS )
        {
            *why = "the rounded chain needs too many points";
            return false;
        }
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  The command.
    // ═══════════════════════════════════════════════════════════════════════
    class KiwiFilletCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Fillet Corners"; }
        bool CanExecute() override        { return KiwiFillet_CanFillet(); }

        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool HudInvalid() const override      { return m_invalid; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KFIL_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out || !m_have )
                return false;
            *out = m_radius;
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !m_have || !m_haveRef )
                return false;
            out3[0] = m_ref[0];  out3[1] = m_ref[1];  out3[2] = m_ref[2];
            return true;
        }

        bool Begin() override
        {
            Reset();
            m_object = SelectedObject();
            const kconObject_t *o = ( m_object >= 0 ) ? KiwiCon_At( m_object ) : 0;
            if ( !o || !Filletable( *o ) )
            {
                Sys_Printf( "Fillet: select a construction polyline or rectangle "
                            "first (a line has no corners, a circle is already round).\n" );
                return false;
            }
            if ( !KiwiCon_ObjectPlane( *o, &m_plane ) )
            {
                Sys_Printf( "Fillet: this chain does not lie in one plane, so its "
                            "corners have no arc.\n" );
                return false;
            }
            m_closed = o->closed;

            std::vector<int> srcIndex;
            if ( !ToPlaneChain( *o, m_plane, &m_src, &srcIndex ) )
            {
                Sys_Printf( "Fillet: fewer than three distinct points.\n" );
                return false;
            }

            const int n = (int)( m_src.size() / 2 );
            // ChooseCorners works in the object's ANCHOR indexing; srcIndex maps a
            // chain vertex back to it, so a welded duplicate cannot shift the
            // user's per-corner choice onto the wrong corner.
            std::vector<char> byAnchor;
            bool narrowed = false;
            ChooseCorners( m_object, KiwiCon_VertCount( *o ), m_closed, &byAnchor, &narrowed );
            m_chosen.assign( (size_t)n, 0 );
            int count = 0;
            for ( int i = 0; i < n; ++i )
            {
                const int a = srcIndex[(size_t)i];
                // A welded chain can be shorter than the anchor list, so the
                // fillable test is re-asked in CHAIN terms too.
                const bool fillable = m_closed ? true : ( i > 0 && i < n - 1 );
                if ( fillable && a >= 0 && a < (int)byAnchor.size() && byAnchor[(size_t)a] )
                {
                    m_chosen[(size_t)i] = 1;
                    ++count;
                }
            }
            if ( count <= 0 )
            {
                // Two different causes, and saying the wrong one sends the user
                // looking in the wrong place: `narrowed` means they named anchors
                // in Point mode and every one of those was either an END of an
                // open chain or a duplicate that ToPlaneChain welded away.
                Sys_Printf( narrowed
                    ? "Fillet: none of the selected anchors is a corner (an open "
                      "chain's two ends are not corners, and coincident points are "
                      "welded).  Select an interior anchor, or the whole chain.\n"
                    : "Fillet: nothing to round (an open chain's two ends are not "
                      "corners).\n" );
                return false;
            }
            m_narrowed = narrowed;
            m_corners  = count;

            // The reference corner: the FIRST chosen one.  The drag measures the
            // cursor's plane-space distance to it, which makes "drag away from the
            // corner" mean "bigger radius" — the same shape as Plasticity's
            // FilletCornerGizmo, which is a magnitude gizmo planted at the corner.
            for ( int i = 0; i < n; ++i )
            {
                if ( !m_chosen[(size_t)i] )
                    continue;
                m_refUV[0] = m_src[(size_t)i*2 + 0];
                m_refUV[1] = m_src[(size_t)i*2 + 1];
                KiwiCon_PlaneToWorld( m_plane, m_refUV, m_ref );
                m_haveRef = true;
                break;
            }

            m_have = true;
            LatchStart();
            Recompute();
            Sys_Printf( "Fillet: drag away from a corner to round %i corner%s, or "
                        "type a radius.  RMB/Enter commits, Esc cancels.%s\n",
                        m_corners, ( m_corners == 1 ) ? "" : "s",
                        m_narrowed ? "  (only the selected anchors)" : "" );
            return true;
        }

        void Rebase() override
        {
            // A PAUSED -> HOT edge.  The `keep` bias is not optional: the mapping is
            // a DELTA from `m_start`, so re-latching alone would collapse the radius
            // to zero the instant the user pressed LMB to resume.  Same three lines
            // as kiwi_extrude.cpp:291-299 and kiwi_offset.cpp.
            const float keep = m_radius;
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
            if ( !m_have || m_invalid || m_preview.empty() )
            {
                Sys_Printf( "Fillet: cancelled — %s.\n",
                            m_why ? m_why : "nothing to round" );
                Reset();
                g_nUpdateBits |= 1;
                return;
            }
            const kconObject_t *src = KiwiCon_At( m_object );
            if ( !src )
            {
                Sys_Printf( "Fillet: the chain went away.\n" );
                Reset();
                g_nUpdateBits |= 1;
                return;
            }

            // Everything below is unconditional, which is why the snapshot lands
            // here (kiwi_fillet.h UNDO — kiwi_trim.cpp's rule).
            kconObject_t o;
            o.type   = KCON_POLYLINE;
            o.plane  = m_plane;                       // a SEED; KiwiCon_Add refits
            o.pts    = m_preview;
            o.closed = m_closed;

            // KiwiCon_Add's only two rejections are ">KCON_MAX_POINTS" and
            // "<2 points" (kiwi_construct.cpp:706-715), and FilletChain2D has
            // already refused both, so the Add below cannot decline.  The branch
            // stays as a belt-and-braces console line rather than a silent
            // half-edit, because the RemoveAt above has already happened.
            KiwiCon_UndoPush();
            KiwiCon_RemoveAt( m_object );             // FILLET REPLACES (kiwi_fillet.h)
            const int added = KiwiCon_Add( o );
            if ( added < 0 )
                Sys_Printf( "Fillet: the store refused the rounded chain — the "
                            "original has been removed; press Ctrl+Z.\n" );
            else
            {
                char b[32];
                KiwiUnits_Format( b, sizeof( b ), m_radius );
                Sys_Printf( "Fillet: %i corner%s rounded at %s%s.\n",
                            m_stats.rounded, ( m_stats.rounded == 1 ) ? "" : "s", b,
                            m_stats.clamped ? " (some clamped by their edges)" : "" );
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
            const float *col = m_invalid ? KFIL_COL_BAD : KFIL_COL_OK;
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
            m_object    = -1;
            m_have      = false;
            m_haveStart = false;
            m_haveRef   = false;
            m_closed    = false;
            m_invalid   = false;
            m_narrowed  = false;
            m_hasNum    = false;
            m_numWorld  = 0.0f;
            m_radius    = 0.0f;
            m_start     = 0.0f;
            m_corners   = 0;
            m_why       = 0;
            m_hud[0]    = '\0';
            m_stats     = filletStats_t();
            m_src.clear();
            m_chosen.clear();
            m_preview.clear();
        }

        // The cursor's plane-space distance to the reference corner.
        bool RawScalar( float *out ) const
        {
            if ( !m_haveRef )
                return false;
            int cx = 0, cy = 0;
            ray_t ray;
            float w[3], uv[2];
            if ( !KiwiCmd_LastCursor( &cx, &cy ) )
                return false;
            if ( !Pick_RayFromImagePos( cx, cy, &ray ) )
                return false;
            if ( !KiwiCon_RayPlaneBounded( m_plane, ray, w ) )
                return false;
            KiwiCon_WorldToPlane( m_plane, w, uv );
            const float dx = uv[0] - m_refUV[0];
            const float dy = uv[1] - m_refUV[1];
            *out = sqrtf( dx*dx + dy*dy );
            return true;
        }

        void LatchStart()
        {
            float raw = 0.0f;
            if ( !RawScalar( &raw ) )
                return;
            m_start     = raw;
            m_haveStart = true;
        }

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
            if ( !KiwiCon_At( m_object ) )
            {
                Sys_Printf( "Fillet: the source went away — cancelled.\n" );
                KiwiCmd_Cancel();
                return;
            }

            if ( m_hasNum )
            {
                m_radius = m_numWorld;
            }
            else
            {
                if ( !m_haveStart )
                    LatchStart();
                float raw = 0.0f;
                if ( m_haveStart && RawScalar( &raw ) )
                {
                    m_radius = SnapScalar( raw - m_start );
                    if ( m_radius < 0.0f )
                        m_radius = 0.0f;              // a radius has no sign
                }
            }

            m_invalid = false;
            m_why     = 0;
            m_preview.clear();
            m_stats   = filletStats_t();

            if ( m_radius < KFIL_MIN_RADIUS )
            {
                m_invalid = true;
                m_why     = "radius too small";
                UpdateHud();
                return;
            }

            std::vector<float> out2;
            const char *why = 0;
            if ( !FilletChain2D( m_src, m_closed, m_chosen, m_radius,
                                 &out2, &m_stats, &why ) )
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

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_radius );
            if ( m_invalid )
                SetHud( "fillet %s  ·  REFUSED: %s", b, m_why ? m_why : "invalid" );
            else
                SetHud( "fillet %s  ·  %i of %i corner%s%s  ·  RMB/Enter: commit",
                        b, m_stats.rounded, m_corners, ( m_corners == 1 ) ? "" : "s",
                        m_stats.clamped ? "  (clamped)" : "" );
        }

        void SetHud( const char *fmt, ... )
        {
            va_list ap;
            va_start( ap, fmt );
            _vsnprintf( m_hud, sizeof( m_hud ), fmt, ap );
            va_end( ap );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        int                m_object    = -1;
        bool               m_have      = false;
        bool               m_haveStart = false;
        bool               m_haveRef   = false;
        bool               m_closed    = false;
        bool               m_invalid   = false;
        bool               m_narrowed  = false;
        bool               m_hasNum    = false;
        float              m_numWorld  = 0.0f;
        float              m_radius    = 0.0f;
        float              m_start     = 0.0f;
        int                m_corners   = 0;
        const char        *m_why       = 0;
        filletStats_t      m_stats;
        kconPlane_t        m_plane;
        float              m_refUV[2]  = { 0.0f, 0.0f };
        float              m_ref[3]    = { 0.0f, 0.0f, 0.0f };
        std::vector<float> m_src;                    // plane-space source chain
        std::vector<char>  m_chosen;                 // per chain vertex
        std::vector<float> m_preview;                // world-space result
        char               m_hud[192] = { 0 };
    };

    KiwiFilletCommand s_fillet;
}

// ─── the public surface ──────────────────────────────────────────────────────
bool KiwiFillet_CanFillet()
{
    const int idx = SelectedObject();
    if ( idx < 0 )
        return false;
    const kconObject_t *o = KiwiCon_At( idx );
    if ( !o || !Filletable( *o ) )
        return false;
    // A plane is a hard precondition (an arc off its own plane is not an arc), so
    // the palette greys the row rather than letting the command refuse on entry.
    kconPlane_t plane;
    return KiwiCon_ObjectPlane( *o, &plane );
}

void KiwiFillet_RegisterCommands()
{
    // Unbound here — the CLASSIC-profile row.  kiwi_keymap.cpp puts Fillet on the
    // bare B in the modern profile, with the SameTargetname 36121 displacement;
    // the audit is in kiwi_keymap.h.
    Radiant_RegisterCommand( "KiwiFilletCorners", 0, 0, KIWI_CMD_FILLET_CURVE );
}

KiwiEditorCommand *KiwiFillet_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_FILLET_CURVE ) ? &s_fillet : 0;
}
