#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Duplication UI over ported clone and transform cores. This file owns input
// mapping, preview, command dispatch, and undo bracketing.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s

#include "kiwi_dupe.h"
#include "kiwi_csg.h"                 // cloneability gate
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_conselect.h"        // edge duplication
#include "kiwi_construct.h"        // construction segments
#include "kiwi_grid.h"
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_transform.h"        // session pivot
#include "kiwi_units.h"
#include "kiwi_vec.h"     // vector helpers

#include <imgui/imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int       Sys_Printf( const char *fmt, ... );                    // win_qe3.cpp:118
extern int       g_nUpdateBits;                                         // 0x25D5A74 (mainfrm.cpp)
extern camera_s *Ed_Camera();                                           // camwnd.cpp:161
extern void      CamWnd_BuildMatrix();                                  // camwnd.cpp 0x403470

extern void      Clone_Selection( float gridSize );                     // select.cpp:2508 0x48F0D0
extern void      Select_Move( const float *delta, char bSnap );         // select.cpp:2152 0x48E9C0
extern void      Select_GetMid( float *mid );                           // select.cpp:2209 0x48FC70
extern void      Select_RotateAxis( int axis, float deg,
                                    float (*rot_around)[4][3] );        // select.cpp:2364 0x48FF40
extern void      Select_ApplyMatrix_SelectedBrushes( int bSnap, float *mat,
                                                     float deg, char bSwap ); // select.cpp:2242 0x48FD10
extern void      sub_47B940( brush_t *def );                            // brush.cpp:5839 (Brush_UpdateSpecialMaterialFlag)
extern float     grid_sizes[];                                          // engine_stubs.cpp:771 (0x6DDE5C)

extern void      Radiant_ExecCommand( unsigned int cmdId );             // mainfrm.cpp:4054
// Keep at file scope; MSVC mangles a block-scope declaration with its namespace.
extern bool      Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                          int commandId );              // mainfrm.cpp:1340

// Duplicate uses a creation bracket: these four calls deliberately omit Undo_AddBrushList.
extern void      Undo_ClearRedo();                                      // undo.cpp:176  0x45e2b0
extern void      Undo_GeneralStart( const char *operation );            // undo.cpp:367  0x45e3f0
extern void      Undo_EndBrushList( selbrush_t *brushlist );            // undo.cpp:576  0x45e870
extern void      Undo_End();                                            // undo.cpp:686  0x45ea20

namespace
{
    // The three CLASSIC mirror ids (mainfrm.cpp:5267-5269).
    const int KDUP_ID_FLIP_X = 32956;
    const int KDUP_ID_FLIP_Y = 32957;
    const int KDUP_ID_FLIP_Z = 32958;

    const float KDUP_COL_GHOST[3] = { 0.60f, 0.85f, 1.00f };
    const float KDUP_COL_BAD[3]   = { 1.00f, 0.30f, 0.25f };
    // Warm color distinguishes the input line from blue copy ghosts.
    const float KDUP_COL_LINE[3]  = { 1.00f, 0.78f, 0.22f };


    // The numeric layer copies field structs but retains their label pointers.
    const kiwiNumField_t KARR_FIELDS[1] = { { "count", KNUM_COUNT, false } };

    // Count is the only editable scalar; linear spacing is derived and read-only.
    const kiwiNumField_t KARR_FIELDS_LINEAR[2] =
    {
        { "count",   KNUM_COUNT,  false },
        { "spacing", KNUM_LENGTH, true  },
    };

    bool RayPlane( const ray_t &ray, const float *pt, const float *n, float *out )
    {
        const float den = Dot3( ray.dir, n );
        if ( fabsf( den ) < 1.0e-5f )
            return false;
        float rel[3];
        Sub3( pt, ray.origin, rel );
        const float t = Dot3( rel, n ) / den;
        if ( !( t > 0.0f ) || t > 1.0e6f )
            return false;
        Mad3( ray.origin, ray.dir, t, out );
        return true;
    }

    bool PickConstructionSegment( int imgX, int imgY, float *outA, float *outB,
                                  int *outObj, int *outSeg )
    {
        return KiwiCon_PickSegmentAt( imgX, imgY, outA, outB, outObj, outSeg, 0 );
    }

    // KIWI (2026-09-09): Clone_Selection clones through the map text now (entities,
    // patches, fixed-size proxies included), so anything with a def is cloneable.
    inline bool Cloneable( const selbrush_t *b ) { return b && b->def && b->owner && b->owner->def; }

    int CloneableSelectedCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( Cloneable( b ) )
                ++n;
        return n;
    }

    // Linear and radial arrays share state except for drag mapping and transform.
    class KiwiArrayCommand : public KiwiEditorCommand
    {
    public:
        explicit KiwiArrayCommand( bool radial ) : m_radial( radial ) {}

        const char *Name() const override
        {
            return m_radial ? "Array (Radial)" : "Array (Linear)";
        }
        bool CanExecute() override { return KiwiDupe_CanArray(); }

        // KNUM_COUNT prevents length-unit formatting; linear spacing is derived/read-only.
        int NumericFields( const kiwiNumField_t **out ) const override
        {
            if ( m_radial )
            {
                *out = KARR_FIELDS;
                return 1;
            }
            *out = KARR_FIELDS_LINEAR;
            return 2;
        }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 0 )
            {
                *out = (float)m_count;
                return true;
            }
            // KNUM_LENGTH expects raw world units.
            if ( field == 1 && !m_radial )
            {
                *out = Len3( m_offset );
                return true;
            }
            return false;
        }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            if ( m_radial )
                return 0;                       // radial invents nothing of its own
            static const kiwiPrompt_t s_noLine[] = {
                { "LMB", "Click a construction line to array along it" },
            };
            static const kiwiPrompt_t s_onLine[] = {
                { "LMB",  "Click another line" },
                { "Move", "Aim by hand (releases the line)" },
            };
            if ( m_lineHave )
            {
                *out = s_onLine;
                return (int)( sizeof( s_onLine ) / sizeof( s_onLine[0] ) );
            }
            *out = s_noLine;
            return (int)( sizeof( s_noLine ) / sizeof( s_noLine[0] ) );
        }

        // WantsClicks would replace the normal HOT/PAUSED grammar. Intercept only an
        // actual line hit so empty-space clicks still park or resume the command.
        bool PressIntercept( int imgX, int imgY ) override
        {
            if ( m_radial )
                return false;                   // a radial array has no vector to set
            float a[3], b[3];
            int   obj = -1, seg = -1;
            if ( !PickConstructionSegment( imgX, imgY, a, b, &obj, &seg ) )
                return false;                   // nothing there — the press is not ours
            AdoptLine( a, b, obj, seg, imgX, imgY );
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 )
                return false;
            Copy3( m_ref, out3 );          // the array's latched reference point
            return true;
        }

        // The array never snaps to the geometry it is about to copy.
        unsigned    PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        bool Begin() override
        {
            Reset();

            if ( CloneableSelectedCount() <= 0 )
            {
                Sys_Printf( "Array: select at least one brush, patch or entity.\n" );
                return false;
            }

            // The capped source-bbox union drives the ghost preview.
            m_srcMins[0] = m_srcMins[1] = m_srcMins[2] =  131072.0f;
            m_srcMaxs[0] = m_srcMaxs[1] = m_srcMaxs[2] = -131072.0f;
            int n = 0;
            for ( selbrush_t *b = selected_brushes.next;
                  b != &selected_brushes && n < KARR_MAX_SOURCE; b = b->next )
            {
                if ( !Cloneable( b ) )
                    continue;
                for ( int k = 0; k < 3; ++k )
                {
                    if ( b->def->mins[k] < m_srcMins[k] ) m_srcMins[k] = b->def->mins[k];
                    if ( b->def->maxs[k] > m_srcMaxs[k] ) m_srcMaxs[k] = b->def->maxs[k];
                }
                ++n;
            }
            m_srcCount = n;

            // Reuse the self-expiring session pivot and latch it once. It is the radial
            // center and linear drag-plane origin; linear mode never moves originals to it.
            m_pivoted = KiwiXform_PivotOverride( m_ref );
            if ( !m_pivoted )
                Select_GetMid( m_ref );

            // Camera navigation stays live, so latch the plane normal for the gesture.
            CamWnd_BuildMatrix();
            Copy3( Ed_Camera()->vpn, m_planeN );

            m_count = m_radial ? KARR_DEF_RADIAL : KARR_DEF_LINEAR;
            LatchStart();
            UpdateHud();

            if ( m_radial )
                Sys_Printf( "Array (radial): type a count 2..%i (full circle about Z "
                            "through %s).\n", KARR_MAX_COUNT,
                            m_pivoted ? "the PIVOT" : "the selection mid" );
            else
                Sys_Printf( "Array (linear): CLICK A CONSTRUCTION LINE to array along it, "
                            "or DRAG to set the step by hand.  TYPED digits set the "
                            "count (2..%i).\n", KARR_MAX_COUNT );
            if ( m_pivoted )
                Sys_Printf( "Array: measuring from the session PIVOT %.3g %.3g %.3g "
                            "(V, set in Move/Rotate) instead of the selection mid.\n",
                            (double)m_ref[0], (double)m_ref[1], (double)m_ref[2] );
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
            m_hasNum = has && KiwiNum_HasValue();
            // A count is unitless; reverse the numeric layer's length conversion.
            m_numCount = (int)floorf( Units_ToDisplay( world ) + 0.5f );
            Recompute();
        }

        void Commit() override
        {
            if ( m_invalid )
            {
                Sys_Printf( "Array: nothing done — %s.\n", m_why ? m_why : "invalid" );
                Reset();
                g_nUpdateBits = -1;
                return;
            }
            Perform();
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Nothing is mutated before Commit, so there is nothing to restore and
            // no bracket to roll back.
            Reset();
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( m_srcCount <= 0 || m_count < KARR_MIN_COUNT )
                return;

            // Line and ghosts share one batch. Draw the seven-segment line first so
            // the preview budget accounts for it; unequal endpoint ticks show direction.
            if ( m_lineHave && KiwiLines_Remaining() >= 7 )
            {
                KiwiLines_Color( KDUP_COL_LINE[0], KDUP_COL_LINE[1], KDUP_COL_LINE[2] );
                KiwiLines_Add( m_lineA, m_lineB );
                DrawTick( m_lineA, 12.0f );
                DrawTick( m_lineB, 6.0f );
            }

            const float *col = m_invalid ? KDUP_COL_BAD : KDUP_COL_GHOST;
            KiwiLines_Color( col[0], col[1], col[2] );

            const int copies = m_count - 1;
            // A box costs 12 segments and the coarse axis cross costs 3; never spill.
            const int room = KiwiLines_Remaining();
            const bool boxes = ( copies * 12 <= room );
            const int shown  = boxes ? copies
                                     : ( room / 3 < copies ? room / 3 : copies );

            for ( int i = 1; i <= shown; ++i )
            {
                float mins[3], maxs[3];
                GhostBox( i, mins, maxs );
                if ( boxes )
                {
                    if ( !DrawBox( mins, maxs ) )
                        return;
                }
                else
                {
                    float c[3];
                    for ( int k = 0; k < 3; ++k )
                        c[k] = ( mins[k] + maxs[k] ) * 0.5f;
                    const float r = 8.0f;
                    for ( int k = 0; k < 3; ++k )
                    {
                        float a[3], b[3];
                        Copy3( c, a ); Copy3( c, b );
                        a[k] -= r; b[k] += r;
                        if ( !KiwiLines_Add( a, b ) )
                            return;
                    }
                }
            }
        }

    private:
        void Reset()
        {
            m_srcCount  = 0;
            m_count     = m_radial ? KARR_DEF_RADIAL : KARR_DEF_LINEAR;
            m_hasNum    = false;
            m_numCount  = 0;
            m_invalid   = false;
            m_why       = 0;
            m_haveStart = false;
            m_offset[0] = m_offset[1] = m_offset[2] = 0.0f;
            m_hud[0]    = '\0';
            // Forget per-gesture line/pivot state, not the session pivot itself.
            m_lineHave  = false;
            m_lineObj   = -1;
            m_lineSeg   = -1;
            m_lineLen   = 0.0f;
            m_linePixHave = false;
            m_pivoted   = false;
        }

        // A picked segment is an extent: spacing = length / (count - 1). Orient it
        // away from m_ref because stored endpoint order has no user-visible direction.
        void AdoptLine( const float *a, const float *b, int obj, int seg,
                        int imgX, int imgY )
        {
            float d[3];
            Sub3( b, a, d );
            const float len = Len3( d );
            if ( len < KARR_MIN_OFFSET )
            {
                Sys_Printf( "Array: that construction segment is degenerate "
                            "(%.3g units) — nothing to array along.\n", (double)len );
                return;
            }

            float da[3], db[3];
            Sub3( a, m_ref, da );
            Sub3( b, m_ref, db );
            if ( Len3( db ) < Len3( da ) )
            {
                Copy3( b, m_lineA );
                Copy3( a, m_lineB );
            }
            else
            {
                Copy3( a, m_lineA );
                Copy3( b, m_lineB );
            }
            Sub3( m_lineB, m_lineA, m_lineDir );
            const float inv = 1.0f / len;
            m_lineDir[0] *= inv;  m_lineDir[1] *= inv;  m_lineDir[2] *= inv;
            m_lineLen  = len;
            m_lineObj  = obj;
            m_lineSeg  = seg;
            m_lineHave = true;

            // The deadzone's centre (kiwi_dupe.h KARR_LINE_BREAK_PIXELS).
            m_linePixX    = imgX;
            m_linePixY    = imgY;
            m_linePixHave = true;

            char blen[32];
            KiwiUnits_Format( blen, sizeof( blen ), m_lineLen );
            Sys_Printf( "Linear array: following construction line %i segment %i "
                        "(%s across, count %i) — move the mouse to aim by hand again.\n",
                        m_lineObj, m_lineSeg, blen, m_count );

            Recompute();
            g_nUpdateBits |= 1;
        }

        void ReleaseLineIfHandAimed()
        {
            if ( !m_lineHave || !m_linePixHave )
                return;
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return;
            const float dx = (float)( x - m_linePixX );
            const float dy = (float)( y - m_linePixY );
            if ( sqrtf( dx * dx + dy * dy ) <= KARR_LINE_BREAK_PIXELS )
                return;

            m_lineHave    = false;
            m_linePixHave = false;
            // Rebase so this cursor still yields the line offset; a raw handoff jumps,
            // while relatching at the cursor collapses the offset to zero.
            RebaseStartToOffset();
            Sys_Printf( "Linear array: released construction line %i — the step "
                        "follows the cursor again.\n", m_lineObj );
            m_lineObj = -1;
            m_lineSeg = -1;
        }

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
            if ( !CursorRay( &ray ) || !RayPlane( ray, m_ref, m_planeN, p ) )
                return;
            Copy3( p, m_start );
            m_haveStart = true;
        }

        // Preserve the current offset when line mode hands control back to the cursor.
        void RebaseStartToOffset()
        {
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !RayPlane( ray, m_ref, m_planeN, p ) )
            {
                LatchStart();
                return;
            }
            Sub3( p, m_offset, m_start );
            m_haveStart = true;
        }

        void Recompute()
        {
            if ( m_hasNum )
            {
                int c = m_numCount;
                if ( c < KARR_MIN_COUNT ) c = KARR_MIN_COUNT;
                if ( c > KARR_MAX_COUNT ) c = KARR_MAX_COUNT;
                m_count = c;
            }

            if ( !m_radial )
            {
                // Exactly one source owns m_offset per frame; line mode wins until released.
                ReleaseLineIfHandAimed();

                if ( m_lineHave )
                {
                    // Count is clamped to at least 2; retain the guard for future defaults.
                    int span = m_count - 1;
                    if ( span < 1 )
                        span = 1;
                    const float step = m_lineLen / (float)span;
                    for ( int k = 0; k < 3; ++k )
                        m_offset[k] = m_lineDir[k] * step;

                    // Do not grid-round exact line spacing or copies stop spanning the line.
                    m_invalid = ( step < KARR_MIN_OFFSET );
                    m_why     = m_invalid
                                ? "the line is too short for this count — lower the count"
                                : 0;
                }
                else
                {
                    if ( !m_haveStart )
                        LatchStart();
                    ray_t ray;
                    float p[3];
                    if ( m_haveStart && CursorRay( &ray ) && RayPlane( ray, m_ref, m_planeN, p ) )
                    {
                        float d[3];
                        Sub3( p, m_start, d );
                        if ( m_snap.valid && m_snap.type != SNAP_NONE
                             && !KiwiSnap_IsGeometry( m_snap.type ) )
                        {
                            float snapped[3];
                            if ( KiwiGrid_Snap( d, snapped ) )
                                Copy3( snapped, d );
                        }
                        Copy3( d, m_offset );
                    }
                    m_invalid = ( Len3( m_offset ) < KARR_MIN_OFFSET );
                    m_why     = m_invalid ? "step offset is zero — drag to set it" : 0;
                }
            }
            else
            {
                m_invalid = false;
                m_why     = 0;
            }
            UpdateHud();
        }

        // The bbox of copy `i` (i >= 1), for the preview only.
        void GhostBox( int i, float *mins, float *maxs ) const
        {
            if ( !m_radial )
            {
                for ( int k = 0; k < 3; ++k )
                {
                    mins[k] = m_srcMins[k] + m_offset[k] * (float)i;
                    maxs[k] = m_srcMaxs[k] + m_offset[k] * (float)i;
                }
                return;
            }

            // Rotate the box center around Z but keep extents; the ghost shows placement,
            // not the exact axis-aligned bounds of rotated geometry.
            const float deg = 360.0f / (float)m_count * (float)i;
            const float rad = deg * 3.14159265358979323846f / 180.0f;
            const float cs  = cosf( rad ), sn = sinf( rad );
            float c[3], h[3];
            for ( int k = 0; k < 3; ++k )
            {
                c[k] = ( m_srcMins[k] + m_srcMaxs[k] ) * 0.5f;
                h[k] = ( m_srcMaxs[k] - m_srcMins[k] ) * 0.5f;
            }
            const float rx = c[0] - m_ref[0];
            const float ry = c[1] - m_ref[1];
            c[0] = m_ref[0] + rx * cs - ry * sn;
            c[1] = m_ref[1] + rx * sn + ry * cs;
            for ( int k = 0; k < 3; ++k )
            {
                mins[k] = c[k] - h[k];
                maxs[k] = c[k] + h[k];
            }
        }

        static void DrawTick( const float *c, float r )
        {
            for ( int k = 0; k < 3; ++k )
            {
                float a[3], b[3];
                Copy3( c, a ); Copy3( c, b );
                a[k] -= r; b[k] += r;
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

        static bool DrawBox( const float *mins, const float *maxs )
        {
            float v[KIWI_BOX_CORNERS][3];
            KiwiBox_Corners( mins, maxs, v );
            for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
                if ( !KiwiLines_Add( v[KIWI_BOX_EDGE[e][0]], v[KIWI_BOX_EDGE[e][1]] ) )
                    return false;
            return true;
        }

        void Perform()
        {
            // Every original saved by UndoBegin must be selected again at UndoCommit.
            std::vector<selbrush_t *> originals;
            for ( selbrush_t *b = selected_brushes.next;
                  b != &selected_brushes && (int)originals.size() < KARR_MAX_SOURCE;
                  b = b->next )
                originals.push_back( b );
            if ( originals.empty() )
                return;

            const int copies = m_count - 1;
            if ( copies <= 0 )
            {
                Sys_Printf( "Array: count %i — nothing to do.\n", m_count );
                return;
            }

            KiwiCmd_UndoBegin( m_radial ? "radial array" : "linear array" );

            std::vector<selbrush_t *> made;
            const float stepDeg = 360.0f / (float)m_count;

            for ( int i = 0; i < copies; ++i )
            {
                // Clone the current selection so cumulative transforms land copy i
                // at i × step without reselecting originals mid-loop.
                Clone_Selection( grid_sizes[g_qeglobals.d_gridsize] );
                if ( selected_brushes.next == &selected_brushes )
                {
                    Sys_Printf( "Array: nothing cloneable — stopped after %i copy(ies).\n", i );
                    break;
                }

                if ( m_radial )
                {
                    // Match the ported Radiant_RotateSelection transform sequence.
                    float rot_around[4][3];
                    Copy3( m_ref, rot_around[0] );
                    Select_RotateAxis( 2, stepDeg, (float (*)[4][3])rot_around );
                    Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], stepDeg, 0 );
                }
                else
                {
                    Select_Move( m_offset, 0 );      // bSnap 0 — this layer owns snapping
                }

                for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
                {
                    made.push_back( b );
                    // Match Cmd_OnSelectionClone (mainfrm.cpp 0x425480): refresh the
                    // 2D back-face-cull hint from face materials.
                    sub_47B940( b->def );
                }
            }

            // Rebuild originals + copies through the typed-to-legacy crossing.
            selection_t &sel = KiwiSel();
            Sel_Clear( sel );
            for ( size_t i = 0; i < originals.size(); ++i )
                Sel_Add( sel, Sel_MakeObject( originals[i] ) );
            for ( size_t i = 0; i < made.size(); ++i )
                Sel_Add( sel, Sel_MakeObject( made[i] ) );
            sel.active = originals.empty() ? sel.active : Sel_MakeObject( originals[0] );
            Sel_SyncToLegacy();

            if ( m_radial )
                Sys_Printf( "Radial array: %i new brush(es) from %i, about Z through "
                            "%s (%.3g deg step, count %i).\n",
                            (int)made.size(), (int)originals.size(),
                            m_pivoted ? "the PIVOT" : "the selection mid",
                            (double)stepDeg, m_count );
            else if ( m_lineHave )
                Sys_Printf( "Linear array: %i new brush(es) from %i, along construction "
                            "line %i segment %i, step %.3g %.3g %.3g (count %i)%s.\n",
                            (int)made.size(), (int)originals.size(),
                            m_lineObj, m_lineSeg,
                            (double)m_offset[0], (double)m_offset[1], (double)m_offset[2],
                            m_count, m_pivoted ? ", measured from the PIVOT" : "" );
            else
                Sys_Printf( "Linear array: %i new brush(es) from %i, step %.3g %.3g %.3g "
                            "(count %i)%s.\n",
                            (int)made.size(), (int)originals.size(),
                            (double)m_offset[0], (double)m_offset[1], (double)m_offset[2],
                            m_count, m_pivoted ? ", measured from the PIVOT" : "" );
            g_nUpdateBits = -1;
        }

        void UpdateHud()
        {
            // Name the active line/hand and pivot/centroid sources, not just their values.
            const char *ref = m_pivoted ? "  from PIVOT" : "";

            if ( m_radial )
            {
                char step[32];
                _snprintf( m_hud, sizeof( m_hud ),
                           "radial array  count %i  (type a count; %s deg step)%s",
                           m_count,
                           KiwiFmt_Num( step, sizeof( step ), 360.0f / (float)m_count ),
                           ref );
            }
            else if ( m_lineHave )
            {
                char bspan[32], bstep[32];
                KiwiUnits_Format( bspan, sizeof( bspan ), m_lineLen );
                KiwiUnits_Format( bstep, sizeof( bstep ), Len3( m_offset ) );
                if ( m_invalid )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "linear array  ON LINE %i.%i  span %s  count %i  "
                               "SPACING %s TOO SMALL%s",
                               m_lineObj, m_lineSeg, bspan, m_count, bstep, ref );
                else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "linear array  ON LINE %i.%i  span %s  count %i  "
                               "spacing %s  (typed = count; move = aim by hand)%s",
                               m_lineObj, m_lineSeg, bspan, m_count, bstep, ref );
            }
            else
            {
                char bx[32], by[32], bz[32];
                KiwiUnits_Format( bx, sizeof( bx ), m_offset[0] );
                KiwiUnits_Format( by, sizeof( by ), m_offset[1] );
                KiwiUnits_Format( bz, sizeof( bz ), m_offset[2] );
                if ( m_invalid )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "linear array  count %i  step %s %s %s  "
                               "DRAG TO SET THE STEP (or click a line)%s",
                               m_count, bx, by, bz, ref );
                else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "linear array  count %i  step %s %s %s  "
                               "(drag = step, click a line = span, typed = count)%s",
                               m_count, bx, by, bz, ref );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        const bool    m_radial;
        int           m_srcCount  = 0;
        float         m_srcMins[3] = { 0.0f, 0.0f, 0.0f };
        float         m_srcMaxs[3] = { 0.0f, 0.0f, 0.0f };
        float         m_ref[3]     = { 0.0f, 0.0f, 0.0f };
        float         m_planeN[3]  = { 0.0f, 0.0f, 1.0f };
        float         m_start[3]   = { 0.0f, 0.0f, 0.0f };
        bool          m_haveStart  = false;
        float         m_offset[3]  = { 0.0f, 0.0f, 0.0f };
        int           m_count      = KARR_DEF_LINEAR;
        bool          m_hasNum     = false;
        int           m_numCount   = 0;
        bool          m_invalid    = false;
        const char   *m_why        = 0;
        snap_result_t m_snap;
        char          m_hud[192]   = { 0 };

        // Cache whether the latched reference was a session pivot; the query self-expires.
        bool          m_pivoted    = false;
        // m_lineA is the endpoint nearer m_ref; m_lineDir is unit length.
        bool          m_lineHave   = false;
        int           m_lineObj    = -1;
        int           m_lineSeg    = -1;
        float         m_lineA[3]   = { 0.0f, 0.0f, 0.0f };
        float         m_lineB[3]   = { 0.0f, 0.0f, 0.0f };
        float         m_lineDir[3] = { 1.0f, 0.0f, 0.0f };
        float         m_lineLen    = 0.0f;
        // The pixel the line was picked at — the centre of the hand-aim deadzone.
        bool          m_linePixHave = false;
        int           m_linePixX    = 0;
        int           m_linePixY    = 0;
    };

    KiwiArrayCommand s_linear( false );
    KiwiArrayCommand s_radial( true );
}

bool KiwiDupe_CanMirror()
{
    return selected_brushes.next != &selected_brushes;
}

bool KiwiDupe_CanArray()
{
    return CloneableSelectedCount() > 0;
}

bool KiwiDupe_CanDuplicate()
{
    // Edge-only typed selections leave selected_brushes empty; include that dispatch arm.
    return CloneableSelectedCount() > 0 || KiwiConSel_CanDuplicateEdges();
}

bool KiwiDupe_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned)KIWI_CMD_DUPLICATE )
        return false;

    // Any live typed edge routes to construction-line duplication. Test it first:
    // edge-only selection has no legacy selected brushes, and mixed selection must
    // not clone the selected edges' owner brushes.
    if ( KiwiConSel_CanDuplicateEdges() )
    {
        if ( KiwiCmd_Active() )
        {
            Sys_Printf( "Duplicate: finish the current command first.\n" );
            return true;
        }
        KiwiConSel_DuplicateEdgesAsLines();
        return true;
    }

    if ( CloneableSelectedCount() <= 0 )
    {
        Sys_Printf( "Duplicate: nothing is selected.\n" );
        return true;
    }
    if ( KiwiCmd_Active() )
    {
        // Do not replace a live gesture.
        Sys_Printf( "Duplicate: finish the current command first.\n" );
        return true;
    }

    // Creation bracket: stamp copies without saving unmodified originals.
    Undo_ClearRedo();
    Undo_GeneralStart( "duplicate" );        // stores the POINTER — literals only

    Clone_Selection( grid_sizes[g_qeglobals.d_gridsize] );

    // Match Cmd_OnSelectionClone: refresh the material-derived 2D cull hint.
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        sub_47B940( i->def );
    for ( selbrush_t *j = active_brushes.next;   j != &active_brushes;   j = j->next )
        sub_47B940( j->def );

    Undo_EndBrushList( &selected_brushes );
    Undo_End();

    // Clone_Selection changed only legacy lists; invalidate the typed cache.
    Sel_InvalidateFromLegacy();

    Sys_Printf( "Duplicated the selection.\n" );
    g_nUpdateBits = -1;

    // Match the classic paste/clone tail: enter Move in its paused state.
    KiwiCmd_AfterPaste();
    return true;
}

void KiwiDupe_RegisterCommands()
{
    // Arrays are unbound here; modern Shift+D is Duplicate. Mirrors retain classic ids.
    Radiant_RegisterCommand( "KiwiArrayLinear", 0, 0, KIWI_CMD_ARRAY_LINEAR );
    Radiant_RegisterCommand( "KiwiArrayRadial", 0, 0, KIWI_CMD_ARRAY_RADIAL );
    Radiant_RegisterCommand( "KiwiDuplicate",   0, 0, KIWI_CMD_DUPLICATE );
}

KiwiEditorCommand *KiwiDupe_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_ARRAY_LINEAR )
        return &s_linear;
    if ( commandId == KIWI_CMD_ARRAY_RADIAL )
        return &s_radial;
    return 0;
}

void KiwiDupe_MenuItems()
{
    ImGui::SeparatorText( "Duplicate" );

    const bool canMirror = KiwiDupe_CanMirror();
    const bool canArray  = KiwiDupe_CanArray();

    // Aggregate hover because IsItemHovered after the loop sees only the last button.
    bool mirrorHover = false;
    ImGui::BeginDisabled( !canMirror );
    ImGui::TextDisabled( "Mirror" );
    ImGui::SameLine();
    if ( ImGui::Button( "X##kiwimirror" ) ) Radiant_ExecCommand( (unsigned int)KDUP_ID_FLIP_X );
    mirrorHover = mirrorHover || ImGui::IsItemHovered();
    ImGui::SameLine();
    if ( ImGui::Button( "Y##kiwimirror" ) ) Radiant_ExecCommand( (unsigned int)KDUP_ID_FLIP_Y );
    mirrorHover = mirrorHover || ImGui::IsItemHovered();
    ImGui::SameLine();
    if ( ImGui::Button( "Z##kiwimirror" ) ) Radiant_ExecCommand( (unsigned int)KDUP_ID_FLIP_Z );
    mirrorHover = mirrorHover || ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if ( mirrorHover )
        ImGui::SetTooltip( "Mirrors the selection about the selection mid\n"
                           "(the ported Select_FlipAxis; fixed-size entities\n"
                           "also get their `angles` key flipped)." );

    ImGui::BeginDisabled( !KiwiDupe_CanDuplicate() );
    if ( ImGui::Button( "Duplicate (and move)" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_DUPLICATE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Clones the selection and enters Move, paused —\n"
                           "drag a gizmo handle or type a distance, then RMB/Enter." );

    bool arrayHover = false;               // aggregate both buttons
    ImGui::BeginDisabled( !canArray );
    if ( ImGui::Button( "Array (Linear)" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_ARRAY_LINEAR );
    arrayHover = arrayHover || ImGui::IsItemHovered();
    ImGui::SameLine();
    if ( ImGui::Button( "Array (Radial)" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_ARRAY_RADIAL );
    arrayHover = arrayHover || ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if ( arrayHover )
    {
        if ( !canArray )
            ImGui::SetTooltip( "Select at least one brush, patch or entity." );
        else
            ImGui::SetTooltip( "Linear: click a CONSTRUCTION LINE to array across it\n"
                               "(the count spans the line), or drag to set the step\n"
                               "by hand.  Typed digits are always the count.\n"
                               "Both: a session PIVOT (V, placed in Move/Rotate) is\n"
                               "used as the reference point instead of the selection\n"
                               "mid — for Radial that is the axis it revolves about." );
    }
}
