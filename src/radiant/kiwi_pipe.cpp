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
#include "kiwi_region.h"            // Sweep: the construction face
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <utility>                  // std::swap
#include <vector>

// Ported entry points; signatures match their definitions (kiwi_primitive.cpp uses the same).
extern void         Patch_KiwiFinishNew( patchMesh_t *p );                      // pmesh.cpp:1558

namespace
{
    const float KPIPE_COL_OK[3]  = { 0.55f, 0.85f, 1.00f };   // the primitives' preview blue
    const float KPIPE_PI         = (float)KIWI_PI;

    const kiwiNumField_t KPIPE_FIELDS[3] = { { "radius", KNUM_LENGTH, false },
                                             { "sides",  KNUM_COUNT,  false },
                                             { "bend",   KNUM_LENGTH, false } };

    // KIWI (2026-10-28, user: "the corners are blank and need to have bent pipe. I want you
    // to fix this tool so it will create nice corners in cases like this. Add more options to
    // the imgui dialogue"): how a straight spine's corners are joined.
    //   Butt  - every segment ends square at the vertex (the old behaviour: wedge gaps);
    //   Miter - segment ends are cut on the corner's bisector plane, so they meet exactly;
    //   Bend  - segments are pulled back and an elbow (curve patch) is swept round an arc of
    //           the bend radius, measured to the pipe's centre line.
    enum { KPIPE_CORNER_BUTT = 0, KPIPE_CORNER_MITER, KPIPE_CORNER_BEND, KPIPE_CORNER_COUNT };
    const char *const KPIPE_CORNER_NAMES[KPIPE_CORNER_COUNT] = { "Butt (open)", "Miter", "Bend" };
    const float KPIPE_MIN_TURN_DEG   = 0.5f;    // straighter than this is no corner
    const int   KPIPE_BEND_SEGS_MAX  = 8;       // curve spans per 90 degrees of bend

    // Remembered across runs of the tool this session (Plasticity keeps its dialog values).
    float s_lastRadius = 0.0f;              // 0 = seed from the grid
    int   s_lastSides  = 0;                 // 0 = round
    int   s_lastAngle  = 0;
    int   s_lastCorner = KPIPE_CORNER_BEND;
    float s_lastBend   = 0.0f;              // bend radius; 0 = follow the section size
    int   s_lastBendSegs = 2;
    int   s_lastMiterMax = 135;             // sharper corners bend instead of spiking
    // KIWI (2026-10-31, user: "add an option for the pipetool to create double sided faces (for
    // tunnels you can walk on)"): every curve patch also gets its inward-facing twin.
    bool  s_lastDoubleSided = false;

    // One spine: world points and whether the pipe along it is straight-sided.
    struct pipeSpine_t
    {
        std::vector<float> pts;             // 3 floats per point
        bool               closed = false;
        bool               curved = false;  // spline spans, circle or arc
        bool               haveHint = false;
        float              hint[3] = { 0.0f, 0.0f, 1.0f };   // the object's plane normal
        // Sweep only: the face's section at this spine's start in its first frame's (u, v) -
        // the outer loop CCW about the tangent, holes the other way - and the convex pieces of
        // the whole face (CCW) the brush prisms take.
        std::vector< std::vector<float> > loops, pieces;
    };

    // One section frame along the spine.  `cut` is the normal of the plane the section lies
    // in: the tangent for a square end, the corner's bisector for a mitered one (section
    // points are slid along t onto it).
    struct pipeFrame_t
    {
        float p[3], t[3], u[3], v[3], cut[3];
    };

    // One piece of a straight spine: a segment (prism / patch cylinder from f.p along f.t for
    // len, each end square or mitered onto the plane through its corner), or an elbow
    // (stations on an exact arc, as a curved spine's resampled points).
    struct pipePiece_t
    {
        bool  elbow = false;
        bool  valid = true;
        pipeFrame_t f;
        float len = 0.0f;
        bool  miter[2] = { false, false };
        float cut[2][3];
        float corner[2][3];
        std::vector<float>       pts;       // elbow: 2 * spans + 1 stations
        std::vector<pipeFrame_t> frames;
        int   spans = 0;
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
        Copy3( f->t, f->cut );              // square end unless a miter sets it
    }

    // Slide `pt` along `t` onto the plane through `origin` with normal `cut`.
    void OntoCut( float pt[3], const float t[3], const float cut[3], const float origin[3] )
    {
        const float tc = Dot3( t, cut );
        if ( fabsf( tc ) < 1.0e-3f )
            return;
        float d[3];
        Sub3( origin, pt, d );
        Mad3( pt, t, Dot3( d, cut ) / tc, pt );
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
        OntoCut( out, f.t, f.cut, f.p );    // no-op for a square end
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
            return 3;
        }
        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 0 )      *out = m_radius;
            else if ( field == 1 ) *out = (float)m_sides;
            else if ( field == 2 ) *out = BendRadius();
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
            else if ( field == 2 )
            {
                m_bend = world < KPIPE_MIN_RADIUS ? KPIPE_MIN_RADIUS : world;
                s_lastBend = m_bend;
            }
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        // Plasticity's PipeDialog: vertex count, section size, angle (+ KIWI's patch density
        // and corner joints).  The corner rows depend on the corner style (enabledBy can only
        // gate on non-zero, so the table is rebuilt, as Loft does).
        int CommandOptions( const kiwiOption_t **out ) const override
        {
            static const kiwiOption_t base[6] = {
                { "Vertex count (0 = round)", KOPT_INT,      nullptr, 0, 0, 0, KPIPE_MAX_SIDES, -1 },
                { "Section size (radius)",    KOPT_NUMFIELD, nullptr, 0, 0, KPIPE_MIN_RADIUS, 4096, -1 },
                { "Angle (degrees)",          KOPT_INT,      nullptr, 0, 0, 0, 360, -1 },
                { "Curve density",            KOPT_INT,      nullptr, 0, 0, KPRIM_PATCH_DENSITY_MIN,
                                                                           KPRIM_PATCH_DENSITY_MAX, -1 },
                { "Corners",                  KOPT_ENUM,     KPIPE_CORNER_NAMES, KPIPE_CORNER_COUNT, 0,
                                                                           0, 0, -1 },
                { "Double-sided (walk inside)", KOPT_TOGGLE, nullptr, 0, 0, 0, 1, -1 },
            };
            static const kiwiOption_t bend[2] = {
                { "Bend radius (to centre line)", KOPT_NUMFIELD, nullptr, 0, 2, KPIPE_MIN_RADIUS, 4096, -1 },
                { "Bend smoothness (per 90 deg)", KOPT_INT,      nullptr, 0, 0, 1, KPIPE_BEND_SEGS_MAX, -1 },
            };
            static const kiwiOption_t miter[1] = {
                { "Miter up to (degrees)",        KOPT_INT,      nullptr, 0, 0, 15, 170, -1 },
            };
            static kiwiOption_t rows[8];
            for ( int i = 0; i < 6; ++i )
                rows[i] = base[i];
            int n = 6;
            if ( m_corner == KPIPE_CORNER_BEND )
            {
                rows[n++] = bend[0];
                rows[n++] = bend[1];
            }
            else if ( m_corner == KPIPE_CORNER_MITER )
                rows[n++] = miter[0];
            *out = rows;
            return n;
        }
        int OptionValue( int opt ) const override
        {
            if ( opt == 0 ) return m_sides;
            if ( opt == 2 ) return m_angle;
            if ( opt == 3 ) return m_density;
            if ( opt == 4 ) return m_corner;
            if ( opt == 5 ) return m_doubleSided ? 1 : 0;
            if ( opt == 6 ) return m_corner == KPIPE_CORNER_MITER ? m_miterMax : 0;   // bend: numfield
            if ( opt == 7 ) return m_bendSegs;
            return 0;
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
            else if ( opt == 4 && value >= 0 && value < KPIPE_CORNER_COUNT )
                m_corner = s_lastCorner = value;
            else if ( opt == 5 )
                m_doubleSided = s_lastDoubleSided = value != 0;
            else if ( opt == 6 && m_corner == KPIPE_CORNER_MITER )
                m_miterMax = s_lastMiterMax = value < 15 ? 15 : value > 170 ? 170 : value;
            else if ( opt == 7 )
                m_bendSegs = s_lastBendSegs = value < 1 ? 1 : value > KPIPE_BEND_SEGS_MAX ? KPIPE_BEND_SEGS_MAX : value;
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
            m_corner  = s_lastCorner;
            m_bend    = s_lastBend;
            m_bendSegs = s_lastBendSegs;
            m_miterMax = s_lastMiterMax;
            m_doubleSided = s_lastDoubleSided;
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
                if ( s.curved && CurvedAsPatches() )
                    BuildCurved( s, &defs, &patches, &rejected );
                else
                    BuildStraight( s, &defs, &brushes, &patches, &rejected );
            }
            if ( defs.empty() )
            {
                Sys_Printf( "%s: nothing built%s.\n", Name(), rejected ? " - every piece was rejected" : "" );
                g_nUpdateBits |= 1;
                return;
            }
            // KIWI (2026-10-31): a walkable tunnel - every curve patch also faces inward.  Brush
            // prisms are solid and stay as they are.
            int backs = 0;
            if ( m_doubleSided )
            {
                const size_t front = defs.size();
                for ( size_t i = 0; i < front; ++i )
                    if ( brush_t *back = KiwiPrim_BuildPatchBackface( defs[i] ) )
                    {
                        defs.push_back( back );
                        ++backs;
                    }
            }
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( UndoName() );
            for ( size_t i = 0; i < defs.size(); ++i )
                KiwiExtrude_LandDef( defs[i] );
            ReportBuilt( brushes, patches, backs, rejected );
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
            for ( size_t i = 0; i < m_spines.size(); ++i )
            {
                // Runs of frames: a curved spine is one run; a straight one a run per segment
                // (start, end - mitered ends ring on their cut plane) and per elbow.
                std::vector<std::vector<pipeFrame_t> > runs;
                PreviewRuns( m_spines[i], &runs );
                for ( size_t r = 0; r < runs.size(); ++r )
                {
                    const std::vector<pipeFrame_t> &frames = runs[r];
                    for ( size_t f = 0; f < frames.size(); ++f )
                    {
                        if ( !DrawSection( m_spines[i], frames[f] ) )
                            return;
                        if ( f + 1 < frames.size() && !DrawRails( m_spines[i], frames[f], frames[f + 1] ) )
                            return;
                    }
                }
            }
        }

    protected:
        // ── what a Sweep (KiwiSweepCommand below) does differently ─────────────
        virtual const char *UndoName() const { return "pipe"; }
        // A curved spine as curve patches (else as mitered brush segments on its tessellation).
        virtual bool CurvedAsPatches() const { return true; }
        // Elbows are curve patches, which hold KPIPE_MAX_PATCH_SIDES regular sides at most.
        virtual bool BendsAllowed() const { return m_sides <= KPIPE_MAX_PATCH_SIDES; }

        // One section ring at `f`; false when the line budget ran out.
        virtual bool DrawSection( const pipeSpine_t &s, const pipeFrame_t &f )
        {
            (void)s;
            const int ringSegs = m_sides >= 3 ? m_sides : 16;
            float prev[3];
            for ( int k = 0; k <= ringSegs; ++k )
            {
                const float a = 2.0f * KPIPE_PI * (float)( k % ringSegs ) / (float)ringSegs;
                float p[3];
                SectionPoint( f, (float)m_angle, cosf( a ) * m_radius, sinf( a ) * m_radius, p );
                if ( k && !KiwiLines_Add( prev, p ) )
                    return false;
                Copy3( p, prev );
            }
            return true;
        }

        // Four longitudinal rails from ring `a` to ring `b`.
        virtual bool DrawRails( const pipeSpine_t &s, const pipeFrame_t &a, const pipeFrame_t &b )
        {
            (void)s;
            const int ringSegs = m_sides >= 3 ? m_sides : 16;
            for ( int k = 0; k < 4; ++k )
            {
                const int idx = ( k * ringSegs ) / 4;
                const float ang = 2.0f * KPIPE_PI * (float)idx / (float)ringSegs;
                float p0[3], p1[3];
                SectionPoint( a, (float)m_angle, cosf( ang ) * m_radius, sinf( ang ) * m_radius, p0 );
                SectionPoint( b, (float)m_angle, cosf( ang ) * m_radius, sinf( ang ) * m_radius, p1 );
                if ( !KiwiLines_Add( p0, p1 ) )
                    return false;
            }
            return true;
        }

        virtual void ReportBuilt( int brushes, int patches, int backs, int rejected )
        {
            char r[32];
            KiwiUnits_Format( r, sizeof( r ), m_radius );
            Sys_Printf( "Pipe: radius %s - %i brush%s, %i curve patch%s%s%s.\n", r,
                        brushes, brushes == 1 ? "" : "es", patches, patches == 1 ? "" : "es",
                        backs ? " (each double-sided)" : "",
                        rejected ? " (some segments were too short or degenerate)" : "" );
            if ( m_doubleSided && brushes > 0 )
                Sys_Printf( "Pipe: double-sided applies to curve patches; the %i brush prism%s %s "
                            "solid - use vertex count 0 (round) for a tunnel you can walk inside.\n",
                            brushes, brushes == 1 ? "" : "s", brushes == 1 ? "is" : "are" );
        }

        // Curve patches through 2 * spans + 1 stations: an elbow, or a curved spine.
        virtual void BuildTube( const pipeSpine_t &s, const std::vector<float> &pts,
                                const std::vector<pipeFrame_t> &frames, int spans,
                                std::vector<brush_t *> *defs, int *patches, int *rejected )
        {
            (void)s;
            BuildSweep( pts, frames, spans, m_sides, defs, patches, rejected );
        }
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

        // Bend radius to the centre line: typed / dragged, else the section size (a tight
        // elbow whose inside just closes).
        float BendRadius() const
        {
            return m_bend > 0.0f ? m_bend : m_radius;
        }

        // Curved spines ring at their patch stations; straight ones at each piece's ends and
        // at every elbow station.
        void PreviewRuns( const pipeSpine_t &s, std::vector<std::vector<pipeFrame_t> > *runs ) const
        {
            runs->clear();
            if ( s.curved && CurvedAsPatches() )
            {
                std::vector<float> pts;
                runs->push_back( std::vector<pipeFrame_t>() );
                Resample( s, CurvedSpans( s ) + 1, &pts );
                CurvedFrames( pts, s.closed, s.haveHint ? s.hint : nullptr, &runs->back() );
                return;
            }
            std::vector<pipePiece_t> pieces;
            PlanStraight( s, &pieces, nullptr, s.curved );
            for ( size_t i = 0; i < pieces.size(); ++i )
            {
                const pipePiece_t &pc = pieces[i];
                if ( !pc.valid )
                    continue;
                if ( pc.elbow )
                {
                    runs->push_back( pc.frames );
                    continue;
                }
                pipeFrame_t a = pc.f, b = pc.f;
                Mad3( pc.f.p, pc.f.t, pc.len, b.p );
                if ( pc.miter[0] ) { Copy3( pc.corner[0], a.p ); Copy3( pc.cut[0], a.cut ); }
                if ( pc.miter[1] ) { Copy3( pc.corner[1], b.p ); Copy3( pc.cut[1], b.cut ); }
                runs->push_back( std::vector<pipeFrame_t>() );
                runs->back().push_back( a );
                runs->back().push_back( b );
            }
        }

        // ── straight spines: segments + corner joints ──────────────────────────
        // Lays out every segment (pulled back for bends, cut planes for miters) and every
        // elbow, carrying the section frame through the elbows so faceted sections line up.
        // `notes` (optional) gets bit 1 when bends fell back to miters (sections over
        // KPIPE_MAX_PATCH_SIDES), bit 2 when a bend radius was shrunk to fit a short segment.
        // `forceMiter` (a curved spine built as brush segments on its tessellation) miters every
        // corner whatever the Corners option says.
        void PlanStraight( const pipeSpine_t &s, std::vector<pipePiece_t> *out, int *notes,
                           bool forceMiter = false ) const
        {
            const int corner   = forceMiter ? KPIPE_CORNER_MITER : m_corner;
            const int miterMax = forceMiter ? 170 : m_miterMax;
            out->clear();
            const float *hint = s.haveHint ? s.hint : nullptr;
            const int n = PointCount( s );
            const int segs = s.closed ? n : n - 1;
            if ( segs < 1 )
                return;
            std::vector<float> dir( (size_t)segs * 3 ), len( (size_t)segs );
            std::vector<char>  ok( (size_t)segs );
            for ( int j = 0; j < segs; ++j )
            {
                float *d = &dir[(size_t)j * 3];
                Sub3( Pt( s, ( j + 1 ) % n ), Pt( s, j ), d );
                len[(size_t)j] = Len3( d );
                ok[(size_t)j] = len[(size_t)j] >= 1.0f && Norm3( d );
            }

            // Joint per vertex: kind (BUTT = none), miter plane, bend pull-back.
            struct joint_t { int kind; float theta, pull, cut[3]; };
            std::vector<joint_t> jt( (size_t)n );
            for ( int v = 0; v < n; ++v )
            {
                joint_t &j = jt[(size_t)v];
                j.kind = KPIPE_CORNER_BUTT;
                j.theta = j.pull = 0.0f;
                if ( corner == KPIPE_CORNER_BUTT || ( !s.closed && ( v == 0 || v == n - 1 ) ) )
                    continue;
                const int a = ( v - 1 + segs ) % segs, b = v % segs;
                if ( !ok[(size_t)a] || !ok[(size_t)b] )
                    continue;
                const float *d0 = &dir[(size_t)a * 3], *d1 = &dir[(size_t)b * 3];
                float c = Dot3( d0, d1 );
                c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
                const float theta = acosf( c );
                if ( theta * 180.0f / KPIPE_PI < KPIPE_MIN_TURN_DEG )
                    continue;
                j.theta = theta;
                int kind = corner;
                // Miters spike on sharp corners: past the limit they bend.
                if ( kind == KPIPE_CORNER_MITER && theta * 180.0f / KPIPE_PI > (float)miterMax )
                    kind = KPIPE_CORNER_BEND;
                // Elbows are curve patches, which hold KPIPE_MAX_PATCH_SIDES sides at most.
                if ( kind == KPIPE_CORNER_BEND && !BendsAllowed() )
                {
                    kind = KPIPE_CORNER_MITER;
                    if ( notes ) *notes |= 1;
                }
                if ( kind == KPIPE_CORNER_MITER )
                {
                    for ( int k = 0; k < 3; ++k )
                        j.cut[k] = d0[k] + d1[k];
                    if ( theta > 170.0f * KPIPE_PI / 180.0f || !Norm3( j.cut ) )
                        continue;                       // a U-turn has no miter
                }
                else
                {
                    const float half = theta * 0.5f < 89.5f * KPIPE_PI / 180.0f ? theta * 0.5f
                                                                               : 89.5f * KPIPE_PI / 180.0f;
                    j.pull = BendRadius() * tanf( half );
                }
                j.kind = kind;
            }

            // A segment can give at most 90% of its length to the bends at its two ends.
            std::vector<float> scale( (size_t)n, 1.0f );
            for ( int sg = 0; sg < segs; ++sg )
            {
                const int va = sg, vb = ( sg + 1 ) % n;
                const float want = jt[(size_t)va].pull + jt[(size_t)vb].pull;
                if ( want > 0.9f * len[(size_t)sg] && want > 0.0f )
                {
                    const float f = 0.9f * len[(size_t)sg] / want;
                    if ( f < scale[(size_t)va] ) scale[(size_t)va] = f;
                    if ( f < scale[(size_t)vb] ) scale[(size_t)vb] = f;
                }
            }
            for ( int v = 0; v < n; ++v )
                if ( scale[(size_t)v] < 1.0f && jt[(size_t)v].pull > 0.0f )
                {
                    jt[(size_t)v].pull *= scale[(size_t)v];
                    if ( notes ) *notes |= 2;
                }

            float prevU[3];
            bool  havePrev = false;
            for ( int sg = 0; sg < segs; ++sg )
            {
                const int va = sg, vb = ( sg + 1 ) % n;
                pipePiece_t pc;
                if ( !ok[(size_t)sg] )
                {
                    pc.valid = false;
                    out->push_back( pc );
                    continue;
                }
                const float *d = &dir[(size_t)sg * 3];
                const joint_t &ja = jt[(size_t)va], &jb = jt[(size_t)vb];
                Copy3( d, pc.f.t );
                Mad3( Pt( s, va ), d, ja.kind == KPIPE_CORNER_BEND ? ja.pull : 0.0f, pc.f.p );
                pc.len = len[(size_t)sg] - ( ja.kind == KPIPE_CORNER_BEND ? ja.pull : 0.0f )
                                         - ( jb.kind == KPIPE_CORNER_BEND ? jb.pull : 0.0f );
                FinishFrame( &pc.f, havePrev ? prevU : nullptr, hint );
                Copy3( pc.f.u, prevU );
                havePrev = true;
                pc.miter[0] = ja.kind == KPIPE_CORNER_MITER;
                pc.miter[1] = jb.kind == KPIPE_CORNER_MITER;
                Copy3( ja.kind == KPIPE_CORNER_MITER ? ja.cut : d, pc.cut[0] );
                Copy3( jb.kind == KPIPE_CORNER_MITER ? jb.cut : d, pc.cut[1] );
                Copy3( Pt( s, va ), pc.corner[0] );
                Copy3( Pt( s, vb ), pc.corner[1] );
                out->push_back( pc );

                if ( jb.kind != KPIPE_CORNER_BEND || !( jb.pull > 0.0f ) )
                    continue;
                // The elbow at vb: an arc of radius pull / tan(theta / 2) from the pulled-back
                // end of this segment to the pulled-back start of the next.
                const int nx = vb % segs;
                const float *d1 = &dir[(size_t)nx * 3];
                float inward[3];
                const float c = Dot3( d, d1 );
                for ( int k = 0; k < 3; ++k )
                    inward[k] = d1[k] - c * d[k];
                if ( !Norm3( inward ) )
                    continue;
                const float half = jb.theta * 0.5f;
                const float R = jb.pull / tanf( half );
                float A[3], C[3];
                Mad3( Pt( s, vb ), d, -jb.pull, A );
                Mad3( A, inward, R, C );
                pipePiece_t el;
                el.elbow = true;
                el.spans = (int)ceilf( jb.theta / ( 0.5f * KPIPE_PI ) * (float)m_bendSegs );
                if ( el.spans < 1 ) el.spans = 1;
                const int stations = el.spans * 2 + 1;
                for ( int q = 0; q < stations; ++q )
                {
                    const float phi = jb.theta * (float)q / (float)( stations - 1 );
                    const float cp = cosf( phi ), sp = sinf( phi );
                    pipeFrame_t f;
                    for ( int k = 0; k < 3; ++k )
                    {
                        f.p[k] = C[k] + R * ( -inward[k] * cp + d[k] * sp );
                        f.t[k] = d[k] * cp + inward[k] * sp;
                    }
                    Norm3( f.t );
                    FinishFrame( &f, prevU, hint );
                    Copy3( f.u, prevU );
                    el.pts.insert( el.pts.end(), f.p, f.p + 3 );
                    el.frames.push_back( f );
                }
                out->push_back( el );
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
            std::vector<pipePiece_t> pieces;
            int notes = 0;
            PlanStraight( s, &pieces, &notes, s.curved );
            if ( notes & 1 )
                Sys_Printf( "Pipe: bends are curve patches (at most %i sides) - the corners of this "
                            "%i-sided pipe are mitered instead.\n", KPIPE_MAX_PATCH_SIDES, m_sides );
            if ( notes & 2 )
                Sys_Printf( "%s: some bend radii were reduced to fit short segments.\n", Name() );
            for ( size_t i = 0; i < pieces.size(); ++i )
            {
                const pipePiece_t &pc = pieces[i];
                if ( !pc.valid )
                {
                    ++*rejected;
                    continue;
                }
                if ( pc.elbow )
                {
                    BuildTube( s, pc.pts, pc.frames, pc.spans, defs, patches, rejected );
                    continue;
                }
                if ( pc.len < 0.05f )
                {
                    ++*rejected;
                    continue;
                }
                BuildSegment( s, pc, defs, brushes, patches, rejected );
            }
        }

        // One straight segment from pc.f along its tangent for pc.len, each end square or
        // mitered onto its corner's cut plane: a regular brush prism, or a patch cylinder.
        virtual void BuildSegment( const pipeSpine_t &s, const pipePiece_t &pc,
                                   std::vector<brush_t *> *defs, int *brushes, int *patches,
                                   int *rejected )
        {
            (void)s;
            {
                const pipeFrame_t &f = pc.f;
                const float len = pc.len;

                kconPlane_t pl;
                if ( !SegmentPlane( f, &pl ) )
                {
                    ++*rejected;
                    return;
                }
                float centre[2];
                KiwiCon_WorldToPlane( pl, f.p, centre );

                if ( m_sides < 3 )
                {
                    brush_t *def = KiwiPrim_BuildPatchCylinder( pl, centre, m_radius, 0.0f, len, m_density );
                    if ( !def )
                    {
                        ++*rejected;
                        return;
                    }
                    // Miter: slide the end rings along the axis onto the cut planes; the middle
                    // row goes back halfway so the sides stay straight.
                    if ( ( pc.miter[0] || pc.miter[1] ) && def->patch
                      && def->patch->height == KPRIM_PATCH_ROWS )
                    {
                        patchMesh_t *p = def->patch;
                        for ( int col = 0; col < p->width; ++col )
                        {
                            float *r0 = p->ctrl[col][0].xyz, *r2 = p->ctrl[col][2].xyz;
                            if ( pc.miter[0] ) OntoCut( r0, f.t, pc.cut[0], pc.corner[0] );
                            if ( pc.miter[1] ) OntoCut( r2, f.t, pc.cut[1], pc.corner[1] );
                            for ( int k = 0; k < 3; ++k )
                                p->ctrl[col][1].xyz[k] = 0.5f * ( r0[k] + r2[k] );
                        }
                        Patch_Rebuild( p, 1 );
                        KiwiValid_Rebuild( def );
                    }
                    defs->push_back( def );
                    ++*patches;
                    return;
                }

                // CCW about the plane normal: the section's own angle order.
                std::vector<float> loop;
                for ( int k = 0; k < m_sides; ++k )
                {
                    const float ang = 2.0f * KPIPE_PI * (float)k / (float)m_sides;
                    loop.push_back( cosf( ang ) * m_radius );
                    loop.push_back( sinf( ang ) * m_radius );
                }
                if ( brush_t *def = BuildSegmentPrism( pc, pl, loop ) )
                {
                    defs->push_back( def );
                    ++*brushes;
                }
                else
                    ++*rejected;
            }
        }

        // The segment's section plane: through f.p, normal along the segment, u = the turned
        // section axis.
        bool SegmentPlane( const pipeFrame_t &f, kconPlane_t *pl ) const
        {
            float turnedU[3];
            const float a = (float)m_angle * KPIPE_PI / 180.0f;
            for ( int k = 0; k < 3; ++k )
                turnedU[k] = cosf( a ) * f.u[k] + sinf( a ) * f.v[k];
            return KiwiCon_MakePlane( f.p, f.t, turnedU, pl );
        }

        // A convex section (c, s pairs, CCW about the tangent) as a brush prism along the
        // segment, validated and unlinked; null when rejected.
        brush_t *BuildSegmentPrism( const pipePiece_t &pc, const kconPlane_t &pl,
                                    const std::vector<float> &section ) const
        {
            const pipeFrame_t &f = pc.f;
            const float len = pc.len;
            const int n = (int)( section.size() / 2 );
            if ( n < 3 )
                return nullptr;
            std::vector<float> loop;
            for ( int k = 0; k < n; ++k )
            {
                float w[3], uv[2];
                SectionPoint( f, (float)m_angle, section[(size_t)k * 2], section[(size_t)k * 2 + 1], w );
                KiwiCon_WorldToPlane( pl, w, uv );
                loop.push_back( uv[0] );
                loop.push_back( uv[1] );
            }
            const char *why = nullptr;
            brush_t *def = KiwiExtrude_BuildPrismDef( pl, &loop[0], n, 0.0f, len, &why );
            if ( !def )
                return nullptr;
            // Miter: the cap faces' plane points slide along the axis onto the cut planes
            // (a shear along t keeps their winding, so the caps still face outward).
            if ( pc.miter[0] || pc.miter[1] )
                for ( int fi = 0; fi < def->faceCount; ++fi )
                {
                    face_t *fc = &def->faces[fi];
                    float e1[3], e2[3], nrm[3];
                    Sub3( fc->planepts[1], fc->planepts[0], e1 );
                    Sub3( fc->planepts[2], fc->planepts[0], e2 );
                    Cross3( e1, e2, nrm );
                    if ( !Norm3( nrm ) || fabsf( Dot3( nrm, f.t ) ) < 0.999f )
                        continue;                   // a side face
                    float rel[3];
                    Sub3( fc->planepts[0], f.p, rel );
                    const int end = Dot3( rel, f.t ) > 0.5f * len ? 1 : 0;
                    if ( !pc.miter[end] )
                        continue;
                    for ( int q = 0; q < 3; ++q )
                        OntoCut( fc->planepts[q], f.t, pc.cut[end], pc.corner[end] );
                }
            KiwiValid_Rebuild( def );
            if ( !KiwiValid_CheckBrush( def, &why ) )
            {
                Brush_Free_R( def );               // still unlinked
                return nullptr;
            }
            return def;
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
            if ( m_sides > KPIPE_MAX_PATCH_SIDES )      // BuildSweep clamps
                Sys_Printf( "Pipe: a curved pipe is made of curve patches, which hold at most %i "
                            "sides - using %i.\n", KPIPE_MAX_PATCH_SIDES, KPIPE_MAX_PATCH_SIDES );
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
            BuildTube( s, pts, frames, spans, defs, patches, rejected );
        }

        // Curve patches swept through 2 * spans + 1 stations (even = on the spine, odd = the
        // span midpoints the quadratic passes through) - a curved spine, or one elbow.
        void BuildSweep( const std::vector<float> &pts, const std::vector<pipeFrame_t> &frames,
                         int spans, int sides, std::vector<brush_t *> *defs,
                         int *patches, int *rejected )
        {
            if ( sides > KPIPE_MAX_PATCH_SIDES )
                sides = KPIPE_MAX_PATCH_SIDES;
            const int cols = ( sides < 3 ) ? ( m_density + 3 ) * 2 + 1 : sides * 2 + 1;
            BuildSweepColumns( pts, frames, spans, cols,
                               [&]( int col, float *c, float *sn ) { SectionColumn( col, sides, c, sn ); },
                               defs, patches, rejected );
        }

        // BuildSweep's patches with any section: `column( col, &c, &s )` gives column col's
        // section control point (c, s along the frame's turned u / v) - KIWI (2026-11-01): a
        // Sweep's face loops come through here too.
        template <typename ColumnFn>
        void BuildSweepColumns( const std::vector<float> &pts, const std::vector<pipeFrame_t> &frames,
                                int spans, int cols, const ColumnFn &column,
                                std::vector<brush_t *> *defs, int *patches, int *rejected )
        {
            if ( spans < 1 || (int)frames.size() < spans * 2 + 1 || cols < 3 || cols > KPATCH_MAX_WIDTH )
            {
                ++*rejected;
                return;
            }

            // Section control points per resampled station.
            auto station = [&]( int row, int col, float out[3] )
            {
                float c, sn;
                column( col, &c, &sn );
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

        virtual void UpdateHud()
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
            char corners[48];
            if ( !anyStraight )
                corners[0] = '\0';
            else if ( m_corner == KPIPE_CORNER_BEND )
            {
                char b[32];
                KiwiUnits_Format( b, sizeof( b ), BendRadius() );
                _snprintf( corners, sizeof( corners ), "  bend %s", b );
            }
            else
                _snprintf( corners, sizeof( corners ), "  %s corners",
                           m_corner == KPIPE_CORNER_MITER ? "miter" : "butt" );
            corners[sizeof( corners ) - 1] = '\0';
            const char *sided = m_doubleSided ? "  double-sided" : "";
            if ( m_sides < 3 )
                _snprintf( m_hud, sizeof( m_hud ), "pipe  radius %s  round  angle %i%s%s  ->  %s",
                           r, m_angle, corners, sided, what );
            else
                _snprintf( m_hud, sizeof( m_hud ), "pipe  radius %s  %i sides  angle %i%s%s  ->  %s",
                           r, m_sides, m_angle, corners, sided, what );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        std::vector<pipeSpine_t> m_spines;
        pipeFrame_t m_first;
        float       m_handleBase[3] = { 0.0f, 0.0f, 0.0f };
        float       m_radius  = 8.0f;
        int         m_sides   = 0;
        int         m_angle   = 0;
        int         m_density = KPRIM_PATCH_DENSITY_DEF;
        int         m_corner  = KPIPE_CORNER_BEND;
        float       m_bend    = 0.0f;           // 0 = the section size
        int         m_bendSegs = 2;
        int         m_miterMax = 135;
        bool        m_doubleSided = false;      // curve patches get inward-facing twins
        bool        m_grab    = false;
        float       m_start   = 0.0f;
        char        m_hud[192] = { 0 };
    };

    KiwiPipeCommand s_pipe;

    // ── Sweep ────────────────────────────────────────────────────────────────────
    // KIWI (2026-11-01, user: "add a sweep command that is basically the pipe tool, but for a
    // custom construction face. ... Allow any construction face to be swept along the line. Also
    // allow picking the point of the construction face that will adhere exactly to the line
    // being swept."): see kiwi_pipe.h.
    const kiwiNumField_t KSWEEP_FIELDS[2] = { { "angle", KNUM_ANGLE,  false },
                                              { "bend",  KNUM_LENGTH, false } };
    const char *const KSWEEP_CURVED_NAMES[2] = { "Curve patches", "Brush segments (mitered)" };
    const float KSWEEP_PICK_PIXELS   = 16.0f;   // anchor / path click radius
    const float KSWEEP_CORNER_DEG    = 5.0f;    // a face loop turning less is no corner
    const int   KSWEEP_MAX_CANDIDATES = 256;
    const float KSWEEP_COL_CAND[3]   = { 1.00f, 0.85f, 0.30f };
    const float KSWEEP_COL_ANCHOR[3] = { 1.00f, 0.35f, 0.25f };

    int s_sweepAngle  = 0;
    int s_sweepCurved = 0;                      // 0 = curve patches, 1 = mitered brush segments

    enum { SWEEP_PICK_PATH = 0, SWEEP_PICK_FACE, SWEEP_READY };

    class KiwiSweepCommand : public KiwiPipeCommand
    {
    public:
        const char *Name() const override { return "Sweep"; }
        bool CanExecute() override        { return KiwiSweep_CanExecute(); }
        bool HudInvalid() const override  { return m_phase != SWEEP_READY || m_spines.empty() || m_anchor < 0; }

        int NumericFields( const kiwiNumField_t **out ) const override
        {
            *out = KSWEEP_FIELDS;
            return 2;
        }
        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 0 )      *out = (float)m_angle;    // degrees - KNUM_ANGLE formats it
            else if ( field == 1 ) *out = BendRadius();
            else                   return false;
            return true;
        }
        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( !has )
                return;
            if ( field == 0 )      // an angle is not a length: undo the numeric layer's scale
                SetAngle( (int)floorf( Units_ToDisplay( world ) + 0.5f ) );
            else if ( field == 1 )
            {
                m_bend = world < KPIPE_MIN_RADIUS ? KPIPE_MIN_RADIUS : world;
                s_lastBend = m_bend;
            }
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        int CommandOptions( const kiwiOption_t **out ) const override
        {
            static const kiwiOption_t base[5] = {
                { "Angle (degrees)",            KOPT_INT,  nullptr, 0, 0, 0, 360, -1 },
                { "Curved paths",               KOPT_ENUM, KSWEEP_CURVED_NAMES, 2, 0, 0, 0, -1 },
                { "Curve density",              KOPT_INT,  nullptr, 0, 0, KPRIM_PATCH_DENSITY_MIN,
                                                                       KPRIM_PATCH_DENSITY_MAX, -1 },
                { "Corners",                    KOPT_ENUM, KPIPE_CORNER_NAMES, KPIPE_CORNER_COUNT, 0,
                                                                       0, 0, -1 },
                { "Double-sided (walk inside)", KOPT_TOGGLE, nullptr, 0, 0, 0, 1, -1 },
            };
            static const kiwiOption_t bend[2] = {
                { "Bend radius (to the path)",    KOPT_NUMFIELD, nullptr, 0, 1, KPIPE_MIN_RADIUS, 4096, -1 },
                { "Bend smoothness (per 90 deg)", KOPT_INT,      nullptr, 0, 0, 1, KPIPE_BEND_SEGS_MAX, -1 },
            };
            static const kiwiOption_t miter[1] = {
                { "Miter up to (degrees)",        KOPT_INT,      nullptr, 0, 0, 15, 170, -1 },
            };
            static kiwiOption_t rows[7];
            for ( int i = 0; i < 5; ++i )
                rows[i] = base[i];
            int n = 5;
            if ( m_corner == KPIPE_CORNER_BEND )
            {
                rows[n++] = bend[0];
                rows[n++] = bend[1];
            }
            else if ( m_corner == KPIPE_CORNER_MITER )
                rows[n++] = miter[0];
            *out = rows;
            return n;
        }
        int OptionValue( int opt ) const override
        {
            if ( opt == 0 ) return m_angle;
            if ( opt == 1 ) return m_curved;
            if ( opt == 2 ) return m_density;
            if ( opt == 3 ) return m_corner;
            if ( opt == 4 ) return m_doubleSided ? 1 : 0;
            if ( opt == 5 ) return m_corner == KPIPE_CORNER_MITER ? m_miterMax : 0;   // bend: numfield
            if ( opt == 6 ) return m_bendSegs;
            return 0;
        }
        void OptionChanged( int opt, int value ) override
        {
            if ( opt == 0 )
                SetAngle( value );
            else if ( opt == 1 )
                m_curved = s_sweepCurved = value ? 1 : 0;
            else if ( opt == 2 )
            {
                m_density = value < KPRIM_PATCH_DENSITY_MIN ? KPRIM_PATCH_DENSITY_MIN
                          : value > KPRIM_PATCH_DENSITY_MAX ? KPRIM_PATCH_DENSITY_MAX : value;
                KiwiPrim_SetPatchDensity( m_density );
            }
            else if ( opt == 3 && value >= 0 && value < KPIPE_CORNER_COUNT )
                m_corner = s_lastCorner = value;
            else if ( opt == 4 )
                m_doubleSided = s_lastDoubleSided = value != 0;
            else if ( opt == 5 && m_corner == KPIPE_CORNER_MITER )
                m_miterMax = s_lastMiterMax = value < 15 ? 15 : value > 170 ? 170 : value;
            else if ( opt == 6 )
                m_bendSegs = s_lastBendSegs = value < 1 ? 1 : value > KPIPE_BEND_SEGS_MAX ? KPIPE_BEND_SEGS_MAX : value;
            PlaceAll();                       // the first frame follows the curved-path mode
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t path[]   = { { "Click", "Path line / spline" } };
            static const kiwiPrompt_t face[]   = { { "Click", "Construction face" } };
            static const kiwiPrompt_t anchor[] = { { "Click", "Anchor point on the face" },
                                                   { "[ ]",   "Next / previous anchor" } };
            if ( m_phase == SWEEP_PICK_PATH ) { *out = path; return 1; }
            if ( m_phase == SWEEP_PICK_FACE ) { *out = face; return 1; }
            *out = anchor;
            return 2;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( mods || ( vk != 0xDB && vk != 0xDD ) || m_phase != SWEEP_READY || m_cands.empty() )
                return false;
            const int n = (int)( m_cands.size() / 3 );
            m_anchor = ( ( m_anchor < 0 ? 0 : m_anchor ) + ( vk == 0xDD ? 1 : n - 1 ) ) % n;
            m_anchorSet = true;
            PlaceAll();
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;
        }

        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            (void)outAnchor;  (void)outDir;
            return false;                     // the face is the section: no size handle
        }
        void Rebase() override {}

        // Clicks pick: the path, the face, then (any number of times) the anchor.
        bool PressIntercept( int imgX, int imgY ) override
        {
            if ( m_phase == SWEEP_PICK_PATH )
            {
                const int obj = PickPathAt( imgX, imgY );
                if ( obj >= 0 )
                {
                    m_paths.assign( 1, obj );
                    m_phase = m_haveFace ? SWEEP_READY : SWEEP_PICK_FACE;
                    Rebuild();
                }
            }
            else if ( m_phase == SWEEP_PICK_FACE )
            {
                ray_t ray;
                const int reg = Pick_RayFromImagePos( imgX, imgY, &ray ) ? KiwiRegion_PickAt( ray ) : -1;
                const std::vector<kregion_t> &regions = KiwiRegion_All();
                if ( reg >= 0 && reg < (int)regions.size() )
                {
                    SetFace( regions[(size_t)reg] );
                    m_phase = SWEEP_READY;
                    Rebuild();
                }
            }
            else
            {
                const int c = PickCandidateAt( imgX, imgY );
                if ( c >= 0 )
                {
                    m_anchor = c;
                    m_anchorSet = true;
                    PlaceAll();
                }
            }
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;                      // a stray click never resumes a mouse-follow
        }

        bool Begin() override
        {
            m_spines.clear();
            m_paths.clear();
            m_cands.clear();
            m_haveFace = false;
            m_anchor = -1;
            m_anchorSet = false;
            m_grab = false;
            m_hud[0] = '\0';
            if ( KiwiRegion_SelectedCount() > 0 )
            {
                const int r = KiwiRegion_SelectedAt( 0 );
                const std::vector<kregion_t> &regions = KiwiRegion_All();
                if ( r >= 0 && r < (int)regions.size() )
                    SetFace( regions[(size_t)r] );
            }
            const int n = KiwiConSel_Count();
            for ( int i = 0; i < n; ++i )
            {
                const kconSelItem_t *it = KiwiConSel_At( i );
                if ( !it || !Pipeable( KiwiCon_At( it->object ) ) )
                    continue;
                bool dup = false;
                for ( size_t k = 0; k < m_paths.size() && !dup; ++k )
                    dup = m_paths[k] == it->object;
                if ( !dup )
                    m_paths.push_back( it->object );
            }
            if ( !m_haveFace && m_paths.empty() )
            {
                Sys_Printf( "Sweep: select a construction face and the line / spline to sweep it along.\n" );
                return false;
            }
            m_sides    = 0;
            m_angle    = s_sweepAngle;
            m_curved   = s_sweepCurved;
            m_corner   = s_lastCorner;
            m_bend     = s_lastBend;
            m_bendSegs = s_lastBendSegs;
            m_miterMax = s_lastMiterMax;
            m_doubleSided = s_lastDoubleSided;
            m_density  = KiwiPrim_PatchDensity();
            m_phase = m_paths.empty() ? SWEEP_PICK_PATH : m_haveFace ? SWEEP_READY : SWEEP_PICK_FACE;
            Rebuild();
            UpdateHud();
            KiwiCmd_Pause();                  // nothing follows the mouse; clicks pick
            if ( m_phase == SWEEP_PICK_PATH )
                Sys_Printf( "Sweep: click the line / spline to sweep the face along.\n" );
            else if ( m_phase == SWEEP_PICK_FACE )
                Sys_Printf( "Sweep: click the construction face to sweep.\n" );
            Sys_Printf( "Sweep: click a marked point of the face (or [ ]) for the point that runs along "
                        "the path.  Enter / RMB / Confirm builds it.\n" );
            return true;
        }

        void Cancel() override
        {
            m_spines.clear();
            m_paths.clear();
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( m_phase == SWEEP_READY )
                KiwiPipeCommand::DrawWorld();
            if ( !m_haveFace )
                return;
            // The anchor candidates, where the face is drawn.
            const int n = (int)( m_cands.size() / 3 );
            for ( int i = 0; i < n; ++i )
            {
                const float *p = &m_cands[(size_t)i * 3];
                const bool chosen = i == m_anchor;
                const float *col = chosen ? KSWEEP_COL_ANCHOR : KSWEEP_COL_CAND;
                KiwiLines_Color( col[0], col[1], col[2] );
                const float r = KiwiCam_WorldPerPixel( p ) * ( chosen ? 8.0f : 4.0f );
                float a[3], b[3];
                for ( int axis = 0; axis < 2; ++axis )
                {
                    const float *d = axis ? m_face.plane.v : m_face.plane.u;
                    Mad3( p, d, -r, a );
                    Mad3( p, d,  r, b );
                    if ( !KiwiLines_Add( a, b ) )
                        return;
                }
                if ( chosen )                 // a diamond round the chosen one
                    for ( int k = 0; k < 4; ++k )
                    {
                        const float *d0 = ( k & 1 ) ? m_face.plane.v : m_face.plane.u;
                        const float *d1 = ( k & 1 ) ? m_face.plane.u : m_face.plane.v;
                        const float s0 = ( k < 2 ) ? r : -r, s1 = ( k == 0 || k == 3 ) ? r : -r;
                        Mad3( p, d0, s0, a );
                        Mad3( p, d1, s1, b );
                        if ( !KiwiLines_Add( a, b ) )
                            return;
                    }
            }
        }

    protected:
        const char *UndoName() const override { return "sweep"; }
        bool CurvedAsPatches() const override { return m_curved == 0; }
        bool BendsAllowed() const override    { return true; }   // each loop's tube is chunked

        bool DrawSection( const pipeSpine_t &s, const pipeFrame_t &f ) override
        {
            for ( size_t l = 0; l < s.loops.size(); ++l )
            {
                const std::vector<float> &loop = s.loops[l];
                const int n = (int)( loop.size() / 2 );
                for ( int k = 0; k < n; ++k )
                {
                    const int j = ( k + 1 ) % n;
                    float a[3], b[3];
                    SectionPoint( f, (float)m_angle, loop[(size_t)k * 2], loop[(size_t)k * 2 + 1], a );
                    SectionPoint( f, (float)m_angle, loop[(size_t)j * 2], loop[(size_t)j * 2 + 1], b );
                    if ( !KiwiLines_Add( a, b ) )
                        return false;
                }
            }
            return true;
        }

        bool DrawRails( const pipeSpine_t &s, const pipeFrame_t &a, const pipeFrame_t &b ) override
        {
            if ( s.loops.empty() )
                return true;
            const std::vector<float> &loop = s.loops[0];
            const int n = (int)( loop.size() / 2 );
            const int rails = n < 4 ? n : 4;
            for ( int k = 0; k < rails; ++k )
            {
                const int idx = ( k * n ) / rails;
                float p0[3], p1[3];
                SectionPoint( a, (float)m_angle, loop[(size_t)idx * 2], loop[(size_t)idx * 2 + 1], p0 );
                SectionPoint( b, (float)m_angle, loop[(size_t)idx * 2], loop[(size_t)idx * 2 + 1], p1 );
                if ( !KiwiLines_Add( p0, p1 ) )
                    return false;
            }
            return true;
        }

        void ReportBuilt( int brushes, int patches, int backs, int rejected ) override
        {
            Sys_Printf( "Sweep: %i brush%s, %i curve patch%s%s%s.\n",
                        brushes, brushes == 1 ? "" : "es", patches, patches == 1 ? "" : "es",
                        backs ? " (each double-sided)" : "",
                        rejected ? " (some pieces were too short or degenerate)" : "" );
            if ( m_doubleSided && brushes > 0 )
                Sys_Printf( "Sweep: double-sided applies to curve patches; the %i brush prism%s %s solid.\n",
                            brushes, brushes == 1 ? "" : "s", brushes == 1 ? "is" : "are" );
        }

        // A straight segment: one brush prism per convex piece of the face.
        void BuildSegment( const pipeSpine_t &s, const pipePiece_t &pc,
                           std::vector<brush_t *> *defs, int *brushes, int *patches,
                           int *rejected ) override
        {
            (void)patches;
            kconPlane_t pl;
            if ( s.pieces.empty() || !SegmentPlane( pc.f, &pl ) )
            {
                ++*rejected;
                return;
            }
            for ( size_t i = 0; i < s.pieces.size(); ++i )
            {
                if ( brush_t *def = BuildSegmentPrism( pc, pl, s.pieces[i] ) )
                {
                    defs->push_back( def );
                    ++*brushes;
                }
                else
                    ++*rejected;
            }
        }

        // Elbows and curved paths: each face loop as a tube of curve patches, at most 7 of its
        // edges (15 columns) per patch - even columns on its corners, odd ones mid-edge, so the
        // sides stay flat.
        void BuildTube( const pipeSpine_t &s, const std::vector<float> &pts,
                        const std::vector<pipeFrame_t> &frames, int spans,
                        std::vector<brush_t *> *defs, int *patches, int *rejected ) override
        {
            const int kMaxEdges = ( KPATCH_MAX_WIDTH - 1 ) / 2;
            for ( size_t l = 0; l < s.loops.size(); ++l )
            {
                const std::vector<float> &loop = s.loops[l];
                const int n = (int)( loop.size() / 2 );
                if ( n < 3 )
                    continue;
                const int chunks = ( n + kMaxEdges - 1 ) / kMaxEdges;
                int e0 = 0;
                for ( int ch = 0; ch < chunks; ++ch )
                {
                    const int edges = n / chunks + ( ch < n % chunks ? 1 : 0 );
                    const int first = e0;
                    BuildSweepColumns( pts, frames, spans, edges * 2 + 1,
                        [&]( int col, float *c, float *sn )
                        {
                            const int k0 = ( first + col / 2 ) % n, k1 = ( k0 + 1 ) % n;
                            if ( !( col & 1 ) )
                            {
                                *c  = loop[(size_t)k0 * 2];
                                *sn = loop[(size_t)k0 * 2 + 1];
                                return;
                            }
                            *c  = 0.5f * ( loop[(size_t)k0 * 2] + loop[(size_t)k1 * 2] );
                            *sn = 0.5f * ( loop[(size_t)k0 * 2 + 1] + loop[(size_t)k1 * 2 + 1] );
                        },
                        defs, patches, rejected );
                    e0 += edges;
                }
            }
        }

        void UpdateHud() override
        {
            if ( m_phase == SWEEP_PICK_PATH )
                _snprintf( m_hud, sizeof( m_hud ), "sweep  ->  click the line / spline to sweep along" );
            else if ( m_phase == SWEEP_PICK_FACE )
                _snprintf( m_hud, sizeof( m_hud ), "sweep  ->  click the construction face to sweep" );
            else
            {
                char anchor[32];
                if ( m_anchor < 0 )
                    _snprintf( anchor, sizeof( anchor ), "none (degenerate face)" );
                else if ( m_anchor == m_drawnCand )
                    _snprintf( anchor, sizeof( anchor ), "as drawn" );
                else if ( m_anchor == m_centroidCand )
                    _snprintf( anchor, sizeof( anchor ), "centroid" );
                else
                    _snprintf( anchor, sizeof( anchor ), "corner %i", m_anchor + 1 );
                anchor[sizeof( anchor ) - 1] = '\0';
                char corners[48];
                if ( m_corner == KPIPE_CORNER_BEND )
                {
                    char b[32];
                    KiwiUnits_Format( b, sizeof( b ), BendRadius() );
                    _snprintf( corners, sizeof( corners ), "  bend %s", b );
                }
                else
                    _snprintf( corners, sizeof( corners ), "  %s corners",
                               m_corner == KPIPE_CORNER_MITER ? "miter" : "butt" );
                corners[sizeof( corners ) - 1] = '\0';
                _snprintf( m_hud, sizeof( m_hud ), "sweep  anchor %s  angle %i%s%s%s",
                           anchor, m_angle, corners, m_curved ? "  curves as brushes" : "",
                           m_doubleSided ? "  double-sided" : "" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

    private:
        void SetAngle( int deg )
        {
            m_angle = s_sweepAngle = ( deg % 360 + 360 ) % 360;
        }

        void FaceWorld( const float uv[2], float out[3] ) const
        {
            KiwiCon_PlaneToWorld( m_face.plane, uv, out );
        }

        // The face's loops: its outer loop, then its holes.
        void FaceLoops( std::vector<const std::vector<float> *> *out ) const
        {
            out->clear();
            out->push_back( &KiwiRegion_OuterLoop( m_face ) );
            for ( size_t h = 0; h < m_face.holes.size(); ++h )
                out->push_back( &m_face.holes[h] );
        }

        // Latch the face and its anchor candidates: every corner of its loops, then its area
        // centroid.
        void SetFace( const kregion_t &face )
        {
            m_face = face;
            m_haveFace = true;
            m_cands.clear();
            m_anchor = m_drawnCand = -1;
            m_anchorSet = false;
            std::vector<const std::vector<float> *> loops;
            FaceLoops( &loops );
            const float minTurn = KSWEEP_CORNER_DEG * KPIPE_PI / 180.0f;
            double area = 0.0, cx = 0.0, cy = 0.0;
            for ( size_t l = 0; l < loops.size(); ++l )
            {
                const std::vector<float> &L = *loops[l];
                const int n = (int)( L.size() / 2 );
                for ( int i = 0; i < n; ++i )
                {
                    const float *p0 = &L[(size_t)( ( i + n - 1 ) % n ) * 2];
                    const float *p1 = &L[(size_t)i * 2];
                    const float *p2 = &L[(size_t)( ( i + 1 ) % n ) * 2];
                    // Shoelace centroid; hole loops run clockwise, so they subtract.
                    const double cr = (double)p1[0] * p2[1] - (double)p2[0] * p1[1];
                    area += cr;
                    cx += ( p1[0] + p2[0] ) * cr;
                    cy += ( p1[1] + p2[1] ) * cr;
                    const float a0 = atan2f( p1[1] - p0[1], p1[0] - p0[0] );
                    const float a1 = atan2f( p2[1] - p1[1], p2[0] - p1[0] );
                    float turn = a1 - a0;
                    while ( turn >  KPIPE_PI ) turn -= 2.0f * KPIPE_PI;
                    while ( turn < -KPIPE_PI ) turn += 2.0f * KPIPE_PI;
                    if ( fabsf( turn ) < minTurn || (int)( m_cands.size() / 3 ) >= KSWEEP_MAX_CANDIDATES )
                        continue;
                    float w[3];
                    FaceWorld( p1, w );
                    m_cands.insert( m_cands.end(), w, w + 3 );
                }
            }
            m_centroidCand = -1;
            if ( fabs( area ) > 1.0e-9 )
            {
                const float c[2] = { (float)( cx / ( 3.0 * area ) ), (float)( cy / ( 3.0 * area ) ) };
                float w[3];
                FaceWorld( c, w );
                m_centroidCand = (int)( m_cands.size() / 3 );
                m_cands.insert( m_cands.end(), w, w + 3 );
            }
        }

        // The nearest distance from `p` to the face's corners and centroid.
        float DistToFace( const float p[3] ) const
        {
            float best = 1.0e30f;
            for ( size_t i = 0; i + 2 < m_cands.size(); i += 3 )
            {
                const float d = Dist3( p, &m_cands[i] );
                if ( d < best )
                    best = d;
            }
            return best;
        }

        // The default anchor: where the path starts, when that lies on the face's plane inside
        // its bounds (the sweep stays exactly as drawn); else the centroid.
        void DefaultAnchor()
        {
            m_anchor = m_centroidCand >= 0 ? m_centroidCand : 0;
            if ( m_spines.empty() || m_cands.empty() )
                return;
            const float *start = Pt( m_spines[0], 0 );
            float rel[3];
            Sub3( start, m_face.plane.origin, rel );
            if ( fabsf( Dot3( rel, m_face.plane.normal ) ) > KiwiRegion_PlaneBand() )
                return;
            float uv[2];
            KiwiCon_WorldToPlane( m_face.plane, start, uv );
            const std::vector<float> &outer = KiwiRegion_OuterLoop( m_face );
            float lo[2] = { 1.0e30f, 1.0e30f }, hi[2] = { -1.0e30f, -1.0e30f };
            for ( size_t i = 0; i + 1 < outer.size(); i += 2 )
                for ( int k = 0; k < 2; ++k )
                {
                    if ( outer[i + k] < lo[k] ) lo[k] = outer[i + k];
                    if ( outer[i + k] > hi[k] ) hi[k] = outer[i + k];
                }
            const float tol = KiwiRegion_WeldDist();
            if ( uv[0] < lo[0] - tol || uv[0] > hi[0] + tol || uv[1] < lo[1] - tol || uv[1] > hi[1] + tol )
                return;
            // A corner it sits on is that corner; anywhere else it becomes a candidate of its own.
            float w[3];
            FaceWorld( uv, w );
            for ( int i = 0; i < (int)( m_cands.size() / 3 ); ++i )
                if ( Dist3( w, &m_cands[(size_t)i * 3] ) <= tol )
                {
                    m_anchor = i;
                    return;
                }
            if ( m_drawnCand < 0 )
            {
                m_drawnCand = (int)( m_cands.size() / 3 );
                m_cands.insert( m_cands.end(), w, w + 3 );
            }
            m_anchor = m_drawnCand;
        }

        // Spines for the picked paths, each running away from the face; then the sections.
        void Rebuild()
        {
            m_spines.clear();
            if ( m_phase != SWEEP_READY )
                return;
            for ( size_t i = 0; i < m_paths.size(); ++i )
            {
                const kconObject_t *o = KiwiCon_At( m_paths[i] );
                pipeSpine_t s;
                if ( !Pipeable( o ) || !SpineFor( *o, &s ) )
                    continue;
                // The sweep starts at the end nearer the face.
                const int n = PointCount( s );
                if ( !s.closed && n >= 2 && DistToFace( Pt( s, n - 1 ) ) < DistToFace( Pt( s, 0 ) ) )
                {
                    std::vector<float> rev;
                    for ( int k = n - 1; k >= 0; --k )
                        rev.insert( rev.end(), Pt( s, k ), Pt( s, k ) + 3 );
                    s.pts.swap( rev );
                }
                m_spines.push_back( s );
            }
            if ( !m_anchorSet )
                DefaultAnchor();
            PlaceAll();
        }

        // The first frame the builders lay this spine's sections in.
        bool StartFrame( const pipeSpine_t &s, pipeFrame_t *f0 ) const
        {
            if ( s.curved && CurvedAsPatches() )
            {
                if ( SpineLength( s ) < 1.0f )
                    return false;
                std::vector<float> pts;
                std::vector<pipeFrame_t> frames;
                Resample( s, CurvedSpans( s ) * 2 + 1, &pts );
                CurvedFrames( pts, s.closed, s.haveHint ? s.hint : nullptr, &frames );
                if ( frames.empty() )
                    return false;
                *f0 = frames[0];
                return true;
            }
            std::vector<pipePiece_t> pieces;
            PlanStraight( s, &pieces, nullptr, s.curved );
            for ( size_t i = 0; i < pieces.size(); ++i )
                if ( pieces[i].valid && !pieces[i].elbow )
                {
                    *f0 = pieces[i].f;
                    return true;
                }
            return false;
        }

        // Place the face at every spine's start: turned by the least rotation that takes its
        // normal (either side) onto the start tangent, then squared up about the tangent, about
        // the anchor, which lands on the path.
        // Section coordinates are along the first frame's u / v, the outer loop counter-clockwise.
        void PlaceAll()
        {
            if ( !m_haveFace || m_anchor < 0 || m_anchor >= (int)( m_cands.size() / 3 ) )
                return;
            const float *anchor = &m_cands[(size_t)m_anchor * 3];
            std::vector<const std::vector<float> *> faceLoops;
            FaceLoops( &faceLoops );
            m_radius = KPIPE_MIN_RADIUS;
            for ( size_t si = 0; si < m_spines.size(); ++si )
            {
                pipeSpine_t &s = m_spines[si];
                s.loops.clear();
                s.pieces.clear();
                pipeFrame_t f0;
                if ( !StartFrame( s, &f0 ) )
                    continue;
                float nrm[3];
                Copy3( m_face.plane.normal, nrm );
                if ( Dot3( nrm, f0.t ) < 0.0f )
                    for ( int k = 0; k < 3; ++k )
                        nrm[k] = -nrm[k];
                float axis[3];
                Cross3( nrm, f0.t, axis );
                const float sn = Len3( axis ), cs = Dot3( nrm, f0.t );
                const bool turn = sn > 1.0e-6f && Norm3( axis );
                auto turned = [&]( float q[3] )
                {
                    if ( !turn )               // Rodrigues
                        return;
                    float kv[3];
                    Cross3( axis, q, kv );
                    const float kd = Dot3( axis, q );
                    for ( int k = 0; k < 3; ++k )
                        q[k] = q[k] * cs + kv[k] * sn + axis[k] * kd * ( 1.0f - cs );
                };
                // KIWI (2026-11-01, user: "make the sweep tool take the normal of the construction
                // face into account with the starting position so setting the angle isn't so
                // hard").  The least turn leaves the face's sides slanted across the path by the
                // path's heading (a flat face on a diagonal path stood up at 45 degrees).  A
                // further twist about the tangent, at most 45 degrees, squares the face's own axes
                // (the world axis lying most in its plane, and normal x that) with the frame's
                // u / v, so the profile stands upright and Angle turns it in quarter turns.
                float side[3] = { 0.0f, 0.0f, 0.0f };
                int flat = 0;
                for ( int k = 1; k < 3; ++k )
                    if ( fabsf( nrm[k] ) < fabsf( nrm[flat] ) )
                        flat = k;
                side[flat] = 1.0f;
                Mad3( side, nrm, -nrm[flat], side );
                float twistC = 1.0f, twistS = 0.0f;
                if ( Norm3( side ) )
                {
                    turned( side );
                    const float quarter = 0.5f * KPIPE_PI;
                    const float phi = atan2f( Dot3( side, f0.v ), Dot3( side, f0.u ) );
                    const float delta = floorf( phi / quarter + 0.5f ) * quarter - phi;
                    if ( fabsf( delta ) > 1.0e-4f )        // float noise on an already square face
                        twistC = cosf( delta ), twistS = sinf( delta );
                }
                auto toSection = [&]( const float uv[2], float *c, float *sc )
                {
                    float w[3], q[3];
                    FaceWorld( uv, w );
                    Sub3( w, anchor, q );
                    turned( q );
                    const float qc = Dot3( q, f0.u ), qs = Dot3( q, f0.v );
                    *c  = qc * twistC - qs * twistS;
                    *sc = qc * twistS + qs * twistC;
                };
                auto convert = [&]( const std::vector<float> &uvLoop, std::vector<float> *out )
                {
                    out->clear();
                    for ( size_t i = 0; i + 1 < uvLoop.size(); i += 2 )
                    {
                        float c, sc;
                        toSection( &uvLoop[i], &c, &sc );
                        out->push_back( c );
                        out->push_back( sc );
                    }
                };
                for ( size_t l = 0; l < faceLoops.size(); ++l )
                {
                    s.loops.push_back( std::vector<float>() );
                    convert( *faceLoops[l], &s.loops.back() );
                }
                std::vector<float> whole;            // the bridged loop: one ring for the pieces
                convert( m_face.pts, &whole );
                // A face turned over (its normal against the tangent) runs clockwise: reverse.
                if ( !s.loops.empty() && KiwiRegion_SignedArea( s.loops[0] ) < 0.0f )
                {
                    auto reverse = []( std::vector<float> &L )
                    {
                        const size_t n = L.size() / 2;
                        for ( size_t i = 0; i < n / 2; ++i )
                        {
                            std::swap( L[i * 2],     L[( n - 1 - i ) * 2] );
                            std::swap( L[i * 2 + 1], L[( n - 1 - i ) * 2 + 1] );
                        }
                    };
                    for ( size_t l = 0; l < s.loops.size(); ++l )
                        reverse( s.loops[l] );
                    reverse( whole );
                }
                for ( size_t i = 0; !s.loops.empty() && i + 1 < s.loops[0].size(); i += 2 )
                {
                    const float r = sqrtf( s.loops[0][i] * s.loops[0][i] + s.loops[0][i + 1] * s.loops[0][i + 1] );
                    if ( r > m_radius )
                        m_radius = r;
                }
                ConvexSectionPieces( whole, &s.pieces );
            }
        }

        // The brush prisms' sections: the face's convex pieces, each cut down to the 64 points a
        // CoD4 brush face holds (as kiwi_extrude.cpp's BuildRegionDefs does).
        static void ConvexSectionPieces( const std::vector<float> &whole,
                                         std::vector< std::vector<float> > *out )
        {
            out->clear();
            const int n = (int)( whole.size() / 2 );
            if ( n < 3 )
                return;
            std::vector< std::vector<int> > idx;
            if ( KiwiRegion_IsConvex( whole ) )
            {
                idx.push_back( std::vector<int>() );
                for ( int i = 0; i < n; ++i )
                    idx.back().push_back( i );
            }
            else if ( !KiwiRegion_ConvexPieces( whole, &idx ) )
                return;
            for ( size_t p = 0; p < idx.size(); ++p )
            {
                if ( (int)idx[p].size() <= KEXT_MAX_FACE_POINTS )
                    continue;
                std::vector<int> rest;
                rest.push_back( idx[p][0] );
                rest.insert( rest.end(), idx[p].begin() + ( KEXT_MAX_FACE_POINTS - 1 ), idx[p].end() );
                idx[p].resize( KEXT_MAX_FACE_POINTS );
                idx.push_back( rest );            // checked again further down this loop
            }
            for ( size_t p = 0; p < idx.size(); ++p )
            {
                out->push_back( std::vector<float>() );
                for ( size_t i = 0; i < idx[p].size(); ++i )
                {
                    out->back().push_back( whole[(size_t)idx[p][i] * 2] );
                    out->back().push_back( whole[(size_t)idx[p][i] * 2 + 1] );
                }
            }
        }

        // The pipeable construction object nearest the click, within the click radius.
        int PickPathAt( int imgX, int imgY ) const
        {
            int best = -1;
            float bestDist = KSWEEP_PICK_PIXELS;
            for ( int i = 0; i < KiwiCon_Count(); ++i )
            {
                const kconObject_t *o = KiwiCon_At( i );
                if ( !Pipeable( o ) )
                    continue;
                const int segs = KiwiCon_SegmentCount( *o );
                for ( int sg = 0; sg < segs; ++sg )
                {
                    float wa[3], wb[3], ax, ay, bx, by;
                    if ( !KiwiCon_SegmentWorld( *o, sg, wa, wb ) )
                        break;
                    if ( !Pick_WorldToImage( wa, &ax, &ay ) || !Pick_WorldToImage( wb, &bx, &by ) )
                        continue;
                    const float d = Pick_SegDist2D( (float)imgX, (float)imgY, ax, ay, bx, by, 0 );
                    if ( d < bestDist )
                    {
                        bestDist = d;
                        best = i;
                    }
                }
            }
            return best;
        }

        // The anchor candidate nearest the click, within the click radius.
        int PickCandidateAt( int imgX, int imgY ) const
        {
            int best = -1;
            float bestDist = KSWEEP_PICK_PIXELS;
            for ( int i = 0; i < (int)( m_cands.size() / 3 ); ++i )
            {
                float px, py;
                if ( !Pick_WorldToImage( &m_cands[(size_t)i * 3], &px, &py ) )
                    continue;
                const float d = sqrtf( ( px - (float)imgX ) * ( px - (float)imgX )
                                     + ( py - (float)imgY ) * ( py - (float)imgY ) );
                if ( d < bestDist )
                {
                    bestDist = d;
                    best = i;
                }
            }
            return best;
        }

        int                m_phase = SWEEP_READY;
        std::vector<int>   m_paths;            // construction objects to sweep along
        kregion_t          m_face;             // the face, latched (region indices move)
        bool               m_haveFace = false;
        std::vector<float> m_cands;            // anchor candidates, world xyz
        int                m_anchor = -1;      // the chosen candidate
        int                m_centroidCand = -1;
        int                m_drawnCand = -1;   // the path's start, when it is on the face
        bool               m_anchorSet = false; // picked by the user (else the default)
        int                m_curved = 0;
    };

    KiwiSweepCommand s_sweep;
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

// A face or a path is enough: the command asks for a click on the other.
bool KiwiSweep_CanExecute()
{
    return KiwiRegion_SelectedCount() > 0 || KiwiPipe_CanExecute();
}

void KiwiPipe_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiPipe", 0, 0, KIWI_CMD_CONSTRUCT_PIPE );
    Radiant_RegisterCommand( "KiwiSweep", 0, 0, KIWI_CMD_CONSTRUCT_SWEEP );   // KIWI: Shift+P (modern)
}

KiwiEditorCommand *KiwiPipe_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_CONSTRUCT_SWEEP )
        return &s_sweep;
    return ( commandId == KIWI_CMD_CONSTRUCT_PIPE ) ? &s_pipe : 0;
}
