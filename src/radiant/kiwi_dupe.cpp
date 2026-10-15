#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Duplication UI over ported clone and transform cores. This file owns input
// mapping, preview, command dispatch, and undo bracketing.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg->m_bNoClamp (exact array copies)

#include "kiwi_dupe.h"
#include "kiwi_angles.h"              // Mirror: a model's frame in double
#include "kiwi_camera.h"              // arrays: the lollipop axis mapping
#include "kiwi_csg.h"                 // cloneability gate
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_conselect.h"        // edge duplication
#include "kiwi_construct.h"        // construction segments
#include "kiwi_droptrace.h"        // arrays: a model's oriented box for the preview
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_vec.h"     // vector helpers

#include <imgui/imgui.h>
#include <universal/com_files.h>   // Mirror: the mirrored model twins (FS_ReadFile / raw writes)
#include <universal/com_math.h>    // Mirror: AnglesToAxis (the fallback's bounds centre)
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern void      Clone_Selection( float gridSize );                     // select.cpp:2508 0x48F0D0
extern bool      g_kiwiImportInCallerUndo;                              // map.cpp (KIWI hook)

namespace
{
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
                                                { "spacing", KNUM_LENGTH, false } };
    const kiwiNumField_t KARR_FIELDS_RAD[2] = { { "count", KNUM_COUNT, false },
                                                { "angle", KNUM_ANGLE, false } };
    const float KARR_PICK_PX = 12.0f;       // a brush edge this near the click is a line

    enum karrStage_t { KARR_STAGE_PICK = 0, KARR_STAGE_OPTIONS };
    enum karrAxis_t  { KARR_AXIS_Z = 0, KARR_AXIS_X, KARR_AXIS_Y, KARR_AXIS_CPLANE, KARR_AXIS_COUNT };

    struct karrSeg_t { float a[3], b[3]; int first; };  // first: 0 = a brush-made shape

    // One selected object as the array sees it, as a convex solid: its outline for the
    // preview (a brush's face edges, a model's oriented box, else its bounding box), its
    // points, and - for the natural spacing's separating-axis test - its face normals and
    // edge directions (unit, one per +/- pair).
    struct karrObj_t
    {
        std::vector<karrSeg_t> edges;
        std::vector<float>     verts;    // xyz triples
        std::vector<float>     normals;  // xyz triples
        std::vector<float>     dirs;     // xyz triples
    };

    // KIWI (2026-10-05, user): linear spacing modes.  FIT spreads the copies over a distance
    // (distance / count, the original mode), FIXED keeps a typed spacing, NATURAL lays them
    // end to end - each copy touching the one before it - plus a gap.
    enum karrSpacing_t { KARR_SPACING_FIT = 0, KARR_SPACING_FIXED, KARR_SPACING_NATURAL, KARR_SPACING_COUNT };
    const char *const KARR_SPACING_NAMES[KARR_SPACING_COUNT] = { "Fit to distance", "Fixed", "Natural (end to end)" };
    const char *const KARR_SPACING_FIELD[KARR_SPACING_COUNT] = { "distance", "spacing", "gap" };
    int s_linearSpacingMode = KARR_SPACING_FIT;          // the last mode chosen, kept between uses
    // Natural: more object pairs than this test face normals only (edge pairs cost n^2).
    const int KARR_NATURAL_EDGE_PAIRS = 40000;

    // A turn of `deg` about unit axis u in the stores' sense (negated degrees, right-hand
    // rule): what Select_RotateAxis does for brushes (kiwi_transform.cpp AxisMatrix); its
    // transpose is the row form Select_ApplyMatrix_SelectedBrushes takes.
    void AxisTurn( const float u[3], float deg, float m[3][3] )
    {
        const float rad = -deg * (float)KIWI_PI / 180.0f;
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
            static const kiwiPrompt_t lin[]     = { { "[ ]", "Count" }, { "F", "Reverse" }, { "P", "Pick another line" } };
            static const kiwiPrompt_t rad[]     = { { "[ ]", "Count" }, { "A", "Axis" },  { "P", "Pick another centre" } };
            if ( m_stage == KARR_STAGE_PICK )
            {
                *out = m_radial ? radPick : linPick;
                return 2;
            }
            *out = m_radial ? rad : lin;
            return 3;
        }

        // Typed: the count, then (Tab) the mode's length - distance (fit), spacing (fixed) or
        // gap (natural) - or the angle.
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
            else if ( field == 1 && m_radial )
                *out = m_angle;
            else if ( field == 1 )
                *out = m_spacingMode == KARR_SPACING_FIT   ? m_distance
                     : m_spacingMode == KARR_SPACING_FIXED ? m_spacing : m_gap;
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

        // The Plasticity dialog: shown once there is something to set.  Linear rows: count,
        // spacing mode, the mode's length (distance / spacing / gap), reverse.
        int CommandOptions( const kiwiOption_t **out ) const override
        {
            if ( m_stage != KARR_STAGE_OPTIONS )
                return 0;
            static const char *const s_axes[KARR_AXIS_COUNT] = { "Z (up)", "X", "Y", "Construction plane" };
            static const kiwiOption_t lin[KARR_SPACING_COUNT][4] = {
                {
                    { "Count",                    KOPT_INT,      nullptr,            0,                  0, KARR_MIN_COUNT, KARR_MAX_COUNT, -1 },
                    { "Spacing",                  KOPT_ENUM,     KARR_SPACING_NAMES, KARR_SPACING_COUNT, 0, 0, 0,     -1 },
                    { "Distance (first to last)", KOPT_NUMFIELD, nullptr,            0,                  1, 1, 8192,  -1 },
                    { "Reverse direction",        KOPT_TOGGLE,   nullptr,            0,                  0, 0, 1,     -1 },
                },
                {
                    { "Count",                    KOPT_INT,      nullptr,            0,                  0, KARR_MIN_COUNT, KARR_MAX_COUNT, -1 },
                    { "Spacing",                  KOPT_ENUM,     KARR_SPACING_NAMES, KARR_SPACING_COUNT, 0, 0, 0,     -1 },
                    { "Spacing",                  KOPT_NUMFIELD, nullptr,            0,                  1, 1, 4096,  -1 },
                    { "Reverse direction",        KOPT_TOGGLE,   nullptr,            0,                  0, 0, 1,     -1 },
                },
                {
                    { "Count",                    KOPT_INT,      nullptr,            0,                  0, KARR_MIN_COUNT, KARR_MAX_COUNT, -1 },
                    { "Spacing",                  KOPT_ENUM,     KARR_SPACING_NAMES, KARR_SPACING_COUNT, 0, 0, 0,     -1 },
                    { "Gap",                      KOPT_NUMFIELD, nullptr,            0,                  1, -1024, 4096, -1 },
                    { "Reverse direction",        KOPT_TOGGLE,   nullptr,            0,                  0, 0, 1,     -1 },
                },
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
            *out = lin[m_spacingMode];
            return 4;
        }
        int OptionValue( int opt ) const override
        {
            if ( opt == 0 )
                return m_count;
            if ( m_radial )
                return opt == 1 ? (int)floorf( m_angle + 0.5f ) : ( opt == 2 ? m_axisChoice : 0 );
            if ( opt == 1 ) return m_spacingMode;
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
                SetSpacingMode( value );
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
            if ( !m_radial )
                KiwiNum_SetFieldLabel( 1, KARR_SPACING_FIELD[m_spacingMode] );
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
            const int per  = m_objEdges + (int)m_segs.size();
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
            m_objs.clear();
            m_objEdges  = 0;
            m_segs.clear();
            m_shapes.clear();
            m_legacyCount = m_conCount = 0;
            m_hoverHave = m_lineHave = false;
            m_spacingMode = s_linearSpacingMode;
            m_gap       = 0.0f;
            m_natHave   = false;
            m_flip      = false;
            m_grab      = false;
            m_haveStart = false;
            m_distance  = m_spacing = 0.0f;
            m_angle     = 360.0f;
            m_axisChoice = KARR_AXIS_Z;
        }

        // A unit axis into `list` (xyz triples) unless it or its opposite is already there.
        static void AddAxis( std::vector<float> &list, const float *v )
        {
            float u[3];
            Copy3( v, u );
            const float len = Len3( u );
            if ( len < 1.0e-6f )
                return;
            for ( int k = 0; k < 3; ++k )
                u[k] /= len;
            for ( size_t i = 0; i + 2 < list.size(); i += 3 )
                if ( fabsf( Dot3( &list[i], u ) ) > 0.99999f )
                    return;
            list.insert( list.end(), u, u + 3 );
        }

        // One object as a convex solid (karrObj_t).  A model is its oriented box (its proxy
        // brush is the class box, not the model); a brush is its face edges, each shared edge
        // once; anything without windings is its bounding box.
        static void ObjectShape( selbrush_t *b, karrObj_t &o )
        {
            auto addEdge = [&]( const float *p, const float *q )
            {
                karrSeg_t s;
                Copy3( p, s.a );
                Copy3( q, s.b );
                s.first = 1;
                o.edges.push_back( s );
                float d[3];
                Sub3( q, p, d );
                AddAxis( o.dirs, d );
            };
            auto addBox = [&]( float c[KIWI_BOX_CORNERS][3] )
            {
                for ( int i = 0; i < KIWI_BOX_CORNERS; ++i )
                    o.verts.insert( o.verts.end(), c[i], c[i] + 3 );
                for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
                    addEdge( c[KIWI_BOX_EDGE[e][0]], c[KIWI_BOX_EDGE[e][1]] );
                for ( int k = 1; k <= 4; k <<= 1 )      // a box's face normals are its edge axes
                {
                    float d[3];
                    Sub3( c[k], c[0], d );
                    AddAxis( o.normals, d );
                }
            };

            float mins[3], maxs[3], angles[3], origin[3], wmins[3], wmaxs[3];
            float scale = 1.0f;
            float c[KIWI_BOX_CORNERS][3];
            if ( KiwiDrop_IsModelEntity( b )
              && KiwiDrop_GetModelInfo( b, mins, maxs, angles, &scale, origin )
              && KiwiDrop_TransformBounds( mins, maxs, angles, scale, wmins, wmaxs, c ) )
            {
                for ( int i = 0; i < KIWI_BOX_CORNERS; ++i )
                    Add3( c[i], origin, c[i] );
                addBox( c );
                return;
            }

            const brush_t *def = b->def;
            for ( int f = 0; def->faces && f < def->faceCount; ++f )
            {
                const winding_t *w = def->faces[f].w;
                if ( !w || w->numpoints < 3 )
                    continue;
                AddAxis( o.normals, def->faces[f].plane.normal );
                for ( int i = 0, j = w->numpoints - 1; i < w->numpoints; j = i++ )
                {
                    o.verts.insert( o.verts.end(), w->p[i], w->p[i] + 3 );
                    // the neighbouring face walks this edge the other way: keep one of the two
                    const float *p = w->p[j], *q = w->p[i];
                    if ( p[0] < q[0] || ( p[0] == q[0] && ( p[1] < q[1] || ( p[1] == q[1] && p[2] < q[2] ) ) ) )
                        addEdge( p, q );
                }
            }
            if ( !o.verts.empty() )
                return;
            o.normals.clear();
            o.dirs.clear();
            KiwiBox_Corners( def->mins, def->maxs, c );
            addBox( c );
        }

        // What the array copies: the selected objects (their outlines for the preview and the
        // natural spacing) and the construction shapes KiwiConSel_ArrayShapes makes of
        // everything else (their segments); m_ref = their centre.
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
                karrObj_t o;
                ObjectShape( b, o );
                for ( size_t v = 0; v + 2 < o.verts.size(); v += 3 )
                    grow( &o.verts[v] );
                m_objEdges += (int)o.edges.size();
                m_objs.push_back( std::move( o ) );
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

        // The line runs from its end nearer the selection to the far end and gives the
        // direction; a fixed spacing starts as the line divided by the count.  False: no length.
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

        // A typed / panel length is the mode's: the distance (fit), the spacing (fixed) or the
        // gap (natural; negative overlaps the copies, down to the TOO CLOSE minimum).
        void SetLength( float world )
        {
            if ( m_spacingMode == KARR_SPACING_NATURAL )
            {
                m_gap = world;
                return;
            }
            if ( world < KARR_MIN_OFFSET )
                world = KARR_MIN_OFFSET;
            if ( m_spacingMode == KARR_SPACING_FIXED ) m_spacing  = world;
            else                                       m_distance = world;
        }

        // Switching keeps what the copies had (Recompute derives the rest), so nothing jumps;
        // the choice sticks for the next array.
        void SetSpacingMode( int mode )
        {
            m_spacingMode = ( mode >= 0 && mode < KARR_SPACING_COUNT ) ? mode : KARR_SPACING_FIT;
            s_linearSpacingMode = m_spacingMode;
            KiwiNum_ClearField( 1 );
            KiwiNum_SetFieldLabel( 1, KARR_SPACING_FIELD[m_spacingMode] );
        }

        // An object's points projected on unit axis u.
        static void Span( const karrObj_t &o, const float *u, float *lo, float *hi )
        {
            *lo =  1.0e30f;
            *hi = -1.0e30f;
            for ( size_t i = 0; i + 2 < o.verts.size(); i += 3 )
            {
                const float p = Dot3( &o.verts[i], u );
                if ( p < *lo ) *lo = p;
                if ( p > *hi ) *hi = p;
            }
        }

        // How far `a`, slid along unit d, travels until it stops overlapping `b`: the end of
        // the slide's overlap interval, by separating axes (both face-normal sets and, with
        // `edgePairs`, every edge-direction cross product - exact for convex solids).  Below
        // zero when the two never overlap anywhere on that slide.
        static float ClearStep( const karrObj_t &a, const karrObj_t &b, const float d[3], bool edgePairs )
        {
            const float KTOUCH = 1.0e-3f;                // touching is not overlapping
            float enter = -1.0e30f, leave = 1.0e30f;
            auto axis = [&]( const float *u ) -> bool    // false: apart on u for every slide
            {
                float aLo, aHi, bLo, bHi;
                Span( a, u, &aLo, &aHi );
                Span( b, u, &bLo, &bHi );
                const float ud = Dot3( u, d );
                if ( fabsf( ud ) < 1.0e-6f )
                    return aHi > bLo + KTOUCH && bHi > aLo + KTOUCH;
                float t0 = ( bLo - aHi ) / ud, t1 = ( bHi - aLo ) / ud;
                if ( t0 > t1 )
                {
                    const float s = t0;
                    t0 = t1;
                    t1 = s;
                }
                if ( t0 > enter ) enter = t0;
                if ( t1 < leave ) leave = t1;
                return enter < leave;
            };
            for ( size_t i = 0; i + 2 < a.normals.size(); i += 3 )
                if ( !axis( &a.normals[i] ) )
                    return -1.0f;
            for ( size_t i = 0; i + 2 < b.normals.size(); i += 3 )
                if ( !axis( &b.normals[i] ) )
                    return -1.0f;
            for ( size_t i = 0; edgePairs && i + 2 < a.dirs.size(); i += 3 )
                for ( size_t j = 0; j + 2 < b.dirs.size(); j += 3 )
                {
                    float u[3];
                    Cross3( &a.dirs[i], &b.dirs[j], u );
                    if ( !Norm3( u, 1.0e-3f ) )
                        continue;                        // parallel edges: no new axis
                    if ( !axis( u ) )
                        return -1.0f;
                }
            return enter < leave ? leave : -1.0f;
        }

        // KIWI (2026-10-05, user: "lay the brushes on top of each other perfectly in the array
        // (and it works with multiple brushes) ... lay their faces perfectly upon each other",
        // then "it shouldnt have a gap here"): the smallest step along dir after which no
        // object of the copy overlaps any object of the original - so the copies touch, and
        // faces turned toward each other lie on each other, whatever the shapes and however
        // the line runs across them.  (The first version matched the end objects' end faces
        // and otherwise fell back to the selection's length along the line, which on a line
        // slanted across the objects left a gap.)  Construction alone: its length along dir.
        // Cached per direction; the gap is added by Recompute.
        float NaturalStep( const float dir[3] )
        {
            if ( m_natHave && Dot3( dir, m_natDir ) > 0.999999f )
                return m_natStep;
            float step = 0.0f;
            if ( !m_objs.empty() )
            {
                // Pairs apart across the line can never meet, wherever the copy slides.
                float p1[3], p2[3];
                const float up[3] = { 0.0f, 0.0f, 1.0f }, side[3] = { 1.0f, 0.0f, 0.0f };
                Cross3( dir, fabsf( dir[2] ) < 0.9f ? up : side, p1 );
                Norm3( p1 );
                Cross3( dir, p1, p2 );
                const size_t n = m_objs.size();
                std::vector<float> span( n * 4 );
                for ( size_t i = 0; i < n; ++i )
                {
                    Span( m_objs[i], p1, &span[4 * i],     &span[4 * i + 1] );
                    Span( m_objs[i], p2, &span[4 * i + 2], &span[4 * i + 3] );
                }
                auto meet = [&]( size_t i, size_t j )
                {
                    return span[4 * i]     < span[4 * j + 1] && span[4 * j]     < span[4 * i + 1]
                        && span[4 * i + 2] < span[4 * j + 3] && span[4 * j + 2] < span[4 * i + 3];
                };
                size_t pairs = 0;
                for ( size_t i = 0; i < n; ++i )
                    for ( size_t j = 0; j < n; ++j )
                        pairs += meet( i, j ) ? 1 : 0;
                const bool edgePairs = pairs <= (size_t)KARR_NATURAL_EDGE_PAIRS;
                for ( size_t i = 0; i < n; ++i )             // i: in the copy
                    for ( size_t j = 0; j < n; ++j )         // j: in the original
                    {
                        if ( !meet( i, j ) )
                            continue;
                        const float t = ClearStep( m_objs[i], m_objs[j], dir, edgePairs );
                        if ( t > step )
                            step = t;
                    }
            }
            if ( step < KARR_MIN_OFFSET )
            {
                float lo = 1.0e30f, hi = -1.0e30f;
                for ( size_t i = 0; i < m_objs.size(); ++i )
                {
                    float a, b;
                    Span( m_objs[i], dir, &a, &b );
                    if ( a < lo ) lo = a;
                    if ( b > hi ) hi = b;
                }
                for ( size_t s = 0; s < m_segs.size(); ++s )
                {
                    const float pa = Dot3( m_segs[s].a, dir ), pb = Dot3( m_segs[s].b, dir );
                    if ( pa < lo ) lo = pa;
                    if ( pb < lo ) lo = pb;
                    if ( pa > hi ) hi = pa;
                    if ( pb > hi ) hi = pb;
                }
                step = hi > lo ? hi - lo : 0.0f;
            }
            Copy3( dir, m_natDir );
            m_natStep = step;
            m_natHave = true;
            return step;
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

        // Linear: fit derives the spacing from the distance; fixed and natural (end to end
        // along the direction, plus the gap) derive the distance to the last copy from the
        // spacing; radial: a full turn spaces the copies around it, a partial one lands the
        // last copy on the angle.
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
                float dir[3];
                Direction( dir );
                if ( m_spacingMode == KARR_SPACING_FIT )
                    m_spacing = m_distance / (float)( m_count - 1 );
                else
                {
                    if ( m_spacingMode == KARR_SPACING_NATURAL )
                        m_spacing = NaturalStep( dir ) + m_gap;
                    m_distance = m_spacing * (float)( m_count - 1 );
                }
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
            if ( m_spacingMode == KARR_SPACING_NATURAL )
            {
                // End to end has one spacing: the handle sets how many fit.
                if ( m_spacing > KARR_MIN_OFFSET )
                {
                    KiwiNum_ClearField( 0 );
                    SetCount( (int)floorf( d / m_spacing + 0.5f ) + 1 );
                }
                return;
            }
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

        // Each copy's object outlines (brush edges, models' oriented boxes - turned with it
        // on a radial array) and construction segments; one box per copy when the budget
        // cannot hold them all.  KIWI (2026-10-05, user: "the linear array preview is WAY
        // wrong, it twists the models"): the ghosts were each object's world-axis bounding
        // box, which on anything rotated - a bench along a 3-degree line - leans away from
        // the object and the copies looked twisted; the clones themselves were right.
        void DrawGhosts()
        {
            KiwiLines_Color( m_invalid ? KDUP_COL_BAD[0] : KDUP_COL_GHOST[0],
                             m_invalid ? KDUP_COL_BAD[1] : KDUP_COL_GHOST[1],
                             m_invalid ? KDUP_COL_BAD[2] : KDUP_COL_GHOST[2] );
            const int copies = m_count - 1;
            const int per    = m_objEdges + (int)m_segs.size();
            const bool full  = copies * per + (int)m_segs.size() <= KiwiLines_Remaining();
            auto addSeg = [&]( int i, const karrSeg_t &s )
            {
                float a[3], c[3];
                CopyPoint( i, s.a, a );
                CopyPoint( i, s.b, c );
                return KiwiLines_Add( a, c );
            };
            // Copy 0 draws only the brush-made shapes laid on the original.
            for ( int i = full ? 0 : 1; i <= copies; ++i )
            {
                if ( full )
                {
                    for ( size_t o = 0; o < m_objs.size() && i > 0; ++o )
                        for ( size_t e = 0; e < m_objs[o].edges.size(); ++e )
                            if ( !addSeg( i, m_objs[o].edges[e] ) )
                                return;
                    for ( size_t s = 0; s < m_segs.size(); ++s )
                    {
                        if ( i < m_segs[s].first )
                            continue;
                        if ( !addSeg( i, m_segs[s] ) )
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
            for ( size_t o = 0; o < m_objs.size(); ++o )
            {
                const std::vector<float> &v = m_objs[o].verts;
                for ( size_t p = 0; p + 2 < v.size(); p += 3 )
                    for ( int k = 0; k < 3; ++k )
                    {
                        if ( v[p + k] < lo[k] ) lo[k] = v[p + k];
                        if ( v[p + k] > hi[k] ) hi[k] = v[p + k];
                    }
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
                // KIWI (2026-10-05, user: "when undo'ing an array operation, it should undo the
                // entire array, not each element 1 by 1"): each clone is a Map_ImportBuffer that
                // opened and closed its own "import buffer" record - one Ctrl+Z per copy.  The
                // imports join our record instead (as kiwi_pasteplace.cpp does).
                // KIWI (2026-10-05, user: "after about the 4th element, it makes trapezoid brushes
                // instead of the rectangular ... it's creating error spots that are flashing"):
                // each clone pastes through Map_ImportBuffer, which rebuilds the pasted brushes
                // with bFull = 1 - Brush_SnapPlanepts rounds every planept to 0.5 - and each copy
                // was cloned from the copy before it, moved by a fractional step along the line.
                // On a rotated wall every copy was the previous one re-rounded: a face's three
                // points rounding apart tilt it (trapezoids), and the tilted faces met at fragile
                // corners.  Every copy is now cloned from the ORIGINALS with the clamp suspended
                // (the pref itself is unchanged, as in kiwi_split.cpp's cut) and moved / turned
                // by its whole offset at once, so it is the original's exact image.
                const int savedNoClamp = g_PrefsDlg->m_bNoClamp;
                g_PrefsDlg->m_bNoClamp = 1;
                g_kiwiImportInCallerUndo = true;
                for ( int i = 1; i <= copies; ++i )
                {
                    Select_Deselect( 1 );
                    for ( size_t k = 0; k < originals.size(); ++k )
                        Select_Brush( originals[k], 0, 0, 0 );
                    Clone_Selection( grid_sizes[g_qeglobals.d_gridsize] );
                    if ( selected_brushes.next == &selected_brushes )
                    {
                        Sys_Printf( "%s: nothing cloneable - stopped after %i cop%s.\n",
                                    Name(), i - 1, i - 1 == 1 ? "y" : "ies" );
                        break;
                    }
                    if ( m_radial )
                    {
                        // The ported rotation pattern: pivot in row 0, the turn in rows 1..3.
                        const float deg = m_stepDeg * (float)i;
                        float rot_around[4][3];
                        Copy3( m_center, rot_around[0] );
                        if ( m_axisChoice == KARR_AXIS_CPLANE )
                        {
                            float m[3][3];
                            AxisTurn( m_axis, deg, m );
                            for ( int a = 0; a < 3; ++a )
                                for ( int b = 0; b < 3; ++b )
                                    rot_around[1 + a][b] = m[b][a];
                        }
                        else
                            Select_RotateAxis( m_axisChoice == KARR_AXIS_X ? 0 : ( m_axisChoice == KARR_AXIS_Y ? 1 : 2 ),
                                               deg, (float (*)[4][3])rot_around );
                        Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], deg, 0 );
                    }
                    else
                    {
                        float move[3];
                        for ( int k = 0; k < 3; ++k )
                            move[k] = m_stepVec[k] * (float)i;
                        Select_Move( move, 0 );                // bSnap 0: the offset is exact
                    }
                    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
                    {
                        clones.push_back( b );
                        Brush_UpdateSpecialMaterialFlag( b->def );                // Cmd_OnSelectionClone's material hint
                    }
                }
                g_kiwiImportInCallerUndo = false;
                g_PrefsDlg->m_bNoClamp = savedNoClamp;
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
                static const char *const s_mode[KARR_SPACING_COUNT] = { "", "fixed ", "natural " };
                _snprintf( m_hud, sizeof( m_hud ), "linear array  %i items  distance %s  %sspacing %s%s",
                           m_count, a, s_mode[m_spacingMode], b, m_invalid ? "  -  TOO CLOSE" : "" );
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
        std::vector<karrObj_t> m_objs;                    // the selected objects' outlines
        int                    m_objEdges    = 0;         // their edge total
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
        int   m_spacingMode = KARR_SPACING_FIT;           // karrSpacing_t
        float m_distance    = 0.0f;                       // the original to the last copy
        float m_spacing     = 0.0f;
        float m_gap         = 0.0f;                       // natural: added to the touching step
        bool  m_natHave     = false;                      // NaturalStep's cache
        float m_natDir[3]   = { 0.0f, 0.0f, 0.0f };
        float m_natStep     = 0.0f;
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

// ── Mirror (KIWI 2026-10-12) ─────────────────────────────────────────────────────────────
// User: "It flips the selected thing (can be line, brush, even model), in the x/y/z direction.
// Mirror command is engaged, then user presses X Y or Z keys and it previews the flip, right
// click confirms as usual.  Try to keep the model in place while mirroring."
//
// The preview IS the flip, inside the command's undo bracket: brushes and patches go through
// the classic flip core (Select_FlipAxisAbout: Select_ApplyMatrix with the winding swap and
// texture lock); another axis key reverts it exactly (KiwiCmd_UndoCancel - Undo_Undo puts the
// originals back, deselected, stamped with Undo_LastUndoStamp, which re-selects them) and
// flips again.  Construction lines go through their store's absolute-from-baseline matrix arm.
//
// Models (and prefabs, lights, any fixed-size entity): an xmodel cannot be mesh-mirrored, so
// it is re-oriented to the mirror image of its frame with its own left/right flipped back
// (exact for a left/right-symmetric model), and placed so its BOUNDS CENTRE lands on the
// mirrored centre - a model mirrored alone stays where it is instead of jumping across its
// origin.  The pivot is the centre of the selection's bounds (models by their world bounds).
extern void Select_FlipAxisAbout( int axis, const float pivot[3], bool fixedSizeOrigins );  // select.cpp
extern int  Undo_LastUndoStamp();                                                             // undo.cpp

// ── mirrored model assets (KIWI 2026-10-12, user: "make it so that models really mirror") ──
// An xmodel cannot be placed mirrored (rotation + uniform modelscale only), so Mirror gives a
// model its TWIN: the same model mirrored across its own Y (left / right), written once under
// raw/ as "<name>_mir" and reused.  Any world mirror is that twin under a rotation, so with
// the frame Mirror computes the result is exact; mirroring a "_mir" model gives back its
// source.  The v25 files are copied with every Y negated - positions, normals, binormals,
// tangents, bounds, and the collision triangles' plane / svec / tvec (a reflection keeps the
// dot products their inside test is made of) - and each triangle's winding reversed.  Rigid
// models on bone 0 only (static props); others keep the old turn-only approximation.
namespace
{
    const char *KMIR_SUFFIX = "_mir";

    bool MrRead( const std::string &qpath, std::vector<unsigned char> *out )
    {
        void *buf = nullptr;
        const int size = FS_ReadFile( qpath.c_str(), &buf );
        if ( size <= 0 || !buf )
        {
            if ( buf )
                FS_FreeFile( (char *)buf );
            return false;
        }
        if ( out )
            out->assign( (const unsigned char *)buf, (const unsigned char *)buf + size );
        FS_FreeFile( (char *)buf );
        return true;
    }

    // raw/, as kiwi_barbwire.cpp WriteRaw: the map compilers search raw/raw_shared/devraw.
    bool MrWrite( const std::string &qpath, const std::vector<unsigned char> &bytes, std::string *err )
    {
        const int h = FS_FOpenFileWriteToDir( qpath.c_str(), "raw" );
        if ( !h )
        {
            *err = "could not open '" + qpath + "' for writing";
            return false;
        }
        const unsigned wrote = bytes.empty() ? 0u : FS_Write( (const char *)&bytes[0], (unsigned)bytes.size(), h );
        FS_FCloseFile( h );
        if ( wrote != (unsigned)bytes.size() )
        {
            FS_DeleteInDir( (char *)qpath.c_str(), (char *)"raw" );
            *err = "short write to '" + qpath + "'";
            return false;
        }
        return true;
    }

    // `base` + "_mir" within `maxLen` characters (the loaders' 64-byte qpath buffers), cut
    // and tagged with a hash of the full name when longer.
    std::string MrName( const std::string &base, size_t maxLen )
    {
        std::string n = base + KMIR_SUFFIX;
        if ( n.size() <= maxLen )
            return n;
        unsigned h = 2166136261u;
        for ( char c : base )
            h = ( h ^ (unsigned char)c ) * 16777619u;
        char tag[16];
        _snprintf( tag, sizeof( tag ), "_%06x", h & 0xFFFFFFu );
        tag[sizeof( tag ) - 1] = '\0';
        return base.substr( 0, maxLen - strlen( tag ) - strlen( KMIR_SUFFIX ) ) + tag + KMIR_SUFFIX;
    }

    struct mrReader_t
    {
        const std::vector<unsigned char> &d;
        size_t at = 0;
        bool   ok = true;
        bool Need( size_t n ) { if ( !ok || d.size() - at < n || at > d.size() ) ok = false; return ok; }
        unsigned U16() { if ( !Need( 2 ) ) return 0; unsigned short v; memcpy( &v, &d[at], 2 ); at += 2; return v; }
        int      S32() { if ( !Need( 4 ) ) return 0; int v; memcpy( &v, &d[at], 4 ); at += 4; return v; }
        float    F32() { if ( !Need( 4 ) ) return 0.0f; float v; memcpy( &v, &d[at], 4 ); at += 4; return v; }
        std::string CStr()
        {
            std::string s;
            while ( Need( 1 ) )
            {
                const char c = (char)d[at++];
                if ( !c ) break;
                s.push_back( c );
            }
            return s;
        }
    };

    struct mrWriter_t
    {
        std::vector<unsigned char> b;
        void Raw( const void *p, size_t n ) { b.insert( b.end(), (const unsigned char *)p, (const unsigned char *)p + n ); }
        void U8( unsigned v )      { const unsigned char c = (unsigned char)v; Raw( &c, 1 ); }
        void U16( unsigned v )     { const unsigned short s = (unsigned short)v; Raw( &s, 2 ); }
        void S32( int v )          { Raw( &v, 4 ); }
        void F32( float v )        { Raw( &v, 4 ); }
        void CStr( const std::string &s ) { Raw( s.c_str(), s.size() + 1 ); }
    };

    // A mins / maxs pair mirrored across Y (y swaps sides).
    void MrBounds( mrReader_t &r, mrWriter_t &w )
    {
        float v[6];
        for ( float &f : v )
            f = r.F32();
        const float lo = -v[4], hi = -v[1];
        v[1] = lo;
        v[4] = hi;
        for ( float f : v )
            w.F32( f );
    }

    void MrNegY( std::vector<unsigned char> &d, size_t vec3At )
    {
        float y;
        memcpy( &y, &d[vec3At + 4], 4 );
        y = -y;
        memcpy( &d[vec3At + 4], &y, 4 );
    }

    // xmodelsurfs v25 (XModelReadSurface), mirrored in place.
    bool MrSurfs( std::vector<unsigned char> &d, std::string *err )
    {
        mrReader_t r{ d };
        if ( r.U16() != 25 ) { *err = "xmodelsurfs is not version 25"; return false; }
        const unsigned surfs = r.U16();
        for ( unsigned s = 0; s < surfs && r.ok; ++s )
        {
            r.at += 3;                                      // tileMode, the unused u16
            const unsigned verts = r.U16();
            const unsigned tris  = r.U16();
            unsigned lists = 0, rigid = 0;
            for ( ;; )
            {
                const unsigned count = r.U16();
                if ( !r.ok || !count )
                    break;
                const unsigned bone = r.U16();
                if ( bone != 0 ) { *err = "a surface is bound to a bone other than the root"; return false; }
                rigid += count;
                ++lists;
            }
            if ( lists != 1 || rigid != verts ) { *err = "the model is skinned or multi-bone"; return false; }
            for ( unsigned v = 0; v < verts && r.ok; ++v )
            {
                if ( !r.Need( 60 ) )          // normal 12, color 4, uv 8, binormal 12, tangent 12, xyz 12
                    break;
                MrNegY( d, r.at );            // normal
                MrNegY( d, r.at + 24 );       // binormal (after color 4 + uv 8)
                MrNegY( d, r.at + 36 );       // tangent
                MrNegY( d, r.at + 48 );       // position
                r.at += 60;
            }
            for ( unsigned t = 0; t < tris && r.ok; ++t )
            {
                if ( !r.Need( 6 ) )
                    break;
                std::swap( d[r.at + 2], d[r.at + 4] );   // reverse the winding: corners 1 <-> 2
                std::swap( d[r.at + 3], d[r.at + 5] );
                r.at += 6;
            }
        }
        if ( !r.ok || r.at != d.size() ) { *err = "xmodelsurfs is truncated or malformed"; return false; }
        return true;
    }

    // xmodel v25 (XModelLoadConfigFile, XModelLoadCollData, the LOD material names, the
    // per-bone bounds), rebuilt with the LOD files renamed and every Y mirrored.
    bool MrConfig( const std::vector<unsigned char> &in, mrWriter_t &w,
                   std::string lodIn[4], std::string lodOut[4], std::string *err )
    {
        mrReader_t r{ in };
        if ( r.U16() != 25 ) { *err = "xmodel is not version 25"; return false; }
        w.U16( 25 );
        if ( !r.Need( 1 ) ) { *err = "xmodel is truncated"; return false; }
        w.U8( in[r.at++] );                                 // flags
        MrBounds( r, w );                                   // the radius cube
        w.CStr( r.CStr() );                                 // physics preset
        for ( int i = 0; i < 4; ++i )
        {
            w.F32( r.F32() );
            lodIn[i]  = r.CStr();
            lodOut[i] = lodIn[i].empty() ? std::string() : MrName( lodIn[i], 50 );
            w.CStr( lodOut[i] );
        }
        w.S32( r.S32() );                                   // collLod
        const int collSurfs = r.S32();
        if ( collSurfs < 0 || collSurfs > 65536 ) { *err = "bad collision surface count"; return false; }
        w.S32( collSurfs );
        for ( int s = 0; s < collSurfs && r.ok; ++s )
        {
            const int tris = r.S32();
            if ( tris < 0 ) { r.ok = false; break; }
            w.S32( tris );
            for ( int t = 0; t < tris && r.ok; ++t )
                for ( int k = 0; k < 12; ++k )              // plane, svec, tvec: y is element 1
                {
                    const float f = r.F32();
                    w.F32( ( k & 3 ) == 1 ? -f : f );
                }
            MrBounds( r, w );
            w.S32( r.S32() );                               // bone
            w.S32( r.S32() );                               // contents
            w.S32( r.S32() );                               // surfFlags
        }
        for ( int i = 0; i < 4 && r.ok; ++i )
        {
            if ( lodIn[i].empty() )
                continue;
            const unsigned n = r.U16();
            w.U16( n );
            for ( unsigned j = 0; j < n && r.ok; ++j )
                w.CStr( r.CStr() );
        }
        if ( !r.ok || ( in.size() - r.at ) % 24 != 0 ) { *err = "xmodel is truncated or malformed"; return false; }
        while ( r.at < in.size() )
            MrBounds( r, w );                               // per-bone bounds
        return true;
    }
}

// The mirrored twin of xmodel `name` (see above); false + `err` when it cannot be made.
static bool KiwiMirror_ModelAsset( const std::string &name, std::string *out, std::string *err )
{
    // A "_mir" twin whose source is on the search path mirrors back to it.
    const size_t sl = strlen( KMIR_SUFFIX );
    if ( name.size() > sl && !_stricmp( name.c_str() + name.size() - sl, KMIR_SUFFIX ) )
    {
        const std::string base = name.substr( 0, name.size() - sl );
        if ( MrRead( "xmodel/" + base, nullptr ) )
        {
            *out = base;
            return true;
        }
    }
    *out = MrName( name, 55 );
    if ( MrRead( "xmodel/" + *out, nullptr ) )
        return true;                                        // made before: reuse it

    std::vector<unsigned char> cfg;
    if ( !MrRead( "xmodel/" + name, &cfg ) ) { *err = "'xmodel/" + name + "' is not on the search path"; return false; }
    mrWriter_t w;
    std::string lodIn[4], lodOut[4];
    if ( !MrConfig( cfg, w, lodIn, lodOut, err ) || lodIn[0].empty() )
    {
        if ( err->empty() ) *err = "the xmodel has no LOD 0";
        return false;
    }
    // Every file is made and checked before any is written; the xmodel goes last.
    std::vector<std::vector<unsigned char>> surfs( 4 );
    for ( int i = 0; i < 4; ++i )
    {
        if ( lodIn[i].empty() )
            continue;
        if ( !MrRead( "xmodelsurfs/" + lodIn[i], &surfs[i] ) ) { *err = "'xmodelsurfs/" + lodIn[i] + "' is missing"; return false; }
        if ( !MrSurfs( surfs[i], err ) )
            return false;
    }
    std::vector<unsigned char> parts;                       // bones: the root carries no transform
    if ( !MrRead( "xmodelparts/" + lodIn[0], &parts ) ) { *err = "'xmodelparts/" + lodIn[0] + "' is missing"; return false; }
    if ( !MrWrite( "xmodelparts/" + lodOut[0], parts, err ) )
        return false;
    for ( int i = 0; i < 4; ++i )
        if ( !lodIn[i].empty() && !MrWrite( "xmodelsurfs/" + lodOut[i], surfs[i], err ) )
            return false;
    if ( !MrWrite( "xmodel/" + *out, w.b, err ) )
        return false;
    Sys_Printf( "Mirror: wrote the mirrored model '%s' (raw/xmodel).\n", out->c_str() );
    return true;
}

namespace
{
    bool MirrorFixedSize( const selbrush_t *b )
    {
        const entity_s_def *owner = b->owner ? (const entity_s_def *)b->owner->def : nullptr;
        return owner && owner->eclass && owner->eclass->fixedsize;
    }

    // A fixed-size entity's world bounds centre relative to its origin (zero for a point
    // entity with no model).
    void MirrorEntityCentre( selbrush_t *b, float out[3] )
    {
        out[0] = out[1] = out[2] = 0.0f;
        float mins[3], maxs[3], angles[3], origin[3], scale = 1.0f, rmin[3], rmax[3];
        if ( !KiwiDrop_GetModelInfo( b, mins, maxs, angles, &scale, origin, nullptr )
          || !KiwiDrop_TransformBounds( mins, maxs, angles, scale, rmin, rmax ) )
            return;
        for ( int k = 0; k < 3; ++k )
            out[k] = ( rmin[k] + rmax[k] ) * 0.5f;
    }

    // The centre of the selection's bounds (models by their world bounds); `half` = the
    // largest half extent (the plane marker's size).
    void MirrorPivot( float out[3], float *half )
    {
        float mins[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
        float maxs[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !b->def )
                continue;
            float lo[3], hi[3];
            if ( MirrorFixedSize( b ) )
            {
                const entity_s_def *e = (const entity_s_def *)b->owner->def;
                float mn[3], mx[3], an[3], org[3], sc = 1.0f;
                if ( KiwiDrop_GetModelInfo( b, mn, mx, an, &sc, org, nullptr )
                  && KiwiDrop_TransformBounds( mn, mx, an, sc, lo, hi ) )
                {
                    Add3( lo, e->origin, lo );
                    Add3( hi, e->origin, hi );
                }
                else
                {
                    Copy3( e->origin, lo );
                    Copy3( e->origin, hi );
                }
            }
            else
            {
                Copy3( b->def->mins, lo );
                Copy3( b->def->maxs, hi );
            }
            for ( int k = 0; k < 3; ++k )
            {
                mins[k] = lo[k] < mins[k] ? lo[k] : mins[k];
                maxs[k] = hi[k] > maxs[k] ? hi[k] : maxs[k];
            }
        }
        *half = 32.0f;
        for ( int k = 0; k < 3; ++k )
        {
            out[k] = mins[k] <= maxs[k] ? ( mins[k] + maxs[k] ) * 0.5f : 0.0f;
            if ( mins[k] <= maxs[k] && ( maxs[k] - mins[k] ) * 0.5f > *half )
                *half = ( maxs[k] - mins[k] ) * 0.5f;
        }
    }

    struct mirrorEnt_t
    {
        selbrush_t *node;
        float       origin[3];
        float       angles[3];
        double      anglesD[3];         // the key text in double (the exact turn below)
        bool        hasAngles;
        float       centre[3];          // world bounds centre relative to the origin
    };

    // Mirror the selected objects across `axis` through `pivot` (the caller's undo bracket).
    void MirrorObjects( int axis, const float pivot[3] )
    {
        // Fixed-size entities first, as they are (the brush flip below leaves them alone).
        std::vector<mirrorEnt_t> ents;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !b->def || !MirrorFixedSize( b ) )
                continue;
            bool dup = false;
            for ( const mirrorEnt_t &e : ents )
                dup = dup || e.node->owner == b->owner;
            if ( dup )
                continue;
            mirrorEnt_t e;
            e.node = b;
            entity_s_def *def = (entity_s_def *)b->owner->def;
            Copy3( def->origin, e.origin );
            double ang[3] = { 0.0, 0.0, 0.0 };
            const char *text = ValueForKey2( (entity_s *)def, "angles" );
            e.hasAngles = text && text[0] && sscanf( text, "%lf %lf %lf", &ang[0], &ang[1], &ang[2] ) == 3;
            for ( int k = 0; k < 3; ++k )
            {
                e.anglesD[k] = e.hasAngles ? ang[k] : 0.0;
                e.angles[k]  = (float)e.anglesD[k];
            }
            MirrorEntityCentre( b, e.centre );
            ents.push_back( e );
        }

        Select_FlipAxisAbout( axis, pivot, false );

        for ( const mirrorEnt_t &e : ents )
        {
            selbrush_t   *b   = e.node;
            entity_s_def *def = (entity_s_def *)b->owner->def;
            // The frame: rows forward / left / up.  Mirror each, then flip the model's own
            // left so the result is a rotation again (det +1).
            float axisM[3][3], turned[3][3];
            AnglesToAxis( e.angles, axisM );
            for ( int r = 0; r < 3; ++r )
            {
                Copy3( axisM[r], turned[r] );
                turned[r][axis] = -turned[r][axis];
            }
            for ( int k = 0; k < 3; ++k )
                turned[1][k] = -turned[1][k];
            // The bounds centre in the model's frame, then where it sits in the new frame.
            const float local[3] = { Dot3( e.centre, axisM[0] ), Dot3( e.centre, axisM[1] ), Dot3( e.centre, axisM[2] ) };
            float newRel[3] = { 0.0f, 0.0f, 0.0f };
            for ( int r = 0; r < 3; ++r )
                for ( int k = 0; k < 3; ++k )
                    newRel[k] += local[r] * turned[r][k];
            // An xmodel takes its mirrored twin: then the mirror is exact and the origin simply
            // mirrors.  Otherwise (prefab, skinned model, unreadable asset) the bounds centre
            // mirrors through the pivot and the origin follows it.
            std::string twin;
            bool        exact = false;
            const bool  isModel = KiwiDrop_IsModelEntity( b ) && !( def->eclass->classtype & 0x10 );
            const char *modelKey = isModel ? ValueForKey2( (entity_s *)def, "model" ) : nullptr;
            if ( modelKey && modelKey[0] )
            {
                std::string err;
                exact = KiwiMirror_ModelAsset( modelKey, &twin, &err );
                if ( !exact )
                    Sys_Printf( "Mirror: '%s' cannot be mirrored as a mesh (%s) - turned instead.\n",
                                modelKey, err.c_str() );
            }
            float want[3], move[3];
            for ( int k = 0; k < 3; ++k )
            {
                if ( exact )
                    want[k] = ( k == axis ) ? 2.0f * pivot[k] - e.origin[k] : e.origin[k];
                else
                {
                    float centre = e.origin[k] + e.centre[k];
                    if ( k == axis )
                        centre = 2.0f * pivot[k] - centre;
                    want[k] = centre - newRel[k];
                }
                move[k] = want[k] - def->origin[k];
            }
            if ( exact )
                SetKeyValue( def, "model", twin.c_str() );
            if ( Len3( move ) > 0.0f )
                Brush_Move( move, b->def, 0 );
            const bool turn = e.hasAngles || KiwiDrop_IsModelEntity( b ) || ( def->eclass->classtype & 0x10 );
            if ( turn )
            {
                // The same frame in DOUBLE (kiwi_angles.h), written nearest the old angles, so
                // a mirror and its mirror back give the original text ("1.715" -> "178.285" ->
                // "1.715"; float AnglesToAxis / AxisToAngles came back as 1.7150143).
                double axisD[3][3], turnedD[3][3], angD[3], nearD[3];
                KiwiAng_ToAxis( e.anglesD, axisD );
                for ( int r = 0; r < 3; ++r )
                    for ( int k = 0; k < 3; ++k )
                        turnedD[r][k] = ( k == axis ? -axisD[r][k] : axisD[r][k] ) * ( r == 1 ? -1.0 : 1.0 );
                KiwiAng_FromAxis( turnedD, angD );
                KiwiAng_Near( angD, e.anglesD, nearD );
                const float newAng[3] = { (float)nearD[0], (float)nearD[1], (float)nearD[2] };
                char text[160];
                SetKeyValue( def, "angles", KiwiFmt_Vec3Exact( text, sizeof( text ), newAng, 0 ) );
            }
            Brush_RebuildAfterEdit( b->def );
        }
        g_nUpdateBits = -1;
    }

    class KiwiMirrorCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Mirror"; }

        bool CanExecute() override { return KiwiDupe_CanMirror() || KiwiConSel_CanMove(); }

        bool Begin() override
        {
            m_axis      = -1;
            m_flipped   = false;
            m_construct = false;
            if ( selected_brushes.next == &selected_brushes )
            {
                if ( KiwiConSel_CanMove() && KiwiConSel_MoveBegin( m_pivot ) )
                {
                    m_construct = true;
                    m_half = 64.0f;
                    return true;
                }
                Sys_Printf( "Mirror: select brushes, models or construction lines first.\n" );
                return false;
            }
            MirrorPivot( m_pivot, &m_half );
            return true;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( mods & 6u )                         // Ctrl / Alt chords are not axis keys
                return false;
            const int axis = vk == 'X' ? 0 : vk == 'Y' ? 1 : vk == 'Z' ? 2 : -1;
            if ( axis < 0 )
                return false;
            SetAxis( axis == m_axis ? -1 : axis );   // the same key again takes the flip back
            return true;
        }

        void Commit() override
        {
            if ( m_construct )
            {
                if ( m_axis >= 0 ) KiwiConSel_MoveCommit();   // the snapshot is the undo record
                else               KiwiConSel_MoveCancel();   // nothing flipped: no empty step
            }
            if ( m_axis >= 0 )
                Sys_Printf( "Mirror: mirrored across %c.\n", "XYZ"[m_axis] );
            m_axis = -1;
            m_flipped = false;
            m_construct = false;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            if ( m_construct )
                KiwiConSel_MoveCancel();
            else if ( m_flipped )
                RevertObjects();                     // exact, and the originals stay selected
            m_axis = -1;
            m_flipped = false;
            m_construct = false;
            g_nUpdateBits = -1;
        }

        const char *HudStatus() const override
        {
            static const char *s_text[4] = { "press X, Y or Z", "across X", "across Y", "across Z" };
            return s_text[m_axis + 1];
        }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_keys[] = {
                { "X", "Mirror across X" },
                { "Y", "Mirror across Y" },
                { "Z", "Mirror across Z" },
            };
            *out = s_keys;
            return 3;
        }

        // The mirror plane through the pivot, or a small cross before an axis is chosen.
        void DrawWorld() override
        {
            if ( m_axis < 0 )
            {
                KiwiLines_Color( KDUP_COL_LINE[0], KDUP_COL_LINE[1], KDUP_COL_LINE[2] );
                for ( int k = 0; k < 3; ++k )
                {
                    float a[3], b[3];
                    Copy3( m_pivot, a );
                    Copy3( m_pivot, b );
                    a[k] -= 8.0f;
                    b[k] += 8.0f;
                    KiwiLines_Add( a, b );
                }
                return;
            }
            const int u = ( m_axis + 1 ) % 3, v = ( m_axis + 2 ) % 3;
            const float h = m_half * 1.1f;
            float c[4][3];
            for ( int i = 0; i < 4; ++i )
            {
                Copy3( m_pivot, c[i] );
                c[i][u] += ( i == 0 || i == 3 ) ? -h : h;
                c[i][v] += ( i < 2 ) ? -h : h;
            }
            KiwiLines_Color( KDUP_COL_LINE[0], KDUP_COL_LINE[1], KDUP_COL_LINE[2] );
            for ( int i = 0; i < 4; ++i )
                KiwiLines_Add( c[i], c[( i + 1 ) % 4] );
            KiwiLines_Add( c[0], c[2] );
            KiwiLines_Add( c[1], c[3] );
        }

    private:
        void SetAxis( int axis )
        {
            if ( m_construct )
            {
                float m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                if ( axis >= 0 )
                    m[axis][axis] = -1.0f;
                KiwiConSel_RotateApplyMatrix( m_pivot, m );   // absolute from the baseline
                m_axis = axis;
                g_nUpdateBits = -1;
                return;
            }
            if ( m_flipped )
                RevertObjects();
            if ( axis >= 0 )
            {
                KiwiCmd_UndoBegin( "mirror selection" );
                MirrorObjects( axis, m_pivot );
                Sel_InvalidateFromLegacy();
                m_flipped = true;
            }
            m_axis = axis;
        }

        // The bracket's own Undo_Undo puts the originals back exactly (deselected, stamped);
        // select them again.
        void RevertObjects()
        {
            KiwiCmd_UndoCancel();
            m_flipped = false;
            const intptr_t stamp = (intptr_t)Undo_LastUndoStamp();
            std::vector<selbrush_t *> back;
            for ( selbrush_t *b = active_brushes.next; b && b != &active_brushes; b = b->next )
                if ( b->def && (intptr_t)b->def->def == stamp )
                    back.push_back( b );
            for ( selbrush_t *b : back )
                Select_Brush( b, 0, 0, 0 );
            Sel_InvalidateFromLegacy();
            g_nUpdateBits = -1;
        }

        int   m_axis      = -1;
        bool  m_flipped   = false;            // objects: a flip is applied in the bracket
        bool  m_construct = false;            // the construction store owns this gesture
        float m_pivot[3]  = { 0.0f, 0.0f, 0.0f };
        float m_half      = 64.0f;
    };

    KiwiMirrorCommand s_mirror;
}

bool KiwiDupe_CanMirror()
{
    return selected_brushes.next != &selected_brushes;
}

bool KiwiDupe_CanMirrorTool()
{
    return KiwiDupe_CanMirror() || KiwiConSel_CanMove();
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
        Brush_UpdateSpecialMaterialFlag( i->def );
    for ( selbrush_t *j = active_brushes.next;   j != &active_brushes;   j = j->next )
        Brush_UpdateSpecialMaterialFlag( j->def );

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
    Radiant_RegisterCommand( "KiwiMirror",      0, 0, KIWI_CMD_MIRROR );
}

KiwiEditorCommand *KiwiDupe_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_ARRAY_LINEAR )
        return &s_linear;
    if ( commandId == KIWI_CMD_ARRAY_RADIAL )
        return &s_radial;
    if ( commandId == KIWI_CMD_MIRROR )
        return &s_mirror;
    return 0;
}

void KiwiDupe_MenuItems()
{
    ImGui::SeparatorText( "Duplicate" );

    const bool canArray  = KiwiDupe_CanArray();

    ImGui::BeginDisabled( !KiwiDupe_CanMirrorTool() );
    if ( ImGui::Button( "Mirror..." ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_MIRROR );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
        ImGui::SetTooltip( "Flip brushes, models or construction lines across an axis through\n"
                           "the selection's centre, previewed before you confirm." );

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
