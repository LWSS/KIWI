#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Live chamfers use KiwiBevel_AppendFace and KiwiValid_Rebuild; committed patches
// follow the ported allocation, material, and linking order.

#include "stdafx.h"
#include "qe3.h"
#include "winding.h"

#include "kiwi_patchfillet.h"
#include "kiwi_bevel.h"                 // chamfer frame and appender
#include "kiwi_camera.h"                // KCAM_RAYAXIS_MIN_DEN
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_lines.h"
#include "kiwi_material.h"              // patch material realization
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // shared vector helpers

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points.
extern int          Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118
extern int          g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)

extern size_t Brush_RemoveFace( brush_t *b, unsigned int faceIndex );   // brush.cpp:345  0x471640
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );         // brush.cpp:669  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                        // brush.cpp:927  0x4765A0
extern void         Select_Deselect( int a1 );                                // select.cpp:1444 0x48E800 (int, NOT char — mangling)

// Patch allocation and linking mirror Patch_BrushToMesh; FinishNew* forwards
// through its file-static material and tessellation workers.
extern patchMesh_t *MakeNewPatch();                                           // pmesh.cpp:136  0x437AC0
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );  // pmesh.cpp:840  0x4386A0
extern void         Patch_KiwiFinishNew( patchMesh_t *p );                    // pmesh.cpp:1558
// Uses the parent face's texture scale instead of the editor default.
extern void         Patch_KiwiFinishNewLike( patchMesh_t *p, const texdef_sub_t *srcTex );
// Per-patch 0x4397B0 forwarder keeps Lmap alignment inside this undo bracket.
extern void         Patch_KiwiLmapAlign( patchMesh_t *p );                    // pmesh.cpp
extern void         Patch_KiwiCapAlign( patchMesh_t *p );                     // pmesh.cpp
// Ported bounds, brush rebuild, curveDef, and version bookkeeping.
extern void         Patch_Rebuild( patchMesh_t *p, char doBounds );           // pmesh.cpp:2137

// Edge selections are absent from selected_brushes, so the undo head misses them.
extern void         Undo_AddBrush( entity_brush_s *pBrushInst );              // undo.cpp:494  0x45E680

// Must remain at file scope for MSVC linkage.
extern bool         Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                             int commandId );                 // mainfrm.cpp:1340

namespace
{
    const float KPF_EPS       = 1.0e-4f;
    const float KPF_MATCH_TOL = 0.1f;      // the ported FindPoint dedup tolerance
    const float KPF_PI        = 3.14159265358979f;

    // Bind the span limit to the patch format's column limit.
    static_assert( KPF_MAX_SPANS * 2 + 1 == KPATCH_MAX_WIDTH,
                   "KPF_MAX_SPANS must be (KPATCH_MAX_WIDTH - 1) / 2" );

    const float KPF_COL_OK [3] = { 0.55f, 0.95f, 0.80f };   // the arc preview
    const float KPF_COL_BAD[3] = { 1.00f, 0.30f, 0.25f };
    const float KPF_COL_EDGE[3]= { 0.45f, 0.62f, 0.72f };   // the edges being filleted

    inline bool PointNear( const float *a, const float *b, float tol )
    {
        float d[3];
        Sub3( a, b, d );
        return fabsf( d[0] ) <= tol && fabsf( d[1] ) <= tol && fabsf( d[2] ) <= tol;
    }


    winding_t *WindingOf( const selbrush_t *b, int faceIndex )
    {
        if ( !b || !b->def || !b->def->faces )
            return 0;
        if ( faceIndex < 0 || faceIndex >= b->def->faceCount )
            return 0;
        return b->def->faces[faceIndex].w;
    }

    bool EdgeEnds( const sel_item_t &it, float *a, float *b )
    {
        winding_t *w = WindingOf( it.brush, it.faceIndex );
        if ( !w || w->numpoints < 2 )
            return false;
        if ( it.edgeIndex < 0 || it.edgeIndex >= w->numpoints )
            return false;
        Copy3( w->p[it.edgeIndex], a );
        Copy3( w->p[( it.edgeIndex + 1 ) % w->numpoints], b );
        return true;
    }

    // Labels need static storage because the numeric layer does not copy them.
    // Fillet omits bias: circular tangency fixes the axis on the bisector;
    // tilting it would require an ellipse rather than this quadratic arc.
    const kiwiNumField_t KPF_FIELDS_CHAMFER[2] = { { "depth",  KNUM_LENGTH, false },
                                                   { "bias",   KNUM_ANGLE,  false } };
    const kiwiNumField_t KPF_FIELDS_FILLET [1] = { { "radius", KNUM_LENGTH, false } };

    // The bias field's guard band: the chamfer plane must stay strictly between
    // the two faces, so |bias| is clamped this far inside the half-angle.
    const float KPF_BIAS_GUARD_DEG = 2.0f;
    // A-key angle drag: degrees per pixel of sideways cursor travel, and the Ctrl step.
    const float KPF_ANGLE_DEG_PER_PIX = 0.25f;
    const float KPF_ANGLE_SNAP_DEG    = 5.0f;

    // One selected edge and its baseline-derived fillet frame.
    struct filletUnit_t
    {
        kiwiBevelEdge_t e;          // node/def/e0/e1 + the bisector frame (kiwi_bevel.h)

        // Derived once, at Begin, from the BASELINE geometry.
        float n1[3], n2[3];         // the two adjacent OUTWARD normals
        float k;                    // n·n1 = cos(phi/2) = sin(theta/2)
        float phi;                  // angle between the outward normals, radians
        float basisA[3];            // = n1
        float basisB[3];            // = normalise(n2 - (n1·n2)·n1)
        int   spans;                // bezier spans across the arc (width = 2*spans+1)
        bool  rowFlip;              // run the patch's rows e1→e0 so it faces outward

        int   faceIndex;            // the appended chamfer face (-1 = absent)

        // Make positive bias point toward adj[1] despite frame discovery order.
        // Resolve the sign once from the adjacent normals.
        float biasSign;
        float biasLimit;            // radians; |bias| must stay under this
    };

    // Cross-section point at the edge midpoint:
    // even columns lie on radius r; odd tangent handles use r/cos(alpha/2).
    void ArcPoint( const filletUnit_t &u, float r, int col, float *out )
    {
        const float alpha = u.phi / (float)u.spans;
        const float psi   = 0.5f * alpha * (float)col;      // col 2i → i·alpha
        const float rad   = ( col & 1 ) ? ( r / cosf( alpha * 0.5f ) ) : r;

        float A[3];
        Mad3( u.e.mid, u.e.n, -( r / u.k ), A );            // the axis at the midpoint

        const float ca = cosf( psi );
        const float sa = sinf( psi );
        for ( int c = 0; c < 3; ++c )
            out[c] = A[c] + rad * ( ca * u.basisA[c] + sa * u.basisB[c] );
    }

    // The chamfer depth this radius asks kiwi_bevel for:  d = r·(1 - k²)/k.
    inline float ChamferDepth( const filletUnit_t &u, float r )
    {
        return r * ( 1.0f - u.k * u.k ) / u.k;
    }

    // Modal chamfer / patch-fillet command.
    class KiwiPatchFilletCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override
        { return m_curve ? "Fillet Edge (patch)" : "Bevel Edge (chamfer)"; }
        bool CanExecute() override { return KiwiPatchFillet_CanFillet(); }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        // Never snap or pick the geometry the gesture is reshaping (kiwi_pick.h).
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            // Parked entry makes the ball the first actionable prompt.
            static const kiwiPrompt_t s_chamfer[] = {
                { "Ball", "Drag it — chamfer depth" },
                { "D",    "Curve it (patch fillet)" },
                { "A",    "Drag the angle (click ends)" },
                { "Tab",  "Type the angle" },
            };
            static const kiwiPrompt_t s_fillet[] = {
                { "Ball", "Drag it — fillet radius" },
                { "D",    "Back to flat chamfer" },
            };
            if ( m_curve )
            {
                *out = s_fillet;
                return (int)( sizeof( s_fillet ) / sizeof( s_fillet[0] ) );
            }
            *out = s_chamfer;
            return (int)( sizeof( s_chamfer ) / sizeof( s_chamfer[0] ) );
        }

        int NumericFields( const kiwiNumField_t **out ) const override
        {
            if ( m_curve ) { *out = KPF_FIELDS_FILLET;  return 1; }
            *out = KPF_FIELDS_CHAMFER;
            return 2;
        }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 0 )
            {
                *out = m_curve ? m_radius : m_depth;
                return true;
            }
            if ( field == 1 && !m_curve )
            {
                *out = m_biasDeg;
                return true;
            }
            return false;
        }

        // TWO fields in chamfer mode, so the per-field hook is the one that runs;
        // the single-field NumericChanged below stays for the fillet's radius.
        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( field == 1 && !m_curve )
            {
                m_hasBias = has;
                // Angles are not lengths: undo the numeric layer's unconditional
                // display-to-world conversion to recover the typed degrees.
                m_biasDeg = has ? Units_ToDisplay( world ) : 0.0f;
                Recompute();
                g_nUpdateBits |= 1;
                return;
            }
            KiwiEditorCommand::NumericFieldChanged( field, has, world );
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        // D toggles modes without moving the solid by converting d = r*(1-k^2)/k.
        // The typed value is cleared because its meaning changes between depth and
        // radius; active-command key handling owns unmodified D during the gesture.
        bool KeyDown( int vk, unsigned mods ) override
        {
            if ( vk == 0x41 && !mods )              // 'A', no modifiers
                return ToggleAngleDrag();
            if ( vk != 0x44 || mods )               // 'D', no modifiers
                return false;
            if ( m_units.empty() )
                return true;
            m_angleDrag = false;                    // D re-latches the depth; one owner at a time

            const filletUnit_t &drv = m_units[0];
            if ( !m_curve )
            {
                // Invert d = r*(1-k^2)/k. Bias is dropped because a tilted
                // chamfer has no tangent circular fillet.
                const float denom = 1.0f - drv.k * drv.k;
                m_radius = ( denom > KPF_EPS ) ? ( m_depth * drv.k / denom ) : m_depth;
                if ( m_hasBias || fabsf( m_biasDeg ) > 1.0e-3f )
                    Sys_Printf( "Fillet: the angle bias is dropped — a fillet arc has "
                                "to be tangent to BOTH faces, which fixes it on the "
                                "bisector.  Press D again for a biased chamfer.\n" );
                m_hasBias = false;
                m_biasDeg = 0.0f;
                m_curve   = true;
            }
            else
            {
                // fillet -> chamfer: the depth the fillet was already cutting.
                m_depth = ChamferDepth( drv, m_radius );
                m_curve = false;
            }
            // Reinstall the mode-specific table and clear the old scalar/focus.
            // KiwiNum_Reset would replace it with the default unnamed field.
            m_hasNum = false;
            {
                const kiwiNumField_t *f = 0;
                const int nf = NumericFields( &f );
                KiwiNum_SetFields( f, nf );
            }

            // Re-latch so this cursor position yields the converted scalar;
            // otherwise the next frame would overwrite it and jump the solid.
            ReLatchFor( m_curve ? m_radius : m_depth );

            Recompute();
            Sys_Printf( "%s: %s.\n", Name(),
                        m_curve ? "curved — a bezier patch lands in the notch"
                                : "flat chamfer — no patch" );
            g_nUpdateBits = -1;
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        // Begin parks at zero; m_grabbed prevents an arbitrary resume click from
        // driving geometry. The shared lollipop arm re-latches before its first
        // frame, and release pauses the command again.
        void HandleGrab( bool held ) override
        {
            if ( held )
                m_angleDrag = false;        // the ball is depth: taking it ends the angle drag
            if ( m_grabbed == held )
                return;
            m_grabbed = held;
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        bool Begin() override
        {
            Reset();
            if ( !Gather() )
                return false;
            LatchStart();
            UpdateHud();
            // Start parked at zero deformation; chamfer is always the initial mode.
            KiwiCmd_Pause();
            Sys_Printf( "Bevel Edge: %i edge(s), parked at 0.  GRAB THE BALL on the "
                        "stem and drag to cut the chamfer, or just type a depth.  "
                        "D curves it into a patch fillet, A drags the chamfer angle with "
                        "the mouse (Tab types it), "
                        "RMB / Enter confirms, Esc cancels.\n", (int)m_units.size() );
            return true;
        }

        void Commit() override
        {
            if ( m_invalid || m_depth < KPF_MIN_RADIUS )
            {
                // Invalid or negligible geometry follows the cancel path.
                RemoveAll();
                RebuildAll();
                KiwiCmd_UndoCancel();
                m_undoOpen = false;
                Sys_Printf( "%s: cancelled — %s.\n", Name(),
                            m_why ? m_why : ( m_depth < KPF_MIN_RADIUS
                                              ? "depth too small" : "invalid geometry" ) );
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            if ( !m_curve )
            {
                // Apply already maintains the live chamfer, so committing flat mode
                // deliberately adds nothing.
                Sys_Printf( "Bevelled %i edge(s) at %g%s.\n", (int)m_units.size(),
                            (double)m_depth,
                            ( fabsf( m_biasDeg ) > 1.0e-3f ) ? " (biased)" : "" );
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            const int made = LandPatches();
            // A fillet lands an arc plus two end caps; report every reason the
            // resulting patch count can be lower than three per edge.
            char skips[160];
            skips[0] = '\0';
            if ( m_skips.deadBrush || m_skips.capRange || m_skips.alloc )
                _snprintf( skips, sizeof( skips ),
                           "  Skipped: %i edge(s) whose solid went away, %i cap(s) "
                           "outside the %i..%i-column patch format, %i patch(es) that "
                           "could not be allocated.",
                           m_skips.deadBrush, m_skips.capRange,
                           KPATCH_MIN_WIDTH, KPATCH_MAX_WIDTH, m_skips.alloc );
            skips[sizeof( skips ) - 1] = '\0';
            Sys_Printf( "Filleted %i edge(s) — %i patch(es) at radius %g "
                        "(arc + sealed ends).%s\n",
                        (int)m_units.size(), made, (double)m_radius, skips );
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            RemoveAll();
            RebuildAll();
            Reset();
            g_nUpdateBits = -1;
        }

        // The handle anchor rides the driving chamfer face. Its outward direction
        // puts the ball in open air; m_units[0] also drives the scalar mapping so
        // the handle and numeric value refer to the same corner.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_units.empty() || !outAnchor || !outDir )
                return false;
            float nb[3];
            KiwiBevel_BiasedNormal( m_units[0].e, nb );
            for ( int k = 0; k < 3; ++k )
            {
                outAnchor[k] = m_units[0].e.mid[k] - nb[k] * m_depth;
                // Only lollipop geometry uses outward +n; depth mapping remains
                // inward-positive along -n.
                outDir[k]    = nb[k];
            }
            return true;
        }

        // Re-latch on grab so cursor movement while parked cannot jump the chamfer.
        void Rebase() override
        {
            if ( m_units.empty() )
                return;
            ReLatchFor( m_curve ? m_radius : m_depth );
            LatchAngle();
        }

        // A click ends the angle drag where it stands (the depth ball stays parked).
        // The lollipop grab is routed before this, so a press ON the ball never gets here.
        bool PressIntercept( int imgX, int imgY ) override
        {
            (void)imgX; (void)imgY;
            if ( !m_angleDrag )
                return false;
            m_angleDrag = false;
            KiwiCmd_Pause();
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;
        }

        void DrawWorld() override
        {
            if ( m_units.empty() )
                return;

            // Show which original edges the gesture owns.
            KiwiLines_Color( KPF_COL_EDGE[0], KPF_COL_EDGE[1], KPF_COL_EDGE[2] );
            for ( size_t i = 0; i < m_units.size(); ++i )
                if ( !KiwiLines_Add( m_units[i].e.e0, m_units[i].e.e1 ) )
                    return;

            if ( m_depth < KPF_MIN_RADIUS )
                return;

            // The live brush already shows the chamfer; add only a bisector stub
            // for depth and bias.
            if ( !m_curve )
            {
                const float *ccol = m_invalid ? KPF_COL_BAD : KPF_COL_OK;
                KiwiLines_Color( ccol[0], ccol[1], ccol[2] );
                for ( size_t i = 0; i < m_units.size(); ++i )
                {
                    float nb[3], tip[3];
                    KiwiBevel_BiasedNormal( m_units[i].e, nb );
                    Mad3( m_units[i].e.mid, nb, -m_depth, tip );
                    if ( !KiwiLines_Add( m_units[i].e.mid, tip ) )
                        return;
                }
                return;
            }

            // Evaluate the same Bezier control points that commit will write.
            const float *col = m_invalid ? KPF_COL_BAD : KPF_COL_OK;
            KiwiLines_Color( col[0], col[1], col[2] );
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                const filletUnit_t &u = m_units[i];
                float lo[3], hi[3];
                RowEnds( u, lo, hi );

                // Draw both patch ends so the wireframe reads as a cylinder.
                for ( int end = 0; end < 2; ++end )
                {
                    const float *base = end ? hi : lo;
                    float off[3];
                    Sub3( base, u.e.mid, off );

                    float prev[3] = { 0.0f, 0.0f, 0.0f };
                    const int steps = u.spans * KPF_PREVIEW_PER_SPAN;
                    for ( int s = 0; s <= steps; ++s )
                    {
                        float p[3];
                        BezierAt( u, m_radius, (float)s / (float)steps, p );
                        for ( int c = 0; c < 3; ++c )
                            p[c] += off[c];
                        if ( s > 0 && !KiwiLines_Add( prev, p ) )
                            return;
                        Copy3( p, prev );
                    }
                }
                // …plus the two rails, so the surface reads as a surface.
                float a[3], b[3];
                for ( int s = 0; s <= u.spans; ++s )
                {
                    float p[3];
                    ArcPoint( u, m_radius, s * 2, p );
                    float offLo[3], offHi[3];
                    Sub3( lo, u.e.mid, offLo );
                    Sub3( hi, u.e.mid, offHi );
                    for ( int c = 0; c < 3; ++c ) { a[c] = p[c] + offLo[c]; b[c] = p[c] + offHi[c]; }
                    if ( !KiwiLines_Add( a, b ) )
                        return;
                }
            }
        }

    private:
        enum { KPF_PREVIEW_PER_SPAN = 6 };   // polyline steps per bezier span

        // True only before the first grabbed or typed deformation.
        bool Parked() const
        {
            return !m_grabbed && !m_hasNum && m_depth < KPF_MIN_RADIUS;
        }

        void Reset()
        {
            m_units.clear();
            m_radius    = 0.0f;
            m_depth     = 0.0f;
            m_curve     = false;        // chamfer is always the initial mode
            m_biasDeg   = 0.0f;
            m_hasBias   = false;
            m_angleDrag = false;
            m_angleX0   = 0;
            m_angleBase = 0.0f;
            m_start     = 0.0f;
            m_haveStart = false;
            m_hasNum    = false;
            m_grabbed   = false;        // lollipop grab gate
            m_invalid   = false;
            m_added     = false;
            m_why       = 0;
            m_undoOpen  = false;
            m_hud[0]    = '\0';
        }

        bool AllLive() const
        {
            for ( size_t i = 0; i < m_units.size(); ++i )
                if ( !Sel_BrushLive( m_units[i].e.node ) )
                    return false;
            return true;
        }

        // Convert selected physical edges into baseline units.
        bool Gather()
        {
            const selection_t &sel = KiwiSel();
            int refusedFlat = 0, refusedSpike = 0, refusedFrame = 0;

            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                if ( it.kind != SEL_EDGE || !Sel_BrushLive( it.brush ) || it.brush->patch )
                    continue;
                float a[3], b[3];
                if ( !EdgeEnds( it, a, b ) )
                    continue;

                // One physical edge appears from both oppositely wound faces;
                // compare segments unordered at the ported 0.1-unit tolerance.
                bool dup = false;
                for ( size_t k = 0; k < m_units.size() && !dup; ++k )
                {
                    const kiwiBevelEdge_t &q = m_units[k].e;
                    if ( q.def != it.brush->def )
                        continue;
                    dup = ( PointNear( q.e0, a, KPF_MATCH_TOL ) && PointNear( q.e1, b, KPF_MATCH_TOL ) )
                       || ( PointNear( q.e0, b, KPF_MATCH_TOL ) && PointNear( q.e1, a, KPF_MATCH_TOL ) );
                }
                if ( dup )
                    continue;

                filletUnit_t u;
                memset( &u, 0, sizeof( u ) );
                u.e.node = it.brush;
                u.e.def  = it.brush->def;
                Copy3( a, u.e.e0 );
                Copy3( b, u.e.e1 );

                // The chamfer frame, from kiwi_bevel — one derivation in the editor.
                if ( !KiwiBevel_MakeFrame( &u.e ) )
                {
                    ++refusedFrame;
                    continue;
                }
                const int why = DeriveArc( u );
                if ( why == 1 ) { ++refusedFlat;  continue; }
                if ( why == 2 ) { ++refusedSpike; continue; }
                if ( why != 0 ) { ++refusedFrame; continue; }

                u.faceIndex = -1;
                if ( (int)m_units.size() >= KPF_MAX_EDGES )
                {
                    Sys_Printf( "Fillet Edge: more than %i edges selected — the rest "
                                "are ignored (each one is a whole patch).\n", KPF_MAX_EDGES );
                    break;
                }
                m_units.push_back( u );
            }

            if ( refusedFlat )
                Sys_Printf( "Fillet Edge: %i edge(s) skipped — the two faces meet at "
                            "more than %g deg, so there is no corner to round.\n",
                            refusedFlat, (double)KPF_MAX_WEDGE_DEG );
            if ( refusedSpike )
                Sys_Printf( "Fillet Edge: %i edge(s) skipped — the two faces meet at "
                            "less than %g deg; any usable radius would swallow the "
                            "brush.\n", refusedSpike, (double)KPF_MIN_WEDGE_DEG );
            if ( refusedFrame )
                Sys_Printf( "Fillet Edge: %i edge(s) skipped — not a two-face corner.\n",
                            refusedFrame );

            if ( m_units.empty() )
            {
                Sys_Printf( "Fillet Edge: no filletable edge is selected "
                            "(select brush edges with mode 2).\n" );
                return false;
            }
            return true;
        }

        // Radius-independent arc frame derived once from baseline geometry.
        // Return: 0 ok, 1 too flat, 2 too sharp, 3 degenerate basis.
        int DeriveArc( filletUnit_t &u )
        {
            const brush_t *def = u.e.def;
            Copy3( def->faces[u.e.adj[0]].plane.normal, u.n1 );
            Copy3( def->faces[u.e.adj[1]].plane.normal, u.n2 );

            float c = Dot3( u.n1, u.n2 );
            if ( c >  1.0f ) c =  1.0f;
            if ( c < -1.0f ) c = -1.0f;
            u.phi = acosf( c );                       // between the OUTWARD normals

            // Read k from the normalized bisector frame to avoid another square root.
            u.k = Dot3( u.e.n, u.n1 );
            if ( u.k < 0.0f )
                u.k = -u.k;

            // theta = pi - phi is the INTERIOR wedge; the two refusals are on it.
            const float thetaDeg = ( KPF_PI - u.phi ) * ( 180.0f / KPF_PI );
            if ( thetaDeg > KPF_MAX_WEDGE_DEG )
                return 1;
            if ( thetaDeg < KPF_MIN_WEDGE_DEG )
                return 2;
            if ( !( u.k > KPF_EPS ) )
                return 3;

            // a=n1 and b=normalize(n2-c*n1) span the edge cross-section.
            Copy3( u.n1, u.basisA );
            for ( int k = 0; k < 3; ++k )
                u.basisB[k] = u.n2[k] - c * u.n1[k];
            if ( !Norm3( u.basisB ) )
                return 3;

            // Keep every span within KPF_SPAN_DEG and the patch column limit.
            const float phiDeg = u.phi * ( 180.0f / KPF_PI );
            int spans = (int)ceilf( phiDeg / KPF_SPAN_DEG );
            if ( spans < 1 )             spans = 1;
            if ( spans > KPF_MAX_SPANS )  spans = KPF_MAX_SPANS;
            u.spans = spans;

            // Curve_ComputeNormals faces cross(dCol,dRow) (pmesh.cpp:502/609).
            // Test the middle arc tangent against the outward bisector once, then
            // reverse the patch rows when required.
            float tau[3], nrm[3];
            const float sh = sinf( u.phi * 0.5f ), ch = cosf( u.phi * 0.5f );
            for ( int k = 0; k < 3; ++k )
                tau[k] = -sh * u.basisA[k] + ch * u.basisB[k];
            Cross3( tau, u.e.u, nrm );
            u.rowFlip = ( Dot3( nrm, u.e.n ) < 0.0f );

            // Choose the frame sign so positive bias consistently points at adj[1].
            u.biasSign  = ( Dot3( u.n2, u.e.v ) >= 0.0f ) ? 1.0f : -1.0f;
            u.biasLimit = KiwiBevel_BiasLimit( u.e );
            return 0;
        }

        // Cursor-to-depth mapping.
        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        void LatchStart()
        {
            m_haveStart = false;
            ray_t ray;
            float p[3];
            const filletUnit_t &d = m_units[0];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, d.e.mid, d.e.n, p ) )
                return;
            float rel[3];
            Sub3( p, d.e.mid, rel );
            m_start     = -Dot3( rel, d.e.n );
            m_haveStart = true;
        }

        // Set m_start so the current cursor produces wanted; failed ray
        // resolution leaves the existing latch unchanged.
        void ReLatchFor( float wanted )
        {
            if ( m_units.empty() )
                return;
            ray_t ray;
            float p[3];
            const filletUnit_t &d = m_units[0];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, d.e.mid, d.e.n, p ) )
                return;
            float rel[3];
            Sub3( p, d.e.mid, rel );
            m_start     = -Dot3( rel, d.e.n ) - wanted;
            m_haveStart = true;
        }

        // KIWI (2026-09-21, user: "pressing A while doing a bevel should allow dynamic
        // angle change during the tool operation. (like in plasticity)").  A hands the
        // MOUSE to the chamfer's angle bias - the Tab field's value, driven live: sideways
        // cursor travel, KPF_ANGLE_DEG_PER_PIX per pixel, depth frozen where it stands.
        // No button is held (the command is resumed so moves arrive); a click, A again,
        // taking the depth ball, D, Enter or Esc all end it.  Ctrl steps by 5 degrees.
        bool ToggleAngleDrag()
        {
            if ( m_units.empty() )
                return true;
            if ( m_angleDrag )
            {
                m_angleDrag = false;
                KiwiCmd_Pause();
                UpdateHud();
                return true;
            }
            if ( m_curve )
            {
                Sys_Printf( "Fillet: no angle to drag - a fillet arc is tangent to BOTH "
                            "faces, which fixes it on the bisector.  Press D for a flat "
                            "chamfer, then A.\n" );
                return true;
            }
            // A typed bias would pin the value under the mouse: the drag takes it over
            // from wherever it stands.
            if ( m_hasBias )
            {
                KiwiNum_ClearField( 1 );
                m_hasBias = false;
            }
            m_angleDrag = true;
            m_grabbed   = false;
            KiwiCmd_Resume();               // parked commands get no MouseMove
            LatchAngle();                   // Resume is a no-op when already hot
            if ( m_depth < KPF_MIN_RADIUS )
                Sys_Printf( "Bevel Edge: angle drag on - but the chamfer has no depth yet, "
                            "so nothing will show until the ball is dragged or a depth "
                            "typed.\n" );
            UpdateHud();
            return true;
        }

        // The current cursor column keeps the current bias.
        void LatchAngle()
        {
            int x, y;
            if ( !m_angleDrag || !KiwiCmd_LastCursor( &x, &y ) )
                return;
            m_angleX0   = x;
            m_angleBase = m_biasDeg;
        }

        // Keep the requested value inside what the DRIVING edge can show, so dragging
        // back answers at once instead of unwinding travel spent past the clamp.
        void DragAngle()
        {
            int x, y;
            if ( !m_angleDrag || m_curve || m_hasBias || m_units.empty()
              || !KiwiCmd_LastCursor( &x, &y ) )
                return;
            float deg = m_angleBase + (float)( x - m_angleX0 ) * KPF_ANGLE_DEG_PER_PIX;
            if ( KiwiCmd_SnapEngaged() )
                deg = floorf( deg / KPF_ANGLE_SNAP_DEG + 0.5f ) * KPF_ANGLE_SNAP_DEG;
            const float lim = m_units[0].biasLimit * ( 180.0f / 3.14159265358979f )
                            - KPF_BIAS_GUARD_DEG;
            bool clamped = true;
            if ( !( lim > 0.0f ) )  deg = 0.0f;
            else if ( deg >  lim )  deg =  lim;
            else if ( deg < -lim )  deg = -lim;
            else                    clamped = false;
            if ( clamped )
            {
                m_angleX0   = x;            // this column IS the limit from now on
                m_angleBase = deg;
            }
            m_biasDeg = deg;
        }

        void Recompute()
        {
            if ( !AllLive() )
            {
                Sys_Printf( "Fillet Edge: selection changed under the gesture — "
                            "cancelled.\n" );
                KiwiCmd_Cancel();
                return;
            }
            if ( m_units.empty() )
                return;
            if ( !m_haveStart )
                LatchStart();

            // Cursor motion stays full-float and unquantized. Chamfer interprets
            // the scalar as depth; fillet interprets it as radius.
            float s = m_curve ? m_radius : m_depth;
            ray_t ray;
            float p[3];
            const filletUnit_t &drv = m_units[0];
            // Only a held lollipop drives the cursor scalar; numeric entry remains
            // active while parked because it bypasses cursor mapping.
            if ( m_grabbed
              && m_haveStart && CursorRay( &ray ) && KiwiCam_RayAxis( ray, drv.e.mid, drv.e.n, p ) )
            {
                float rel[3];
                Sub3( p, drv.e.mid, rel );
                s = -Dot3( rel, drv.e.n ) - m_start;
            }

            if ( m_hasNum )
                s = m_numWorld;
            // Geometry snapping is cursor-driven, so keep it behind the grab gate.
            else if ( m_grabbed && m_snap.valid && KiwiSnap_IsGeometry( m_snap.type ) )
            {
                // Read SNAP_FACE by intersecting its plane with the driving axis,
                // not from the cursor-dependent hit point. AxisDepth measures +axis
                // while depth is inward-positive, hence the negation. A parallel
                // plane leaves the drag-produced scalar unchanged.
                float axisT = 0.0f;
                if ( KiwiSnap_AxisDepth( m_snap, drv.e.mid, drv.e.n, &axisT ) )
                {
                    const float d = -axisT;
                    if ( m_curve )
                    {
                        const float denom = 1.0f - drv.k * drv.k;
                        s = ( denom > KPF_EPS ) ? ( d * drv.k / denom ) : d;
                    }
                    else
                    {
                        s = d;
                    }
                }
            }

            DragAngle();                    // A: the mouse owns the bias, not the depth

            if ( s < 0.0f )
                s = 0.0f;
            if ( m_curve )
            {
                m_radius = s;
                m_depth  = ChamferDepth( drv, m_radius );
            }
            else
            {
                m_depth  = s;
                const float denom = 1.0f - drv.k * drv.k;   // kept in step for the D toggle
                m_radius = ( denom > KPF_EPS ) ? ( m_depth * drv.k / denom ) : m_depth;
            }
            ApplyBias();
            Apply();
            UpdateHud();
        }

        // Clamp bias per edge because legal half-angles differ; fillet mode
        // forces zero because its circular arc has no biased form.
        void ApplyBias()
        {
            const float toRad = 3.14159265358979f / 180.0f;
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                filletUnit_t &u = m_units[i];
                if ( m_curve )
                {
                    u.e.bias = 0.0f;
                    continue;
                }
                float want = m_biasDeg * toRad * u.biasSign;
                const float lim = u.biasLimit - KPF_BIAS_GUARD_DEG * toRad;
                if ( !( lim > 0.0f ) )      { u.e.bias = 0.0f; continue; }
                if ( want >  lim ) want =  lim;
                if ( want < -lim ) want = -lim;
                u.e.bias = want;
            }
        }

        // Rebuild the live chamfer from baseline geometry each frame.
        void Apply()
        {
            if ( m_depth < KPF_MIN_RADIUS )
            {
                if ( m_added )
                {
                    RemoveAll();
                    RebuildAll();
                }
                // Zero is valid only while parked; after a grab or typed value it
                // is a rejected deformation.
                m_invalid = !Parked();
                m_why     = m_invalid ? ( m_curve ? "radius too small" : "depth too small" )
                                      : 0;
                g_nUpdateBits = -1;
                return;
            }

            OpenUndoForBrushes();              // the FIRST real mutation
            RemoveAll();                       // back to the original brush

            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                filletUnit_t &u = m_units[i];
                // Fillet depth is per edge because k differs between corners.
                const float d = m_curve ? ChamferDepth( u, m_radius ) : m_depth;
                u.faceIndex = KiwiBevel_AppendFace( u.e.def, u.e, d );
            }
            m_added = true;

            RebuildAll();
            const char *why = 0;
            bool ok = true;
            for ( size_t i = 0; i < m_units.size() && ok; ++i )
            {
                if ( DefFirstIndex( i ) != i )
                    continue;                               // one check per brush
                ok = KiwiValid_CheckBrush( m_units[i].e.def, &why );
            }

            if ( !ok )
            {
                RemoveAll();
                RebuildAll();
                m_invalid = true;
                m_why     = why;
            }
            else
            {
                m_invalid = false;
                m_why     = 0;
            }
            g_nUpdateBits = -1;
        }

        size_t DefFirstIndex( size_t i ) const
        {
            for ( size_t k = 0; k < i; ++k )
                if ( m_units[k].e.def == m_units[i].e.def )
                    return k;
            return i;
        }

        void RemoveAll()
        {
            if ( !m_added )
                return;
            // Remove appended tail faces in reverse order so later indices never shift.
            for ( size_t i = m_units.size(); i-- > 0; )
            {
                filletUnit_t &u = m_units[i];
                if ( u.faceIndex < 0 || !Sel_BrushLive( u.e.node ) || !u.e.def->faces )
                {
                    u.faceIndex = -1;
                    continue;
                }
                if ( u.faceIndex < u.e.def->faceCount )
                    Brush_RemoveFace( u.e.def, (unsigned int)u.faceIndex );
                u.faceIndex = -1;
            }
            m_added = false;
        }

        void RebuildAll()
        {
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                if ( DefFirstIndex( i ) != i || !Sel_BrushLive( m_units[i].e.node ) )
                    continue;
                KiwiValid_Rebuild( m_units[i].e.def );
            }
        }

        // Edge selections are not in selected_brushes; cover each solid before
        // the first mutation.
        void OpenUndoForBrushes()
        {
            if ( m_undoOpen )
                return;
            // The mode may change after opening, and Undo_GeneralStart stores this
            // pointer, so use one mode-neutral string literal.
            KiwiCmd_UndoBegin( "bevel / fillet edge" );
            m_undoOpen = true;
            for ( size_t i = 0; i < m_units.size(); ++i )
                if ( DefFirstIndex( i ) == i )
                    KiwiCmd_UndoCoverBrush( m_units[i].e.node );
        }

        // Project the live chamfer winding onto the edge so the patch spans the
        // actual notch; fall back to the original edge if no winding is available.
        void RowEnds( const filletUnit_t &u, float *lo, float *hi ) const
        {
            float tLo = 0.0f, tHi = 0.0f;
            bool  have = false;

            const winding_t *w = ( u.faceIndex >= 0 && u.e.def && u.e.def->faces
                                && u.faceIndex < u.e.def->faceCount )
                               ? u.e.def->faces[u.faceIndex].w : 0;
            if ( w && w->numpoints >= 3 && w->numpoints <= MAX_POINTS_ON_WINDING )
            {
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    float rel[3];
                    Sub3( w->p[i], u.e.mid, rel );
                    const float t = Dot3( rel, u.e.u );
                    if ( !have )                 { tLo = tHi = t; have = true; }
                    else if ( t < tLo )          { tLo = t; }
                    else if ( t > tHi )          { tHi = t; }
                }
            }
            if ( !have || !( tHi - tLo > KPF_EPS ) )
            {
                float r0[3], r1[3];
                Sub3( u.e.e0, u.e.mid, r0 );
                Sub3( u.e.e1, u.e.mid, r1 );
                tLo = Dot3( r0, u.e.u );
                tHi = Dot3( r1, u.e.u );
                if ( tLo > tHi ) { const float s = tLo; tLo = tHi; tHi = s; }
            }
            Mad3( u.e.mid, u.e.u, tLo, lo );
            Mad3( u.e.mid, u.e.u, tHi, hi );
        }

        // Brush rebuilds can snap/re-derive the chamfer plane a few thousandths
        // away from the ideal radius construction. At commit, project each ideal
        // rail onto the actual chamfer/adjacent-plane intersection, solve
        // |rail-mid| = r*sqrt(1-k^2)/k, average both radii, and write the rail
        // columns exactly. Failure falls back to the ideal arc.
        bool RefitRails( const filletUnit_t &u, float *rOut,
                         float rail0[3], float railN[3] ) const
        {
            const brush_t *def = u.e.def;
            if ( !def || !def->faces || u.faceIndex < 0 || u.faceIndex >= def->faceCount )
                return false;
            if ( u.e.adj[0] < 0 || u.e.adj[0] >= def->faceCount
              || u.e.adj[1] < 0 || u.e.adj[1] >= def->faceCount )
                return false;
            const float k = u.k;
            const float s = 1.0f - k * k;
            if ( !( k > KPF_EPS ) || !( s > KPF_EPS ) )
                return false;                    // a flat or degenerate wedge

            const float *nc = def->faces[u.faceIndex].plane.normal;
            const float  dc = def->faces[u.faceIndex].plane.dist;

            float  rSum = 0.0f;
            float *out[2] = { rail0, railN };
            for ( int side = 0; side < 2; ++side )
            {
                const float *na = def->faces[u.e.adj[side]].plane.normal;
                const float  da = def->faces[u.e.adj[side]].plane.dist;

                // Start from the ideal rail.
                float seed[3];
                ArcPoint( u, m_radius, ( side == 0 ) ? 0 : ( u.spans * 2 ), seed );

                // Project the seed onto both planes via their 2x2 Gram matrix;
                // parallel planes are degenerate.
                const float g   = Dot3( na, nc );
                const float den = 1.0f - g * g;
                if ( !( fabsf( den ) > KPF_EPS ) )
                    return false;
                const float ra = da - Dot3( seed, na );
                const float rc = dc - Dot3( seed, nc );
                const float a  = ( ra - g * rc ) / den;
                const float b  = ( rc - g * ra ) / den;

                float p[3];
                for ( int c = 0; c < 3; ++c )
                    p[c] = seed[c] + a * na[c] + b * nc[c];

                // Remove the along-edge component; row placement adds it separately.
                float rel[3];
                Sub3( p, u.e.mid, rel );
                const float du = Dot3( rel, u.e.u );
                Mad3( rel, u.e.u, -du, rel );

                const float len = sqrtf( Dot3( rel, rel ) );
                if ( !( len > KPF_EPS ) )
                    return false;
                rSum += len * k / sqrtf( s );
                for ( int c = 0; c < 3; ++c )
                    out[side][c] = u.e.mid[c] + rel[c];
            }

            const float rFit = rSum * 0.5f;
            // Reject a refit that differs by more than 5%; that signals broken
            // corner assumptions rather than plane-point rounding.
            if ( !( rFit > KPF_MIN_RADIUS )
              || fabsf( rFit - m_radius ) > m_radius * 0.05f )
                return false;
            if ( rOut )
                *rOut = rFit;
            return true;
        }

        // Arc and cap paths share this profile so a rail refit cannot close one
        // seam while opening another.
        void CrossSection( const filletUnit_t &u, float r, bool refit,
                           const float rail0[3], const float railN[3],
                           int col, int width, float *out ) const
        {
            if ( refit && col == 0 )                { Copy3( rail0, out ); return; }
            if ( refit && col == width - 1 )        { Copy3( railN, out ); return; }
            ArcPoint( u, r, col, out );
        }

        // Evaluate the whole quadratic chain for preview; each span uses its
        // three consecutive ArcPoint controls.
        static void BezierAt( const filletUnit_t &u, float r, float t, float *out )
        {
            if ( t < 0.0f ) t = 0.0f;
            if ( t > 1.0f ) t = 1.0f;
            float g = t * (float)u.spans;
            int   s = (int)g;
            if ( s >= u.spans ) { s = u.spans - 1; g = (float)u.spans; }
            const float x = g - (float)s;

            float p0[3], p1[3], p2[3];
            ArcPoint( u, r, s * 2 + 0, p0 );
            ArcPoint( u, r, s * 2 + 1, p1 );
            ArcPoint( u, r, s * 2 + 2, p2 );

            const float w0 = ( 1.0f - x ) * ( 1.0f - x );
            const float w1 = 2.0f * x * ( 1.0f - x );
            const float w2 = x * x;
            for ( int c = 0; c < 3; ++c )
                out[c] = w0 * p0[c] + w1 * p1[c] + w2 * p2[c];
        }

        // Creation order mirrors Patch_BrushToMesh (pmesh.cpp:1291-1461):
        // allocate, size/fill, inherit material, finish, AddBrushForPatch,
        // Brush_AddToList, then land. Deliberate differences: land on
        // selected_brushes so the undo tail records creation, and retain the
        // chamfered source brush instead of consuming it.
        int LandPatches()
        {
            // Deselect before landing so the undo tail records only created
            // patches; covered source solids must not remain on this list.
            Select_Deselect( 1 );

            m_skips = landSkips_t();          // reset landing skip counts

            int made = 0;
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                const filletUnit_t &u = m_units[i];
                if ( !Sel_BrushLive( u.e.node ) || u.faceIndex < 0 )
                {
                    ++m_skips.deadBrush;      // reported at commit
                    continue;
                }
                entity_s *owner = u.e.node->owner;
                if ( !owner || !owner->def )
                {
                    ++m_skips.deadBrush;      // reported at commit
                    continue;
                }

                float lo[3], hi[3];
                RowEnds( u, lo, hi );
                if ( u.rowFlip )
                {
                    float t[3];
                    Copy3( lo, t ); Copy3( hi, lo ); Copy3( t, hi );
                }

                patchMesh_t *p = MakeNewPatch();
                if ( !p )
                {
                    ++m_skips.alloc;          // reported at commit
                    continue;
                }
                p->width  = u.spans * 2 + 1;      // bounded by KPATCH_MAX_WIDTH
                p->height = KPF_PATCH_ROWS;
                p->type   = PATCH_BEVEL;          // matches the Curve quarter-cylinder type

                // Inherit from KiwiBevel's chosen source face, not the generated
                // chamfer face, so a caulk neighbour cannot make the fillet invisible.
                const int   srcFi = ( u.e.srcFace >= 0 && u.e.srcFace < u.e.def->faceCount )
                                  ? u.e.srcFace : u.faceIndex;
                const face_t *src = &u.e.def->faces[srcFi];

                // MakeNewPatch leaves flags uninitialized; seed both fields from
                // the source before AddBrushForPatch copies them to the bbox faces.
                p->contents = src->contents;
                p->flags    = src->toolflags;

                // The cross-section is constant along the edge, so end/midpoint/end
                // rows are exact. Refit against the live chamfer planes when possible.
                float rUse = m_radius;
                float rail0[3], railN[3];
                const bool refit = RefitRails( u, &rUse, rail0, railN );

                for ( int col = 0; col < p->width; ++col )
                {
                    float cs[3];
                    CrossSection( u, rUse, refit, rail0, railN, col, p->width, cs );
                    for ( int row = 0; row < KPF_PATCH_ROWS; ++row )
                    {
                        const float f = (float)row / (float)( KPF_PATCH_ROWS - 1 );
                        for ( int c = 0; c < 3; ++c )
                            p->ctrl[col][row].xyz[c] = cs[c]
                                + ( lo[c] + ( hi[c] - lo[c] ) * f ) - u.e.mid[c];
                    }
                }

                // Match Patch_BrushToMesh's cast and both material layers.
                p->texture  = *(patchMesh_material *)&src->mtldef[0].lyrMtl;
                p->lightmap = *(patchMesh_material *)&src->mtldef[1].lyrMtl;

                // Copied MaterialDefs may contain zero-layer handles created while
                // the renderer was unavailable; realize every patch channel before
                // the draw path can treat it as invisible.
                KiwiMtl_RealizePatch( p );

                // Restore MakeNewPatch defaults for missing copied channels; an empty
                // lightmap channel is invisible in lightmap view and loses baked light.
                KiwiMtl_EnsurePatchChannels( p );   // kiwi_material.h:250

                // Naturalize at the parent face's scale so the fillet keeps its texel density.
                Patch_KiwiFinishNewLike( p, &src->mtldef[0].mat_texDef );

                // Finish first to seed the remaining layers and stored sample size;
                // the arc's final alignment is CAP after its symbiont is linked.
                brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
                // The arc uses CAP, while end caps keep Lmap. CAP must follow
                // AddBrushForPatch because Patch_GetAxisFace reads p->pSymbiot;
                // Lmap does not, so LandCaps may align before linking.
                Patch_KiwiCapAlign( p );
                selbrush_t *inst = Brush_AddToList( pdef, owner );
                Brush_AddToList2( inst );         // → selected_brushes, where the
                                                  // bracket TAIL will stamp it
                ++made;

                made += LandCaps( u, src, owner, rUse, refit, rail0, railN );
            }

            // Rebuild typed selection after the solids gained faces and patches landed.
            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();
            return made;
        }

        // The arc already seals both long rails against the chamfer; only its two
        // ends remain open. Build those width-by-3 caps like Patch_Thicken
        // (pmesh.cpp:7529-7581): arc profile, midpoint, projected profile.
        // Each cap is planar and its row order is chosen to face away from the
        // edge midpoint.
        int LandCaps( const filletUnit_t &u, const face_t *src, entity_s *owner,
                      float rUse, bool refit, const float rail0[3], const float railN[3] )
        {
            float lo[3], hi[3];
            RowEnds( u, lo, hi );

            // Fillet mode has zero bias. If rails were refit, project the cap onto
            // that same live chamfer plane or the closed rail seam moves to the end.
            const float d = ChamferDepth( u, rUse );
            float planePt[3];
            Mad3( u.e.mid, u.e.n, -d, planePt );
            if ( refit )
                Copy3( rail0, planePt );         // a rail IS on the real chamfer plane

            int made = 0;
            for ( int end = 0; end < 2; ++end )
            {
                const float *at = end ? hi : lo;

                // A collapsed half-edge has no outward direction and gets no cap.
                float outward[3];
                Sub3( at, u.e.mid, outward );
                if ( !Norm3( outward ) )
                    continue;

                float off[3];
                Sub3( at, u.e.mid, off );

                const int w = u.spans * 2 + 1;
                if ( w < KPATCH_MIN_WIDTH || w > KPATCH_MAX_WIDTH )   // patch format bound
                {
                    ++m_skips.capRange;       // reported at commit
                    continue;
                }

                // Size profiles by the format bound checked above.
                float top[KPATCH_MAX_WIDTH][3], bot[KPATCH_MAX_WIDTH][3];
                for ( int col = 0; col < w; ++col )
                {
                    float cs[3];
                    CrossSection( u, rUse, refit, rail0, railN, col, w, cs );
                    for ( int c = 0; c < 3; ++c )
                        top[col][c] = cs[c] + off[c];
                    float rel[3];
                    Sub3( top[col], planePt, rel );
                    Mad3( top[col], u.e.n, -Dot3( rel, u.e.n ), bot[col] );
                }

                // Surface normals follow cross(dCol,dRow). Use the middle column
                // for dRow because the tangent endpoints coincide with their
                // chamfer-plane projections and yield a zero vector.
                bool flip = false;
                {
                    const int mid = w / 2;
                    float dCol[3], dRow[3], nrm[3];
                    Sub3( top[w - 1], top[0],   dCol );
                    Sub3( bot[mid],   top[mid], dRow );
                    Cross3( dCol, dRow, nrm );
                    flip = ( Dot3( nrm, outward ) < 0.0f );
                }

                patchMesh_t *p = MakeNewPatch();
                if ( !p )
                {
                    ++m_skips.alloc;          // reported at commit
                    continue;
                }
                p->width  = w;
                p->height = 3;
                p->type   = (PATCH_TYPES)( PATCH_BEVEL | PATCH_SEAM );
                p->contents = src->contents;
                p->flags    = src->toolflags;

                for ( int col = 0; col < w; ++col )
                {
                    const float *r0 = flip ? bot[col] : top[col];
                    const float *r2 = flip ? top[col] : bot[col];
                    for ( int c = 0; c < 3; ++c )
                    {
                        p->ctrl[col][0].xyz[c] = r0[c];
                        p->ctrl[col][2].xyz[c] = r2[c];
                        p->ctrl[col][1].xyz[c] = ( r0[c] + r2[c] ) * 0.5f;
                    }
                }

                // Match the arc's material, texel density, and realization.
                p->texture  = *(patchMesh_material *)&src->mtldef[0].lyrMtl;
                p->lightmap = *(patchMesh_material *)&src->mtldef[1].lyrMtl;
                KiwiMtl_RealizePatch( p );
                // Guard copied channels exactly as for the arc.
                KiwiMtl_EnsurePatchChannels( p );   // kiwi_material.h:250
                Patch_KiwiFinishNewLike( p, &src->mtldef[0].mat_texDef );
                Patch_KiwiLmapAlign( p );         // end caps retain Lmap alignment

                brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
                selbrush_t *inst = Brush_AddToList( pdef, owner );
                Brush_AddToList2( inst );
                ++made;
            }
            return made;
        }

        void UpdateHud()
        {
            if ( m_units.empty() )
            {
                m_hud[0] = '\0';
                return;
            }
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_curve ? m_radius : m_depth );

            // Parked HUD names the ball because free cursor dragging is gated.
            if ( Parked() )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "%s  %i edge(s)  parked at 0  ·  grab the ball and drag "
                           "(or type a %s)  (D: %s)",
                           m_curve ? "fillet" : "bevel", (int)m_units.size(),
                           m_curve ? "radius" : "depth",
                           m_curve ? "flat" : "curve it" );
                m_hud[sizeof( m_hud ) - 1] = '\0';
                return;
            }

            if ( m_invalid )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "%s  %i edge(s)  %s %s  INVALID (%s)",
                           m_curve ? "fillet" : "bevel", (int)m_units.size(),
                           m_curve ? "r" : "d", b, m_why ? m_why : "rejected" );
            }
            else if ( m_curve )
            {
                char cb[32];
                KiwiUnits_Format( cb, sizeof( cb ), ChamferDepth( m_units[0], m_radius ) );
                _snprintf( m_hud, sizeof( m_hud ),
                           "fillet  %i edge(s)  r %s  chamfer %s  %i-col patch  (D: flat)",
                           (int)m_units.size(), b, cb, m_units[0].spans * 2 + 1 );
            }
            else
            {
                // Report applied bias because each corner can clamp differently.
                const float applied = m_units[0].e.bias * m_units[0].biasSign
                                    * ( 180.0f / KPF_PI );
                char bias[48] = { 0 };
                if ( fabsf( m_biasDeg ) > 1.0e-3f || m_hasBias || m_angleDrag )
                {
                    char appliedText[32];
                    _snprintf( bias, sizeof( bias ), "  bias %s deg%s",
                               KiwiFmt_Num( appliedText, sizeof( appliedText ), applied, 4 ),
                               ( fabsf( applied - m_biasDeg ) > 0.05f ) ? " (clamped)" : "" );
                }
                _snprintf( m_hud, sizeof( m_hud ),
                           "bevel  %i edge(s)  d %s%s  %s",
                           (int)m_units.size(), b, bias,
                           m_angleDrag ? "ANGLE DRAG - move sideways, Ctrl = 5 deg, click or A ends"
                                       : "(D: curve it, A: drag the angle)" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // Count every landing skip so a result below three patches per edge is explained.
        struct landSkips_t
        {
            int deadBrush = 0;   // the unit's brush or its owner went away mid-gesture
            int capRange  = 0;   // a cap width outside the patch format's 3..15 columns
            int alloc     = 0;   // MakeNewPatch returned NULL
        };
        landSkips_t               m_skips;

        std::vector<filletUnit_t> m_units;
        float                     m_radius    = 0.0f;
        // m_depth is world-space chamfer depth; m_biasDeg is requested degrees.
        float                     m_depth     = 0.0f;
        bool                      m_curve     = false;
        float                     m_biasDeg   = 0.0f;
        bool                      m_hasBias   = false;
        // A: the cursor column drives m_biasDeg (ToggleAngleDrag).
        bool                      m_angleDrag = false;
        int                       m_angleX0   = 0;
        float                     m_angleBase = 0.0f;
        float                     m_start     = 0.0f;
        bool                      m_haveStart = false;
        bool                      m_hasNum    = false;
        // True while the shared lollipop arm holds the ball.
        bool                      m_grabbed   = false;
        float                     m_numWorld  = 0.0f;
        bool                      m_invalid   = false;
        bool                      m_added     = false;
        bool                      m_undoOpen  = false;
        const char               *m_why       = 0;
        snap_result_t             m_snap;
        char                      m_hud[192]  = { 0 };
    };

    KiwiPatchFilletCommand s_patchFillet;
}

bool KiwiPatchFillet_CanFillet()
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( it.kind == SEL_EDGE && Sel_BrushLive( it.brush ) && !it.brush->patch )
            return true;
    }
    return false;
}

// Bare B reaches this initially-flat command for brush-edge selections.
int KiwiPatchFillet_ContextB( int commandId )
{
    if ( commandId != KIWI_CMD_FILLET_CURVE )
        return commandId;
    return KiwiPatchFillet_CanFillet() ? KIWI_CMD_FILLET_EDGE : commandId;
}

void KiwiPatchFillet_RegisterCommands()
{
    // Leave it unbound: contextual B dispatches here, while palette invocation
    // uses the registered command id directly.
    Radiant_RegisterCommand( "KiwiFilletEdgePatch", 0, 0, KIWI_CMD_FILLET_EDGE );
}

KiwiEditorCommand *KiwiPatchFillet_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_FILLET_EDGE ) ? &s_patchFillet : 0;
}

// Carry landed fillet surfaces during supported face-plane translations.
namespace
{
    // World units. Match the editor's on-plane tolerance so snapped/re-derived
    // brush planes still recognize ideal patch rails.
    const float KFIL_ONPLANE_EPS = 0.01f;

    // Deliberately looser than the plane test: 0.5 world units admits boundary
    // rails while rejecting patches merely sharing the infinite plane.
    const float KFIL_INWINDING_EPS = 0.5f;

    // Cheap grown-AABB rejection before per-control-point tests.
    bool PatchNearBrush( const patchMesh_t *p, const brush_t *def, float grow )
    {
        if ( !p || !def )
            return false;
        float lo[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
        float hi[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
        for ( int c = 0; c < p->width; ++c )
            for ( int r = 0; r < p->height; ++r )
                for ( int k = 0; k < 3; ++k )
                {
                    const float v = p->ctrl[c][r].xyz[k];
                    if ( v < lo[k] ) lo[k] = v;
                    if ( v > hi[k] ) hi[k] = v;
                }
        for ( int k = 0; k < 3; ++k )
            if ( lo[k] > def->maxs[k] + grow || hi[k] < def->mins[k] - grow )
                return false;
        return true;
    }

    // Require the point on the finite convex face: first the plane, then every
    // inward winding edge.
    bool PointOnFace( const face_t *f, const float pt[3] )
    {
        if ( !f || !f->w || f->w->numpoints < 3 )
            return false;
        const float d = Dot3( f->plane.normal, pt ) - f->plane.dist;
        if ( fabsf( d ) > KFIL_ONPLANE_EPS )
            return false;

        const winding_t *w = f->w;
        float cen[3] = { 0.0f, 0.0f, 0.0f };
        for ( int j = 0; j < w->numpoints; ++j )
            for ( int k = 0; k < 3; ++k )
                cen[k] += w->p[j][k];
        for ( int k = 0; k < 3; ++k )
            cen[k] /= (float)w->numpoints;

        for ( int i = 0; i < w->numpoints; ++i )
        {
            const float *a = w->p[i];
            const float *b = w->p[( i + 1 ) % w->numpoints];
            float e[3], inward[3], v[3];
            Sub3( b, a, e );
            // Use the centroid to make the winding-edge sign order-independent.
            inward[0] = f->plane.normal[1]*e[2] - f->plane.normal[2]*e[1];
            inward[1] = f->plane.normal[2]*e[0] - f->plane.normal[0]*e[2];
            inward[2] = f->plane.normal[0]*e[1] - f->plane.normal[1]*e[0];
            const float len = Len3( inward );
            if ( len < 1.0e-6f )
                continue;                      // a degenerate edge decides nothing
            for ( int k = 0; k < 3; ++k )
                inward[k] /= len;

            float toCen[3];
            Sub3( cen, a, toCen );
            const float sign = ( Dot3( inward, toCen ) >= 0.0f ) ? 1.0f : -1.0f;

            Sub3( pt, a, v );
            if ( sign * Dot3( inward, v ) < -KFIL_INWINDING_EPS )
                return false;
        }
        return true;
    }
}

int KiwiFillet_CarryOnPlaneMove( const brush_t *def, int movedFace,
                                 const float planeN[3], float planeDistBefore,
                                 float travel, int *outSkipped )
{
    if ( outSkipped )
        *outSkipped = 0;
    if ( !def || !def->faces || movedFace < 0 || movedFace >= def->faceCount
      || !planeN || fabsf( travel ) < 1.0e-4f )
        return 0;

    // The caller already rebuilt the live face, so reconstruct its old plane
    // from the saved normal and distance.
    const face_t *mf = &def->faces[movedFace];
    face_t before = *mf;                          // value copy; winding stays borrowed
    before.plane.normal[0] = planeN[0];
    before.plane.normal[1] = planeN[1];
    before.plane.normal[2] = planeN[2];
    before.plane.dist      = planeDistBefore;

    float delta[3];
    for ( int k = 0; k < 3; ++k )
        delta[k] = planeN[k] * travel;

    int moved   = 0;
    int skipped = 0;

    selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
    for ( int L = 0; L < 2; ++L )
    {
        selbrush_t *head = lists[L];
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( !b->patch || !b->def || !b->def->patch )
                continue;
            patchMesh_t *p = b->def->patch;
            // 16 is patchMesh_t::ctrl's storage extent, not the 15-column builder limit.
            if ( p->width < 3 || p->height < 2
              || p->width > 16 || p->height > 16 )
                continue;
            if ( !PatchNearBrush( p, def, 1.0f ) )
                continue;

            // Identify complete rows on the face's old position.
            bool onRow[16];
            bool any = false;
            for ( int r = 0; r < p->height; ++r )
            {
                bool all = true;
                for ( int c = 0; c < p->width && all; ++c )
                    all = PointOnFace( &before, p->ctrl[c][r].xyz );
                onRow[r] = all;
                any = any || all;
            }

            if ( !any )
            {
                // Partial on-plane controls identify a cross-section move, which
                // requires a radius solve and is refused.
                bool partial = false;
                for ( int c = 0; c < p->width && !partial; ++c )
                    for ( int r = 0; r < p->height && !partial; ++r )
                        partial = PointOnFace( &before, p->ctrl[c][r].xyz );
                if ( partial )
                    ++skipped;
                continue;
            }

            // The caller's undo head may not include this patch, so cover it
            // inside the already-open record before mutation.
            Undo_AddBrush( (entity_brush_s *)b->def );

            for ( int r = 0; r < p->height; ++r )
            {
                if ( !onRow[r] )
                    continue;
                for ( int c = 0; c < p->width; ++c )
                    for ( int k = 0; k < 3; ++k )
                        p->ctrl[c][r].xyz[k] += delta[k];
            }

            // Preserve the end/midpoint/end parameterization by linearly
            // re-interpolating interior rows when only one end moves.
            if ( p->height >= 3 && onRow[0] != onRow[p->height - 1] )
            {
                for ( int r = 1; r < p->height - 1; ++r )
                {
                    const float t = (float)r / (float)( p->height - 1 );
                    for ( int c = 0; c < p->width; ++c )
                        for ( int k = 0; k < 3; ++k )
                            p->ctrl[c][r].xyz[k] =
                                  p->ctrl[c][0].xyz[k]
                                + ( p->ctrl[c][p->height - 1].xyz[k]
                                  - p->ctrl[c][0].xyz[k] ) * t;
                }
            }

            // Ported control-point bookkeeping; Patch_UpdateSelected_0 uses the
            // same Patch_Rebuild(p, 1) at 0x43D800. Do not set bDirty: it means
            // an explicit sample size and would serialize stale data.
            Patch_Rebuild( p, 1 );
            ++moved;
        }
    }

    if ( outSkipped )
        *outSkipped = skipped;
    return moved;
}
