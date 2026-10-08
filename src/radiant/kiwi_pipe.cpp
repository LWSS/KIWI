#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-09-28): Pipe - a regular section swept along construction objects.  See
// kiwi_pipe.h for the straight-brush / curved-patch rule.  Everything is built unlinked,
// validated, then landed in one undo record; nothing is written before the confirm.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_pipe.h"
#include "kiwi_camera.h"            // KiwiCam_RayAxis / AxisPortrayable (the radius handle)
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_extrude.h"           // KiwiExtrude_BuildPrismDef / LandDef
#include "kiwi_lines.h"
#include "kiwi_material.h"          // KiwiMtl_RealizePatch / EnsurePatchChannels
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_primitive.h"         // KiwiPrim_BuildPatchCylinder / PatchControlPoint
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points; signatures match their definitions (kiwi_primitive.cpp uses the same).
extern void         Patch_KiwiFinishNew( patchMesh_t *p );                      // pmesh.cpp:1558

namespace
{
    const float KPIPE_COL_OK[3]  = { 0.55f, 0.85f, 1.00f };   // the primitives' preview blue
    const float KPIPE_PI         = (float)KIWI_PI;

    const kiwiNumField_t KPIPE_FIELDS[2] = { { "radius", KNUM_LENGTH, false },
                                             { "sides",  KNUM_COUNT,  false } };

    // Remembered across runs of the tool this session (Plasticity keeps its dialog values).
    float s_lastRadius = 0.0f;              // 0 = seed from the grid
    int   s_lastSides  = 0;                 // 0 = round
    int   s_lastAngle  = 0;

    // One spine: world points and whether the pipe along it is straight-sided.
    struct pipeSpine_t
    {
        std::vector<float> pts;             // 3 floats per point
        bool               closed = false;
        bool               curved = false;  // spline spans, circle or arc
        bool               haveHint = false;
        float              hint[3] = { 0.0f, 0.0f, 1.0f };   // the object's plane normal
    };

    // One section frame along the spine.
    struct pipeFrame_t
    {
        float p[3], t[3], u[3], v[3];
    };

    int PointCount( const pipeSpine_t &s ) { return (int)( s.pts.size() / 3 ); }
    const float *Pt( const pipeSpine_t &s, int i ) { return &s.pts[(size_t)i * 3]; }

    bool Pipeable( const kconObject_t *o )
    {
        return o && !o->hidden && KiwiCon_VertCount( *o ) >= 2;
    }

    bool SpineFor( const kconObject_t &o, pipeSpine_t *out )
    {
        out->pts.clear();
        out->curved = KiwiCon_IsParametric( o ) || KiwiCon_HasSmooth( o );
        out->closed = o.closed || o.type == KCON_RECT || o.type == KCON_CIRCLE;
        // Straight chains keep their control points; everything else its tessellation.
        const bool rawPts = !out->curved && ( o.type == KCON_LINE || o.type == KCON_POLYLINE );
        if ( rawPts )
            out->pts = o.pts;
        else
        {
            const int n = KiwiCon_VertCount( o );
            for ( int i = 0; i < n; ++i )
            {
                float w[3];
                if ( KiwiCon_VertWorld( o, i, w ) )
                    out->pts.insert( out->pts.end(), w, w + 3 );
            }
        }
        // Drop consecutive duplicates and a repeated closing seam.
        std::vector<float> clean;
        for ( int i = 0; i < PointCount( *out ); ++i )
        {
            const float *p = Pt( *out, i );
            if ( !clean.empty() )
            {
                float d[3];
                Sub3( p, &clean[clean.size() - 3], d );
                if ( Len3( d ) < 1.0e-3f )
                    continue;
            }
            clean.insert( clean.end(), p, p + 3 );
        }
        if ( out->closed && clean.size() >= 6 )
        {
            float d[3];
            Sub3( &clean[0], &clean[clean.size() - 3], d );
            if ( Len3( d ) < 1.0e-3f )
                clean.resize( clean.size() - 3 );
        }
        out->pts.swap( clean );
        if ( PointCount( *out ) < ( out->closed ? 3 : 2 ) )
            return false;
        if ( KiwiCon_IsParametric( o ) || o.planeValid )
        {
            Copy3( o.plane.normal, out->hint );
            out->haveHint = Norm3( out->hint );
        }
        return true;
    }

    // A unit vector perpendicular to `t`, preferring `hint`, then world Z, then world X.
    void Perpendicular( const float t[3], const float *hint, float out[3] )
    {
        static const float Z[3] = { 0.0f, 0.0f, 1.0f }, X[3] = { 1.0f, 0.0f, 0.0f };
        const float *cands[3] = { hint, Z, X };
        for ( int c = 0; c < 3; ++c )
        {
            if ( !cands[c] )
                continue;
            const float d = Dot3( cands[c], t );
            for ( int k = 0; k < 3; ++k )
                out[k] = cands[c][k] - d * t[k];
            if ( Len3( out ) > 0.1f && Norm3( out ) )
                return;
        }
        out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f;
    }

    // Carry u from the previous frame onto the new tangent (projection transport), then v =
    // t x u so the section runs counter-clockwise about the tangent.  The frame stays
    // unturned (transport must not accumulate the angle); SectionPoint applies the angle.
    void FinishFrame( pipeFrame_t *f, const float *prevU, const float *hint )
    {
        float u[3];
        if ( prevU )
        {
            const float d = Dot3( prevU, f->t );
            for ( int k = 0; k < 3; ++k )
                u[k] = prevU[k] - d * f->t[k];
            if ( !( Len3( u ) > 1.0e-3f ) || !Norm3( u ) )
                Perpendicular( f->t, hint, u );
        }
        else
            Perpendicular( f->t, hint, u );
        float v[3];
        Cross3( f->t, u, v );
        Copy3( u, f->u );
        Copy3( v, f->v );
    }

    // World position of a section point (c, s in world units along the turned u / v).
    void SectionPoint( const pipeFrame_t &f, float angleDeg, float c, float s, float out[3] )
    {
        const float a = angleDeg * KPIPE_PI / 180.0f;
        const float ca = cosf( a ), sa = sinf( a );
        for ( int k = 0; k < 3; ++k )
        {
            const float tu = ca * f.u[k] + sa * f.v[k];
            const float tv = -sa * f.u[k] + ca * f.v[k];
            out[k] = f.p[k] + c * tu + s * tv;
        }
    }

    // Arc-length resample of a polyline into `count` points (closed wraps; the last equals
    // the first on a closed spine).
    void Resample( const pipeSpine_t &s, int count, std::vector<float> *out )
    {
        const int n = PointCount( s );
        const int segs = s.closed ? n : n - 1;
        std::vector<float> cum( (size_t)segs + 1, 0.0f );
        for ( int i = 0; i < segs; ++i )
        {
            float d[3];
            Sub3( Pt( s, ( i + 1 ) % n ), Pt( s, i ), d );
            cum[(size_t)i + 1] = cum[(size_t)i] + Len3( d );
        }
        const float total = cum[(size_t)segs];
        out->clear();
        int seg = 0;
        for ( int j = 0; j < count; ++j )
        {
            const float target = total * (float)j / (float)( count - 1 );
            while ( seg < segs - 1 && cum[(size_t)seg + 1] < target )
                ++seg;
            const float len = cum[(size_t)seg + 1] - cum[(size_t)seg];
            const float f = len > 1.0e-6f ? ( target - cum[(size_t)seg] ) / len : 0.0f;
            const float *a = Pt( s, seg ), *b = Pt( s, ( seg + 1 ) % n );
            for ( int k = 0; k < 3; ++k )
                out->push_back( a[k] + ( b[k] - a[k] ) * f );
        }
    }

    float SpineLength( const pipeSpine_t &s )
    {
        const int n = PointCount( s );
        const int segs = s.closed ? n : n - 1;
        float total = 0.0f;
        for ( int i = 0; i < segs; ++i )
        {
            float d[3];
            Sub3( Pt( s, ( i + 1 ) % n ), Pt( s, i ), d );
            total += Len3( d );
        }
        return total;
    }

    // Frames at resampled points: central-difference tangents (wrapping when closed).
    void CurvedFrames( const std::vector<float> &pts, bool closed, const float *hint,
                       std::vector<pipeFrame_t> *out )
    {
        const int n = (int)( pts.size() / 3 );
        out->assign( (size_t)n, pipeFrame_t() );
        for ( int i = 0; i < n; ++i )
        {
            pipeFrame_t &f = ( *out )[(size_t)i];
            Copy3( &pts[(size_t)i * 3], f.p );
            int a = i - 1, b = i + 1;
            if ( closed )
            {
                // The last point repeats the first: neighbours skip the duplicate.
                if ( a < 0 )      a = n - 2;
                if ( b > n - 1 )  b = 1;
                if ( i == n - 1 ) b = 1;
            }
            else
            {
                if ( a < 0 )     a = 0;
                if ( b > n - 1 ) b = n - 1;
            }
            Sub3( &pts[(size_t)b * 3], &pts[(size_t)a * 3], f.t );
            if ( !Norm3( f.t ) )
                f.t[0] = 1.0f, f.t[1] = 0.0f, f.t[2] = 0.0f;
            FinishFrame( &f, i ? ( *out )[(size_t)i - 1].u : nullptr, hint );
        }
        if ( closed && n > 1 )
            ( *out )[(size_t)n - 1] = ( *out )[0];   // bit-identical seam
    }

    // Material for new patches, as KiwiPrim_BuildPatchCylinder takes it (an unlinked probe box).
    bool ProbeMaterial( const float mins[3], const float maxs[3],
                        patchMesh_material *tex, patchMesh_material *lm )
    {
        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *probe = Brush_Alloc( g_qeglobals.random_texture_stuff, 0 );
        if ( !probe )
            return false;
        float lo[3], hi[3];
        Copy3( mins, lo );
        Copy3( maxs, hi );
        Brush_Create( lo, hi, probe, 0 );
        bool ok = false;
        if ( probe->faces && probe->faceCount > 0 )
        {
            *tex = *(patchMesh_material *)&probe->faces[0].mtldef[0].lyrMtl;
            *lm  = *(patchMesh_material *)&probe->faces[0].mtldef[1].lyrMtl;
            ok = true;
        }
        Brush_Free_R( probe );                  // never linked: refCount 0
        return ok;
    }

    class KiwiPipeCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Pipe"; }
        bool CanExecute() override        { return KiwiPipe_CanExecute(); }
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_spines.empty(); }
        kiwiSnapCtx_t SnapContext() const override { return KSNAPCTX_TRANSFORM; }

        int NumericFields( const kiwiNumField_t **out ) const override
        {
            *out = KPIPE_FIELDS;
            return 2;
        }
        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 0 )      *out = m_radius;
            else if ( field == 1 ) *out = (float)m_sides;
            else                   return false;
            return true;
        }
        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( !has )
                return;
            if ( field == 0 )
                SetRadius( world );
            else if ( field == 1 )
                SetSides( (int)floorf( Units_ToDisplay( world ) + 0.5f ), 0 );
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        // Plasticity's PipeDialog: vertex count, section size, angle (+ KIWI's patch density).
        int CommandOptions( const kiwiOption_t **out ) const override
        {
            static const kiwiOption_t rows[] = {
                { "Vertex count (0 = round)", KOPT_INT,      nullptr, 0, 0, 0, KPIPE_MAX_SIDES, -1 },
                { "Section size (radius)",    KOPT_NUMFIELD, nullptr, 0, 0, KPIPE_MIN_RADIUS, 4096, -1 },
                { "Angle (degrees)",          KOPT_INT,      nullptr, 0, 0, 0, 360, -1 },
                { "Curve density",            KOPT_INT,      nullptr, 0, 0, KPRIM_PATCH_DENSITY_MIN,
                                                                           KPRIM_PATCH_DENSITY_MAX, -1 },
            };
            *out = rows;
            return 4;
        }
        int OptionValue( int opt ) const override
        {
            return opt == 0 ? m_sides : opt == 2 ? m_angle : opt == 3 ? m_density : 0;
        }
        void OptionChanged( int opt, int value ) override
        {
            if ( opt == 0 )
            {
                KiwiNum_ClearField( 1 );
                SetSides( value, value - m_sides );
            }
            else if ( opt == 2 )
                m_angle = ( value % 360 + 360 ) % 360;
            else if ( opt == 3 )
            {
                m_density = value < KPRIM_PATCH_DENSITY_MIN ? KPRIM_PATCH_DENSITY_MIN
                          : value > KPRIM_PATCH_DENSITY_MAX ? KPRIM_PATCH_DENSITY_MAX : value;
                KiwiPrim_SetPatchDensity( m_density );
            }
            s_lastAngle = m_angle;
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t p[] = { { "Drag", "Section size" }, { "[ ]", "Vertex count" } };
            *out = p;
            return 2;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( mods || ( vk != 0xDB && vk != 0xDD ) )      // [ / ]
                return false;
            KiwiNum_ClearField( 1 );
            SetSides( m_sides + ( vk == 0xDD ? 1 : -1 ), vk == 0xDD ? 1 : -1 );
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;
        }

        // The radius handle sits on the first section, along its turned u axis.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_spines.empty() || !outAnchor || !outDir )
                return false;
            HandleAxis( outDir );
            Mad3( m_handleBase, outDir, m_radius, outAnchor );
            return true;
        }
        void HandleGrab( bool held ) override { m_grab = held; }
        void Rebase() override               { LatchStart(); }
        // A stray click never resumes a mouse-follow: only the handle drives the radius.
        bool PressIntercept( int imgX, int imgY ) override
        {
            (void)imgX;  (void)imgY;
            return true;
        }

        bool Begin() override
        {
            m_spines.clear();
            m_grab = false;
            m_hud[0] = '\0';
            std::vector<int> seen;
            const int n = KiwiConSel_Count();
            for ( int i = 0; i < n; ++i )
            {
                const kconSelItem_t *it = KiwiConSel_At( i );
                if ( !it )
                    continue;
                bool dup = false;
                for ( size_t k = 0; k < seen.size() && !dup; ++k )
                    dup = seen[k] == it->object;
                if ( dup )
                    continue;
                seen.push_back( it->object );
                const kconObject_t *o = KiwiCon_At( it->object );
                pipeSpine_t s;
                if ( Pipeable( o ) && SpineFor( *o, &s ) )
                    m_spines.push_back( s );
            }
            if ( m_spines.empty() )
            {
                Sys_Printf( "Pipe: select a construction line, polyline, spline, circle or arc first.\n" );
                return false;
            }
            if ( !( s_lastRadius > 0.0f ) )
            {
                const float g = KiwiUnits_GridSpacingWorld();
                s_lastRadius = g > KPIPE_MIN_RADIUS ? g * 2.0f : 8.0f;
            }
            m_radius  = s_lastRadius;
            m_sides   = s_lastSides;
            m_angle   = s_lastAngle;
            m_density = KiwiPrim_PatchDensity();
            FirstFrame( &m_first );
            Copy3( m_first.p, m_handleBase );
            UpdateHud();
            KiwiCmd_Pause();                      // nothing follows the mouse until a grab
            Sys_Printf( "Pipe: drag the handle for the section size, or type it; [ ] vertex "
                        "count (0 = round).  Enter / RMB / Confirm builds it.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;  (void)snap;
            if ( !m_grab )
                return;
            float r = 0.0f;
            if ( !CursorRadius( &r ) )
                return;
            r += m_start;
            if ( KiwiCmd_SnapEngaged() )
            {
                const float g = KiwiUnits_GridSpacingWorld();
                if ( g > 0.0f )
                    r = floorf( r / g + 0.5f ) * g;
            }
            KiwiNum_ClearField( 0 );
            SetRadius( r );
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        void Commit() override
        {
            if ( m_spines.empty() )
                return;
            std::vector<brush_t *> defs;
            int brushes = 0, patches = 0, rejected = 0;
            for ( size_t i = 0; i < m_spines.size(); ++i )
            {
                const pipeSpine_t &s = m_spines[i];
                if ( s.curved )
                    BuildCurved( s, &defs, &patches, &rejected );
                else
                    BuildStraight( s, &defs, &brushes, &patches, &rejected );
            }
            if ( defs.empty() )
            {
                Sys_Printf( "Pipe: nothing built%s.\n", rejected ? " - every piece was rejected" : "" );
                g_nUpdateBits |= 1;
                return;
            }
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "pipe" );
            for ( size_t i = 0; i < defs.size(); ++i )
                KiwiExtrude_LandDef( defs[i] );
            char r[32];
            KiwiUnits_Format( r, sizeof( r ), m_radius );
            Sys_Printf( "Pipe: radius %s - %i brush%s, %i curve patch%s%s.\n", r,
                        brushes, brushes == 1 ? "" : "es", patches, patches == 1 ? "" : "es",
                        rejected ? " (some segments were too short or degenerate)" : "" );
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            m_spines.clear();
            g_nUpdateBits |= 1;
        }

        int LineBudget() const override { return KCMD_LINE_BUDGET_MAX; }

        void DrawWorld() override
        {
            if ( m_spines.empty() )
                return;
            KiwiLines_Color( KPIPE_COL_OK[0], KPIPE_COL_OK[1], KPIPE_COL_OK[2] );
            const int ringSegs = m_sides >= 3 ? m_sides : 16;
            for ( size_t i = 0; i < m_spines.size(); ++i )
            {
                std::vector<pipeFrame_t> frames;
                PreviewFrames( m_spines[i], &frames );
                // Straight spines come as (start, end) pairs per segment; curved as one run.
                const bool pairs = !m_spines[i].curved;
                for ( size_t f = 0; f < frames.size(); ++f )
                {
                    float prev[3];
                    for ( int k = 0; k <= ringSegs; ++k )
                    {
                        const float a = 2.0f * KPIPE_PI * (float)( k % ringSegs ) / (float)ringSegs;
                        float p[3];
                        SectionPoint( frames[f], (float)m_angle, cosf( a ) * m_radius,
                                      sinf( a ) * m_radius, p );
                        if ( k && !KiwiLines_Add( prev, p ) )
                            return;
                        Copy3( p, prev );
                    }
                    // Four longitudinal rails to the next ring.
                    if ( f + 1 < frames.size() && ( !pairs || ( f & 1 ) == 0 ) )
                        for ( int k = 0; k < 4; ++k )
                        {
                            const int idx = ( k * ringSegs ) / 4;
                            const float a = 2.0f * KPIPE_PI * (float)idx / (float)ringSegs;
                            float p0[3], p1[3];
                            SectionPoint( frames[f],     (float)m_angle, cosf( a ) * m_radius, sinf( a ) * m_radius, p0 );
                            SectionPoint( frames[f + 1], (float)m_angle, cosf( a ) * m_radius, sinf( a ) * m_radius, p1 );
                            if ( !KiwiLines_Add( p0, p1 ) )
                                return;
                        }
                }
            }
        }

    private:
        // ── section / frame helpers ─────────────────────────────────────────────
        void SetRadius( float r )
        {
            m_radius = r < KPIPE_MIN_RADIUS ? KPIPE_MIN_RADIUS : r;
            s_lastRadius = m_radius;
        }

        // 1 and 2 are not sections: stepping jumps them (0 <-> 3), a typed 1 / 2 becomes 3.
        void SetSides( int n, int dir )
        {
            if ( n == 1 || n == 2 )
                n = ( dir < 0 ) ? 0 : 3;
            if ( n < 0 ) n = 0;
            if ( n > KPIPE_MAX_SIDES ) n = KPIPE_MAX_SIDES;
            m_sides = n;
            s_lastSides = n;
        }

        void FirstFrame( pipeFrame_t *f ) const
        {
            const pipeSpine_t &s = m_spines[0];
            Copy3( Pt( s, 0 ), f->p );
            Sub3( Pt( s, 1 ), Pt( s, 0 ), f->t );
            if ( !Norm3( f->t ) )
                f->t[0] = 1.0f, f->t[1] = 0.0f, f->t[2] = 0.0f;
            FinishFrame( f, nullptr, s.haveHint ? s.hint : nullptr );
        }

        void HandleAxis( float dir[3] ) const
        {
            const float a = (float)m_angle * KPIPE_PI / 180.0f;
            for ( int k = 0; k < 3; ++k )
                dir[k] = cosf( a ) * m_first.u[k] + sinf( a ) * m_first.v[k];
        }

        bool CursorRadius( float *out ) const
        {
            int x = 0, y = 0;
            ray_t ray;
            float dir[3], q[3], rel[3];
            HandleAxis( dir );
            if ( !KiwiCmd_LastCursor( &x, &y ) || !Pick_RayFromImagePos( x, y, &ray )
              || !KiwiCam_AxisPortrayable( dir ) || !KiwiCam_RayAxis( ray, m_handleBase, dir, q ) )
                return false;
            Sub3( q, m_handleBase, rel );
            *out = Dot3( rel, dir );
            return true;
        }

        // The grab keeps the handle where it shows: travel is measured from here.
        void LatchStart()
        {
            m_start = 0.0f;
            float r = 0.0f;
            if ( CursorRadius( &r ) )
                m_start = m_radius - r;
        }

        // Straight spines ring at every vertex; curved ones at their patch stations.
        void PreviewFrames( const pipeSpine_t &s, std::vector<pipeFrame_t> *out ) const
        {
            out->clear();
            const float *hint = s.haveHint ? s.hint : nullptr;
            if ( s.curved )
            {
                std::vector<float> pts;
                Resample( s, CurvedSpans( s ) + 1, &pts );
                CurvedFrames( pts, s.closed, hint, out );
                return;
            }
            const int n = PointCount( s );
            const int segs = s.closed ? n : n - 1;
            float prevU[3];
            bool  havePrev = false;          // a copy: push_back may move the vector
            pipeFrame_t f;
            for ( int i = 0; i < segs; ++i )
            {
                Sub3( Pt( s, ( i + 1 ) % n ), Pt( s, i ), f.t );
                if ( !Norm3( f.t ) )
                    continue;
                Copy3( Pt( s, i ), f.p );
                FinishFrame( &f, havePrev ? prevU : nullptr, hint );
                out->push_back( f );
                pipeFrame_t e = f;
                Copy3( Pt( s, ( i + 1 ) % n ), e.p );
                out->push_back( e );
                Copy3( f.u, prevU );
                havePrev = true;
            }
        }

        // Quadratic spans along a curved spine: about one per two tessellation segments.
        int CurvedSpans( const pipeSpine_t &s ) const
        {
            const int segs = s.closed ? PointCount( s ) : PointCount( s ) - 1;
            int spans = ( segs + 1 ) / 2;
            if ( spans < 2 ) spans = 2;
            if ( spans > KPIPE_SPANS_PER_PATCH * KPIPE_MAX_PATCHES )
                spans = KPIPE_SPANS_PER_PATCH * KPIPE_MAX_PATCHES;
            return spans;
        }

        // ── straight: brush prisms, or patch cylinders for a round section ─────
        void BuildStraight( const pipeSpine_t &s, std::vector<brush_t *> *defs,
                            int *brushes, int *patches, int *rejected )
        {
            const float *hint = s.haveHint ? s.hint : nullptr;
            const int n = PointCount( s );
            const int segs = s.closed ? n : n - 1;
            float prevU[3];
            bool  havePrev = false;
            for ( int i = 0; i < segs; ++i )
            {
                pipeFrame_t f;
                Copy3( Pt( s, i ), f.p );
                Sub3( Pt( s, ( i + 1 ) % n ), Pt( s, i ), f.t );
                const float len = Len3( f.t );
                if ( len < 1.0f || !Norm3( f.t ) )
                {
                    ++*rejected;
                    continue;
                }
                FinishFrame( &f, havePrev ? prevU : nullptr, hint );
                Copy3( f.u, prevU );
                havePrev = true;

                // The section plane: normal along the segment, u = the turned section axis.
                float turnedU[3];
                const float a = (float)m_angle * KPIPE_PI / 180.0f;
                for ( int k = 0; k < 3; ++k )
                    turnedU[k] = cosf( a ) * f.u[k] + sinf( a ) * f.v[k];
                kconPlane_t pl;
                if ( !KiwiCon_MakePlane( f.p, f.t, turnedU, &pl ) )
                {
                    ++*rejected;
                    continue;
                }
                float centre[2];
                KiwiCon_WorldToPlane( pl, f.p, centre );

                if ( m_sides < 3 )
                {
                    brush_t *def = KiwiPrim_BuildPatchCylinder( pl, centre, m_radius, 0.0f, len, m_density );
                    if ( def ) { defs->push_back( def ); ++*patches; }
                    else         ++*rejected;
                    continue;
                }

                // CCW about the plane normal: the section's own angle order.
                std::vector<float> loop;
                for ( int k = 0; k < m_sides; ++k )
                {
                    const float ang = 2.0f * KPIPE_PI * (float)k / (float)m_sides;
                    float w[3], uv[2];
                    SectionPoint( f, (float)m_angle, cosf( ang ) * m_radius, sinf( ang ) * m_radius, w );
                    KiwiCon_WorldToPlane( pl, w, uv );
                    loop.push_back( uv[0] );
                    loop.push_back( uv[1] );
                }
                const char *why = nullptr;
                brush_t *def = KiwiExtrude_BuildPrismDef( pl, &loop[0], m_sides, 0.0f, len, &why );
                if ( !def )
                {
                    ++*rejected;
                    continue;
                }
                KiwiValid_Rebuild( def );
                if ( !KiwiValid_CheckBrush( def, &why ) )
                {
                    Brush_Free_R( def );               // still unlinked
                    ++*rejected;
                    continue;
                }
                defs->push_back( def );
                ++*brushes;
            }
        }

        // ── curved: patches only ────────────────────────────────────────────────
        // Columns run round the section (a quadratic circle, or a polygon whose odd columns
        // sit mid-edge so its sides stay flat); rows run along the spine, even rows on the
        // curve, odd rows placed so the quadratic passes through the resampled midpoint.
        void SectionColumn( int col, int sides, float *c, float *s ) const
        {
            if ( sides < 3 )
            {
                float uv[2];
                KiwiPrim_PatchControlPoint( m_radius, m_density, col, uv );
                *c = uv[0];
                *s = uv[1];
                return;
            }
            const int k0 = ( col / 2 ) % sides;
            const float a0 = 2.0f * KPIPE_PI * (float)k0 / (float)sides;
            if ( !( col & 1 ) )
            {
                *c = cosf( a0 ) * m_radius;
                *s = sinf( a0 ) * m_radius;
                return;
            }
            const float a1 = 2.0f * KPIPE_PI * (float)( ( k0 + 1 ) % sides ) / (float)sides;
            *c = ( cosf( a0 ) + cosf( a1 ) ) * 0.5f * m_radius;
            *s = ( sinf( a0 ) + sinf( a1 ) ) * 0.5f * m_radius;
        }

        void BuildCurved( const pipeSpine_t &s, std::vector<brush_t *> *defs,
                          int *patches, int *rejected )
        {
            int sides = m_sides;
            if ( sides > KPIPE_MAX_PATCH_SIDES )
            {
                Sys_Printf( "Pipe: a curved pipe is made of curve patches, which hold at most %i "
                            "sides - using %i.\n", KPIPE_MAX_PATCH_SIDES, KPIPE_MAX_PATCH_SIDES );
                sides = KPIPE_MAX_PATCH_SIDES;
            }
            const int cols = ( sides < 3 ) ? ( m_density + 3 ) * 2 + 1 : sides * 2 + 1;
            if ( SpineLength( s ) < 1.0f )
            {
                ++*rejected;
                return;
            }

            const int spans = CurvedSpans( s );
            std::vector<float> pts;
            Resample( s, spans * 2 + 1, &pts );      // even = on the curve, odd = midpoints
            std::vector<pipeFrame_t> frames;
            CurvedFrames( pts, s.closed, s.haveHint ? s.hint : nullptr, &frames );

            // Section control points per resampled station.
            auto station = [&]( int row, int col, float out[3] )
            {
                float c, sn;
                SectionColumn( col, sides, &c, &sn );
                SectionPoint( frames[(size_t)row], (float)m_angle, c, sn, out );
            };

            float mins[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
            float maxs[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
            for ( size_t i = 0; i + 2 < pts.size(); i += 3 )
                for ( int k = 0; k < 3; ++k )
                {
                    if ( pts[i + k] - m_radius < mins[k] ) mins[k] = pts[i + k] - m_radius;
                    if ( pts[i + k] + m_radius > maxs[k] ) maxs[k] = pts[i + k] + m_radius;
                }
            patchMesh_material tex = {}, lm = {};
            const bool haveMtl = ProbeMaterial( mins, maxs, &tex, &lm );

            for ( int span0 = 0; span0 < spans; span0 += KPIPE_SPANS_PER_PATCH )
            {
                const int chunk = ( spans - span0 < KPIPE_SPANS_PER_PATCH ) ? spans - span0
                                                                           : KPIPE_SPANS_PER_PATCH;
                patchMesh_t *p = MakeNewPatch();
                if ( !p )
                {
                    ++*rejected;
                    return;
                }
                p->width      = cols;
                p->height     = chunk * 2 + 1;
                p->type       = PATCH_BEVEL;           // a swept quadratic, as Loft's strips
                p->contents   = 0;
                p->flags      = 0;                     // MakeNewPatch does NOT seed this
                p->subDivType = 1 << ( KPRIM_PATCH_DENSITY_MAX - m_density );
                for ( int col = 0; col < cols; ++col )
                    for ( int r = 0; r <= chunk * 2; ++r )
                    {
                        const int row = span0 * 2 + r;
                        float out[3];
                        if ( !( r & 1 ) )
                            station( row, col, out );
                        else
                        {
                            float a[3], m[3], b[3];
                            station( row - 1, col, a );
                            station( row,     col, m );
                            station( row + 1, col, b );
                            for ( int k = 0; k < 3; ++k )
                                out[k] = 2.0f * m[k] - 0.5f * ( a[k] + b[k] );
                        }
                        Copy3( out, p->ctrl[col][r].xyz );
                    }
                if ( haveMtl )
                {
                    p->texture  = tex;
                    p->lightmap = lm;
                }
                KiwiMtl_RealizePatch( p );             // unrealized materials are invisible
                KiwiMtl_EnsurePatchChannels( p );
                Patch_KiwiFinishNew( p );
                brush_t *def = AddBrushForPatch( p, nullptr );
                KiwiValid_Rebuild( def );
                defs->push_back( def );
                ++*patches;
            }
        }

        void UpdateHud()
        {
            char r[32];
            KiwiUnits_Format( r, sizeof( r ), m_radius );
            bool anyCurved = false, anyStraight = false;
            for ( size_t i = 0; i < m_spines.size(); ++i )
                ( m_spines[i].curved ? anyCurved : anyStraight ) = true;
            const char *what = anyCurved && anyStraight ? "brushes + curves"
                             : anyCurved                ? "curve patches"
                             : m_sides < 3              ? "patch cylinders"
                                                        : "brushes";
            if ( m_sides < 3 )
                _snprintf( m_hud, sizeof( m_hud ), "pipe  radius %s  round  angle %i  ->  %s", r, m_angle, what );
            else
                _snprintf( m_hud, sizeof( m_hud ), "pipe  radius %s  %i sides  angle %i  ->  %s",
                           r, m_sides, m_angle, what );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        std::vector<pipeSpine_t> m_spines;
        pipeFrame_t m_first;
        float       m_handleBase[3] = { 0.0f, 0.0f, 0.0f };
        float       m_radius  = 8.0f;
        int         m_sides   = 0;
        int         m_angle   = 0;
        int         m_density = KPRIM_PATCH_DENSITY_DEF;
        bool        m_grab    = false;
        float       m_start   = 0.0f;
        char        m_hud[192] = { 0 };
    };

    KiwiPipeCommand s_pipe;
}

bool KiwiPipe_CanExecute()
{
    const int n = KiwiConSel_Count();
    for ( int i = 0; i < n; ++i )
    {
        const kconSelItem_t *it = KiwiConSel_At( i );
        if ( it && Pipeable( KiwiCon_At( it->object ) ) )
            return true;
    }
    return false;
}

void KiwiPipe_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiPipe", 0, 0, KIWI_CMD_CONSTRUCT_PIPE );
}

KiwiEditorCommand *KiwiPipe_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_CONSTRUCT_PIPE ) ? &s_pipe : 0;
}
