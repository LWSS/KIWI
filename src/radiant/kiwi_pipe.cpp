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
            KiwiCmd_UndoBegin( "pipe" );
            for ( size_t i = 0; i < defs.size(); ++i )
                KiwiExtrude_LandDef( defs[i] );
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
                // Runs of frames: a curved spine is one run; a straight one a run per segment
                // (start, end - mitered ends ring on their cut plane) and per elbow.
                std::vector<std::vector<pipeFrame_t> > runs;
                PreviewRuns( m_spines[i], &runs );
                for ( size_t r = 0; r < runs.size(); ++r )
                {
                    const std::vector<pipeFrame_t> &frames = runs[r];
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
                        if ( f + 1 < frames.size() )
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
            if ( s.curved )
            {
                std::vector<float> pts;
                runs->push_back( std::vector<pipeFrame_t>() );
                Resample( s, CurvedSpans( s ) + 1, &pts );
                CurvedFrames( pts, s.closed, s.haveHint ? s.hint : nullptr, &runs->back() );
                return;
            }
            std::vector<pipePiece_t> pieces;
            PlanStraight( s, &pieces, nullptr );
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
        void PlanStraight( const pipeSpine_t &s, std::vector<pipePiece_t> *out, int *notes ) const
        {
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
                if ( m_corner == KPIPE_CORNER_BUTT || ( !s.closed && ( v == 0 || v == n - 1 ) ) )
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
                int kind = m_corner;
                // Miters spike on sharp corners: past the limit they bend.
                if ( kind == KPIPE_CORNER_MITER && theta * 180.0f / KPIPE_PI > (float)m_miterMax )
                    kind = KPIPE_CORNER_BEND;
                // Elbows are curve patches, which hold KPIPE_MAX_PATCH_SIDES sides at most.
                if ( kind == KPIPE_CORNER_BEND && m_sides > KPIPE_MAX_PATCH_SIDES )
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
            PlanStraight( s, &pieces, &notes );
            if ( notes & 1 )
                Sys_Printf( "Pipe: bends are curve patches (at most %i sides) - the corners of this "
                            "%i-sided pipe are mitered instead.\n", KPIPE_MAX_PATCH_SIDES, m_sides );
            if ( notes & 2 )
                Sys_Printf( "Pipe: some bend radii were reduced to fit short segments.\n" );
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
                    BuildSweep( pc.pts, pc.frames, pc.spans, m_sides, defs, patches, rejected );
                    continue;
                }
                const pipeFrame_t &f = pc.f;
                const float len = pc.len;
                if ( len < 0.05f )
                {
                    ++*rejected;
                    continue;
                }

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
                    if ( !def )
                    {
                        ++*rejected;
                        continue;
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
            BuildSweep( pts, frames, spans, sides, defs, patches, rejected );
        }

        // Curve patches swept through 2 * spans + 1 stations (even = on the spine, odd = the
        // span midpoints the quadratic passes through) - a curved spine, or one elbow.
        void BuildSweep( const std::vector<float> &pts, const std::vector<pipeFrame_t> &frames,
                         int spans, int sides, std::vector<brush_t *> *defs,
                         int *patches, int *rejected )
        {
            if ( sides > KPIPE_MAX_PATCH_SIDES )
                sides = KPIPE_MAX_PATCH_SIDES;
            if ( spans < 1 || (int)frames.size() < spans * 2 + 1 )
            {
                ++*rejected;
                return;
            }
            const int cols = ( sides < 3 ) ? ( m_density + 3 ) * 2 + 1 : sides * 2 + 1;

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
