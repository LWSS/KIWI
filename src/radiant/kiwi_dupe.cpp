#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Duplication UI over ported clone and transform cores. This file owns input
// mapping, preview, command dispatch, and undo bracketing.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s

#include "kiwi_dupe.h"
#include "kiwi_camera.h"              // arrays: the lollipop axis mapping
#include "kiwi_csg.h"                 // cloneability gate
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_conselect.h"        // edge duplication
#include "kiwi_construct.h"        // construction segments
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
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
extern void      Clone_Selection( float gridSize );                     // select.cpp:2508 0x48F0D0
extern void      Select_Move( const float *delta, char bSnap );         // select.cpp:2152 0x48E9C0
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


    // KIWI (2026-09-27 REDO, user: "the array tool doesn't work and needs a redo" - 1. select
    // anything, 2. choose the tool, 3. choose a LINE to array along (Radial: a point to turn
    // around), 4. set the options like Plasticity, 5. confirm).  Plasticity
    // (commands/array/{Rectangular,Radial}ArrayCommand.ts, ArrayFactory.ts): an object picker,
    // a point picker, then a dialog - Distance / Number, or Degrees / Number - with +/- on the
    // count, committed from the dialog.  Here: stage PICK (LMB on a line / a snapped point),
    // stage OPTIONS (the command panel, typed count then length / angle, [ ] count, a lollipop
    // on the last copy of a linear array, P to pick again), then Enter / RMB / Confirm.
    // Nothing is written before the confirm.
    const kiwiNumField_t KARR_FIELDS_LIN[2] = { { "count", KNUM_COUNT, false },
                                                { "distance", KNUM_LENGTH, false } };
    const kiwiNumField_t KARR_FIELDS_RAD[2] = { { "count", KNUM_COUNT, false },
                                                { "angle", KNUM_ANGLE, false } };
    const float KARR_PICK_PX = 12.0f;       // a brush edge this near the click is a line

    enum karrStage_t { KARR_STAGE_PICK = 0, KARR_STAGE_OPTIONS };
    enum karrAxis_t  { KARR_AXIS_Z = 0, KARR_AXIS_X, KARR_AXIS_Y, KARR_AXIS_CPLANE, KARR_AXIS_COUNT };

    struct karrBox_t { float mins[3], maxs[3]; };
    struct karrSeg_t { float a[3], b[3]; int first; };  // first: 0 = a brush-made shape

    // A turn of `deg` about unit axis u in the stores' sense (negated degrees, right-hand
    // rule): what Select_RotateAxis does for brushes (kiwi_transform.cpp AxisMatrix); its
    // transpose is the row form Select_ApplyMatrix_SelectedBrushes takes.
    void AxisTurn( const float u[3], float deg, float m[3][3] )
    {
        const float rad = -deg * 3.14159265358979323846f / 180.0f;
        const float c = cosf( rad ), s = sinf( rad );
        const float ux[3][3] = { {  0.0f, -u[2],  u[1] },
                                 {  u[2],  0.0f, -u[0] },
                                 { -u[1],  u[0],  0.0f } };
        for ( int a = 0; a < 3; ++a )
            for ( int b = 0; b < 3; ++b )
                m[a][b] = ( a == b ? c : 0.0f ) + ( 1.0f - c ) * u[a] * u[b] + s * ux[a][b];
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

    class KiwiArrayCommand : public KiwiEditorCommand
    {
    public:
        explicit KiwiArrayCommand( bool radial ) : m_radial( radial ) {}

        const char *Name() const override { return m_radial ? "Radial Array" : "Linear Array"; }
        bool CanExecute() override { return KiwiDupe_CanArray(); }

        // Picking the line / centre snaps like drawing; the options stage (the lollipop
        // drag) is a transform: raw, Ctrl snaps.
        kiwiSnapCtx_t SnapContext() const override
        {
            return m_stage == KARR_STAGE_PICK ? KSNAPCTX_CONSTRUCT : KSNAPCTX_TRANSFORM;
        }
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_stage == KARR_STAGE_OPTIONS && m_invalid; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t linPick[] = { { "LMB", "Pick a line to array along" }, { "[ ]", "Count" } };
            static const kiwiPrompt_t radPick[] = { { "LMB", "Pick the centre" },            { "[ ]", "Count" } };
            static const kiwiPrompt_t lin[]     = { { "[ ]", "Count" }, { "F", "Flip" },  { "P", "Pick another line" } };
            static const kiwiPrompt_t rad[]     = { { "[ ]", "Count" }, { "A", "Axis" },  { "P", "Pick another centre" } };
            if ( m_stage == KARR_STAGE_PICK )
            {
                *out = m_radial ? radPick : linPick;
                return 2;
            }
            *out = m_radial ? rad : lin;
            return 3;
        }

        // Typed: the count, then (Tab) the distance / spacing or the angle.
        int NumericFields( const kiwiNumField_t **out ) const override
        {
            *out = m_radial ? KARR_FIELDS_RAD : KARR_FIELDS_LIN;
            return 2;
        }
        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 0 )
                *out = (float)m_count;
            else if ( field == 1 )
                *out = m_radial ? m_angle : ( m_spacingMode ? m_spacing : m_distance );
            else
                return false;
            return true;
        }
        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( !has )
                return;                                  // cleared: keep what is there
            // Counts and angles are not lengths: undo the numeric layer's length scale.
            if ( field == 0 )
                SetCount( (int)floorf( Units_ToDisplay( world ) + 0.5f ) );
            else if ( field == 1 && m_radial )
                m_angle = ClampAngle( Units_ToDisplay( world ) );
            else if ( field == 1 )
                SetLength( world );
            Recompute();
            g_nUpdateBits |= 1;
        }

        // The Plasticity dialog: shown once there is something to set.
        int CommandOptions( const kiwiOption_t **out ) const override
        {
            if ( m_stage != KARR_STAGE_OPTIONS )
                return 0;
            static const char *const s_axes[KARR_AXIS_COUNT] = { "Z (up)", "X", "Y", "Construction plane" };
            static const kiwiOption_t linDist[] = {
                { "Count",                    KOPT_INT,      nullptr, 0,               0, KARR_MIN_COUNT, KARR_MAX_COUNT, -1 },
                { "Fixed spacing",            KOPT_TOGGLE,   nullptr, 0,               0, 0, 1,     -1 },
                { "Distance (first to last)", KOPT_NUMFIELD, nullptr, 0,               1, 1, 8192,  -1 },
                { "Flip direction",           KOPT_TOGGLE,   nullptr, 0,               0, 0, 1,     -1 },
            };
            static const kiwiOption_t linSpace[] = {
                { "Count",                    KOPT_INT,      nullptr, 0,               0, KARR_MIN_COUNT, KARR_MAX_COUNT, -1 },
                { "Fixed spacing",            KOPT_TOGGLE,   nullptr, 0,               0, 0, 1,     -1 },
                { "Spacing",                  KOPT_NUMFIELD, nullptr, 0,               1, 1, 4096,  -1 },
                { "Flip direction",           KOPT_TOGGLE,   nullptr, 0,               0, 0, 1,     -1 },
            };
            static const kiwiOption_t rad[] = {
                { "Count",                    KOPT_INT,      nullptr, 0,               0, KARR_MIN_COUNT, KARR_MAX_COUNT, -1 },
                { "Angle (degrees)",          KOPT_INT,      nullptr, 0,               0, 1, 360,   -1 },
                { "Axis",                     KOPT_ENUM,     s_axes,  KARR_AXIS_COUNT, 0, 0, 0,     -1 },
            };
            if ( m_radial )
            {
                *out = rad;
                return 3;
            }
            *out = m_spacingMode ? linSpace : linDist;
            return 4;
        }
        int OptionValue( int opt ) const override
        {
            if ( opt == 0 )
                return m_count;
            if ( m_radial )
                return opt == 1 ? (int)floorf( m_angle + 0.5f ) : ( opt == 2 ? m_axisChoice : 0 );
            if ( opt == 1 ) return m_spacingMode ? 1 : 0;
            if ( opt == 3 ) return m_flip ? 1 : 0;
            return 0;
        }
        void OptionChanged( int opt, int value ) override
        {
            if ( opt == 0 )
            {
                KiwiNum_ClearField( 0 );
                SetCount( value );
            }
            else if ( m_radial && opt == 1 )
            {
                KiwiNum_ClearField( 1 );
                m_angle = ClampAngle( (float)value );
            }
            else if ( m_radial && opt == 2 )
                m_axisChoice = value;
            else if ( !m_radial && opt == 1 )
                SetSpacingMode( value != 0 );
            else if ( !m_radial && opt == 3 )
                m_flip = value != 0;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( mods )
                return false;
            if ( vk == 0xDD || vk == 0xDB )              // ] / [ : one more / one fewer
            {
                KiwiNum_ClearField( 0 );
                SetCount( m_count + ( vk == 0xDD ? 1 : -1 ) );
            }
            else if ( vk == 'P' && m_stage == KARR_STAGE_OPTIONS )
            {
                m_stage = KARR_STAGE_PICK;                // pick the line / centre again
                m_grab  = false;
                KiwiCmd_Resume();
            }
            else if ( vk == 'F' && !m_radial && m_stage == KARR_STAGE_OPTIONS )
                m_flip = !m_flip;
            else if ( vk == 'A' && m_radial && m_stage == KARR_STAGE_OPTIONS )
                m_axisChoice = ( m_axisChoice + 1 ) % KARR_AXIS_COUNT;
            else
                return false;
            Recompute();
            return true;
        }

        // Stage PICK takes every click; OPTIONS takes a click on another line (linear).
        bool PressIntercept( int imgX, int imgY ) override
        {
            if ( m_radial )
            {
                if ( m_stage != KARR_STAGE_PICK )
                    return false;
                if ( !m_snap.valid )
                {
                    Sys_Printf( "Radial Array: no point under the cursor.\n" );
                    return true;
                }
                Copy3( m_snap.position, m_center );
                EnterOptions();
                return true;
            }
            float a[3], b[3];
            if ( !LineAt( imgX, imgY, a, b ) )
            {
                if ( m_stage != KARR_STAGE_PICK )
                    return false;
                Sys_Printf( "Linear Array: click a construction line or a brush edge.\n" );
                return true;
            }
            if ( !AdoptLine( a, b ) )
            {
                Sys_Printf( "Linear Array: that line has no length - pick another.\n" );
                return true;
            }
            EnterOptions();
            return true;
        }

        // Linear: the handle sits on the last copy and pulls it along the line.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_radial || m_stage != KARR_STAGE_OPTIONS || !m_lineHave || !outAnchor || !outDir )
                return false;
            float dir[3];
            Direction( dir );
            Mad3( m_ref, dir, m_distance, outAnchor );
            Copy3( dir, outDir );
            return true;
        }
        void HandleGrab( bool held ) override { m_grab = held; }
        void Rebase() override { LatchStart(); }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || m_stage != KARR_STAGE_OPTIONS )
                return false;
            if ( m_radial )
                Copy3( m_center, out3 );
            else
            {
                float dir[3];
                Direction( dir );
                Mad3( m_ref, dir, m_distance, out3 );
            }
            return true;
        }

        bool Begin() override
        {
            // KIWI (2026-09-27, user: "Stop converting my selection to an object, it's highly
            // annoying"): no conversion - every selected thing arrays as itself (kiwi_conselect.h).
            Reset();
            GatherSources();
            if ( m_legacyCount + m_conCount <= 0 )
            {
                Sys_Printf( "%s: nothing to copy - select objects, edges, faces, lines or regions "
                            "(points have no shape).\n", Name() );
                return false;
            }
            Recompute();
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            if ( m_stage == KARR_STAGE_PICK && !m_radial )
            {
                int x, y;
                m_hoverHave = KiwiCmd_LastCursor( &x, &y ) && LineAt( x, y, m_hoverA, m_hoverB );
            }
            else if ( m_stage == KARR_STAGE_OPTIONS && !m_radial && m_grab )
                DragDistance();
            g_nUpdateBits |= 1;
        }

        void Commit() override
        {
            if ( m_stage != KARR_STAGE_OPTIONS || m_invalid )
            {
                Sys_Printf( "%s: nothing done - %s.\n", Name(),
                            m_stage != KARR_STAGE_OPTIONS ? ( m_radial ? "no centre picked" : "no line picked" )
                                                          : "the copies would sit on each other" );
                Reset();
                return;
            }
            Perform();
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            Reset();                                     // nothing was written
            g_nUpdateBits |= 1;
        }

        int LineBudget() const override
        {
            const int per  = (int)m_boxes.size() * 12 + (int)m_segs.size();
            const int want = ( m_count - 1 ) * per + (int)m_segs.size() + 64;
            return want < KCMD_LINE_BUDGET_MAX ? want : KCMD_LINE_BUDGET_MAX;
        }

        void DrawWorld() override
        {
            KiwiLines_Color( KDUP_COL_LINE[0], KDUP_COL_LINE[1], KDUP_COL_LINE[2] );
            if ( !m_radial )
            {
                if ( m_stage == KARR_STAGE_PICK && m_hoverHave )
                    KiwiLines_Add( m_hoverA, m_hoverB );         // the line a click would take
                else if ( m_stage == KARR_STAGE_OPTIONS && m_lineHave )
                    KiwiLines_Add( m_lineA, m_lineB );
            }
            else if ( m_stage == KARR_STAGE_OPTIONS )
            {
                // The centre and the axis the copies turn about.
                const float r = 24.0f * KiwiCam_WorldPerPixel( m_center );
                float a[3], b[3];
                Mad3( m_center, m_axis, -r, a );
                Mad3( m_center, m_axis,  r, b );
                KiwiLines_Add( a, b );
                DrawTick( m_center, r * 0.35f );
            }
            if ( m_stage == KARR_STAGE_OPTIONS )
                DrawGhosts();
        }

    private:
        void Reset()
        {
            m_stage     = KARR_STAGE_PICK;
            m_count     = m_radial ? KARR_DEF_RADIAL : KARR_DEF_LINEAR;
            m_invalid   = false;
            m_hud[0]    = '\0';
            m_boxes.clear();
            m_segs.clear();
            m_shapes.clear();
            m_legacyCount = m_conCount = 0;
            m_hoverHave = m_lineHave = false;
            m_spacingMode = false;
            m_flip      = false;
            m_grab      = false;
            m_haveStart = false;
            m_distance  = m_spacing = 0.0f;
            m_angle     = 360.0f;
            m_axisChoice = KARR_AXIS_Z;
        }

        // What the array copies: the selected objects (their boxes for the preview) and the
        // construction shapes KiwiConSel_ArrayShapes makes of everything else (their
        // segments); m_ref = their centre.
        void GatherSources()
        {
            float lo[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
            float hi[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
            auto grow = [&]( const float *p )
            {
                for ( int k = 0; k < 3; ++k )
                {
                    if ( p[k] < lo[k] ) lo[k] = p[k];
                    if ( p[k] > hi[k] ) hi[k] = p[k];
                }
            };
            for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            {
                if ( !Cloneable( b ) )
                    continue;
                ++m_legacyCount;
                if ( (int)m_boxes.size() < KARR_MAX_SOURCE )
                {
                    karrBox_t box;
                    Copy3( b->def->mins, box.mins );
                    Copy3( b->def->maxs, box.maxs );
                    m_boxes.push_back( box );
                }
                grow( b->def->mins );
                grow( b->def->maxs );
            }
            // Construction, brush edges / faces, regions: each as itself, nothing converted.
            KiwiConSel_ArrayShapes( &m_shapes );
            m_conCount = (int)m_shapes.size();
            for ( size_t i = 0; i < m_shapes.size(); ++i )
            {
                const kconObject_t &o = m_shapes[i].shape;
                const int segs = KiwiCon_SegmentCount( o );
                for ( int s = 0; s < segs; ++s )
                {
                    karrSeg_t sg;
                    if ( !KiwiCon_SegmentWorld( o, s, sg.a, sg.b ) )
                        break;
                    sg.first = m_shapes[i].firstCopy;
                    grow( sg.a );
                    grow( sg.b );
                    if ( (int)m_segs.size() < KARR_MAX_SOURCE )
                        m_segs.push_back( sg );
                }
            }
            for ( int k = 0; k < 3; ++k )
                m_ref[k] = lo[k] <= hi[k] ? ( lo[k] + hi[k] ) * 0.5f : 0.0f;
        }

        // The line under image pixel (x, y): a construction segment or a brush edge.
        bool LineAt( int x, int y, float a[3], float b[3] ) const
        {
            float ca[3], cb[3], cpx = 0.0f;
            int   obj = -1, seg = -1;
            const bool con = KiwiCon_PickSegmentAt( x, y, ca, cb, &obj, &seg, &cpx );
            float ea[3], eb[3], epx = 1.0e30f;
            bool  edge = false;
            ray_t ray;
            if ( Pick_RayFromImagePos( x, y, &ray ) )
            {
                const pick_result_t hit = Pick( ray, SEL_MASK_EDGE );
                if ( hit.valid && hit.item.kind == SEL_EDGE && hit.screenDist <= KARR_PICK_PX
                  && Sel_EdgeEnds( hit.item, ea, eb ) )
                {
                    edge = true;
                    epx  = hit.screenDist;
                }
            }
            if ( con && ( !edge || cpx <= epx ) )
            {
                Copy3( ca, a );
                Copy3( cb, b );
                return true;
            }
            if ( !edge )
                return false;
            Copy3( ea, a );
            Copy3( eb, b );
            return true;
        }

        // The line runs from its end nearer the selection to the far end; its length is
        // the first distance offered (the copies fill the line).  False: no length.
        bool AdoptLine( const float *a, const float *b )
        {
            float d[3];
            Sub3( b, a, d );
            const float len = Len3( d );
            if ( len < KARR_MIN_OFFSET )
                return false;
            float da[3], db[3];
            Sub3( a, m_ref, da );
            Sub3( b, m_ref, db );
            const bool bNearer = Len3( db ) < Len3( da );
            Copy3( bNearer ? b : a, m_lineA );
            Copy3( bNearer ? a : b, m_lineB );
            Sub3( m_lineB, m_lineA, m_lineDir );
            for ( int k = 0; k < 3; ++k )
                m_lineDir[k] /= len;
            m_lineLen  = len;
            m_lineHave = true;
            m_flip     = false;
            m_distance = len;
            m_spacing  = len / (float)( m_count - 1 );
            return true;
        }

        void EnterOptions()
        {
            m_stage = KARR_STAGE_OPTIONS;
            Recompute();
            KiwiCmd_Pause();                             // the handle and the panel drive now
            g_nUpdateBits |= 1;
        }

        void Direction( float out[3] ) const
        {
            for ( int k = 0; k < 3; ++k )
                out[k] = m_flip ? -m_lineDir[k] : m_lineDir[k];
        }

        void SetCount( int n )
        {
            m_count = n < KARR_MIN_COUNT ? KARR_MIN_COUNT : ( n > KARR_MAX_COUNT ? KARR_MAX_COUNT : n );
            Recompute();
        }

        // A typed / panel length is the distance (first to last) or, fixed, the spacing.
        void SetLength( float world )
        {
            if ( world < KARR_MIN_OFFSET )
                world = KARR_MIN_OFFSET;
            if ( m_spacingMode ) m_spacing  = world;
            else                 m_distance = world;
        }

        void SetSpacingMode( bool on )
        {
            m_spacingMode = on;
            KiwiNum_ClearField( 1 );
            KiwiNum_SetFieldLabel( 1, on ? "spacing" : "distance" );
        }

        static float ClampAngle( float deg )
        {
            return deg < 1.0f ? 1.0f : ( deg > 360.0f ? 360.0f : deg );
        }

        void AxisVector( float out[3] ) const
        {
            out[0] = out[1] = out[2] = 0.0f;
            switch ( m_axisChoice )
            {
            case KARR_AXIS_X: out[0] = 1.0f; break;
            case KARR_AXIS_Y: out[1] = 1.0f; break;
            case KARR_AXIS_CPLANE:
                Copy3( KiwiCon_ActivePlane().normal, out );
                if ( Norm3( out ) )
                    break;
                out[0] = out[1] = 0.0f;
                // fall through: a degenerate plane turns about Z
            default:          out[2] = 1.0f; break;
            }
        }

        // The one dependent quantity follows the one the user set: fit = distance ->
        // spacing, fixed spacing -> distance; radial: a full turn spaces the copies around
        // it, a partial one lands the last copy on the angle.
        void Recompute()
        {
            if ( m_radial )
            {
                AxisVector( m_axis );
                m_stepDeg = m_angle >= 359.999f ? 360.0f / (float)m_count
                                                : m_angle / (float)( m_count - 1 );
                m_invalid = m_stepDeg < 0.01f;
            }
            else
            {
                if ( m_spacingMode ) m_distance = m_spacing * (float)( m_count - 1 );
                else                 m_spacing  = m_distance / (float)( m_count - 1 );
                float dir[3];
                Direction( dir );
                for ( int k = 0; k < 3; ++k )
                    m_stepVec[k] = dir[k] * m_spacing;
                m_invalid = !m_lineHave || m_spacing < KARR_MIN_OFFSET;
            }
            UpdateHud();
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            return KiwiCmd_LastCursor( &x, &y ) && Pick_RayFromImagePos( x, y, out );
        }

        // The cursor's travel along the array direction is measured from here.
        void LatchStart()
        {
            m_haveStart = false;
            if ( m_radial || !m_lineHave )
                return;
            float dir[3], q[3], rel[3];
            Direction( dir );
            ray_t ray;
            if ( !KiwiCam_AxisPortrayable( dir ) || !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, dir, q ) )
                return;
            Sub3( q, m_ref, rel );
            m_start     = Dot3( rel, dir ) - m_distance;
            m_haveStart = true;
        }

        // Handle drag: the last copy follows the cursor along the line (Ctrl snaps it to
        // aimed geometry, else to the grid lattice).
        void DragDistance()
        {
            if ( !m_haveStart )
                LatchStart();
            float dir[3], q[3], rel[3];
            Direction( dir );
            ray_t ray;
            if ( !m_haveStart || !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, dir, q ) )
                return;
            Sub3( q, m_ref, rel );
            float d = Dot3( rel, dir ) - m_start;
            if ( KiwiCmd_SnapEngaged() )
            {
                float sd = 0.0f;
                bool  major = false;
                if ( m_snap.valid && KiwiSnap_IsGeometry( m_snap.type )
                  && KiwiSnap_AxisDepth( m_snap, m_ref, dir, &sd ) && sd > KARR_MIN_OFFSET )
                    d = sd;
                else
                    d = KiwiSnap_LatticeAxis( d, m_ref, dir, true, &major );
            }
            if ( d < KARR_MIN_OFFSET )
                d = KARR_MIN_OFFSET;                     // never back past the original: F flips
            KiwiNum_ClearField( 1 );
            m_distance = d;
            m_spacing  = d / (float)( m_count - 1 );
            Recompute();
        }

        // Copy i's image of world point `in`.
        void CopyPoint( int i, const float in[3], float out[3] ) const
        {
            if ( !m_radial )
            {
                for ( int k = 0; k < 3; ++k )
                    out[k] = in[k] + m_stepVec[k] * (float)i;
                return;
            }
            float m[3][3], rel[3];
            AxisTurn( m_axis, m_stepDeg * (float)i, m );
            Sub3( in, m_center, rel );
            for ( int a = 0; a < 3; ++a )
                out[a] = m_center[a] + m[a][0] * rel[0] + m[a][1] * rel[1] + m[a][2] * rel[2];
        }

        // Each copy's source boxes (turned with it on a radial array) and construction
        // segments; one box per copy when the budget cannot hold them all.
        void DrawGhosts()
        {
            KiwiLines_Color( m_invalid ? KDUP_COL_BAD[0] : KDUP_COL_GHOST[0],
                             m_invalid ? KDUP_COL_BAD[1] : KDUP_COL_GHOST[1],
                             m_invalid ? KDUP_COL_BAD[2] : KDUP_COL_GHOST[2] );
            const int copies = m_count - 1;
            const int per    = (int)m_boxes.size() * 12 + (int)m_segs.size();
            const bool full  = copies * per + (int)m_segs.size() <= KiwiLines_Remaining();
            // Copy 0 draws only the brush-made shapes laid on the original.
            for ( int i = full ? 0 : 1; i <= copies; ++i )
            {
                if ( full )
                {
                    for ( size_t b = 0; b < m_boxes.size() && i > 0; ++b )
                        if ( !DrawBox( i, m_boxes[b].mins, m_boxes[b].maxs ) )
                            return;
                    for ( size_t s = 0; s < m_segs.size(); ++s )
                    {
                        if ( i < m_segs[s].first )
                            continue;
                        float a[3], c[3];
                        CopyPoint( i, m_segs[s].a, a );
                        CopyPoint( i, m_segs[s].b, c );
                        if ( !KiwiLines_Add( a, c ) )
                            return;
                    }
                }
                else
                {
                    // Too much to show one by one: each copy as the box around all of it.
                    float lo[3], hi[3];
                    SourceBounds( lo, hi );
                    if ( !DrawBox( i, lo, hi ) )
                        return;
                }
            }
        }

        void SourceBounds( float lo[3], float hi[3] ) const
        {
            for ( int k = 0; k < 3; ++k )
            {
                lo[k] =  1.0e30f;
                hi[k] = -1.0e30f;
            }
            for ( size_t b = 0; b < m_boxes.size(); ++b )
                for ( int k = 0; k < 3; ++k )
                {
                    if ( m_boxes[b].mins[k] < lo[k] ) lo[k] = m_boxes[b].mins[k];
                    if ( m_boxes[b].maxs[k] > hi[k] ) hi[k] = m_boxes[b].maxs[k];
                }
            for ( size_t s = 0; s < m_segs.size(); ++s )
                for ( int k = 0; k < 3; ++k )
                {
                    const float v0 = m_segs[s].a[k], v1 = m_segs[s].b[k];
                    if ( v0 < lo[k] ) lo[k] = v0;
                    if ( v1 < lo[k] ) lo[k] = v1;
                    if ( v0 > hi[k] ) hi[k] = v0;
                    if ( v1 > hi[k] ) hi[k] = v1;
                }
        }

        bool DrawBox( int i, const float *mins, const float *maxs ) const
        {
            float v[KIWI_BOX_CORNERS][3], w[KIWI_BOX_CORNERS][3];
            KiwiBox_Corners( mins, maxs, v );
            for ( int c = 0; c < KIWI_BOX_CORNERS; ++c )
                CopyPoint( i, v[c], w[c] );
            for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
                if ( !KiwiLines_Add( w[KIWI_BOX_EDGE[e][0]], w[KIWI_BOX_EDGE[e][1]] ) )
                    return false;
            return true;
        }

        static void DrawTick( const float *c, float r )
        {
            for ( int k = 0; k < 3; ++k )
            {
                float a[3], b[3];
                Copy3( c, a );
                Copy3( c, b );
                a[k] -= r;
                b[k] += r;
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

        void Perform()
        {
            const int copies = m_count - 1;
            int made = 0;

            // Construction first: the brush clones below reselect the legacy stores.
            if ( m_conCount > 0 )
            {
                float m[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
                const float zero[3] = { 0.0f, 0.0f, 0.0f };
                if ( m_radial )
                    AxisTurn( m_axis, m_stepDeg, m );
                made += KiwiConSel_ArrayAdd( m_shapes, m_count, m_radial ? zero : m_stepVec,
                                             m_radial ? m_center : zero, m );
            }

            if ( m_legacyCount > 0 )
            {
                // Every original saved by UndoBegin must be selected again at UndoCommit.
                std::vector<selbrush_t *> originals;
                for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
                    originals.push_back( b );
                // Faces / edges / points picked beside the objects stay picked.
                std::vector<sel_item_t> fine;
                {
                    const selection_t &before = KiwiSel();
                    for ( size_t i = 0; i < before.items.size(); ++i )
                        if ( before.items[i].kind != SEL_OBJECT )
                            fine.push_back( before.items[i] );
                }

                KiwiCmd_UndoBegin( m_radial ? "radial array" : "linear array" );
                std::vector<selbrush_t *> clones;
                for ( int i = 0; i < copies; ++i )
                {
                    // Clone the last copy and move / turn it one more step.
                    Clone_Selection( grid_sizes[g_qeglobals.d_gridsize] );
                    if ( selected_brushes.next == &selected_brushes )
                    {
                        Sys_Printf( "%s: nothing cloneable - stopped after %i cop%s.\n",
                                    Name(), i, i == 1 ? "y" : "ies" );
                        break;
                    }
                    if ( m_radial )
                    {
                        // The ported rotation pattern: pivot in row 0, the turn in rows 1..3.
                        float rot_around[4][3];
                        Copy3( m_center, rot_around[0] );
                        if ( m_axisChoice == KARR_AXIS_CPLANE )
                        {
                            float m[3][3];
                            AxisTurn( m_axis, m_stepDeg, m );
                            for ( int a = 0; a < 3; ++a )
                                for ( int b = 0; b < 3; ++b )
                                    rot_around[1 + a][b] = m[b][a];
                        }
                        else
                            Select_RotateAxis( m_axisChoice == KARR_AXIS_X ? 0 : ( m_axisChoice == KARR_AXIS_Y ? 1 : 2 ),
                                               m_stepDeg, (float (*)[4][3])rot_around );
                        Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], m_stepDeg, 0 );
                    }
                    else
                        Select_Move( m_stepVec, 0 );          // bSnap 0: the step is exact
                    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
                    {
                        clones.push_back( b );
                        sub_47B940( b->def );                // Cmd_OnSelectionClone's material hint
                    }
                }
                made += (int)clones.size();

                // Originals + copies selected, through the typed-to-legacy crossing.
                selection_t &sel = KiwiSel();
                Sel_Clear( sel );
                for ( size_t i = 0; i < originals.size(); ++i )
                    Sel_Add( sel, Sel_MakeObject( originals[i] ) );
                for ( size_t i = 0; i < clones.size(); ++i )
                    Sel_Add( sel, Sel_MakeObject( clones[i] ) );
                for ( size_t i = 0; i < fine.size(); ++i )
                    if ( Sel_BrushLive( fine[i].brush ) )
                        Sel_Add( sel, fine[i] );
                if ( !originals.empty() )
                    sel.active = Sel_MakeObject( originals[0] );
                Sel_SyncToLegacy();
            }
            Sys_Printf( "%s: %i new object%s.\n", Name(), made, made == 1 ? "" : "s" );
        }

        void UpdateHud()
        {
            char a[32], b[32];
            if ( m_stage == KARR_STAGE_PICK )
                _snprintf( m_hud, sizeof( m_hud ), "%s  count %i  ·  %s", m_radial ? "radial array" : "linear array",
                           m_count, m_radial ? "pick the centre" : "pick a line" );
            else if ( m_radial )
            {
                static const char *const s_axisName[KARR_AXIS_COUNT] = { "Z", "X", "Y", "the construction plane normal" };
                _snprintf( m_hud, sizeof( m_hud ), "radial array  %i items over %s deg  (%s deg apart)  about %s",
                           m_count, KiwiFmt_Num( a, sizeof( a ), m_angle ), KiwiFmt_Num( b, sizeof( b ), m_stepDeg ),
                           s_axisName[m_axisChoice] );
            }
            else
            {
                KiwiUnits_Format( a, sizeof( a ), m_distance );
                KiwiUnits_Format( b, sizeof( b ), m_spacing );
                _snprintf( m_hud, sizeof( m_hud ), "linear array  %i items  distance %s  spacing %s%s",
                           m_count, a, b, m_invalid ? "  -  TOO CLOSE" : "" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        const bool    m_radial;
        karrStage_t   m_stage       = KARR_STAGE_PICK;
        int           m_count       = KARR_DEF_LINEAR;
        bool          m_invalid     = false;
        snap_result_t m_snap;
        char          m_hud[192]    = { 0 };

        // Sources.
        int                    m_legacyCount = 0;         // cloneable selected objects
        int                    m_conCount    = 0;         // construction shapes
        std::vector<kconArrayShape_t> m_shapes;           // copied as construction
        std::vector<karrBox_t> m_boxes;                   // preview boxes (capped)
        std::vector<karrSeg_t> m_segs;                    // preview construction segments
        float                  m_ref[3]      = { 0.0f, 0.0f, 0.0f };

        // Linear.
        bool  m_hoverHave   = false;
        float m_hoverA[3]   = { 0.0f, 0.0f, 0.0f };
        float m_hoverB[3]   = { 0.0f, 0.0f, 0.0f };
        bool  m_lineHave    = false;
        float m_lineA[3]    = { 0.0f, 0.0f, 0.0f };        // the end nearer the selection
        float m_lineB[3]    = { 0.0f, 0.0f, 0.0f };
        float m_lineDir[3]  = { 1.0f, 0.0f, 0.0f };
        float m_lineLen     = 0.0f;
        bool  m_spacingMode = false;
        float m_distance    = 0.0f;                       // the original to the last copy
        float m_spacing     = 0.0f;
        bool  m_flip        = false;
        float m_stepVec[3]  = { 0.0f, 0.0f, 0.0f };
        bool  m_grab        = false;
        bool  m_haveStart   = false;
        float m_start       = 0.0f;

        // Radial.
        float m_center[3]   = { 0.0f, 0.0f, 0.0f };
        float m_angle       = 360.0f;                     // the sweep
        int   m_axisChoice  = KARR_AXIS_Z;
        float m_axis[3]     = { 0.0f, 0.0f, 1.0f };
        float m_stepDeg     = 60.0f;
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
    // Anything, each as itself: objects, construction, regions, brush edges / faces.
    return CloneableSelectedCount() > 0 || KiwiConSel_CanArray();
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
            ImGui::SetTooltip( "Select something to array first (points alone have no shape)." );
        else
            ImGui::SetTooltip( "Arrays the selection as it is: objects clone; lines,\n"
                               "regions, brush edges and faces copy as construction.\n"
                               "Linear: pick a line (construction line or brush edge) to\n"
                               "array along, then set count and distance and confirm.\n"
                               "Radial: pick the centre point, then count, angle and\n"
                               "axis, and confirm." );
    }
}
