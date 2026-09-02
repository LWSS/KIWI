#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Transform input, constraints, baselines, undo, and validity over ported cores.
// Per-frame totals come from the gesture baseline; incremental cores get residuals.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (texture / lightmap lock)

#include "kiwi_transform.h"
#include "kiwi_boxselect.h"          // IdlePressReselect click grammar
#include "kiwi_camera.h"             // KiwiCam_WorldPerPixel (the pivot marker's scale)
#include "kiwi_command.h"
#include "kiwi_droptrace.h"
#include "kiwi_fmt.h"
#include "kiwi_conselect.h"          // construction move arm
#include "kiwi_extrude.h"            // shared KEXT_SELF_SNAP_BAND
#include "kiwi_grid.h"
#include "kiwi_lines.h"
#include "kiwi_lollipop.h"           // KiwiLollipop_FaceSide (the handle's display side)
#include "kiwi_numeric.h"
#include "kiwi_patchfillet.h"        // KiwiFillet_CarryOnPlaneMove
#include "kiwi_pick.h"
#include "kiwi_refimage.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"                // shared Dot3/Sub3 helpers

#include <math.h>
#include <float.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points.
extern int   Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp
extern camera_s *Ed_Camera();                                          // camwnd.cpp
extern void  CamWnd_BuildMatrix();                                     // camwnd.cpp 0x403470
extern int   g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)
extern int   Entity_GetVec3ForKey( entity_s_def *e, float *out, const char *key ); // entity.cpp:100
extern void  SetKeyValue( entity_s_def *e, const char *key, const char *value );    // entity.cpp:213
extern void  Select_Move( const float *delta, char bSnap );            // select.cpp 0x48E9C0
extern void  Select_Scale( float sx, float sy, float sz );             // select.cpp 0x48FDC0
extern void  Select_GetMid( float *mid );                              // select.cpp 0x48FC70
extern void  Select_GetTrueMid( float *center );                       // select.cpp 0x48FC20
extern void  Select_RotateAxis( int axis, float deg, float (*rot_around)[4][3] );  // select.cpp 0x48FF40
extern void  Select_ApplyMatrix_SelectedBrushes( int bSnap, float *mat,
                                                  float deg, char bSwap );          // select.cpp 0x48FD10

extern int   Face_MakePlane( face_t *face );                           // brush.cpp 0x470470
extern int   Brush_MoveVertex( vec3_t delta, brush_t *b, vec3_t move_points, vec3_t end ); // brush.cpp 0x471C30
extern void  Patch_Rebuild( patchMesh_t *p, char doBounds );           // pmesh.cpp
extern void  MarkMapModified();                                        // win_qe3.cpp 0x499BB0

// Forwarders for brush.cpp's file-static texture-lock halves.
extern void  Ed_FaceTexLockSave( float *saveBuf, face_t *face );
extern void  Ed_FaceTexLockReproject( face_t *face, const float *saveBuf, const byte *lockFlags );

// Face selections need per-brush undo coverage beyond selected_brushes.
extern void  Undo_AddBrush( entity_brush_s *pBrushInst );              // undo.cpp:494  (0x45E680)
extern void  Undo_AddEntity( int a1 );                                 // undo.cpp:601  (0x45E8B0)
// Push-through deletion uses the classic selection delete core.
extern void  Undo_AddEntity_W( entity_s *a1 );                         // undo.cpp:633
extern void  Select_Deselect( int bAlsoFreeFaces );                    // select.cpp:1444 (0x48E800)
extern void  Select_Brush( selbrush_t *brush, char some_overwrite,
                           char bStatus, char center_grid_on_selection ); // select.cpp:884
extern void  Select_Delete();                                          // select.cpp:1520 (0x48E760)

extern bool  ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );   // imgui_shell.cpp

namespace
{
    // Transform tuning. R changes only from ring or numeric input.
    const float KX_SCALE_PER_PIXEL = 0.005f;   // S
    const float KX_SCALE_MIN       = 0.01f;    // S clamp
    const float KX_ANGLE_STEP      = 5.0f;     // R snap increment, degrees
    // Shared with the face-push/extrude delete threshold.
    const float KX_EPS             = KXPUSH_EPS;

    enum { KX_MAX_EDGE_FACES = 8 };            // faces one brush edge may touch

    // X/Y/Z constraint colors.
    const float KX_AXIS_COL[3][3] =
    {
        { 1.00f, 0.35f, 0.35f },
        { 0.40f, 1.00f, 0.45f },
        { 0.45f, 0.60f, 1.00f },
    };
    const float KX_RUBBER_COL[3] = { 1.00f, 0.80f, 0.25f };
    const float KX_BAD_COL[3]    = { 1.00f, 0.30f, 0.25f };

    enum constraint_t { CON_FREE = 0, CON_AXIS, CON_PLANE };

    // Static labels: the numeric layer copies structs, not label strings.
    const kiwiNumField_t KXF_MOVE  [1] = { { "length", KNUM_LENGTH, false } };
    const kiwiNumField_t KXF_ROTATE[1] = { { "angle",  KNUM_ANGLE,  false } };
    const kiwiNumField_t KXF_SCALE [1] = { { "factor", KNUM_FACTOR, false } };

    inline bool PointNear( const float *a, const float *b, float tol )
    {
        float d[3];
        Sub3( a, b, d );
        return fabsf( d[0] ) <= tol && fabsf( d[1] ) <= tol && fabsf( d[2] ) <= tol;
    }

    // Brush thickness along n, measured once from baseline windings.
    float FaceDepthAlong( const brush_t *def, const float n[3], const float basePts[9] )
    {
        if ( !def || !def->faces )
            return 0.0f;
        const float d0 = Dot3( n, &basePts[0] );
        float deepest = 0.0f;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                const float d = d0 - Dot3( n, w->p[i] );
                if ( d > deepest )
                    deepest = d;
            }
        }
        return deepest;
    }

    // Session pivot is memory-only and outlives individual commands.
    bool     s_pivotHave   = false;
    float    s_pivotPos[3] = { 0.0f, 0.0f, 0.0f };
    unsigned s_pivotSig    = 0;

    // Order-independent typed-selection hash; construction selection contributes
    // only its item count.
    unsigned SelectionSignature()
    {
        const selection_t &sel = KiwiSel();
        unsigned h = 0x9E3779B9u ^ (unsigned)sel.items.size();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            unsigned e = (unsigned)(uintptr_t)it.brush;
            e = e * 31u + (unsigned)it.kind;
            e = e * 31u + (unsigned)( it.faceIndex + 2 );
            e = e * 31u + (unsigned)( it.edgeIndex + 2 );
            e = e * 31u + (unsigned)( it.vertIndex + 2 );
            h += e * 2654435761u;
        }
        h = h * 31u + (unsigned)KiwiConSel_Count();
        return h;
    }

    // Session pivot self-expires when the selection signature changes.
    bool PivotActive( float *out3 )
    {
        if ( !s_pivotHave )
            return false;
        if ( SelectionSignature() != s_pivotSig )
        {
            s_pivotHave = false;               // the selection moved on — drop it
            return false;
        }
        if ( out3 )
        {
            out3[0] = s_pivotPos[0];
            out3[1] = s_pivotPos[1];
            out3[2] = s_pivotPos[2];
        }
        return true;
    }

    void PivotStore( const float p[3] )
    {
        s_pivotPos[0] = p[0];
        s_pivotPos[1] = p[1];
        s_pivotPos[2] = p[2];
        s_pivotSig    = SelectionSignature();
        s_pivotHave   = true;
    }

    // Ray/plane hit; reject near-parallel, behind-camera, and remote intersections.
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

    bool DropSelectionOnlyModels( std::vector<selbrush_t *> *out )
    {
        if ( out )
            out->clear();
        if ( !KiwiConSel_Empty() )
            return false;

        const selection_t &typed = KiwiSel();
        if ( typed.items.empty() )
            return false;
        for ( size_t i = 0; i < typed.items.size(); ++i )
            if ( typed.items[i].kind != SEL_OBJECT
              || !KiwiDrop_IsModelEntity( typed.items[i].brush ) )
                return false;

        for ( selbrush_t *node = selected_brushes.next;
              node != &selected_brushes; node = node->next )
        {
            if ( !KiwiDrop_IsModelEntity( node ) )
                return false;
            if ( !out )
                continue;
            bool duplicate = false;
            for ( size_t i = 0; i < out->size(); ++i )
                if ( ( *out )[i]->owner == node->owner )
                    duplicate = true;
            if ( !duplicate )
                out->push_back( node );
        }
        return !out || !out->empty();
    }

    bool WindingCentre( const winding_t *w, float *out )
    {
        if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
            return false;
        out[0] = out[1] = out[2] = 0.0f;
        for ( int i = 0; i < w->numpoints; ++i )
            Add3( out, w->p[i], out );
        const float inv = 1.0f / (float)w->numpoints;
        out[0] *= inv; out[1] *= inv; out[2] *= inv;
        return true;
    }


    // Dominance is objects > faces > edges > vertices. Palette-rate checks skip
    // the display-list liveness walk; Begin performs it.
    bool DominantKind( sel_kind_t *out, bool checkLive = true )
    {
        const selection_t &sel = KiwiSel();
        int n[SEL_KIND_COUNT] = { 0, 0, 0, 0 };
        for ( size_t i = 0; i < sel.items.size(); ++i )
            if ( !checkLive ? ( sel.items[i].brush != 0 )
                            : Sel_BrushLive( sel.items[i].brush ) )
                ++n[ sel.items[i].kind ];

        if ( n[SEL_OBJECT] ) { *out = SEL_OBJECT; return true; }
        if ( n[SEL_FACE]   ) { *out = SEL_FACE;   return true; }
        if ( n[SEL_EDGE]   ) { *out = SEL_EDGE;   return true; }
        if ( n[SEL_VERTEX] ) { *out = SEL_VERTEX; return true; }
        return false;
    }

    bool SelectionHasObjects()
    {
        // Whole-object operations act on selected_brushes, not typed sub-elements.
        return selected_brushes.next != &selected_brushes;
    }

    struct xformEntityAngles_t
    {
        entity_s_def *def;
        float angles[3];
        float axisAligned[3];
        bool  isAxisAligned;
    };

    double XformRoundNearest( double value )
    {
        return value >= 0.0 ? floor( value + 0.5 ) : ceil( value - 0.5 );
    }

    double XformSnapAngleScalar( double value )
    {
        value = XformRoundNearest( value * 1000.0 ) / 1000.0;
        const double integer = XformRoundNearest( value );
        if ( fabs( value - integer ) <= 0.01 )
            value = integer;
        return value;
    }

    float XformNormalizeAngle( double value )
    {
        value = fmod( value, 360.0 );
        if ( value < 0.0 )
            value += 360.0;
        if ( fabs( value ) < 0.0005 || fabs( value - 360.0 ) < 0.0005 )
            value = 0.0;
        return (float)value;
    }

    float XformSnapAngle( float value )
    {
        return XformNormalizeAngle( XformSnapAngleScalar( value ) );
    }

    float XformAngleDistance( float a, float b )
    {
        double d = fabs( (double)XformNormalizeAngle( a )
                       - (double)XformNormalizeAngle( b ) );
        if ( d > 180.0 )
            d = 360.0 - d;
        return (float)d;
    }

    bool XformAxisAlignedAngles( const float angles[3], float exact[3] )
    {
        for ( int i = 0; i < 3; ++i )
        {
            if ( !_finite( angles[i] ) )
                return false;
            const float normalized = XformNormalizeAngle( angles[i] );
            const double multiple = XformRoundNearest( normalized / 90.0 ) * 90.0;
            exact[i] = XformNormalizeAngle( multiple );
            if ( XformAngleDistance( normalized, exact[i] ) > 0.01f )
                return false;
        }
        return true;
    }

    const char *XformEntityKey( const entity_s_def *def, const char *key )
    {
        if ( !def || !key )
            return nullptr;
        for ( const epair_t *ep = def->epairs; ep; ep = ep->next )
            if ( ep->key && !_stricmp( ep->key, key ) )
                return ep->value ? ep->value : "";
        return nullptr;
    }

    void KiwiXform_CaptureEntityAngles( std::vector<xformEntityAngles_t> &out )
    {
        out.clear();
        for ( selbrush_t *brush = selected_brushes.next;
              brush && brush != &selected_brushes; brush = brush->next )
        {
            entity_s_def *def = brush->owner
                              ? (entity_s_def *)brush->owner->def : nullptr;
            if ( !def || !def->eclass || !def->eclass->fixedsize )
                continue;

            bool duplicate = false;
            for ( size_t i = 0; i < out.size(); ++i )
                if ( out[i].def == def ) { duplicate = true; break; }
            if ( duplicate )
                continue;

            xformEntityAngles_t base;
            base.def = def;
            if ( !Entity_GetVec3ForKey( def, base.angles, "angles" ) )
                base.angles[0] = base.angles[1] = base.angles[2] = 0.0f;
            base.isAxisAligned = XformAxisAlignedAngles( base.angles,
                                                         base.axisAligned );
            out.push_back( base );
        }
    }

    bool XformExpectedAngles( const xformEntityAngles_t &base, float exactDelta,
                              const float result[3], float out[3] )
    {
        if ( !base.isAxisAligned || !_finite( exactDelta ) )
            return false;

        const double delta = XformSnapAngleScalar( exactDelta );
        for ( int component = 0; component < 3; ++component )
        {
            for ( int sign = 1; sign >= -1; sign -= 2 )
            {
                float candidate[3] =
                {
                    base.axisAligned[0], base.axisAligned[1], base.axisAligned[2]
                };
                candidate[component] = XformNormalizeAngle(
                    (double)candidate[component] + (double)sign * delta );
                if ( XformAngleDistance( candidate[0], result[0] ) <= 0.05f
                     && XformAngleDistance( candidate[1], result[1] ) <= 0.05f
                     && XformAngleDistance( candidate[2], result[2] ) <= 0.05f )
                {
                    out[0] = candidate[0];
                    out[1] = candidate[1];
                    out[2] = candidate[2];
                    return true;
                }
            }
        }
        return false;
    }

    void KiwiXform_SnapEntityAngles( const std::vector<xformEntityAngles_t> &baseline,
                                     float exactDelta )
    {
        std::vector<entity_s_def *> defs;
        for ( size_t i = 0; i < baseline.size(); ++i )
            defs.push_back( baseline[i].def );

        for ( selbrush_t *brush = selected_brushes.next;
              brush && brush != &selected_brushes; brush = brush->next )
        {
            entity_s_def *def = brush->owner
                              ? (entity_s_def *)brush->owner->def : nullptr;
            if ( !def || !def->eclass || !def->eclass->fixedsize )
                continue;
            bool duplicate = false;
            for ( size_t i = 0; i < defs.size(); ++i )
                if ( defs[i] == def ) { duplicate = true; break; }
            if ( !duplicate )
                defs.push_back( def );
        }

        for ( size_t i = 0; i < defs.size(); ++i )
        {
            entity_s_def *def = defs[i];
            const char *current = XformEntityKey( def, "angles" );
            float result[3];
            if ( !current || sscanf( current, "%f %f %f",
                                     result, result + 1, result + 2 ) != 3
                 || !_finite( result[0] ) || !_finite( result[1] )
                 || !_finite( result[2] ) )
                continue;

            float clean[3] =
            {
                XformSnapAngle( result[0] ),
                XformSnapAngle( result[1] ),
                XformSnapAngle( result[2] )
            };
            for ( size_t j = 0; j < baseline.size(); ++j )
            {
                if ( baseline[j].def == def )
                {
                    XformExpectedAngles( baseline[j], exactDelta, result, clean );
                    break;
                }
            }

            char x[48], y[48], z[48], value[160];
            _snprintf( value, sizeof( value ), "%s %s %s",
                       KiwiFmt_Num( x, sizeof( x ), clean[0], 6 ),
                       KiwiFmt_Num( y, sizeof( y ), clean[1], 6 ),
                       KiwiFmt_Num( z, sizeof( z ), clean[2], 6 ) );
            value[sizeof( value ) - 1] = '\0';
            if ( strcmp( current, value ) )
                SetKeyValue( def, "angles", value );
        }
    }


    // Shared transform constraints, numeric state, HUD, and undo latch.
    class KiwiXformBase : public KiwiEditorCommand
    {
    public:
        // Dragging excludes selected geometry from pick/snap; pivot placement permits
        // self targets so a pivot can land on its own selection.
        unsigned PickFlags() const override
        { return m_pivotPlacing ? PICKF_NONE : PICKF_EXCLUDE_SELECTED; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        // Pivot-placement clicks commit before modal pause/resume handling.
        bool PressIntercept( int imgX, int imgY ) override
        {
            (void)imgX; (void)imgY;
            if ( !m_pivotPlacing )
                return false;
            CommitPivot();
            return true;
        }

    protected:
        // Shared V-placement; subclasses define their natural anchor and adoption.

        // V toggles placement; Esc leaves placement without cancelling the transform.
        bool HandlePivotKey( int vk )
        {
            if ( !SupportsPivot() )
                return false;
            if ( vk == 0x56 )                      // 'V'
            {
                if ( m_pivotPlacing )
                    CommitPivot();
                else
                    BeginPivot();
                return true;
            }
            if ( vk == 0x1B && m_pivotPlacing )    // VK_ESCAPE
            {
                m_pivotPlacing = false;
                Sys_Printf( "Pivot: placement cancelled.\n" );
                RefreshHud();                      // drop the PIVOT line
                g_nUpdateBits |= 1;
                return true;
            }
            return false;
        }

        void BeginPivot()
        {
            m_pivotPlacing = true;
            // Seed from the live anchor; baseline/session positions may lag an active move.
            Copy3( PivotAnchor(), m_pivotWip );
            m_pivotWipHave = true;
            // Pivot placement resumes because paused commands receive no MouseMove.
            KiwiCmd_Resume();
            Sys_Printf( "Pivot: move the cursor (snapping is live), click or V to "
                        "place, Esc to leave it where it was.\n" );
            UpdatePivotHud();
            g_nUpdateBits |= 1;
        }

        void CommitPivot()
        {
            m_pivotPlacing = false;
            if ( m_pivotWipHave )
            {
                PivotStore( m_pivotWip );
                ApplyPivot( m_pivotWip );
                char bx[32], by[32], bz[32];
                KiwiUnits_Format( bx, sizeof( bx ), m_pivotWip[0] );
                KiwiUnits_Format( by, sizeof( by ), m_pivotWip[1] );
                KiwiUnits_Format( bz, sizeof( bz ), m_pivotWip[2] );
                Sys_Printf( "Pivot: %s, %s, %s (kept until the selection changes).\n",
                            bx, by, bz );
            }
            RefreshHud();                          // ApplyPivot may have changed it
            g_nUpdateBits = -1;
        }

        // A valid SNAP_NONE still supplies raw placement. Only an invalid snap falls
        // back to the view-facing plane through the current WIP point.
        // True means V-placement consumed this MouseMove.
        bool TrackPivot( const snap_result_t &snap )
        {
            if ( !m_pivotPlacing )
                return false;
            if ( snap.valid )
            {
                Copy3( snap.position, m_pivotWip );
                m_pivotWipHave = true;
            }
            else
            {
                ray_t ray;
                if ( CursorRay( &ray ) )
                {
                    CamWnd_BuildMatrix();
                    float p[3];
                    if ( RayPlane( ray, m_pivotWip, Ed_Camera()->vpn, p ) )
                    {
                        Copy3( p, m_pivotWip );
                        m_pivotWipHave = true;
                    }
                }
            }
            UpdatePivotHud();
            g_nUpdateBits |= 1;
            return true;
        }

        void UpdatePivotHud()
        {
            if ( !m_pivotPlacing )
                return;
            char bx[32], by[32], bz[32];
            KiwiUnits_Format( bx, sizeof( bx ), m_pivotWip[0] );
            KiwiUnits_Format( by, sizeof( by ), m_pivotWip[1] );
            KiwiUnits_Format( bz, sizeof( bz ), m_pivotWip[2] );
            SetHud( "PIVOT  %s, %s, %s  click / V places, Esc leaves it", bx, by, bz );
        }

        // Pivot marker: axis ticks plus a view-facing diamond; WIP is brighter.
        void DrawPivotMarker( const float *p, bool wip )
        {
            camera_s *c = Ed_Camera();
            if ( c->width < 1 || c->height < 1 )
                return;
            const float s = KiwiCam_WorldPerPixel( p ) * ( wip ? 11.0f : 8.0f );
            if ( !( s > 0.0f ) )
                return;

            if ( wip ) KiwiLines_Color( 1.00f, 0.95f, 0.45f );
            else       KiwiLines_Color( 0.95f, 0.55f, 1.00f );
            for ( int k = 0; k < 3; ++k )
            {
                float a[3], b[3];
                Copy3( p, a ); Copy3( p, b );
                a[k] -= s; b[k] += s;
                KiwiLines_Add( a, b );
            }
            float d[4][3];
            for ( int i = 0; i < 4; ++i )
            {
                const float sx = ( i == 0 ) ? -1.0f : ( i == 2 ) ? 1.0f : 0.0f;
                const float sy = ( i == 1 ) ? -1.0f : ( i == 3 ) ? 1.0f : 0.0f;
                for ( int k = 0; k < 3; ++k )
                    d[i][k] = p[k] + c->vright[k] * ( sx * s ) + c->vup[k] * ( sy * s );
            }
            for ( int i = 0; i < 4; ++i )
                KiwiLines_Add( d[i], d[( i + 1 ) & 3] );
        }

        // Rebuild the subclass-owned HUD after shared pivot state changes.
        virtual void RefreshHud() {}

        // Natural pivot anchor when no override exists.
        virtual const float *PivotAnchor() const { return s_pivotPos; }
        // Adopt a placed pivot into the command anchor.
        virtual void ApplyPivot( const float p[3] ) { (void)p; }
        // Scale has no V placement until it has visible handles.
        virtual bool SupportsPivot() const { return false; }

    public:
        // Read-only placement state for external drawing gates.
        bool PivotPlacingNow() const { return m_pivotPlacing; }

    protected:
        bool  m_pivotPlacing = false;
        bool  m_pivotWipHave = false;
        float m_pivotWip[3]  = { 0.0f, 0.0f, 0.0f };

        // Ray from the framework's last cursor position over the camera.
        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        bool CursorPixels( int *x, int *y ) const
        {
            return KiwiCmd_LastCursor( x, y );
        }

        // Degrees/factors are not lengths; recover the exact typed scalar.
        float NumRaw() const { return Units_ToDisplay( m_numWorld ); }

        // This command's snap result is active only when its context engages snapping.
        bool SnapActive() const { return KiwiSnap_Active( m_snap ); }

        void OpenUndo( const char *literalOp )
        {
            if ( m_undoOpen )
                return;
            KiwiCmd_UndoBegin( literalOp );     // string LITERAL — stored by pointer
            m_undoOpen = true;
        }

        // X/Y/Z lock; repeat the same axis to release. Shift selects a plane where
        // permitted.
        bool HandleAxisKey( int vk, unsigned mods, bool allowPlane )
        {
            if ( mods & 6u )                      // Ctrl / Alt chords are never axis locks
                return false;
            int axis = -1;
            if      ( vk == 0x58 ) axis = 0;      // X
            else if ( vk == 0x59 ) axis = 1;      // Y
            else if ( vk == 0x5A ) axis = 2;      // Z
            if ( axis < 0 )
                return false;

            const bool wantPlane = allowPlane && ( mods & 1 ) != 0;   // Shift
            const constraint_t want = wantPlane ? CON_PLANE : CON_AXIS;

            if ( m_con == want && m_axis == axis )
                SetConstraint( CON_FREE, 0 );     // same key again = release
            else
                SetConstraint( want, axis );
            return true;
        }

        // Constraint rebase invariant: its first zero-delta frame must reproduce the
        // current pose. Carry the accumulated delta, then latch the current cursor.
        virtual void SetConstraint( constraint_t con, int axis )
        {
            m_con  = con;
            m_axis = axis;
            OnConstraintChanged();
            Recompute();
        }

        virtual void OnConstraintChanged() {}
        virtual void Recompute() = 0;

        void SetHud( const char *fmt, ... )
        {
            va_list ap;
            va_start( ap, fmt );
            _vsnprintf( m_hud, sizeof( m_hud ), fmt, ap );
            va_end( ap );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        static const char *AxisName( int a )
        {
            return ( a == 0 ) ? "X" : ( a == 1 ) ? "Y" : "Z";
        }

        // Caller-owned buffer permits multiple constraint strings in one expression.
        const char *ConstraintText( char *buf, size_t n ) const
        {
            if ( m_con == CON_AXIS )  { _snprintf( buf, n, "axis %s",  AxisName( m_axis ) ); buf[n - 1] = '\0'; return buf; }
            if ( m_con == CON_PLANE ) { _snprintf( buf, n, "plane %s", AxisName( m_axis ) ); buf[n - 1] = '\0'; return buf; }
            return "free";
        }

        // Constraint accent plus the start-to-current rubber band.
        void DrawConstraint( const float *origin, const float *from, const float *to )
        {
            camera_s *c = Ed_Camera();
            if ( c->width < 1 || c->height < 1 )
                return;

            if ( m_con == CON_AXIS || m_con == CON_PLANE )
            {
                // Bound accent length by view distance.
                float rel[3];
                Sub3( origin, c->origin, rel );
                float len = Len3( rel ) * 2.0f;
                if ( len < 256.0f )   len = 256.0f;
                if ( len > 16384.0f ) len = 16384.0f;

                const float *col = KX_AXIS_COL[m_axis];
                KiwiLines_Color( col[0], col[1], col[2] );

                if ( m_con == CON_AXIS )
                {
                    float ax[3] = { 0.0f, 0.0f, 0.0f };
                    ax[m_axis] = 1.0f;
                    float a[3], b[3];
                    Mad3( origin, ax, -len, a );
                    Mad3( origin, ax,  len, b );
                    KiwiLines_Add( a, b );
                }
                else
                {
                    // A plane lock draws its two free axes.
                    for ( int k = 0; k < 3; ++k )
                    {
                        if ( k == m_axis )
                            continue;
                        float ax[3] = { 0.0f, 0.0f, 0.0f };
                        ax[k] = 1.0f;
                        float a[3], b[3];
                        Mad3( origin, ax, -len, a );
                        Mad3( origin, ax,  len, b );
                        KiwiLines_Add( a, b );
                    }
                }
            }

            // Skip a zero-length rubber band (rotate/scale pass identical endpoints).
            float span[3];
            Sub3( to, from, span );
            if ( Len3( span ) > 0.01f )
            {
                const float *rc = m_invalid ? KX_BAD_COL : KX_RUBBER_COL;
                KiwiLines_Color( rc[0], rc[1], rc[2] );
                KiwiLines_Add( from, to );
            }
        }

        constraint_t  m_con      = CON_FREE;
        int           m_axis     = 2;
        bool          m_hasNum   = false;
        float         m_numWorld = 0.0f;
        bool          m_undoOpen = false;
        bool          m_invalid  = false;
        const char   *m_why      = 0;
        snap_result_t m_snap;
        char          m_hud[192] = { 0 };
    };

    // Context-aware move command.
    class KiwiMoveCommand : public KiwiXformBase
    {
    public:
        const char *Name() const override { return "Move"; }
        bool CanExecute() override { return KiwiXform_CanMove(); }

        void ArmDrop( selbrush_t *anchor )
        {
            m_dropPending = true;
            m_dropPendingNode = anchor;
        }
        void ClearDropArm()
        {
            m_dropPending = false;
            m_dropPendingNode = 0;
        }
        bool DropMode() const { return m_dropActive; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KXF_MOVE; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            // Face numeric output keeps its sign; other move contexts report delta length.
            *out = ( m_kind == SEL_FACE ) ? m_scalar : Len3( m_total );
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 )
                return false;
            // Bubble follows the live reference, not the latched mapping origin.
            if ( m_kind == SEL_FACE )
                for ( int k = 0; k < 3; ++k ) out3[k] = m_ref[k] + m_pushDir[k] * m_scalar;
            else
                for ( int k = 0; k < 3; ++k ) out3[k] = m_ref[k] + m_total[k];
            return true;
        }

        void Rebase() override
        {
            // Paused-to-hot uses the same zero-delta rebase as a constraint change.
            OnConstraintChanged();
        }

        // HandleGrab gates all cursor and snap mapping. Press re-latches the mapping
        // and freezes absolute snap/grid arms until the cursor leaves the press pixel;
        // this keeps the first zero-delta frame bit-for-bit on the current pose.
        void NoteGrab( bool held )
        {
            if ( m_grabbed == held )
                return;
            m_grabbed = held;
            if ( held )
            {
                m_grabFresh = true;
                if ( !CursorPixels( &m_grabPixX, &m_grabPixY ) )
                    m_grabFresh = false;   // no cursor to compare against: no freeze
                OnConstraintChanged();
            }
            UpdateHud();
        }

        void HandleGrab( bool held ) override { NoteGrab( held ); }

        // Snap dots use held state so they appear before the first pixel of travel.
        bool HandleHeld() const { return m_grabbed; }

        // Drive-face normal for the fourth gizmo arrow.
        bool PushDir( float *out3 ) const
        {
            if ( m_kind != SEL_FACE || m_faces.empty() || m_construct )
                return false;
            Copy3( m_driveNormal, out3 );
            return true;
        }

        // Face lollipop stays on the live pushed plane. Its display direction flips
        // toward the drag side; the push scalar's sign convention does not change.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_kind != SEL_FACE || m_faces.empty() || m_construct )
                return false;
            if ( !outAnchor || !outDir )
                return false;
            // Deletion previews use the baseline anchor. At rest, camera side chooses the
            // display stem so a persistent face selection remains legible after orbiting.
            const float s = m_deleting ? 0.0f : m_scalar;
            for ( int k = 0; k < 3; ++k )
                outAnchor[k] = m_ref[k] + m_pushDir[k] * s;
            const float side = KiwiLollipop_FaceSide( outAnchor, m_pushDir, s );
            for ( int k = 0; k < 3; ++k )
                outDir[k] = m_pushDir[k] * side;
            return true;
        }


        // An untouched auto-entered face push yields its paused click to the shared
        // click grammar. Shift re-enters over the enlarged face set; moved, grabbed,
        // or typed gestures remain modal. The predicate guarantees cancel is geometry-
        // and record-free.
        bool PreemptIdle() const override { return IdleUnmovedFace(); }

        // Transform dragging snaps only in its engaged Ctrl context. V placement always
        // snaps because its purpose is exact pivot placement.
        kiwiSnapCtx_t SnapContext() const override
        { return m_pivotPlacing ? KSNAPCTX_ALWAYS : KSNAPCTX_TRANSFORM; }

        // Transform swapping is blocked only during pivot placement and drop mode.
        // Construction and reference-image moves swap like brush moves: Rotate and
        // Scale serve both stores, and Commit closes the store snapshot before the
        // next tool opens its own.  A moved face push cannot yield because its
        // commit path deselects the face.
        bool CanSwapTo( int commandId ) const override
        {
            (void)commandId;
            if ( m_pivotPlacing || m_dropActive )
                return false;
            if ( m_kind == SEL_FACE && GestureMoved() )
                return false;
            return true;
        }

        // An open undo bracket is the definitive evidence of an applied mutation.
        bool GestureMoved() const override
        {
            if ( m_undoOpen || m_deleting || m_hasNum )
                return true;
            if ( m_kind == SEL_FACE )
                return fabsf( m_scalar ) > KX_EPS;
            return Len3( m_total ) > KX_EPS;
        }

        // Untouched face gestures exclude every mutation source and open undo state.
        bool IdleUnmovedFace() const
        {
            if ( m_kind != SEL_FACE || m_construct || m_faces.empty() )
                return false;
            if ( m_undoOpen || m_deleting || m_grabbed || m_hasNum )
                return false;
            return !( fabsf( m_scalar ) > KX_EPS );
        }

        bool IdlePressReselect( int imgX, int imgY, bool shift ) override
        {
            if ( !IdleUnmovedFace() )
                return false;

            KiwiCmd_Cancel();                    // provably record-free — see above
            KiwiBox_ClickSelectAt( imgX, imgY, shift, false );

            // Shift-click grows the selection, then explicitly re-enters paused face push.
            if ( shift && !KiwiCmd_Active() && KiwiXform_CanMove() )
            {
                const selection_t &sel = KiwiSel();
                bool haveFace = false;
                for ( size_t i = 0; i < sel.items.size() && !haveFace; ++i )
                    haveFace = ( sel.items[i].kind == SEL_FACE );
                if ( haveFace && KiwiCmd_Start( KIWI_CMD_MOVE ) )
                    KiwiCmd_Pause();
            }
            return true;
        }

        // One selected face push unit.
        struct faceUnit_t
        {
            selbrush_t *node;
            brush_t    *def;
            int         faceIndex;
            float       normal[3];      // baseline outward normal
            float       basePts[9];     // baseline planepts
            // Texture lock rewrites mtldef, so every frame restores both plane and texture
            // baselines before one Save/move/Reproject pass.
            byte        baseMtl[sizeof( MaterialDef ) * 4];

            // Inward deletion threshold: baseline thickness along the outward normal,
            // measured once before the gesture mutates the brush.
            float       depth;
            bool        doomed;         // this frame's push annihilates the brush
        };

        // One unique edge and its adjacent-face solve data.
        struct edgeUnit_t
        {
            selbrush_t *node;
            brush_t    *def;
            float       e0[3], e1[3];   // baseline endpoints
            int         adjCount;
            int         adjFace[KX_MAX_EDGE_FACES];
            float       adjAnchor[KX_MAX_EDGE_FACES][3];   // retained third point
            float       adjNormal[KX_MAX_EDGE_FACES][3];   // baseline normal (orientation)
        };

        // One dragged brush vertex or patch control point.
        struct vertUnit_t
        {
            selbrush_t *node;
            brush_t    *def;
            bool        patchPoint;
            int         col, row;       // patch control point
            float       basePos[3];     // baseline world position
            float       curPos[3];      // where the ported solver actually left it
        };

        struct dropUnit_t
        {
            selbrush_t *node;
            float       baseOrigin[3];
            float       relativeCorners[8][3];
        };

        bool Begin() override
        {
            const bool startDrop = m_dropPending;
            selbrush_t *dropAnchor = m_dropPendingNode;
            Reset();

            if ( !DominantKind( &m_kind ) )
            {
                // G moves construction geometry only when the typed selection is empty.
                // Construction sub-elements currently move their whole construction object.
                if ( KiwiConSel_CanMove() && KiwiConSel_MoveBegin( m_ref ) )
                {
                    m_construct = true;
                    m_kind      = SEL_OBJECT;   // the free/axis/plane cursor mapping
                    // A session pivot overrides MoveBegin's construction centroid once and stays
                    // latched. This preserves live reference == m_ref + m_total for absolute snaps.
                    m_pivotOverridden = PivotActive( m_ref );
                    CamWnd_BuildMatrix();
                    Copy3( Ed_Camera()->vpn, m_planeN );
                    LatchMapStart();
                    UpdateHud();
                    return true;
                }
                const krefImage_t *image = KiwiRefImage_At( KiwiRefImage_Selected() );
                if ( image && KiwiRefImage_CanMove() && KiwiRefImage_MoveBegin( m_ref ) )
                {
                    m_refImage = true;
                    m_refImageAxis = image->axis;
                    if ( !KiwiRefImage_PlaneNormal( KiwiRefImage_Selected(), m_refImageNormal ) )
                    {
                        m_refImageNormal[0] = m_refImageNormal[1] = m_refImageNormal[2] = 0.0f;
                        m_refImageNormal[m_refImageAxis] = 1.0f;
                    }
                    m_kind = SEL_OBJECT;
                    CamWnd_BuildMatrix();
                    Copy3( Ed_Camera()->vpn, m_planeN );
                    LatchMapStart();
                    UpdateHud();
                    return true;
                }
                Sys_Printf( "Move: nothing movable is selected.\n" );
                return false;
            }

            switch ( m_kind )
            {
            case SEL_OBJECT: if ( !BeginObjects() ) return false; break;
            case SEL_FACE:   if ( !BeginFaces()   ) return false; break;
            case SEL_EDGE:   if ( !BeginEdges()   ) return false; break;
            default:         if ( !BeginVerts()   ) return false; break;
            }

            if ( startDrop )
            {
                if ( m_kind != SEL_OBJECT || !BeginDrop( dropAnchor ) )
                {
                    Sys_Printf( "Drop: the selected models have no usable resident bounds.\n" );
                    return false;
                }
                CamWnd_BuildMatrix();
                Copy3( Ed_Camera()->vpn, m_planeN );
                UpdateHud();
                return true;
            }

            // Override each kind's natural reference with the session pivot after baseline
            // capture; the chosen m_ref remains latched for the gesture.
            m_pivotOverridden = PivotActive( m_ref );

            // Latch the camera normal so orbiting cannot rotate the movement plane.
            CamWnd_BuildMatrix();
            Copy3( Ed_Camera()->vpn, m_planeN );

            LatchMapStart();
            UpdateHud();
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            if ( m_dropActive )
            {
                RecomputeDrop();
                g_nUpdateBits |= 1;
                return;
            }
            if ( TrackPivot( snap ) )         // V-placement owns the move
                return;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            if ( m_dropActive )
            {
                if ( vk == 'G' && mods == 0 )
                {
                    HandoffDropToMove();
                    return true;
                }
                return false;
            }
            if ( HandlePivotKey( vk ) )       // V / Esc while placing
                return true;
            return HandleAxisKey( vk, mods, true );
        }

        bool SupportsPivot() const override { return true; }

        // Pivot placement starts at the live anchor, not the baseline reference.
        const float *PivotAnchor() const override
        {
            LiveAnchor( m_liveRef );
            return m_liveRef;
        }
        void RefreshHud() override { UpdateHud(); }

        // A picked pivot p is a current world position and already includes applied
        // travel. Store the baseline anchor p - applied so live == m_ref + applied;
        // for faces subtract pushDir * scalar. This keeps drawing, snapping, and commit
        // ride on the exact placed point.
        void ApplyPivot( const float p[3] ) override
        {
            if ( m_kind == SEL_FACE )
                Mad3( p, m_pushDir, -( m_deleting ? 0.0f : m_scalar ), m_ref );
            else
                Sub3( p, m_total, m_ref );
            m_pivotOverridden = true;         // enables commit-time pivot ride
            // Re-latch after moving the mapping origin; the current pose must not jump.
            OnConstraintChanged();
            UpdateHud();
        }

        // Keep m_ref fixed for mapping, but draw the move anchor at m_ref + applied.
        // Whole-object commits carry a session pivot by the same translation.
        void LiveAnchor( float *out3 ) const
        {
            if ( m_kind == SEL_FACE )
                Mad3( m_ref, m_pushDir, m_deleting ? 0.0f : m_scalar, out3 );
            else
                Add3( m_ref, m_total, out3 );
        }

        // Snap at the cursor; mapping already resolves target - m_ref, so redirecting
        // the query to the pivot would double-count the anchor.

        // Only whole-selection translations carry the session pivot. Reshaping faces,
        // edges, or vertices has no single pivot-ride vector.
        bool PivotRide( float *out3 ) const
        {
            if ( !m_pivotOverridden || m_kind != SEL_OBJECT )
                return false;
            if ( Len3( m_total ) <= KX_EPS )
                return false;
            Copy3( m_total, out3 );
            return true;
        }

        void Commit() override
        {
            // Push-through deletion is a valid removal state and precedes invalid handling.
            if ( m_deleting )
            {
                CommitDelete();
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            if ( m_invalid )
            {
                // Invalid geometry restores baseline and takes the undo-cancel path.
                RestoreAll();
                KiwiCmd_UndoCancel();
                m_undoOpen = false;
                Sys_Printf( "Move: cancelled — %s.\n", m_why ? m_why : "invalid geometry" );
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            // Construction commit keeps its store snapshot as the undo record.
            if ( m_construct )
            {
                // Carry a construction session pivot before commit/reset forgets m_total.
                {
                    float ride[3];
                    if ( PivotRide( ride ) )
                    {
                        float moved[3];
                        Add3( m_ref, ride, moved );
                        PivotStore( moved );
                    }
                }
                KiwiConSel_MoveCommit();
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            if ( m_refImage )
            {
                KiwiRefImage_MoveCommit();
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            // DELIBERATE DIVERGENCE from Brush_MoveVertex (brush.cpp 0x471C30): rebuild
            // touched brush windings once at commit because the ported solver leaves bounds
            // stale. Face pushes deselect after commit; object/edge/vertex moves remain
            // selected for follow-up edits.
            const bool deselectAfter = ( m_kind == SEL_FACE && !m_construct );

            // RMB explicitly finishes a non-face move: deselect only after a real mutation
            // and only when the framework classified this confirm as RMB. Other confirm
            // routes keep the selection.
            if ( !deselectAfter && !m_construct && m_undoOpen && KiwiCmd_ConfirmIsRmb() )
                KiwiCmd_DeselectAfterCommit();

            // Ride the session pivot before Reset forgets m_total and before any deselect
            // changes the selection signature.
            {
                float ride[3];
                if ( PivotRide( ride ) )
                {
                    float moved[3];
                    Add3( m_ref, ride, moved );
                    PivotStore( moved );
                }
            }

            if ( m_kind == SEL_VERTEX )
            {
                for ( size_t i = 0; i < m_verts.size(); ++i )
                {
                    if ( m_verts[i].patchPoint || !Sel_BrushLive( m_verts[i].node ) )
                        continue;
                    bool done = false;
                    for ( size_t k = 0; k < i && !done; ++k )
                        done = ( !m_verts[k].patchPoint && m_verts[k].def == m_verts[i].def );
                    if ( !done )
                        KiwiValid_Rebuild( m_verts[i].def );
                }
            }

            Reset();
            if ( deselectAfter )
            {
                Sel_Clear( KiwiSel() );
                Sel_SyncToLegacy();
            }
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            RestoreAll();
            Reset();
            g_nUpdateBits = -1;
        }

        void DrawWorld() override
        {
            if ( m_dropActive )
                return;
            // Pivot placement owns the draw batch; constraints would describe a moving anchor.
            if ( m_pivotPlacing )
            {
                DrawPivotMarker( m_pivotWip, true );
                return;
            }

            // Deletion preview outlines doomed brushes over their restored baseline geometry.
            if ( m_deleting )
            {
                KiwiLines_Color( 0.60f, 0.13f, 0.13f );
                for ( size_t i = 0; i < m_faces.size(); ++i )
                {
                    if ( !m_faces[i].doomed || !Sel_BrushLive( m_faces[i].node ) )
                        continue;
                    const brush_t *def = m_faces[i].def;
                    if ( !def || !def->faces )
                        continue;
                    bool drawn = false;
                    for ( size_t k = 0; k < i && !drawn; ++k )
                        drawn = ( m_faces[k].doomed && m_faces[k].def == def );
                    if ( drawn )
                        continue;
                    for ( int f = 0; f < def->faceCount; ++f )
                    {
                        const winding_t *w = def->faces[f].w;
                        if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                            continue;
                        for ( int p = 0; p < w->numpoints; ++p )
                            if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                                return;
                    }
                }
                return;
            }

            if ( !m_haveMapStart )
                return;
            float now[3];
            if ( m_kind == SEL_FACE )
                Mad3( m_ref, m_pushDir, m_scalar, now );
            else
                Add3( m_ref, m_total, now );
            DrawConstraint( m_ref, m_ref, now );
            // Draw the pivot marker at the live anchor, not the mapping origin.
            if ( m_pivotOverridden )
                DrawPivotMarker( now, false );
        }

        // Gizmo presets use the same constraint-rebase path as X/Y/Z keys.
        void PresetConstraint( int con, int axis )
        {
            if ( axis < 0 || axis > 2 )
                axis = 0;
            constraint_t c = ( con == KIWI_XCON_AXIS )  ? CON_AXIS
                           : ( con == KIWI_XCON_PLANE ) ? CON_PLANE
                                                        : CON_FREE;
            SetConstraint( c, axis );
            UpdateHud();
        }

    private:
        void Reset()
        {
            m_dropPending   = false;
            m_dropPendingNode = 0;
            m_dropActive    = false;
            m_dropUnits.clear();
            m_dropSupportCorners.clear();
            m_construct    = false;
            m_refImage     = false;
            m_refImageAxis = 2;
            m_faces.clear();
            m_edges.clear();
            m_verts.clear();
            m_base.clear();
            m_haveMapStart = false;
            m_axisWarned   = false;
            m_invalid      = false;
            m_why          = 0;
            m_hasNum       = false;
            m_grabbed      = false;    // cursor-mapping gate
            m_grabFresh    = false;    // press-pixel freeze
            m_deleting     = false;    // push-through-delete state
            m_pivotPlacing = false;
            m_pivotOverridden = false; // m_ref is the session pivot
            m_con          = CON_FREE;
            m_axis         = 2;
            m_undoOpen     = false;
            m_scalar       = 0.0f;
            m_scalarBase   = 0.0f;
            m_scalarStart  = 0.0f;
            m_total[0] = m_total[1] = m_total[2] = 0.0f;
            m_lockBase[0] = m_lockBase[1] = m_lockBase[2] = 0.0f;
            m_applied[0] = m_applied[1] = m_applied[2] = 0.0f;
            m_hud[0] = '\0';
        }

        // Stamp each unit's liveness once per Recompute.
        bool StampLiveness()
        {
            m_liveFace.assign( m_faces.size(), 0 );
            m_liveEdge.assign( m_edges.size(), 0 );
            m_liveVert.assign( m_verts.size(), 0 );
            bool all = true;
            for ( size_t i = 0; i < m_faces.size(); ++i )
                if ( Sel_BrushLive( m_faces[i].node ) ) m_liveFace[i] = 1; else all = false;
            for ( size_t i = 0; i < m_edges.size(); ++i )
                if ( Sel_BrushLive( m_edges[i].node ) ) m_liveEdge[i] = 1; else all = false;
            for ( size_t i = 0; i < m_verts.size(); ++i )
                if ( Sel_BrushLive( m_verts[i].node ) ) m_liveVert[i] = 1; else all = false;
            m_liveStamped = true;
            return all;
        }

        // Clear the liveness stamp on every Recompute exit.
        struct liveScope_t
        {
            KiwiMoveCommand *c;
            explicit liveScope_t( KiwiMoveCommand *cmd ) : c( cmd ) {}
            ~liveScope_t() { c->m_liveStamped = false; }
        };

        // Use the frame stamp in Recompute; otherwise perform a direct liveness test.
        bool FaceLive( size_t i ) const
        {
            if ( m_liveStamped && i < m_liveFace.size() ) return m_liveFace[i] != 0;
            return Sel_BrushLive( m_faces[i].node );
        }
        bool EdgeLive( size_t i ) const
        {
            if ( m_liveStamped && i < m_liveEdge.size() ) return m_liveEdge[i] != 0;
            return Sel_BrushLive( m_edges[i].node );
        }
        bool VertLive( size_t i ) const
        {
            if ( m_liveStamped && i < m_liveVert.size() ) return m_liveVert[i] != 0;
            return Sel_BrushLive( m_verts[i].node );
        }

        // Snapshot each brush definition once.
        void AddBaseline( brush_t *def )
        {
            for ( size_t i = 0; i < m_base.size(); ++i )
                if ( m_base[i].def == def )
                    return;
            kiwiBaseBrush_t b;
            if ( KiwiValid_Snapshot( def, &b ) )
                m_base.push_back( b );
        }

        void RestoreAll()
        {
            // Construction rollback pops its store snapshot; it has no brush baseline.
            if ( m_construct )
            {
                KiwiConSel_MoveCancel();
                return;
            }
            if ( m_refImage )
            {
                KiwiRefImage_MoveCancel();
                return;
            }

            // Restore face texture baselines in addition to KiwiValid_Restore's geometry.
            for ( size_t i = 0; i < m_faces.size(); ++i )
            {
                faceUnit_t &u = m_faces[i];
                if ( !FaceLive( i ) || !u.def->faces
                  || u.faceIndex >= u.def->faceCount )
                    continue;
                memcpy( &u.def->faces[u.faceIndex].mtldef[0], u.baseMtl, sizeof( u.baseMtl ) );
            }

            for ( size_t i = 0; i < m_base.size(); ++i )
            {
                // Never write through a node the map has freed.
                if ( !BaseNodeLive( m_base[i].def ) )
                    continue;
                KiwiValid_Restore( m_base[i] );
            }
            if ( m_kind == SEL_OBJECT && m_undoOpen )
            {
                // Object rollback is the exact inverse of residual Select_Move calls.
                const float back[3] = { -m_applied[0], -m_applied[1], -m_applied[2] };
                if ( Len3( back ) > KX_EPS && SelectionHasObjects() )
                    Select_Move( back, 0 );
                m_applied[0] = m_applied[1] = m_applied[2] = 0.0f;
            }
        }

        bool BaseNodeLive( const brush_t *def ) const
        {
            // Prefer the frame stamp during rollback to avoid repeated display-list walks.
            for ( size_t i = 0; i < m_faces.size(); ++i )
                if ( m_faces[i].def == def ) return FaceLive( i );
            for ( size_t i = 0; i < m_edges.size(); ++i )
                if ( m_edges[i].def == def ) return EdgeLive( i );
            for ( size_t i = 0; i < m_verts.size(); ++i )
                if ( m_verts[i].def == def ) return VertLive( i );
            return false;
        }

        bool BeginObjects()
        {
            if ( !SelectionHasObjects() )
            {
                Sys_Printf( "Move: no whole objects are selected.\n" );
                return false;
            }
            // Use the active object's center, else the unsnapped selection center.
            // Select_GetMid would bias the reference to the legacy grid.
            const sel_item_t &act = KiwiSel().active;
            if ( act.kind == SEL_OBJECT && Sel_BrushLive( act.brush ) && act.brush->def )
            {
                for ( int k = 0; k < 3; ++k )
                    m_ref[k] = ( act.brush->def->mins[k] + act.brush->def->maxs[k] ) * 0.5f;
            }
            else
            {
                Select_GetTrueMid( m_ref );
            }
            return true;
        }

        bool BeginDrop( selbrush_t *anchor )
        {
            std::vector<selbrush_t *> nodes;
            if ( !DropSelectionOnlyModels( &nodes ) || !anchor || !anchor->owner )
                return false;

            entity_s *anchorOwner = anchor->owner;
            bool haveAnchor = false;
            for ( size_t i = 0; i < nodes.size(); ++i )
            {
                float mins[3], maxs[3], angles[3], scale, origin[3];
                if ( !KiwiDrop_GetModelInfo( nodes[i], mins, maxs, angles, &scale, origin ) )
                    return false;
                float relMins[3], relMaxs[3];

                dropUnit_t unit;
                unit.node = nodes[i];
                Copy3( origin, unit.baseOrigin );
                if ( !KiwiDrop_TransformBounds( mins, maxs, angles, scale,
                                                relMins, relMaxs,
                                                unit.relativeCorners ) )
                    return false;
                m_dropUnits.push_back( unit );

                if ( nodes[i]->owner == anchorOwner )
                {
                    haveAnchor = true;
                    Copy3( origin, m_dropAnchorBase );
                    Copy3( mins, m_dropAnchorMins );
                    Copy3( maxs, m_dropAnchorMaxs );
                    Copy3( angles, m_dropAnchorAngles );
                    m_dropAnchorScale = scale;
                }
            }
            if ( !haveAnchor )
                return false;

            m_dropSupportCorners.clear();
            m_dropSupportCorners.reserve( m_dropUnits.size() * 8u * 3u );
            for ( size_t i = 0; i < m_dropUnits.size(); ++i )
                for ( int corner = 0; corner < 8; ++corner )
                    for ( int axis = 0; axis < 3; ++axis )
                        m_dropSupportCorners.push_back(
                            m_dropUnits[i].baseOrigin[axis] - m_dropAnchorBase[axis]
                          + m_dropUnits[i].relativeCorners[corner][axis] );
            Copy3( m_dropAnchorBase, m_ref );
            m_dropActive = true;
            return true;
        }

        bool BeginFaces()
        {
            const selection_t &sel = KiwiSel();
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                if ( it.kind != SEL_FACE || !Sel_BrushLive( it.brush ) )
                    continue;
                if ( it.brush->patch )                 // patches have no plane faces
                    continue;
                brush_t *def = it.brush->def;
                if ( !def || !def->faces || it.faceIndex < 0 || it.faceIndex >= def->faceCount )
                    continue;
                if ( !def->faces[it.faceIndex].w )
                    continue;

                bool dup = false;
                for ( size_t k = 0; k < m_faces.size() && !dup; ++k )
                    dup = ( m_faces[k].def == def && m_faces[k].faceIndex == it.faceIndex );
                if ( dup )
                    continue;

                faceUnit_t u;
                u.node      = it.brush;
                u.def       = def;
                u.faceIndex = it.faceIndex;
                Copy3( def->faces[it.faceIndex].plane.normal, u.normal );
                memcpy( u.basePts, &def->faces[it.faceIndex].planepts[0][0], sizeof( float ) * 9 );
                memcpy( u.baseMtl, &def->faces[it.faceIndex].mtldef[0], sizeof( u.baseMtl ) );
                // Measure the shared push-through threshold from the untouched brush.
                u.depth  = FaceDepthAlong( def, u.normal, u.basePts );
                u.doomed = false;
                m_faces.push_back( u );
                AddBaseline( def );
            }
            if ( m_faces.empty() )
            {
                Sys_Printf( "Move: no pushable faces in the selection.\n" );
                return false;
            }

            // The active face drives one scalar; every selected face follows its own normal.
            size_t drive = 0;
            const sel_item_t &act = KiwiSel().active;
            if ( act.kind == SEL_FACE )
                for ( size_t i = 0; i < m_faces.size(); ++i )
                    if ( m_faces[i].node == act.brush && m_faces[i].faceIndex == act.faceIndex )
                        drive = i;

            if ( !WindingCentre( m_faces[drive].def->faces[m_faces[drive].faceIndex].w, m_ref ) )
                return false;
            Copy3( m_faces[drive].normal, m_driveNormal );
            if ( !Norm3( m_driveNormal ) )
                return false;
            Copy3( m_driveNormal, m_pushDir );
            return true;
        }

        bool BeginEdges()
        {
            const selection_t &sel = KiwiSel();
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                if ( it.kind != SEL_EDGE || !Sel_BrushLive( it.brush ) || it.brush->patch )
                    continue;
                float a[3], b[3];
                if ( !Sel_EdgeEnds( it, a, b, false ) )   // live: tested above
                    continue;

                // Deduplicate the two opposite-winding selection records for one physical edge.
                // Compare unordered endpoints at the ported selection's 0.1-unit tolerance.
                bool dup = false;
                for ( size_t k = 0; k < m_edges.size() && !dup; ++k )
                {
                    if ( m_edges[k].def != it.brush->def )
                        continue;
                    dup = ( PointNear( m_edges[k].e0, a, 0.1f ) && PointNear( m_edges[k].e1, b, 0.1f ) )
                       || ( PointNear( m_edges[k].e0, b, 0.1f ) && PointNear( m_edges[k].e1, a, 0.1f ) );
                }
                if ( dup )
                    continue;

                edgeUnit_t u;
                u.node = it.brush;
                u.def  = it.brush->def;
                Copy3( a, u.e0 );
                Copy3( b, u.e1 );
                if ( !GatherAdjacent( u ) )
                    continue;

                // Two selected edges sharing a face are underconstrained here: neither leaves a
                // fixed third point. Keep the first unit rather than let last-write win.
                bool clash = false;
                for ( size_t k = 0; k < m_edges.size() && !clash; ++k )
                {
                    if ( m_edges[k].def != u.def )
                        continue;
                    for ( int x = 0; x < m_edges[k].adjCount && !clash; ++x )
                        for ( int y = 0; y < u.adjCount && !clash; ++y )
                            clash = ( m_edges[k].adjFace[x] == u.adjFace[y] );
                }
                if ( clash )
                {
                    Sys_Printf( "Move: two selected edges share a face — "
                                "only the first is moved (v1 limit).\n" );
                    continue;
                }

                m_edges.push_back( u );
                AddBaseline( u.def );
            }
            if ( m_edges.empty() )
            {
                Sys_Printf( "Move: no movable edges in the selection.\n" );
                return false;
            }

            // Reference is the active edge midpoint, else the first retained edge midpoint.
            size_t drive = 0;
            const sel_item_t &act = KiwiSel().active;
            if ( act.kind == SEL_EDGE )
            {
                float a[3], b[3];
                if ( Sel_EdgeEnds( act, a, b ) )          // live: not tested above
                    for ( size_t i = 0; i < m_edges.size(); ++i )
                        if ( ( PointNear( m_edges[i].e0, a, 0.1f ) && PointNear( m_edges[i].e1, b, 0.1f ) )
                          || ( PointNear( m_edges[i].e0, b, 0.1f ) && PointNear( m_edges[i].e1, a, 0.1f ) ) )
                            drive = i;
            }
            for ( int k = 0; k < 3; ++k )
                m_ref[k] = ( m_edges[drive].e0[k] + m_edges[drive].e1[k] ) * 0.5f;
            return true;
        }

        // For each adjacent face retain the winding point farthest from the edge line;
        // near-collinear anchors make the plane solve unstable.
        bool GatherAdjacent( edgeUnit_t &u )
        {
            u.adjCount = 0;
            brush_t *def = u.def;
            if ( !def || !def->faces )
                return false;

            float dir[3];
            Sub3( u.e1, u.e0, dir );
            if ( !Norm3( dir ) )
                return false;

            for ( int f = 0; f < def->faceCount && u.adjCount < KX_MAX_EDGE_FACES; ++f )
            {
                winding_t *w = def->faces[f].w;
                if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
                    continue;

                bool has0 = false, has1 = false;
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    if ( PointNear( w->p[i], u.e0, 0.1f ) ) has0 = true;
                    if ( PointNear( w->p[i], u.e1, 0.1f ) ) has1 = true;
                }
                if ( !has0 || !has1 )
                    continue;

                int   best     = -1;
                float bestDist = 0.0f;
                for ( int i = 0; i < w->numpoints; ++i )
                {
                    if ( PointNear( w->p[i], u.e0, 0.1f ) || PointNear( w->p[i], u.e1, 0.1f ) )
                        continue;
                    float rel[3], proj[3], perp[3];
                    Sub3( w->p[i], u.e0, rel );
                    const float t = Dot3( rel, dir );
                    proj[0] = dir[0] * t; proj[1] = dir[1] * t; proj[2] = dir[2] * t;
                    Sub3( rel, proj, perp );
                    const float d = Len3( perp );
                    if ( best < 0 || d > bestDist )
                    {
                        best     = i;
                        bestDist = d;
                    }
                }
                if ( best < 0 || bestDist < 0.1f )
                    continue;                     // no usable third point on this face

                u.adjFace[u.adjCount] = f;
                Copy3( w->p[best], u.adjAnchor[u.adjCount] );
                Copy3( def->faces[f].plane.normal, u.adjNormal[u.adjCount] );
                ++u.adjCount;
            }
            return u.adjCount > 0;
        }

        bool BeginVerts()
        {
            const selection_t &sel = KiwiSel();
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                if ( it.kind != SEL_VERTEX || !Sel_BrushLive( it.brush ) || !it.brush->def )
                    continue;
                float p[3];
                if ( !Sel_ItemWorldPos( it, p, false ) )  // live: tested above
                    continue;

                vertUnit_t u;
                u.node       = it.brush;
                u.def        = it.brush->def;
                u.patchPoint = ( it.faceIndex < 0 );
                u.col = u.row = 0;
                Copy3( p, u.basePos );
                Copy3( p, u.curPos );

                if ( u.patchPoint )
                {
                    patchMesh_t *pm = u.def->patch;
                    if ( !pm || pm->height <= 0 )
                        continue;
                    u.col = it.vertIndex / pm->height;
                    u.row = it.vertIndex % pm->height;
                    bool dup = false;
                    for ( size_t k = 0; k < m_verts.size() && !dup; ++k )
                        dup = ( m_verts[k].def == u.def && m_verts[k].patchPoint
                             && m_verts[k].col == u.col && m_verts[k].row == u.row );
                    if ( dup )
                        continue;
                    AddBaseline( u.def );      // patch ctrl baseline IS restorable
                }
                else
                {
                    // Brush_MoveVertex already fans one world corner across its incident faces;
                    // drive each (brush, world position) only once.
                    bool dup = false;
                    for ( size_t k = 0; k < m_verts.size() && !dup; ++k )
                        dup = ( m_verts[k].def == u.def && !m_verts[k].patchPoint
                             && PointNear( m_verts[k].basePos, u.basePos, 0.1f ) );
                    if ( dup )
                        continue;
                    // Brush_MoveVertex may change faceCount, so a plane-point snapshot is invalid.
                    // Its undo record and internal non-convex rollback own brush-vertex restoration.
                }
                m_verts.push_back( u );
            }
            if ( m_verts.empty() )
            {
                Sys_Printf( "Move: no movable vertices in the selection.\n" );
                return false;
            }

            const sel_item_t &act = KiwiSel().active;
            float p[3];
            if ( act.kind == SEL_VERTEX && Sel_ItemWorldPos( act, p ) )   // live: not tested above
                Copy3( p, m_ref );
            else
                Copy3( m_verts[0].basePos, m_ref );
            return true;
        }

        // A grab stays fresh until the cursor leaves its press pixel. While fresh, no
        // cursor delta, geometry snap, or lattice quantization contributes.
        void AgeGrab()
        {
            if ( !m_grabFresh )
                return;
            int cx, cy;
            if ( !CursorPixels( &cx, &cy ) )
                return;
            if ( cx != m_grabPixX || cy != m_grabPixY )
                m_grabFresh = false;
        }

        bool GrabLive() const { return m_grabbed && !m_grabFresh; }

        // Map the cursor into the active constraint space.
        bool MapCursor( float *out )
        {
            ray_t ray;
            if ( !CursorRay( &ray ) )
                return false;

            if ( m_kind == SEL_FACE )
            {
                float p[3];
                if ( !KiwiCam_RayAxis( ray, m_ref, m_pushDir, p ) )
                    return false;
                float rel[3];
                Sub3( p, m_ref, rel );
                out[0] = Dot3( rel, m_pushDir );      // the scalar lives in out[0]
                out[1] = out[2] = 0.0f;
                return true;
            }

            if ( m_con == CON_AXIS )
            {
                float ax[3] = { 0.0f, 0.0f, 0.0f };
                ax[m_axis] = 1.0f;
                return KiwiCam_RayAxis( ray, m_ref, ax, out );
            }
            if ( m_con == CON_PLANE )
            {
                float n[3] = { 0.0f, 0.0f, 0.0f };
                n[m_axis] = 1.0f;
                return RayPlane( ray, m_ref, n, out );
            }
            return RayPlane( ray, m_ref, m_planeN, out );
        }

        void LatchMapStart()
        {
            float p[3];
            if ( !MapCursor( p ) )
            {
                // KiwiCam_RayAxis rejects near-end-on solves using KCAM_RAYAXIS_MIN_DEN.
                // Warn once only for a portrayability failure, not for a missing cursor ray;
                // keep the gesture active so the user can orbit or type a value.
                float axis[3] = { 0.0f, 0.0f, 0.0f };
                const bool oneAxis = ( m_kind == SEL_FACE ) || ( m_con == CON_AXIS );
                if ( m_kind == SEL_FACE )      Copy3( m_pushDir, axis );
                else if ( m_con == CON_AXIS )  axis[m_axis] = 1.0f;
                if ( !m_axisWarned && oneAxis && !KiwiCam_AxisPortrayable( axis ) )
                {
                    m_axisWarned = true;
                    Sys_Printf( "%s: this view looks straight along the drag axis, so "
                                "the cursor cannot express movement along it — orbit a "
                                "little and grab again, or type a distance.\n", Name() );
                }
                m_haveMapStart = false;
                return;
            }
            m_axisWarned = false;
            if ( m_kind == SEL_FACE ) m_scalarStart = p[0];
            else                      Copy3( p, m_mapStart );
            m_haveMapStart = true;
        }

        void OnConstraintChanged() override
        {
            // Rebase invariant: carry the current total and latch this cursor so changing
            // constraints alone does not add a delta.
            if ( m_kind == SEL_FACE )
            {
                // Face constraints re-aim the push: face normal when free, world axis when locked.
                if ( m_con == CON_AXIS )
                {
                    m_pushDir[0] = m_pushDir[1] = m_pushDir[2] = 0.0f;
                    m_pushDir[m_axis] = 1.0f;
                }
                else if ( !m_faces.empty() )
                {
                    Copy3( m_driveNormal, m_pushDir );      // back to the DRIVE face's normal
                    m_con = CON_FREE;             // plane locks are meaningless here
                }
                m_scalarBase = m_scalar;
            }
            else
            {
                Copy3( m_total, m_lockBase );
                // Arm pivot rebase on each grab/constraint change, but consume it only after
                // freshness ends; the press frame must reproduce the current pose exactly.
                m_pivotRebase = true;
            }
            LatchMapStart();
        }

        // One predicate defines which world axes the active constraint owns.
        bool OwnsAxis( int k ) const
        {
            return ( m_con == CON_FREE )
                || ( m_con == CON_AXIS  && k == m_axis )
                || ( m_con == CON_PLANE && k != m_axis );
        }

        // Project only this grab's delta onto the active constraint.
        void Constrain( float *d ) const
        {
            if ( m_con == CON_AXIS )
            {
                for ( int k = 0; k < 3; ++k )
                    if ( k != m_axis )
                        d[k] = 0.0f;
            }
            else if ( m_con == CON_PLANE )
            {
                d[m_axis] = 0.0f;
            }
        }

        void ConstrainRefImage( float *d ) const
        {
            if ( !m_refImage || m_con != CON_FREE )
                return;
            // FREE movement stays in the picture's own plane (its tilt included).
            // An axis or plane lock is the user's explicit choice and moves exactly
            // as it says: a gizmo plane square must never collapse to one direction
            // because the picture happens to lie on another plane.
            const float dn = d[0] * m_refImageNormal[0] + d[1] * m_refImageNormal[1]
                           + d[2] * m_refImageNormal[2];
            for ( int k = 0; k < 3; ++k )
                d[k] -= dn * m_refImageNormal[k];
        }

        // Direction for a bare typed move scalar.
        bool NumericDirection( float *out )
        {
            if ( m_con == CON_AXIS )
            {
                out[0] = out[1] = out[2] = 0.0f;
                out[m_axis] = 1.0f;
                return true;
            }
            float d[3];
            Copy3( m_total, d );
            if ( m_refImage )
            {
                d[m_refImageAxis] = 0.0f;
                if ( m_con == CON_PLANE ) d[m_axis] = 0.0f;
            }
            else if ( m_con == CON_PLANE )
                d[m_axis] = 0.0f;
            else
                d[2] = 0.0f;                    // project on the ground plane (Z=0)
            if ( !Norm3( d ) )
            {
                out[0] = out[1] = out[2] = 0.0f;
                int fallback = 0;
                if ( m_refImage )
                    while ( fallback < 2 && ( fallback == m_refImageAxis
                          || ( m_con == CON_PLANE && fallback == m_axis ) ) )
                        ++fallback;
                out[fallback] = 1.0f;
                return true;
            }
            Copy3( d, out );
            return true;
        }

        // Per-frame move totals change only from a live handle or numeric input.
        // With no handle they remain latched; absolute snap and lattice arms are gated
        // by the same grab-freshness rule.
        void RecomputeDrop()
        {
            for ( size_t i = 0; i < m_dropUnits.size(); ++i )
            {
                if ( !Sel_BrushLive( m_dropUnits[i].node ) )
                {
                    Sys_Printf( "Drop: selection changed under the gesture - cancelled.\n" );
                    KiwiCmd_Cancel();
                    return;
                }
            }

            ray_t ray;
            if ( !CursorRay( &ray ) )
                return;
            float target[3];
            if ( !KiwiDrop_ComputePlacement( ray,
                                             m_dropAnchorMins, m_dropAnchorMaxs,
                                             m_dropAnchorAngles, m_dropAnchorScale,
                                             target, 0, 0,
                                             m_dropSupportCorners.empty()
                                                 ? 0 : &m_dropSupportCorners[0],
                                             (int)( m_dropSupportCorners.size() / 3u ) ) )
                return;

            Sub3( target, m_dropAnchorBase, m_total );
            Apply();
            UpdateHud();
        }

        void HandoffDropToMove()
        {
            // Continue model drop as a free move from this pixel without closing undo.
            m_dropActive = false;
            m_dropUnits.clear();
            m_dropSupportCorners.clear();
            m_con = CON_FREE;
            m_axis = 2;
            Copy3( m_total, m_lockBase );
            m_grabbed = true;
            m_grabFresh = CursorPixels( &m_grabPixX, &m_grabPixY );
            LatchMapStart();
            m_pivotRebase = false;
            UpdateHud();
        }

        void Recompute() override
        {
            // One liveness sweep for this frame, cleared by liveScope_t.
            liveScope_t liveThisFrame( this );
            if ( !StampLiveness() )
            {
                // Cancel rather than dereference geometry freed under the gesture.
                Sys_Printf( "Move: selection changed under the gesture — cancelled.\n" );
                KiwiCmd_Cancel();
                return;
            }

            if ( !m_haveMapStart )
                LatchMapStart();

            AgeGrab();                       // release the press-pixel freeze on travel

            if ( m_kind == SEL_FACE )
            {
                RecomputeFace();
                return;
            }

            float total[3] = { m_total[0], m_total[1], m_total[2] };
            float p[3];
            if ( GrabLive() && m_haveMapStart && MapCursor( p ) )
            {
                // On the first live frame, rebase owned axes so the pivot meets the cursor's
                // constrained projection. Off-axis carried totals remain untouched; m_ref stays
                // latched and m_mapStart becomes this projection for subsequent deltas.
                if ( m_pivotRebase )
                {
                    m_pivotRebase = false;
                    for ( int k = 0; k < 3; ++k )
                        if ( OwnsAxis( k ) )
                            m_lockBase[k] = p[k] - m_ref[k];
                    Copy3( p, m_mapStart );
                }
                float d[3];
                Sub3( p, m_mapStart, d );
                // Constrain this grab's delta, not the carried total; otherwise changing axes
                // would erase movement accumulated under earlier constraints.
                Constrain( d );
                Add3( m_lockBase, d, total );
            }
            // Publish this frame's mouse total before NumericDirection reads it.
            ConstrainRefImage( total );
            Copy3( total, m_total );

            // Recompute the major-grid HUD flag from scratch each frame.
            m_majorLock = false;

            if ( m_hasNum )
            {
                float dir[3];
                NumericDirection( dir );
                const float dist = m_numWorld;
                total[0] = dir[0] * dist;
                total[1] = dir[1] * dist;
                total[2] = dir[2] * dist;
            }
            else if ( GrabLive() && SnapActive() )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // Geometry snaps place the reference on the target only along axes this
                    // constraint owns. Under an axis lock, KiwiSnap_AxisDepth treats SNAP_FACE as
                    // a plane/axis intersection and rejects near-parallel targets without changing
                    // the latched total. SNAP_FACE uses an area magnet; named point/edge targets
                    // remain exact. The unified lattice pass below handles unnamed/grid answers.
                    const bool areaOnly = ( m_snap.type == SNAP_FACE );
                    bool axisDepthDone = false;
                    if ( m_con == CON_AXIS )
                    {
                        float ax[3] = { 0.0f, 0.0f, 0.0f };
                        ax[m_axis] = 1.0f;
                        float t = 0.0f;
                        if ( KiwiSnap_AxisDepth( m_snap, m_ref, ax, &t ) )
                        {
                            // Axis snaps replace only the locked component; off-axis carried totals survive.
                            if ( areaOnly )
                            {
                                float at[3];
                                Add3( m_ref, total, at );
                                total[m_axis] = KiwiSnap_AreaMagnet( total[m_axis], t, at );
                            }
                            else
                            {
                                total[m_axis] = t;
                            }
                        }
                        else if ( !areaOnly )
                        {
                            Copy3( m_total, total );   // refused: leave it where it is
                        }
                        axisDepthDone = true;
                    }
                    if ( !axisDepthDone )
                    {
                        // Plane locks replace their two owned components; free move owns all three.
                        float absT[3];
                        Sub3( m_snap.position, m_ref, absT );
                        float at[3];
                        Add3( m_ref, total, at );
                        if ( m_con == CON_PLANE )
                        {
                            for ( int k = 0; k < 3; ++k )
                                if ( k != m_axis )
                                    total[k] = areaOnly
                                             ? KiwiSnap_AreaMagnet( total[k], absT[k], at )
                                             : absT[k];
                        }
                        else if ( areaOnly )
                        {
                            for ( int k = 0; k < 3; ++k )
                                total[k] = KiwiSnap_AreaMagnet( total[k], absT[k], at );
                        }
                        else
                        {
                            Copy3( absT, total );
                            Constrain( total );
                        }
                    }

                }

                // Apply the absolute lattice to owned axes. Grid answers are hard quantization;
                // SNAP_FACE keeps its capture band. Named geometry targets are exempt so exact
                // vertices, midpoints, centers, endpoints, and intersections are never rounded.
                // KiwiSnap_LatticeAxis also supplies the major-line preference.
                const bool namedTarget = KiwiSnap_IsGeometry( m_snap.type )
                                      && m_snap.type != SNAP_FACE;
                if ( !namedTarget )
                {
                    const bool hard = !KiwiSnap_IsGeometry( m_snap.type );
                    for ( int k = 0; k < 3; ++k )
                    {
                        if ( !OwnsAxis( k ) )
                            continue;
                        float ax[3] = { 0.0f, 0.0f, 0.0f };
                        ax[k] = 1.0f;
                        bool major = false;
                        total[k] = KiwiSnap_LatticeAxis( total[k], m_ref, ax,
                                                         hard, &major );
                        if ( major )
                            m_majorLock = true;
                    }
                }
            }

            // Geometry/lattice snapping may have filled the plane-normal component
            // after the cursor delta was projected, so enforce the image plane once
            // more at the final publication point.
            ConstrainRefImage( total );
            Copy3( total, m_total );
            Apply();
            UpdateHud();
        }

        void RecomputeFace()
        {
            float dist = m_scalar;
            float p[3];
            // Major-grid HUD state is recomputed for each face frame.
            m_majorLock = false;
            // Ctrl makes face distance absolute along the push axis. On Ctrl release, latch
            // the current scalar/cursor pair so returning to relative mode cannot jump.
            const bool absNow = KiwiExt_AbsoluteHeld();
            if ( !absNow && m_absPrev && GrabLive() && m_haveMapStart )
            {
                float q[3];
                if ( MapCursor( q ) )
                {
                    m_scalarStart = q[0];
                    m_scalarBase  = m_scalar;
                }
            }
            m_absPrev  = absNow;
            m_absolute = false;
            // Face cursor mapping uses the same live-grab/freshness gate as object move.
            float rawAbs  = dist;
            bool  haveRaw = false;
            if ( GrabLive() && m_haveMapStart && MapCursor( p ) )
            {
                rawAbs  = p[0];
                haveRaw = true;
                dist    = absNow ? rawAbs : ( m_scalarBase + ( p[0] - m_scalarStart ) );
                m_absolute = absNow;
            }

            if ( m_hasNum )
            {
                dist = m_numWorld;
            }
            else if ( m_absolute && haveRaw )
            {
                // Absolute one-axis snapping ranks geometry, then face-plane magnet, then the
                // lattice (including major lines).
                dist = KiwiExt_LadderDepth( m_snap, m_ref, m_pushDir, rawAbs,
                                            &m_majorLock );
            }
            else if ( GrabLive() && SnapActive() )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // A face snap is a target plane, not the ray's sliding surface point.
                    // KiwiSnap_AxisDepth returns one stable scalar and rejects edge-on planes.
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_pushDir, &sd ) )
                    {
                        // Reject near-zero one-axis snaps to the selected face or coincident guides.
                        // KEXT_SELF_SNAP_BAND is shared by all one-axis push/extrude gestures.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            dist = sd;
                    }
                }
                else
                {
                    // Axis-aligned pushes snap the moved plane's absolute world coordinate.
                    // Slanted normals have no single world-axis coordinate, so they quantize
                    // distance instead. KiwiSnap_LatticeAxis supplies the shared minor/major rule.
                    bool major = false;
                    dist = KiwiSnap_LatticeAxis( dist, m_ref, m_pushDir, true, &major );
                    if ( major )
                        m_majorLock = true;
                }
            }
            // With Ctrl disengaged, raw face distance receives no lattice magnet.

            m_scalar = dist;

            // A face is doomed when inward travel along its baseline normal reaches its
            // measured thickness: scalar * dot(pushDir, normal) <= -depth + KX_EPS.
            // The delete state restores baseline geometry and is recomputed every frame.
            const bool wasDeleting = m_deleting;
            m_deleting = false;
            for ( size_t i = 0; i < m_faces.size(); ++i )
            {
                faceUnit_t &u = m_faces[i];
                float dir[3];
                if ( m_con == CON_AXIS ) Copy3( m_pushDir, dir );
                else                     Copy3( u.normal,  dir );
                const float travel = m_scalar * Dot3( dir, u.normal );
                u.doomed = ( u.depth > KX_EPS ) && ( travel <= -( u.depth ) + KX_EPS );
                if ( u.doomed )
                    m_deleting = true;
            }

            if ( m_deleting )
            {
                // Deletion preview uses restored baseline geometry.
                if ( wasDeleting )
                {
                    UpdateHud();
                    return;                      // already restored last frame
                }
                RestoreAll();
                for ( size_t i = 0; i < m_base.size(); ++i )
                    KiwiValid_Rebuild( m_base[i].def );
                m_invalid = false;
                m_why     = 0;
                UpdateHud();
                g_nUpdateBits = -1;
                return;
            }

            Apply();
            UpdateHud();
        }

        // Delete ordering is load-bearing: restore baseline; open/cover undo; cover
        // each owner entity; select only doomed nodes; call Select_Delete; clear typed
        // selection. Face-selected brushes are absent from selected_brushes, so each
        // touched brush must be covered explicitly before deletion. The framework
        // closes the single gesture record.
        void CommitDelete()
        {
            RestoreAll();

            std::vector<selbrush_t *> doomed;
            for ( size_t i = 0; i < m_faces.size(); ++i )
            {
                if ( !m_faces[i].doomed || !Sel_BrushLive( m_faces[i].node ) )
                    continue;
                bool dup = false;
                for ( size_t k = 0; k < doomed.size() && !dup; ++k )
                    dup = ( doomed[k] == m_faces[i].node );
                if ( !dup )
                    doomed.push_back( m_faces[i].node );
            }
            if ( doomed.empty() )
            {
                Sys_Printf( "Move: nothing left to delete.\n" );
                return;
            }

            // Deselect before the undo head: any brush cloned from selected_brushes must
            // still be selected at commit or undo would restore a duplicate.
            Select_Deselect( 1 );

            OpenUndoForBrushes();                       // head (may already be open)
            // Undo_AddEntity_W takes the entity definition cast used by the classic delete
            // path. Add entities after brushes; undo.cpp warns about the opposite order.
            for ( size_t i = 0; i < doomed.size(); ++i )
                if ( doomed[i]->owner && doomed[i]->owner->def )
                    Undo_AddEntity_W( (entity_s *)doomed[i]->owner->def );

            for ( size_t i = 0; i < doomed.size(); ++i )
                Select_Brush( doomed[i], 0, 0, 0 );

            Select_Delete();                            // the CLASSIC core
            Sel_Clear( KiwiSel() );

            Sys_Printf( "Move: pushed through — %i brush(es) deleted.\n", (int)doomed.size() );
        }

        void Apply()
        {
            // Construction applies one absolute total from its store baseline.
            if ( m_construct )
            {
                KiwiConSel_MoveApply( m_total );
                m_invalid = false;
                m_why     = 0;
                g_nUpdateBits |= 1;
                return;
            }
            if ( m_refImage )
            {
                KiwiRefImage_MoveApply( m_total );
                m_invalid = false;
                m_why = 0;
                g_nUpdateBits |= 1;
                return;
            }
            switch ( m_kind )
            {
            case SEL_OBJECT: ApplyObjects(); break;
            case SEL_FACE:   ApplyFaces();   break;
            case SEL_EDGE:   ApplyEdges();   break;
            default:         ApplyVerts();   break;
            }
        }

        void ApplyObjects()
        {
            float d[3];
            Sub3( m_total, m_applied, d );
            if ( Len3( d ) <= KX_EPS )
                return;
            OpenUndo( "move selection" );          // the FIRST real mutation
            Select_Move( d, 0 );                   // bSnap 0 — this layer owns snapping
            Copy3( m_total, m_applied );
            m_invalid = false;
            m_why     = 0;
            g_nUpdateBits = -1;
        }

        // Each face starts from its own plane/texture baseline and uses the ported lock.
        void ApplyFaces()
        {
            if ( m_faces.empty() )
                return;
            // Do not open undo for a zero face push. Once open, baseline rewrites must still
            // run so returning the drag to zero restores the exact start.
            if ( !m_undoOpen && fabsf( m_scalar ) <= KX_EPS )
                return;
            OpenUndoForBrushes();

            byte  lockFlags[3];
            lockFlags[0] = (byte)( g_PrefsDlg->m_bTextureLock  != 0 );
            lockFlags[1] = (byte)( g_PrefsDlg->m_bLightmapLock != 0 );
            lockFlags[2] = 1;
            float saveBuf[19];

            for ( size_t i = 0; i < m_faces.size(); ++i )
            {
                faceUnit_t &u = m_faces[i];
                if ( !u.def->faces || u.faceIndex >= u.def->faceCount )
                    continue;
                face_t *f = &u.def->faces[u.faceIndex];

                // One scalar; each face uses its own normal unless a world-axis lock is active.
                float dir[3];
                if ( m_con == CON_AXIS ) Copy3( m_pushDir, dir );
                else                     Copy3( u.normal,  dir );

                // Restore plane and texture baselines before one texture-lock pass.
                // Face_MakePlane must refresh the baseline normal before TexLockSave.
                memcpy( &f->mtldef[0], u.baseMtl, sizeof( u.baseMtl ) );
                memcpy( &f->planepts[0][0], u.basePts, sizeof( float ) * 9 );
                Face_MakePlane( f );

                if ( f->w )
                    Ed_FaceTexLockSave( saveBuf, f );

                for ( int p = 0; p < 3; ++p )
                    Mad3( &u.basePts[p * 3], dir, m_scalar, f->planepts[p] );

                if ( f->w )
                    Ed_FaceTexLockReproject( f, saveBuf, lockFlags );
            }

            RebuildAndValidate();
        }

        // Edge invariant: moved endpoints vary while the retained third point stays
        // fixed; Face_MakePlane remains the plane solver. Texture lock is intentionally
        // absent because this path reorients planes and the ported edge/vertex path does
        // not define a rotating-plane lock behavior.
        void ApplyEdges()
        {
            if ( m_edges.empty() )
                return;
            // Do not open undo for a zero edge move.
            if ( !m_undoOpen && Len3( m_total ) <= KX_EPS )
                return;
            OpenUndoForBrushes();

            for ( size_t i = 0; i < m_edges.size(); ++i )
            {
                edgeUnit_t &u = m_edges[i];
                float a[3], b[3];
                Add3( u.e0, m_total, a );
                Add3( u.e1, m_total, b );

                for ( int k = 0; k < u.adjCount; ++k )
                {
                    const int fi = u.adjFace[k];
                    if ( !u.def->faces || fi >= u.def->faceCount )
                        continue;
                    face_t *f = &u.def->faces[fi];

                    // Preserve the baseline outward-normal orientation when ordering plane points.
                    const float *anchor = u.adjAnchor[k];
                    float e1v[3], e2v[3], n[3];
                    Sub3( a, b, e1v );
                    Sub3( anchor, b, e2v );
                    Cross3( e1v, e2v, n );
                    const bool flip = ( Dot3( n, u.adjNormal[k] ) < 0.0f );

                    Copy3( flip ? b : a, f->planepts[0] );
                    Copy3( flip ? a : b, f->planepts[1] );
                    Copy3( anchor,        f->planepts[2] );
                }
            }

            RebuildAndValidate();
        }

        // Brush vertices use the ported solver; patch points use their baseline.
        void ApplyVerts()
        {
            if ( m_verts.empty() )
                return;
            // Do not open undo for a zero vertex move.
            if ( !m_undoOpen && Len3( m_total ) <= KX_EPS )
                return;
            OpenUndoForBrushes();

            bool touchedPatch = false;
            bool touchedBrush = false;

            for ( size_t i = 0; i < m_verts.size(); ++i )
            {
                vertUnit_t &u = m_verts[i];
                if ( u.patchPoint )
                {
                    patchMesh_t *pm = u.def->patch;
                    if ( !pm || u.col >= pm->width || u.row >= pm->height )
                        continue;
                    // Patch controls apply directly from baseline.
                    Add3( u.basePos, m_total, pm->ctrl[u.col][u.row].xyz );
                    Copy3( pm->ctrl[u.col][u.row].xyz, u.curPos );
                    touchedPatch = true;
                    continue;
                }

                // Brush_MoveVertex is incremental: feed the residual to the desired baseline
                // position and adopt the solver's returned endpoint.
                float want[3], delta[3], end[3];
                Add3( u.basePos, m_total, want );
                Sub3( want, u.curPos, delta );
                if ( Len3( delta ) <= KX_EPS )
                    continue;
                if ( Brush_MoveVertex( delta, u.def, u.curPos, end ) )
                {
                    Copy3( end, u.curPos );
                    touchedBrush = true;
                }
            }

            if ( touchedPatch )
            {
                // Rebuild each touched patch once per frame, not once per control point.
                // Patch_Rebuild(..., 1) matches Patch_UpdateSelected_0 (pmesh.cpp 0x43D800).
                for ( size_t i = 0; i < m_verts.size(); ++i )
                {
                    if ( !m_verts[i].patchPoint || !m_verts[i].def->patch )
                        continue;
                    bool done = false;
                    for ( size_t k = 0; k < i && !done; ++k )
                        done = ( m_verts[k].patchPoint && m_verts[k].def == m_verts[i].def );
                    if ( !done )
                        Patch_Rebuild( m_verts[i].def->patch, 1 );
                }
                MarkMapModified();
            }
            if ( touchedBrush || touchedPatch )
                g_nUpdateBits = -1;

            ValidateOnly();
        }

        // Sub-element brushes are absent from selected_brushes; cover each touched
        // brush before the first mutation.
        void OpenUndoForBrushes()
        {
            if ( m_undoOpen )
                return;
            OpenUndo( ( m_kind == SEL_FACE ) ? "push face"
                    : ( m_kind == SEL_EDGE ) ? "move edge"
                                             : "move vertex" );
            for ( size_t i = 0; i < m_faces.size(); ++i ) KiwiCmd_UndoCoverBrush( m_faces[i].node );
            for ( size_t i = 0; i < m_edges.size(); ++i ) KiwiCmd_UndoCoverBrush( m_edges[i].node );
            for ( size_t i = 0; i < m_verts.size(); ++i ) KiwiCmd_UndoCoverBrush( m_verts[i].node );
        }

        // Rebuild, validate, and restore baseline on rejection.
        void RebuildAndValidate()
        {
            for ( size_t i = 0; i < m_base.size(); ++i )
                KiwiValid_Rebuild( m_base[i].def );

            const char *why = 0;
            bool ok = true;
            for ( size_t i = 0; i < m_base.size() && ok; ++i )
                ok = KiwiValid_CheckBrush( m_base[i].def, &why );

            if ( !ok )
            {
                for ( size_t i = 0; i < m_base.size(); ++i )
                    KiwiValid_Restore( m_base[i] );      // never leave it live
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

        // Brush vertices rely on Brush_MoveVertex's convexity gate and internal rollback;
        // rebuilding them per frame would invalidate the solver's stale-bounds contract.
        // Patch points run the bounds check. This path raises m_invalid and defers
        // restoration to invalid commit/cancel.
        void ValidateOnly()
        {
            const char *why = 0;
            bool ok = true;
            for ( size_t i = 0; i < m_verts.size() && ok; ++i )
                if ( m_verts[i].patchPoint )
                    ok = KiwiValid_CheckBounds( m_verts[i].def, &why );
            m_invalid = !ok;
            m_why     = ok ? 0 : why;
        }

        void UpdateHud()
        {
            if ( m_dropActive )
            {
                SetHud( "Drop to ground  G Move gizmo  Ctrl Snap  Esc Cancel" );
                return;
            }
            const char *what = m_construct               ? "construction"
                             : m_refImage                ? "image"
                             : ( m_kind == SEL_OBJECT ) ? "objects"
                             : ( m_kind == SEL_FACE   ) ? "faces"
                             : ( m_kind == SEL_EDGE   ) ? "edges" : "verts";

            if ( m_pivotPlacing )
            {
                UpdatePivotHud();
                return;
            }

            // Deletion is a valid confirmable state, not HudInvalid.
            if ( m_deleting )
            {
                int n = 0;
                for ( size_t i = 0; i < m_faces.size(); ++i )
                    if ( m_faces[i].doomed )
                        ++n;
                SetHud( "%s  PUSHED THROUGH - confirm DELETES %i brush(es), pull back to undo",
                        what, n );
                return;
            }

            // Idle HUD names the only two move sources: handle drag or numeric input.
            if ( !m_grabbed && !m_hasNum
              && ( ( m_kind == SEL_FACE ) ? ( fabsf( m_scalar ) <= KX_EPS )
                                          : ( Len3( m_total ) <= KX_EPS ) ) )
            {
                char cbuf[24];
                SetHud( "%s  %s  drag a handle / type a value", what, ConstraintText( cbuf, sizeof( cbuf ) ) );
                return;
            }

            if ( m_invalid )
            {
                char cbuf[24];
                SetHud( "%s  %s  INVALID: %s", what, ConstraintText( cbuf, sizeof( cbuf ) ),
                        m_why ? m_why : "rejected" );
                return;
            }

            if ( m_kind == SEL_FACE )
            {
                char b[32], cbuf[24];
                KiwiUnits_Format( b, sizeof( b ), m_scalar );
                // Absolute face mode shows target position plus delta; slanted directions have
                // no single world coordinate and report outward distance.
                if ( m_absolute )
                {
                    char abso[32];
                    int  worldAxis = -1;
                    for ( int k = 0; k < 3; ++k )
                        if ( fabsf( m_pushDir[k] ) > 0.999f )
                            worldAxis = k;
                    if ( worldAxis >= 0 )
                    {
                        char v[32];
                        KiwiUnits_Format( v, sizeof( v ),
                                          m_ref[worldAxis] + m_pushDir[worldAxis] * m_scalar );
                        _snprintf( abso, sizeof( abso ), "%c %s", "XYZ"[worldAxis], v );
                    }
                    else
                    {
                        _snprintf( abso, sizeof( abso ), "out %s", b );
                    }
                    abso[sizeof( abso ) - 1] = '\0';
                    SetHud( "%s  %s  %s  (%s)%s  CTRL", what,
                            ( m_con == CON_AXIS ) ? ConstraintText( cbuf, sizeof( cbuf ) ) : "normal",
                            abso, b, m_majorLock ? "  ·  MAJOR GRID" : "" );
                    return;
                }
                SetHud( "%s  %s  %s%s", what,
                        ( m_con == CON_AXIS ) ? ConstraintText( cbuf, sizeof( cbuf ) ) : "normal", b,
                        m_majorLock ? "  ·  MAJOR GRID" : "" );
                return;
            }

            char bx[32], by[32], bz[32], cbuf[24];
            KiwiUnits_Format( bx, sizeof( bx ), m_total[0] );
            KiwiUnits_Format( by, sizeof( by ), m_total[1] );
            KiwiUnits_Format( bz, sizeof( bz ), m_total[2] );
            // State explicitly when the lattice chose a major line.
            SetHud( "%s  %s  %s, %s, %s%s", what, ConstraintText( cbuf, sizeof( cbuf ) ),
                    bx, by, bz, m_majorLock ? "  ·  MAJOR GRID" : "" );
        }

        sel_kind_t m_kind = SEL_OBJECT;
        // Construction is a separate store-backed move arm, not another sel_kind_t;
        // it shares only the object cursor mapping.
        bool       m_construct = false;
        bool       m_refImage = false;
        int        m_refImageAxis = 2;
        float      m_refImageNormal[3] = { 0.0f, 0.0f, 1.0f };   // the picture's real plane

        bool        m_dropPending = false;
        selbrush_t *m_dropPendingNode = 0;
        bool        m_dropActive = false;
        std::vector<dropUnit_t> m_dropUnits;
        std::vector<float> m_dropSupportCorners;
        float       m_dropAnchorBase[3]   = { 0.0f, 0.0f, 0.0f };
        float       m_dropAnchorMins[3]   = { 0.0f, 0.0f, 0.0f };
        float       m_dropAnchorMaxs[3]   = { 0.0f, 0.0f, 0.0f };
        float       m_dropAnchorAngles[3] = { 0.0f, 0.0f, 0.0f };
        float       m_dropAnchorScale     = 1.0f;

        std::vector<faceUnit_t>      m_faces;
        std::vector<edgeUnit_t>      m_edges;
        std::vector<vertUnit_t>      m_verts;
        std::vector<kiwiBaseBrush_t> m_base;

        // Sel_BrushLive is a display-list walk. Stamp all move units once at the top of
        // Recompute, use the stamp only for that call, and fall back to direct checks
        // from Commit/Cancel where stale liveness would permit a freed-node write.
        std::vector<unsigned char> m_liveFace;
        std::vector<unsigned char> m_liveEdge;
        std::vector<unsigned char> m_liveVert;
        bool                       m_liveStamped = false;

        float m_ref[3]      = { 0.0f, 0.0f, 0.0f };   // the moved reference point
        float m_planeN[3]   = { 0.0f, 0.0f, 1.0f };   // latched movement-plane normal
        float m_pushDir[3]  = { 0.0f, 0.0f, 1.0f };   // face push direction (live)
        float m_driveNormal[3] = { 0.0f, 0.0f, 1.0f };// the DRIVE face's own normal
        float m_mapStart[3] = { 0.0f, 0.0f, 0.0f };
        float m_lockBase[3] = { 0.0f, 0.0f, 0.0f };
        float m_total[3]    = { 0.0f, 0.0f, 0.0f };
        float m_applied[3]  = { 0.0f, 0.0f, 0.0f };   // objects only
        float m_scalar      = 0.0f;                   // faces only
        // Ctrl absolute-mode edge state and this frame's HUD mode.
        bool  m_absPrev     = false;
        bool  m_absolute    = false;
        float m_scalarBase  = 0.0f;
        float m_scalarStart = 0.0f;
        bool  m_haveMapStart = false;
        bool  m_axisWarned   = false;   // say it once per refusal run
        // Cursor mapping is gated by a held move handle.
        bool  m_grabbed      = false;
        // Press-pixel freshness and whether m_ref is the session pivot.
        bool  m_grabFresh    = false;
        int   m_grabPixX     = 0;
        int   m_grabPixY     = 0;
        bool  m_pivotOverridden = false;
        // Mutable scratch returns the derived live anchor through a pointer API.
        mutable float m_liveRef[3] = { 0.0f, 0.0f, 0.0f };
        // Confirm deletes when this frame's push annihilates a brush.
        bool  m_deleting     = false;
        // Pivot rebase is consumed on the first live grab frame. majorLock is a
        // per-frame HUD result from the lattice pass.
        bool  m_pivotRebase  = false;
        bool  m_majorLock    = false;
    };

    // Whole-selection rotate.
    class KiwiRotateCommand : public KiwiXformBase
    {
    public:
        const char *Name() const override { return "Rotate"; }
        bool CanExecute() override { return KiwiXform_CanRotate(); }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KXF_ROTATE; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_deg;                     // degrees — KNUM_ANGLE formats it
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 )
                return false;
            Copy3( m_pivot, out3 );           // the latched pivot IS the action point
            return true;
        }

        void Rebase() override
        {
            // Rotate has no free cursor mapping to re-latch; its value changes only from
            // a ring sweep or typed degrees.
        }

        bool Begin() override
        {
            m_deg = m_applied = 0.0f;
            m_chain.clear();
            m_axis = 2;                       // Z default
            m_con  = CON_AXIS;                // R is ALWAYS about an axis
            m_hasNum   = false;
            m_invalid  = false;
            m_undoOpen = false;
            m_ringActive = false;             // no ring held yet
            m_ringDeg    = 0.0f;
            m_ringBase   = 0.0f;
            m_ringFresh  = false;
            m_angleBase.clear();

            m_construct = m_refImage = false;
            if ( !SelectionHasObjects() )
            {
                // R also serves a pure construction or reference-image selection; the
                // store's MoveBegin latches the baseline + undo snapshot and reports the
                // anchor centroid as the natural pivot.
                if ( KiwiConSel_CanMove() && KiwiConSel_MoveBegin( m_pivot ) )
                    m_construct = true;
                else if ( KiwiRefImage_CanMove() && KiwiRefImage_MoveBegin( m_pivot ) )
                    m_refImage = true;
                else
                {
                    Sys_Printf( "Rotate: nothing rotatable is selected.\n" );
                    return false;
                }
            }
            else
            {
                KiwiXform_CaptureEntityAngles( m_angleBase );
                // Latch the pivot once; repeated Select_GetMid calls would let it crawl
                // while rotating. This matches the ported rotate-mode nudge.
                Select_GetMid( m_pivot );
            }
            // A session pivot replaces row 0 of the ported rot_around matrix.
            m_pivotOverridden = PivotActive( m_pivot );
            m_pivotPlacing    = false;
            UpdateHud();
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            if ( TrackPivot( snap ) )         // V-placement owns the move
                return;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            if ( HandlePivotKey( vk ) )       // V / Esc while placing
                return true;
            return HandleAxisKey( vk, mods, false );
        }

        bool SupportsPivot() const override { return true; }
        const float *PivotAnchor() const override { return m_pivot; }
        void RefreshHud() override { UpdateHud(); }
        void ApplyPivot( const float p[3] ) override
        {
            // Moving a used rotation center first undoes everything turned about the
            // old center (the whole chain), then adopts the new center at zero degrees.
            UnwindAll();
            Copy3( p, m_pivot );
            m_pivotOverridden = true;
            UpdateHud();
        }

        // Rotate may yield to another transform unless V placement owns the keyboard.
        bool CanSwapTo( int commandId ) const override
        { (void)commandId; return !m_pivotPlacing; }

        bool GestureMoved() const override
        { return m_undoOpen || m_hasNum || !m_chain.empty() || fabsf( m_deg ) > KX_EPS; }

        void Commit() override
        {
            if ( m_construct )      KiwiConSel_MoveCommit();      // the snapshot IS the undo record
            else if ( m_refImage )  KiwiRefImage_MoveCommit();
            // Clean the ported float round-trip before the undo bracket closes.
            else if ( m_undoOpen )
                KiwiXform_SnapEntityAngles( m_angleBase, m_deg );
            m_angleBase.clear();
            m_undoOpen = false;
            m_pivotPlacing = false;
            m_construct = m_refImage = false;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            if ( m_construct )      KiwiConSel_MoveCancel();      // pops the snapshot
            else if ( m_refImage )  KiwiRefImage_MoveCancel();
            else                    UnwindAll();                  // exact inverse of every segment; the bracket also restores
            m_applied = 0.0f;
            m_chain.clear();
            m_angleBase.clear();
            m_undoOpen = false;
            m_pivotPlacing = false;
            m_construct = m_refImage = false;
            g_nUpdateBits = -1;
        }

        void DrawWorld() override
        {
            if ( m_pivotPlacing )
            {
                DrawPivotMarker( m_pivotWip, true );
                return;
            }
            DrawConstraint( m_pivot, m_pivot, m_pivot );
            if ( m_pivotOverridden )
                DrawPivotMarker( m_pivot, false );
        }

        // Ring handoff feeds the live command's degree scalar.
        const float *Pivot() const { return m_pivot; }

        void PresetAxis( int axis )
        {
            if ( axis < 0 || axis > 2 )
                return;
            SetConstraint( CON_AXIS, axis );
            UpdateHud();
        }

        // A ring grab latches the angle to which this grab's sweep is added; there is
        // no cursor-position mapping to rebase.
        void HandleGrab( bool held ) override
        {
            if ( !held )
                return;
            m_ringBase  = m_deg;              // the sweep is measured FROM here
            m_ringFresh = true;               // 5° snap waits for real travel
        }

        void RingFeed( bool active, float sweepDegrees )
        {
            // Ring release latches the current angle. Feeds are sweeps since this grab,
            // added to m_ringBase so a second grab starts at the already-applied pose.
            m_ringActive = active;
            if ( active )
            {
                if ( m_ringFresh && fabsf( sweepDegrees ) > 0.0f )
                    m_ringFresh = false;
                m_ringDeg = m_ringBase + sweepDegrees;
            }
            Recompute();
        }

    protected:
        void SetConstraint( constraint_t con, int axis ) override
        {
            if ( con != CON_AXIS )
                return;                       // R has no free / plane mode
            if ( axis == m_axis )
                return;                       // second press of the same axis: keep it
            // Re-aiming keeps what the old axis turned: it becomes a finished segment
            // of the chain and the new axis starts at zero, so ring grabs (or X/Y/Z
            // keys) compose within one gesture, one undo record.
            if ( fabsf( m_applied ) > KX_EPS )
            {
                chainSeg_t seg;
                seg.axis = m_axis;
                seg.deg  = m_applied;
                m_chain.push_back( seg );
            }
            m_applied = 0.0f;
            m_deg     = 0.0f;
            m_ringBase = 0.0f;
            m_ringDeg  = 0.0f;
            m_axis    = axis;
            Recompute();
        }

        // Rotate changes only from an active ring sweep or typed degrees. Bare mouse
        // movement leaves m_deg latched and cannot open undo. Five-degree snapping
        // applies only while a non-fresh ring is active.
        void Recompute() override
        {
            float deg = m_deg;

            if ( m_ringActive )
                deg = m_ringDeg;

            if ( m_hasNum )
                deg = NumRaw();                             // exact degrees
            else if ( m_ringActive && !m_ringFresh && SnapActive() )
                deg = floorf( deg / KX_ANGLE_STEP + 0.5f ) * KX_ANGLE_STEP;

            m_deg = deg;
            ApplyDelta( m_deg - m_applied );
            UpdateHud();
        }

    private:
        void ApplyDelta( float delta )
        {
            if ( fabsf( delta ) <= KX_EPS )
                return;
            if ( m_construct || m_refImage )
            {
                // Absolute from the store baseline (kiwi_transform.h rule 1); the
                // store's own snapshot is the undo record, so no legacy bracket opens.
                m_applied += delta;
                ApplyStore();
                return;
            }
            if ( !SelectionHasObjects() )
                return;
            OpenUndo( "rotate selection" );
            RotateBrushes( m_axis, delta );
            m_applied += delta;
        }

        // Ported rotation pattern: pivot in row 0, Select_RotateAxis fills rows 1..3,
        // then Select_ApplyMatrix_SelectedBrushes applies the residual.  Brushes are
        // turned incrementally, so a chain composes by itself.
        void RotateBrushes( int axis, float delta )
        {
            float rot_around[4][3];
            Copy3( m_pivot, rot_around[0] );
            Select_RotateAxis( axis, delta, (float (*)[4][3])rot_around );
            Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], delta, 0 );
            g_nUpdateBits = -1;
        }

        // One world-axis turn as the 3x3 the stores apply, in the stores' own sense
        // (negated degrees, right-hand rule: what Select_RotateAxis does for brushes).
        static void AxisMatrix( int axis, float degrees, float m[3][3] )
        {
            const float rad = -degrees * 3.14159265358979323846f / 180.0f;
            const float c = cosf( rad ), sn = sinf( rad );
            const int i = ( axis + 1 ) % 3, j = ( axis + 2 ) % 3;
            for ( int a = 0; a < 3; ++a )
                for ( int b = 0; b < 3; ++b )
                    m[a][b] = ( a == b ) ? 1.0f : 0.0f;
            m[i][i] = c;  m[i][j] = -sn;
            m[j][i] = sn; m[j][j] = c;
        }

        // The whole gesture as one rotation from the store baseline: finished
        // segments in order, then the live axis.  Stores are absolute, so every
        // feed re-applies the composition.
        void ApplyStore()
        {
            float total[3][3];
            for ( int a = 0; a < 3; ++a )
                for ( int b = 0; b < 3; ++b )
                    total[a][b] = ( a == b ) ? 1.0f : 0.0f;
            for ( size_t s = 0; s <= m_chain.size(); ++s )
            {
                const int   axis = s < m_chain.size() ? m_chain[s].axis : m_axis;
                const float deg  = s < m_chain.size() ? m_chain[s].deg  : m_applied;
                float step[3][3], next[3][3];
                AxisMatrix( axis, deg, step );
                for ( int a = 0; a < 3; ++a )
                    for ( int b = 0; b < 3; ++b )
                        next[a][b] = step[a][0] * total[0][b] + step[a][1] * total[1][b] + step[a][2] * total[2][b];
                memcpy( total, next, sizeof( total ) );
            }
            if ( m_construct ) KiwiConSel_RotateApplyMatrix( m_pivot, total );
            else               KiwiRefImage_RotateApplyMatrix( m_pivot, total );
            g_nUpdateBits = -1;
        }

        // Back to the baseline pose: stores re-apply an identity, brushes are turned
        // back segment by segment in reverse order.
        void UnwindAll()
        {
            if ( m_construct || m_refImage )
            {
                m_chain.clear();
                m_applied = 0.0f;
                m_deg     = 0.0f;
                ApplyStore();
                return;
            }
            if ( fabsf( m_applied ) > KX_EPS && SelectionHasObjects() )
                RotateBrushes( m_axis, -m_applied );
            for ( size_t s = m_chain.size(); s-- > 0; )
                if ( SelectionHasObjects() )
                    RotateBrushes( m_chain[s].axis, -m_chain[s].deg );
            m_chain.clear();
            m_applied = 0.0f;
            m_deg     = 0.0f;
        }

        void UpdateHud()
        {
            if ( m_pivotPlacing )
            {
                UpdatePivotHud();
                return;
            }
            // Finished segments of a chained turn, e.g. "  after X 30.0, Z -15.0".
            char chain[160] = { 0 };
            for ( size_t s = 0; s < m_chain.size(); ++s )
            {
                char one[40];
                _snprintf( one, sizeof( one ), "%s%s %.1f", s ? ", " : "  after ",
                           AxisName( m_chain[s].axis ), (double)m_chain[s].deg );
                one[sizeof( one ) - 1] = '\0';
                strncat( chain, one, sizeof( chain ) - strlen( chain ) - 1 );
            }
            // Idle rotate HUD explains the ring/numeric gate.
            if ( !m_ringActive && !m_hasNum && fabsf( m_deg ) <= KX_EPS )
            {
                SetHud( "%s  axis %s  grab a ring / type degrees%s%s", What(), AxisName( m_axis ),
                        chain, m_pivotOverridden ? "  [pivot moved]" : "  (V moves the pivot)" );
                return;
            }
            SetHud( "%s  axis %s  %.1f deg%s%s", What(), AxisName( m_axis ), (double)m_deg,
                    chain, m_pivotOverridden ? "  [pivot moved]" : "" );
        }

        const char *What() const
        { return m_construct ? "construction" : m_refImage ? "image" : "objects"; }

        float m_pivot[3] = { 0.0f, 0.0f, 0.0f };
        std::vector<xformEntityAngles_t> m_angleBase;
        bool  m_construct = false;       // the construction store owns this gesture
        bool  m_refImage  = false;       // the reference-image store owns it
        float m_deg      = 0.0f;
        float m_applied  = 0.0f;
        // Finished turns of this gesture, in application order; the live axis
        // (m_axis, m_applied) comes after them.  Cleared by Begin / Cancel / pivot.
        struct chainSeg_t { int axis; float deg; };
        std::vector<chainSeg_t> m_chain;
        bool  m_ringActive = false;      // a ring is held
        float m_ringDeg    = 0.0f;       // angle requested by the ring
        // Ring sweep base and press-frame snap freeze.
        float m_ringBase   = 0.0f;
        bool  m_ringFresh  = false;
        // Latch whether the current rotation uses the session pivot.
        bool  m_pivotOverridden = false;
    };

    // Whole-selection scale.
    class KiwiScaleCommand : public KiwiXformBase
    {
    public:
        const char *Name() const override { return "Scale"; }
        bool CanExecute() override { return KiwiXform_CanScale(); }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KXF_SCALE; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_factor;                  // a bare multiplier — KNUM_FACTOR
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 )
                return false;
            Copy3( m_pivot, out3 );
            return true;
        }

        void Rebase() override
        {
            // Rebase the horizontal-pixel mapping at the current factor.
            int x, y;
            if ( !CursorPixels( &x, &y ) )
                return;
            m_startX     = x - (int)( ( m_factor - 1.0f ) / KX_SCALE_PER_PIXEL );
            m_haveStartX = true;
        }

        bool Begin() override
        {
            m_factor = 1.0f;
            m_applied[0] = m_applied[1] = m_applied[2] = 1.0f;
            m_con      = CON_FREE;            // uniform by default
            m_axis     = 0;
            m_hasNum   = false;
            m_invalid  = false;
            m_undoOpen = false;
            m_haveStartX = CursorPixels( &m_startX, &m_dummyY );

            m_construct = m_refImage = false;
            m_pivotOverridden = false;
            m_pivotPlacing    = false;
            if ( !SelectionHasObjects() )
            {
                // S also serves a pure construction or reference-image selection.
                if ( KiwiConSel_CanMove() && KiwiConSel_MoveBegin( m_pivot ) )
                    m_construct = true;
                else if ( KiwiRefImage_CanMove() && KiwiRefImage_MoveBegin( m_pivot ) )
                    m_refImage = true;
                else
                {
                    Sys_Printf( "Scale: nothing scalable is selected.\n" );
                    return false;
                }
            }
            else
            {
                // Select_Scale (select.cpp 0x48FDC0) scales about the selection centre it
                // recomputes per call; a session pivot re-centres the result (ApplyWanted).
                Select_GetMid( m_pivot );
            }
            m_pivotOverridden = PivotActive( m_pivot );
            UpdateHud();
            return true;
        }

        // Scale may yield to another transform unless V placement owns the keyboard.
        bool CanSwapTo( int commandId ) const override
        { (void)commandId; return !m_pivotPlacing; }

        bool SupportsPivot() const override { return true; }
        const float *PivotAnchor() const override { return m_pivot; }
        void RefreshHud() override { UpdateHud(); }
        void ApplyPivot( const float p[3] ) override
        {
            // Undo the residual about the old centre, adopt the new one, then reapply the
            // same factor about it.
            ApplyWanted( 1.0f, 1.0f, 1.0f );
            Copy3( p, m_pivot );
            m_pivotOverridden = true;
            Recompute();
        }

        bool GestureMoved() const override
        { return m_undoOpen || m_hasNum || fabsf( m_factor - 1.0f ) > KX_EPS; }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            if ( TrackPivot( snap ) )         // V-placement owns the move
                return;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            if ( HandlePivotKey( vk ) )       // V / Esc while placing
                return true;
            // X/Y/Z = one axis, Shift+X/Y/Z = the plane across that axis (two axes).
            return HandleAxisKey( vk, mods, true );
        }

        void Commit() override
        {
            if ( m_construct )      KiwiConSel_MoveCommit();      // the snapshot IS the undo record
            else if ( m_refImage )  KiwiRefImage_MoveCommit();
            m_undoOpen = false;
            m_pivotPlacing = false;
            m_construct = m_refImage = false;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            if ( m_construct )      KiwiConSel_MoveCancel();      // pops the snapshot
            else if ( m_refImage )  KiwiRefImage_MoveCancel();
            else                    ApplyWanted( 1.0f, 1.0f, 1.0f );
            m_undoOpen = false;
            m_pivotPlacing = false;
            m_construct = m_refImage = false;
            g_nUpdateBits = -1;
        }

        void DrawWorld() override
        {
            if ( m_pivotPlacing )
            {
                DrawPivotMarker( m_pivotWip, true );
                return;
            }
            DrawConstraint( m_pivot, m_pivot, m_pivot );
            if ( m_pivotOverridden )
                DrawPivotMarker( m_pivot, false );
        }

    protected:
        void SetConstraint( constraint_t con, int axis ) override
        {
            // CON_FREE restores uniform scale; CON_AXIS scales one axis; CON_PLANE scales
            // the two axes across `axis` (the gizmo's plane squares / Shift+X/Y/Z).
            if ( con == CON_FREE )
            {
                m_con = CON_FREE;
                Recompute();
                return;
            }
            if ( con != CON_AXIS && con != CON_PLANE )
                return;
            m_con  = con;
            m_axis = axis;
            Recompute();
        }

        void Recompute() override
        {
            float f = m_factor;
            int x, y;
            if ( m_haveStartX && CursorPixels( &x, &y ) )
                f = 1.0f + (float)( x - m_startX ) * KX_SCALE_PER_PIXEL;

            if ( m_hasNum )
                f = NumRaw();                                 // exact factor
            if ( f < KX_SCALE_MIN )
                f = KX_SCALE_MIN;

            m_factor = f;
            float want[3] = { f, f, f };
            if ( m_con == CON_AXIS )
            {
                for ( int k = 0; k < 3; ++k )
                    if ( k != m_axis )
                        want[k] = 1.0f;
            }
            else if ( m_con == CON_PLANE )
                want[m_axis] = 1.0f;

            ApplyWanted( want[0], want[1], want[2] );
            UpdateHud();
        }

    private:
        // Select_Scale multiplies, so residuals are ratios, not differences.
        void ApplyWanted( float sx, float sy, float sz )
        {
            const float want[3] = { sx, sy, sz };
            if ( m_construct || m_refImage )
            {
                // Absolute from the store baseline; the store's snapshot is the undo record.
                bool same = true;
                for ( int k = 0; k < 3 && same; ++k )
                    same = fabsf( want[k] - m_applied[k] ) <= 1.0e-6f;
                if ( same )
                    return;
                if ( m_construct ) KiwiConSel_ScaleApply( m_pivot, want );
                else               KiwiRefImage_ScaleApply( m_pivot, want );
                m_applied[0] = want[0]; m_applied[1] = want[1]; m_applied[2] = want[2];
                g_nUpdateBits = -1;
                return;
            }
            float ratio[3];
            bool any = false;
            for ( int k = 0; k < 3; ++k )
            {
                ratio[k] = ( fabsf( m_applied[k] ) > 1.0e-6f ) ? ( want[k] / m_applied[k] ) : 1.0f;
                if ( fabsf( ratio[k] - 1.0f ) > 1.0e-5f )
                    any = true;
            }
            if ( !any || !SelectionHasObjects() )
                return;
            OpenUndo( "scale selection" );
            float mid[3];
            Select_GetMid( mid );                 // the centre Select_Scale scales about
            Select_Scale( ratio[0], ratio[1], ratio[2] );
            if ( m_pivotOverridden )
            {
                // Scaling about P equals scaling about the centre plus a shift of
                // (centre - P) * (ratio - 1) on each axis — re-centre on the session pivot.
                float shift[3];
                for ( int k = 0; k < 3; ++k )
                    shift[k] = ( mid[k] - m_pivot[k] ) * ( ratio[k] - 1.0f );
                Select_Move( shift, 0 );
            }
            m_applied[0] = want[0]; m_applied[1] = want[1]; m_applied[2] = want[2];
            g_nUpdateBits = -1;
        }

        const char *What() const
        { return m_construct ? "construction" : m_refImage ? "image" : "objects"; }

        void UpdateHud()
        {
            if ( m_pivotPlacing )
            {
                UpdatePivotHud();
                return;
            }
            const char *tail = m_pivotOverridden ? "  [pivot moved]" : "";
            if ( m_con == CON_AXIS )
                SetHud( "%s  axis %s  x%.3f%s", What(), AxisName( m_axis ), (double)m_factor, tail );
            else if ( m_con == CON_PLANE )
                SetHud( "%s  plane %s  x%.3f%s", What(), AxisName( m_axis ), (double)m_factor, tail );
            else
                SetHud( "%s  uniform  x%.3f%s", What(), (double)m_factor, tail );
        }

        float m_pivot[3]   = { 0.0f, 0.0f, 0.0f };
        float m_factor     = 1.0f;
        float m_applied[3] = { 1.0f, 1.0f, 1.0f };
        int   m_startX     = 0;
        int   m_dummyY     = 0;
        bool  m_haveStartX = false;
        bool  m_construct  = false;       // the construction store owns this gesture
        bool  m_refImage   = false;       // the reference-image store owns it
        bool  m_pivotOverridden = false;  // scaling about a session (V) pivot
    };

    KiwiMoveCommand   s_move;
    KiwiRotateCommand s_rotate;
    KiwiScaleCommand  s_scale;
}

bool KiwiDrop_ComputePlacement( const ray_t &ray,
                                const float modelMins[3], const float modelMaxs[3],
                                const float angles[3], float scale,
                                float outOrigin[3],
                                float outWorldMins[3], float outWorldMaxs[3],
                                const float *supportCorners, int supportCornerCount )
{
    if ( !outOrigin || !KiwiDrop_BoundsValid( modelMins, modelMaxs ) )
        return false;

    float modelCorners[8][3];
    float relativeMins[3], relativeMaxs[3];
    if ( !KiwiDrop_TransformBounds( modelMins, modelMaxs, angles, scale,
                                    relativeMins, relativeMaxs, modelCorners ) )
        return false;

    const float *corners = &modelCorners[0][0];
    int cornerCount = 8;
    if ( supportCorners && supportCornerCount > 0 )
    {
        corners = supportCorners;
        cornerCount = supportCornerCount;
        relativeMins[0] = relativeMins[1] = relativeMins[2] = FLT_MAX;
        relativeMaxs[0] = relativeMaxs[1] = relativeMaxs[2] = -FLT_MAX;
        for ( int corner = 0; corner < cornerCount; ++corner )
            for ( int axis = 0; axis < 3; ++axis )
            {
                const float value = corners[3 * corner + axis];
                if ( !_finite( value ) )
                    return false;
                if ( value < relativeMins[axis] ) relativeMins[axis] = value;
                if ( value > relativeMaxs[axis] ) relativeMaxs[axis] = value;
            }
    }

    kiwiDropHit_t hit;
    const bool excludeDraggedModels = ( supportCorners && supportCornerCount > 0 )
                                    || KiwiDrop_Active();
    if ( !KiwiDrop_Trace( ray, excludeDraggedModels, &hit ) )
        return false;
    float placeX = hit.point[0];
    float placeY = hit.point[1];
    if ( KiwiCmd_SnapEngaged() )
    {
        float snapped[3];
        if ( KiwiGrid_Snap( hit.point, snapped ) )
        {
            placeX = snapped[0];
            placeY = snapped[1];
        }
    }

    // Seat the origin directly on the hit surface. No corner-clearing lift and no
    // hover margin: CoD models are authored with the origin at ground contact, so
    // letting the bbox (grass skirts, tree roots, rock undersides) clip into the
    // surface is the desired placement.
    outOrigin[0] = placeX;
    outOrigin[1] = placeY;
    outOrigin[2] = hit.point[2];

    if ( outWorldMins && outWorldMaxs )
        for ( int k = 0; k < 3; ++k )
        {
            outWorldMins[k] = outOrigin[k] + relativeMins[k];
            outWorldMaxs[k] = outOrigin[k] + relativeMaxs[k];
        }
    return true;
}

bool KiwiDrop_BeginAt( int imgX, int imgY )
{
    if ( KiwiCmd_Active() )
        return false;
    std::vector<selbrush_t *> selectedModels;
    if ( !DropSelectionOnlyModels( &selectedModels ) )
        return false;

    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return false;
    const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT );
    if ( !hit.valid || !hit.item.brush || !KiwiDrop_IsModelEntity( hit.item.brush ) )
        return false;

    bool hitSelectedModel = false;
    for ( size_t i = 0; i < selectedModels.size(); ++i )
        if ( selectedModels[i]->owner == hit.item.brush->owner )
            hitSelectedModel = true;
    if ( !hitSelectedModel )
        return false;

    s_move.ArmDrop( hit.item.brush );
    if ( !KiwiCmd_Start( KIWI_CMD_MOVE ) )
    {
        s_move.ClearDropArm();
        return false;
    }
    return true;
}

bool KiwiDrop_Active()
{
    return KiwiCmd_Active() == &s_move && s_move.DropMode();
}

bool KiwiXform_CanMove()
{
    sel_kind_t k;
    if ( DominantKind( &k, false ) )       // palette-rate: skip the liveness walk
        return true;
    // G also handles pure construction and reference-image selections.
    return KiwiConSel_CanMove() || KiwiRefImage_CanMove();
}

bool KiwiXform_CanRotate()
{
    // R also handles pure construction and reference-image selections.
    return SelectionHasObjects() || KiwiConSel_CanMove() || KiwiRefImage_CanMove();
}

bool KiwiXform_CanScale()
{
    return SelectionHasObjects() || KiwiConSel_CanMove() || KiwiRefImage_CanMove();
}

void KiwiXform_RegisterCommands()
{
    // G/R/S bindings come from the modern profile, not these registry rows.
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    Radiant_RegisterCommand( "KiwiTransformMove",   0, 0, KIWI_CMD_MOVE );
    Radiant_RegisterCommand( "KiwiTransformRotate", 0, 0, KIWI_CMD_ROTATE );
    Radiant_RegisterCommand( "KiwiTransformScale",  0, 0, KIWI_CMD_SCALE );
}

KiwiEditorCommand *KiwiXform_CommandForId( int commandId )
{
    switch ( commandId )
    {
    case KIWI_CMD_MOVE:   return &s_move;
    case KIWI_CMD_ROTATE: return &s_rotate;
    case KIWI_CMD_SCALE:  return &s_scale;
    default:              return 0;
    }
}

bool KiwiXform_DominantKind( sel_kind_t *out )
{
    if ( !out )
        return false;
    return DominantKind( out );                 // liveness-checked (the Begin-rate path)
}

void KiwiXform_PresetMoveConstraint( int con, int axis )
{
    // Ignore presets unless the move command is active.
    if ( KiwiCmd_Active() != &s_move )
        return;
    s_move.PresetConstraint( con, axis );
}

// Identity guards keep gizmo reads/presets bound to the live command.
bool KiwiXform_IsMoveActive()
{
    return KiwiCmd_Active() == &s_move && !s_move.DropMode();
}

bool KiwiXform_IsRotateActive()
{
    return KiwiCmd_Active() == &s_rotate;
}

bool KiwiXform_ActivePivot( float *out3 )
{
    if ( !out3 )
        return false;
    if ( KiwiCmd_Active() == &s_move )
    {
        // Move exposes its live anchor for draw/hit-test while MapCursor keeps using
        // latched m_ref.
        s_move.LiveAnchor( out3 );
        return true;
    }
    if ( KiwiCmd_Active() == &s_rotate )
    {
        Copy3( s_rotate.Pivot(), out3 );
        return true;
    }
    return false;
}

void KiwiXform_PresetRotateAxis( int axis )
{
    if ( KiwiCmd_Active() != &s_rotate )
        return;
    s_rotate.PresetAxis( axis );
}

void KiwiXform_FeedRotateDegrees( bool active, float degrees )
{
    if ( KiwiCmd_Active() != &s_rotate )
        return;
    s_rotate.RingFeed( active, degrees );
}

// Handle-grab state is raised only through KiwiEditorCommand::HandleGrab.
bool KiwiXform_ActivePushDir( float *out3 )
{
    if ( !out3 || KiwiCmd_Active() != &s_move )
        return false;
    return s_move.PushDir( out3 );
}

bool KiwiXform_PivotOverride( float *out3 )
{
    return PivotActive( out3 );
}

// Draw candidate dots only while V places a pivot or a move handle is held;
// parked transforms stay uncluttered.
bool KiwiXform_WantsSnapDots()
{
    if ( KiwiCmd_Active() == &s_move )
        return s_move.PivotPlacingNow() || s_move.HandleHeld();
    // Rotate rings aim at angles, so only rotate pivot placement requests dots.
    if ( KiwiCmd_Active() == &s_rotate )
        return s_rotate.PivotPlacingNow();
    return false;
}

bool KiwiXform_PivotPlacing()
{
    // Query protected placement state through active-command identity guards.
    if ( KiwiCmd_Active() == &s_move )
        return s_move.PivotPlacingNow();
    if ( KiwiCmd_Active() == &s_rotate )
        return s_rotate.PivotPlacingNow();
    return false;
}

// One-shot face push shared with negative extrude. It uses the interactive
// push's depth/delete rule, validity gate, texture-lock bracket, and classic
// delete/undo ordering. Unlike the interactive path it handles one face once;
// the framework closes the undo record opened here.
int KiwiXform_FacePushDepth( selbrush_t *node, int faceIndex, float *outDepth )
{
    if ( !outDepth || !node || !Sel_BrushLive( node ) )
        return 0;
    brush_t *def = node->def;
    if ( !def || !def->faces || faceIndex < 0 || faceIndex >= def->faceCount )
        return 0;
    const face_t *f = &def->faces[faceIndex];
    if ( !f->w )
        return 0;
    *outDepth = FaceDepthAlong( def, f->plane.normal, &f->planepts[0][0] );
    return 1;
}

int KiwiXform_PushFaceOnce( selbrush_t *node, int faceIndex, float dist,
                            const char *undoOp, const char **outWhy )
{
    const char *localWhy = "unknown";
    if ( !outWhy )
        outWhy = &localWhy;
    *outWhy = "unknown";

    if ( !node || !Sel_BrushLive( node ) )
    {
        *outWhy = "the brush went away";
        return KXPUSH_FAILED;
    }
    brush_t *def = node->def;
    if ( !def || !def->faces || faceIndex < 0 || faceIndex >= def->faceCount
      || !def->faces[faceIndex].w )
    {
        *outWhy = "no such face";
        return KXPUSH_FAILED;
    }
    if ( !undoOp )
        undoOp = "push face";

    face_t *f = &def->faces[faceIndex];

    // Rejection baseline includes plane points and texture definition.
    float normal[3];
    float basePts[9];
    byte  baseMtl[sizeof( MaterialDef ) * 4];
    Copy3( f->plane.normal, normal );
    memcpy( basePts, &f->planepts[0][0], sizeof( basePts ) );
    memcpy( baseMtl, &f->mtldef[0],      sizeof( baseMtl ) );

    // Delete before reshape when inward distance reaches baseline thickness.
    const float depth = FaceDepthAlong( def, normal, basePts );
    if ( depth > KX_EPS && dist <= -depth + KX_EPS )
    {
        // Mirror CommitDelete ordering; no restore is needed before the one-shot mutation.
        Select_Deselect( 1 );                     // the list must be EMPTY at head

        KiwiCmd_UndoBegin( undoOp );
        KiwiCmd_UndoCoverBrush( node );       // face is absent from selected_brushes
        // Add the entity after the brush; Undo_AddEntity_W takes the entity def cast
        // used by the classic delete path.
        if ( node->owner && node->owner->def )
            Undo_AddEntity_W( (entity_s *)node->owner->def );

        Select_Brush( node, 0, 0, 0 );
        Select_Delete();                          // the CLASSIC core, unmodified
        Sel_Clear( KiwiSel() );

        *outWhy = "pushed through";
        g_nUpdateBits = -1;
        return KXPUSH_DELETED;
    }

    KiwiCmd_UndoBegin( undoOp );
    KiwiCmd_UndoCoverBrush( node );          // face is absent from selected_brushes

    byte lockFlags[3];
    lockFlags[0] = (byte)( g_PrefsDlg->m_bTextureLock  != 0 );
    lockFlags[1] = (byte)( g_PrefsDlg->m_bLightmapLock != 0 );
    lockFlags[2] = 1;
    float saveBuf[19];

    // One ported texture-lock pass over baseline; refresh the plane normal before
    // TexLockSave.
    Face_MakePlane( f );
    if ( f->w )
        Ed_FaceTexLockSave( saveBuf, f );
    for ( int p = 0; p < 3; ++p )
        Mad3( &basePts[p * 3], normal, dist, f->planepts[p] );
    if ( f->w )
        Ed_FaceTexLockReproject( f, saveBuf, lockFlags );

    KiwiValid_Rebuild( def );

    const char *why = 0;
    if ( !KiwiValid_CheckBrush( def, &why ) )
    {
        // Rejection restores geometry/texture and cancels the undo record.
        memcpy( &f->mtldef[0],      baseMtl, sizeof( baseMtl ) );
        memcpy( &f->planepts[0][0], basePts, sizeof( basePts ) );
        KiwiValid_Rebuild( def );
        KiwiCmd_UndoCancel();
        *outWhy = why ? why : "invalid geometry";
        g_nUpdateBits = -1;
        return KXPUSH_FAILED;
    }

    // Carry associated fillets only after validity succeeds and inside the same
    // undo record. Chamfer-plane fillets are reported rather than approximated.
    {
        const float distBefore = Dot3( normal, basePts );
        int   skipped = 0;
        const int carried = KiwiFillet_CarryOnPlaneMove( def, faceIndex, normal,
                                                         distBefore, dist, &skipped );
        if ( carried > 0 )
            Sys_Printf( "Push: %i fillet patch(es) followed the face.\n", carried );
        if ( skipped > 0 )
            Sys_Printf( "Push: %i fillet patch(es) were NOT moved — this face is "
                        "their chamfer plane, and carrying that correctly means "
                        "re-solving the radius.  Re-run the bevel on that edge.\n",
                        skipped );
    }

    *outWhy = 0;
    g_nUpdateBits = -1;
    return KXPUSH_PUSHED;
}
