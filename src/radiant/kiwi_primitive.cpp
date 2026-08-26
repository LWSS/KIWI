#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Primitive command implementation; brush geometry remains delegated to the
// audited prism writer or the ported Brush_MakeSided variants.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s

#include "kiwi_primitive.h"
#include "kiwi_camera.h"            // KiwiCam_AxisPortrayable
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_extrude.h"
#include "kiwi_lines.h"
#include "kiwi_material.h"          // KiwiMtl_RealizePatch
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"          // Sel_* / KiwiSel_SetModeMask
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // shared Dot3/Sub3 helpers

#include <imgui/imgui.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points; signatures match their definitions.
extern int       Sys_Printf( const char *fmt, ... );                    // win_qe3.cpp
extern int       g_nUpdateBits;                                         // 0x25D5A74 (mainfrm.cpp)
// `planeptsSrc` is the ported definition's name; these callers pass material data.
extern brush_t  *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls ); // brush.cpp:465 (0x4751e0)
extern void      Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:510 (0x475300)
extern void      Brush_Free_R( brush_t *def );                          // brush.cpp:706 (0x475af0)
extern void      Select_Deselect( int bAlsoFreeFaces );                 // select.cpp:1444 (0x48E800)
extern bool      Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
extern void      Radiant_ExecCommand( unsigned int cmdId );             // mainfrm.cpp:4054
// Auto-height is signed toward the camera; this accessor never returns NULL.
extern camera_s *Ed_Camera();                                           // camwnd.cpp:161

// Brush_MakeSided's first `int` really carries brush_t* (0x4731E0).
extern void      Brush_MakeSided( int a1, unsigned int sides, int axis, char snap ); // brush.cpp:3405 (0x4731E0)
extern void      Brush_MakeSidedCone( int sides );                      // brush.cpp:3653 (0x47BC10)
extern void      Brush_MakeSidedSphere( int sides );                    // brush.cpp:3728 (0x47BE90)

// xywnd.cpp:1563 forwarder; see kiwi_extrude.h.
extern void      Ed_EnsureCurrentMaterial_Kiwi();
// entity.cpp 0x25D5B30 - worldspawn.
extern entity_s *world_entity;

// Patch-cylinder entry points; signatures match their definitions.
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );        // brush.cpp:669  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                       // brush.cpp:927  0x4765A0
extern patchMesh_t *MakeNewPatch();                                          // pmesh.cpp:136  0x437AC0
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );  // pmesh.cpp:840  0x4386A0
extern void         Patch_KiwiFinishNew( patchMesh_t *p );                    // pmesh.cpp:1558
// radiant_registry.cpp:31 / :44.
extern int          Radiant_ProfileGetInt( const char *section, const char *entry, int defVal );
extern bool         Radiant_ProfileSetInt( const char *section, const char *entry, int value );

namespace
{
    const float KPRIM_COL_PREVIEW[3] = { 0.55f, 0.85f, 1.00f };   // matches the extrude preview
    const float KPRIM_COL_BAD[3]     = { 1.00f, 0.30f, 0.25f };

    enum primKind_t
    {
        KPRIM_BOX = 0,
        KPRIM_CYLINDER,
        KPRIM_SPHERE,
        KPRIM_CONE,
        // Centre box differs only in BoxLoopUV: first click is the centre and the
        // drag supplies half-extents; all staging and creation paths stay shared.
        KPRIM_BOX_CENTER
    };

    // The plane normal's WORLD axis, or -1 when the plane is not axis-aligned.
    // 0.999 rather than an exact compare because KiwiCon_MakePlane normalises and
    // orthogonalises, so an "XY" plane's normal is 1.0 to within float noise.
    int PlaneWorldAxis( const kconPlane_t &p )
    {
        for ( int k = 0; k < 3; ++k )
            if ( fabsf( p.normal[k] ) > 0.999f )
                return k;
        return -1;
    }

    // One ring of `segs` segments around `centre` in the plane spanned by the
    // orthonormal pair (e0, e1).  Used by every preview here.
    void EmitRing( const float centre[3], const float e0[3], const float e1[3],
                   float radius, int segs, float phase )
    {
        for ( int i = 0; i < segs; ++i )
        {
            const float a0 = phase + 6.283185307f * (float)i       / (float)segs;
            const float a1 = phase + 6.283185307f * (float)( i + 1 ) / (float)segs;
            float p0[3], p1[3];
            for ( int k = 0; k < 3; ++k )
            {
                p0[k] = centre[k] + e0[k] * cosf( a0 ) * radius + e1[k] * sinf( a0 ) * radius;
                p1[k] = centre[k] + e0[k] * cosf( a1 ) * radius + e1[k] * sinf( a1 ) * radius;
            }
            if ( !KiwiLines_Add( p0, p1 ) )
                return;
        }
    }

    // Numeric structs are copied but label pointers persist; field 0 is relabelled
    // per stage, so these string literals need static storage.
    const kiwiNumField_t KPRIM_FIELDS_BOX[1] =
    { { "size", KNUM_LENGTH, false } };
    const kiwiNumField_t KPRIM_FIELDS_RING[2] =
    { { "radius", KNUM_LENGTH, false },
      { "sides",  KNUM_COUNT,  false } };

    // Shared modal command; kind controls base interpretation and commit target.
    class KiwiPrimitiveCommand : public KiwiEditorCommand
    {
    public:
        explicit KiwiPrimitiveCommand( primKind_t kind ) : m_kind( kind ) {}

        const char *Name() const override
        {
            switch ( m_kind )
            {
            case KPRIM_BOX:        return "Box";
            case KPRIM_BOX_CENTER: return "Centre Box";
            case KPRIM_CYLINDER: return "Cylinder";
            case KPRIM_SPHERE:   return "Sphere";
            default:             return "Cone";
            }
        }

        // Primitive clicks place staged points; the command decides when to commit.
        bool WantsClicks() const override { return true; }
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        // Field 0 is relabelled for the active stage; field 1 is ring side count.
        int NumericFields( const kiwiNumField_t **out ) const override
        {
            if ( IsBox() )
            {
                *out = KPRIM_FIELDS_BOX;
                return 1;
            }
            *out = KPRIM_FIELDS_RING;
            return 2;
        }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            if ( field == 1 && !IsBox() )
            {
                *out = (float)m_sides;
                return true;
            }
            if ( field != 0 || m_stage < 1 )
                return false;
            if ( m_stage >= 2 )
            {
                *out = m_height;               // SIGNED: which side of the plane
                return true;
            }
            if ( IsBox() )
            {
                if ( !m_haveCur )
                    return false;
                // The box has no radius: report the LARGER base extent, which is
                // the number its own HUD prompt talks about.
                const float du = fabsf( m_cur[0] - m_p0[0] );
                const float dv = fabsf( m_cur[1] - m_p0[1] );
                *out = ( du > dv ) ? du : dv;
                return true;
            }
            *out = m_radius;
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || m_stage < 1 || !m_haveCur )
                return false;
            if ( m_stage >= 2 )
            {
                // The moving CAP, which is what the height drag is dragging.
                float base[3];
                KiwiCon_PlaneToWorld( m_plane, m_p0, base );
                for ( int k = 0; k < 3; ++k )
                    out3[k] = base[k] + m_plane.normal[k] * m_height;
                return true;
            }
            KiwiCon_PlaneToWorld( m_plane, m_cur, out3 );
            return true;
        }

        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( field == 1 && !IsBox() )
            {
                if ( !has )
                    return;                    // cleared: keep the current count
                // Counts pass through length conversion, so convert back to display units.
                int n = (int)floorf( Units_ToDisplay( world ) + 0.5f );
                const int lo = ( m_kind == KPRIM_SPHERE ) ? KPRIM_SPH_SIDES_MIN : KPRIM_CYL_SIDES_MIN;
                const int hi = ( m_kind == KPRIM_SPHERE ) ? KPRIM_SPH_SIDES_MAX : KPRIM_CYL_SIDES_MAX;
                if ( n < lo ) n = lo;
                if ( n > hi ) n = hi;
                m_sides = n;
                Recompute();
                g_nUpdateBits |= 1;
                return;
            }
            KiwiEditorCommand::NumericFieldChanged( field, has, world );
        }

        bool Begin() override
        {
            m_stage    = 0;
            m_haveCur  = false;
            m_hasNum   = false;
            m_numWorld = 0.0f;
            m_radius   = 0.0f;
            m_height   = 0.0f;
            m_invalid  = false;
            m_haveHeightStart = false;
            m_zBlocked = false;                // height view gate
            m_autoHeight = false;              // default-height path
            // Singletons must not carry the Ctrl edge detector across gestures.
            m_snap     = snap_result_t();
            m_absPrev  = false;
            m_absolute = false;
            m_hud[0]   = '\0';
            m_sides    = ( m_kind == KPRIM_SPHERE ) ? KPRIM_SPH_SIDES_DEF : KPRIM_CYL_SIDES_DEF;
            // Patch mode is a remembered cylinder preference.
            m_patchMode = ( m_kind == KPRIM_CYLINDER ) && KiwiPrim_PatchMode();

            KiwiCon_AutoPlaneForTool();
            m_plane = KiwiCon_ActivePlane();

            // Ported cores cannot express these non-world-axis orientations.
            const int axis = PlaneWorldAxis( m_plane );
            if ( m_kind == KPRIM_CYLINDER && axis < 0 )
            {
                Sys_Printf( "Cylinder: needs an axis-aligned construction plane "
                            "(XY / XZ / YZ) — the ported Brush_MakeSided takes a WORLD axis.\n" );
                return false;
            }
            if ( m_kind == KPRIM_CONE && axis != 2 )
            {
                Sys_Printf( "Cone: needs the XY construction plane — the ported "
                            "Brush_MakeSidedCone fixes the apex at +Z.\n" );
                return false;
            }

            // Plane-placement snapping uses the construction plane instead of the
            // ground-grid fallback. Set only after refusal checks; Commit/Cancel clear it.
            KiwiCon_SetPlanePlacement( true );

            LatchFromCursor();
            RelabelStageField();               // "size" / "radius"
            UpdateHud();
            Sys_Printf( "%s: click to place, Esc cancels.\n", Name() );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            // The height ladder also consumes the ranked snap after base placement.
            m_snap = snap;
            if ( m_stage < 2 )
            {
                // Geometry snaps project onto the construction plane; otherwise the
                // cursor ray intersects it directly so fallback snaps cannot pull the
                // rubber band onto another plane.
                bool got = false;
                if ( snap.valid && KiwiSnap_IsGeometry( snap.type ) )
                {
                    KiwiCon_WorldToPlane( m_plane, snap.position, m_cur );
                    got = true;
                }
                if ( !got )
                {
                    ray_t ray;
                    float w[3];
                    if ( CursorRay( &ray ) && KiwiCon_RayPlaneBounded( m_plane, ray, w ) )
                    {
                        KiwiCon_WorldToPlane( m_plane, w, m_cur );
                        // World-anchored in-plane lattice.
                        KiwiCon_SnapUV( m_plane, m_cur );
                        got = true;
                    }
                    else if ( snap.valid )
                    {
                        KiwiCon_WorldToPlane( m_plane, snap.position, m_cur );
                        got = true;
                    }
                }
                if ( got )
                    m_haveCur = true;
                // Keep the last point through a transient unusable/edge-on frame.
            }
            Recompute();
            g_nUpdateBits |= 1;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        // Prompt storage must remain valid after this callback returns.
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_box[] = {
                { "Z", "Working plane" },
            };
            static const kiwiPrompt_t s_ring[] = {
                { "Z",   "Working plane" },
                { "[ ]", "Sides" },
            };
            if ( IsBox() )
            {
                *out = s_box;
                return (int)( sizeof( s_box ) / sizeof( s_box[0] ) );
            }
            *out = s_ring;
            return (int)( sizeof( s_ring ) / sizeof( s_ring[0] ) );
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            // Z cycles XY -> XZ -> YZ only before the base is placed; changing the
            // plane later would move an already committed point.
            if ( vk == 0x5A && !mods )                  // Z
            {
                if ( m_stage != 0 )
                {
                    Sys_Printf( "%s: the base is already placed — Esc and start again "
                                "to change the working plane.\n", Name() );
                    return true;
                }
                const int axis = PlaneWorldAxis( m_plane );
                // 2 (XY) -> 1 (XZ) -> 0 (YZ) -> 2.  A plane that is not axis-aligned
                // at all (derived from a slanted face) enters the cycle at XY.
                const int next = ( axis == 2 ) ? 1 : ( axis == 1 ) ? 0 : 2;
                KiwiCon_SetPlaneAxis( next );
                m_plane = KiwiCon_ActivePlane();
                if ( m_kind == KPRIM_CONE && next != 2 )
                    Sys_Printf( "Cone: the ported Brush_MakeSidedCone fixes the apex at "
                                "+Z — this plane will be refused on commit.\n" );
                LatchFromCursor();
                Recompute();
                Sys_Printf( "%s: working plane %s.\n", Name(), PlaneName() );
                g_nUpdateBits |= 1;
                return true;
            }

            // P toggles the remembered patch-cylinder mode. Cone/sphere lack a
            // faithful stock patch creation path; brackets adjust non-box side count.
            if ( m_kind == KPRIM_CYLINDER && !mods && vk == 0x50 )
            {
                m_patchMode = !m_patchMode;
                KiwiPrim_SetPatchMode( m_patchMode );
                UpdateHud();
                Sys_Printf( "Cylinder: %s.\n",
                            m_patchMode
                              ? "PATCH mode (experimental) — a q3 curve, no collision"
                              : "brush mode" );
                g_nUpdateBits |= 1;
                return true;
            }

            if ( !IsBox() && !mods && ( vk == 0xDB || vk == 0xDD ) )
            {
                const int lo = ( m_kind == KPRIM_SPHERE ) ? KPRIM_SPH_SIDES_MIN : KPRIM_CYL_SIDES_MIN;
                const int hi = ( m_kind == KPRIM_SPHERE ) ? KPRIM_SPH_SIDES_MAX : KPRIM_CYL_SIDES_MAX;
                m_sides += ( vk == 0xDD ) ? 1 : -1;
                if ( m_sides < lo ) m_sides = lo;
                if ( m_sides > hi ) m_sides = hi;
                UpdateHud();
                g_nUpdateBits |= 1;
                return true;
            }
            return false;
        }

        // Placing a point.  Returning false COMMITS (kiwi_command.h).
        bool Click() override
        {
            if ( !m_haveCur )
                return true;

            if ( m_stage == 0 )
            {
                m_p0[0] = m_cur[0];
                m_p0[1] = m_cur[1];
                m_stage = 1;
                ClearNumeric();
                RelabelStageField();
                Recompute();
                return true;
            }
            if ( m_stage == 1 )
            {
                if ( !BaseIsUsable() )
                {
                    Sys_Printf( "%s: zero base extent — pick a point away from the first.\n", Name() );
                    return true;
                }
                m_p1[0] = m_cur[0];
                m_p1[1] = m_cur[1];
                if ( FinalStage() == 1 )
                {
                    return false;                 // the sphere ends here
                }
                m_stage = 2;
                ClearNumeric();
                RelabelStageField();           // field 0 is "height" now
                LatchHeightStart();
                Recompute();
                // A blocked height axis takes the auto-height commit path.
                if ( AutoHeightIfBlocked() )
                    return false;
                return true;
            }
            // Refuse a cursor-derived height while its axis is unportrayable;
            // typed heights do not depend on the cursor mapping.
            if ( m_zBlocked && !m_hasNum )
            {
                Sys_Printf( "%s: this view looks straight along %s, so the cursor "
                            "cannot express a height — orbit away from the "
                            "top/bottom lock, or type one.\n", Name(), HeightAxisName() );
                return true;                      // stay in the gesture
            }
            return false;                         // the height click COMMITS
        }

        // Enter advances only with a complete typed value. Recompute has already
        // applied it; the final stage returns false so Enter commits as usual.
        bool AdvanceStage() override
        {
            if ( m_stage >= FinalStage() )
                return false;                     // last stage: Enter still commits

            if ( m_stage == 0 )
            {
                // Without a current cursor, m_p0 remains unchanged.
                if ( m_haveCur )
                {
                    m_p0[0] = m_cur[0];
                    m_p0[1] = m_cur[1];
                }
                m_stage = 1;
                ClearNumeric();
                RelabelStageField();
                Recompute();
                return true;
            }

            // Stage 1 -> 2; sphere was returned above because its final stage is 1.
            if ( !BaseIsUsable() )
            {
                Sys_Printf( "%s: zero base extent — type a size, or pick a point "
                            "away from the first.\n", Name() );
                return true;                      // consumed: do NOT fall through to commit
            }
            if ( m_haveCur )
            {
                m_p1[0] = m_cur[0];
                m_p1[1] = m_cur[1];
            }
            m_stage = 2;
            ClearNumeric();
            RelabelStageField();
            LatchHeightStart();
            Recompute();
            // Auto-height returns false here so Enter commits immediately.
            if ( AutoHeightIfBlocked() )
                return false;
            return true;
        }

        // On height-stage entry, an unportrayable axis receives the 5 ft default
        // toward the camera unless a height exists. True means commit immediately.
        bool AutoHeightIfBlocked()
        {
            if ( !m_zBlocked || m_hasNum || m_stage < 2 )
                return false;

            const camera_s *c = Ed_Camera();
            // The camera-facing cap lies on the -vpn side; the blocked-axis gate
            // guarantees this dot product is far from a tie.
            const float away = Dot3( m_plane.normal, c->vpn );
            m_height     = ( away < 0.0f ) ? KPRIM_AUTO_HEIGHT : -KPRIM_AUTO_HEIGHT;
            m_autoHeight = true;
            m_invalid    = !PlacementUsable();

            char hb[32];
            KiwiUnits_Format( hb, sizeof( hb ), KPRIM_AUTO_HEIGHT );
            Sys_Printf( "%s: this view looks straight along %s, so the height was set "
                        "to %s and the top face's push handle is armed — orbit and pull "
                        "the lollipop to change it.\n", Name(), HeightAxisName(), hb );
            return true;
        }

        void Commit() override
        {
            KiwiCon_SetPlanePlacement( false );        // clear plane-placement snapping
            if ( m_stage < FinalStage() )
            {
                Sys_Printf( "%s: not enough points — nothing created.\n", Name() );
                return;
            }
            if ( m_invalid )
            {
                Sys_Printf( "%s: degenerate placement — nothing created.\n", Name() );
                return;
            }
            Create();
            // Auto-height hands off only after Create has had a chance to land.
            if ( m_autoHeight )
                ArmTopFacePush();
        }

        // Arm the same state as a face click: select the cap, switch to face mode,
        // then start Move paused so only the lollipop grab begins motion.
        void ArmTopFacePush()
        {
            // Successful creation paths deselect before landing the new selection.
            selbrush_t *inst = ( selected_brushes.next != &selected_brushes )
                             ? selected_brushes.next : nullptr;
            if ( !inst || !inst->def || inst->patch || !inst->def->faces )
                return;                       // a patch cylinder has no face to push

            // m_height's sign identifies the cap facing the camera.
            const float s = ( m_height >= 0.0f ) ? 1.0f : -1.0f;
            const float want[3] = { m_plane.normal[0] * s,
                                    m_plane.normal[1] * s,
                                    m_plane.normal[2] * s };
            int   best = -1;
            float bestDot = 0.70710678f;      // 45 degrees — below this it is not a cap
            for ( int f = 0; f < inst->def->faceCount; ++f )
            {
                const float d = Dot3( inst->def->faces[f].plane.normal, want );
                if ( d > bestDot )
                {
                    bestDot = d;
                    best    = f;
                }
            }
            if ( best < 0 )
                return;                       // no cap found: leave the solid selected

            sel_item_t item;
            item.kind      = SEL_FACE;
            item.brush     = inst;
            item.faceIndex = best;
            if ( !Sel_ItemValid( item ) )
                return;

            selection_t &sel = KiwiSel();
            Sel_Clear( sel );
            Sel_Add( sel, item );
            Sel_SyncToLegacy();
            KiwiSel_SetModeMask( SEL_MASK_FACE );
            g_nUpdateBits |= ( W_CAMERA | W_XY | W_Z );

            // Starting Move inside Commit would close the wrong command bracket and
            // reset its numeric fields, so defer it until KiwiCmd_Commit finishes.
            KiwiCmd_StartDeferred( KIWI_CMD_MOVE, /*paused*/ true );
        }

        void Cancel() override
        {
            KiwiCon_SetPlanePlacement( false );        // clear plane-placement snapping
            m_stage = 0;
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( m_stage < 1 || !m_haveCur )
                return;
            const float *col = m_invalid ? KPRIM_COL_BAD : KPRIM_COL_PREVIEW;
            KiwiLines_Color( col[0], col[1], col[2] );

            float base[3];
            KiwiCon_PlaneToWorld( m_plane, m_p0, base );

            if ( IsBox() )
            {
                float loop[8];
                BoxLoopUV( loop );
                float lo, hi;
                CapOffsets( &lo, &hi );
                for ( int i = 0; i < 4; ++i )
                {
                    const int j = ( i + 1 ) & 3;
                    float a[3], b[3];
                    RingPoint( &loop[i * 2], hi, a );
                    RingPoint( &loop[j * 2], hi, b );
                    if ( !KiwiLines_Add( a, b ) )
                        return;
                    RingPoint( &loop[i * 2], lo, a );
                    RingPoint( &loop[j * 2], lo, b );
                    if ( !KiwiLines_Add( a, b ) )
                        return;
                    RingPoint( &loop[i * 2], lo, a );
                    RingPoint( &loop[i * 2], hi, b );
                    if ( !KiwiLines_Add( a, b ) )
                        return;
                }
                return;
            }

            if ( m_kind == KPRIM_SPHERE )
            {
                // Three great circles: the in-plane one plus the two that contain
                // the normal.  Enough to read as a sphere, cheap enough to fit the
                // command batch at any side count.
                EmitRing( base, m_plane.u, m_plane.v,      m_radius, m_sides * 2, 0.0f );
                EmitRing( base, m_plane.u, m_plane.normal, m_radius, m_sides * 2, 0.0f );
                EmitRing( base, m_plane.v, m_plane.normal, m_radius, m_sides * 2, 0.0f );
                return;
            }

            // Cylinder / cone: the base ring, plus a top ring or an apex fan.
            float lo, hi;
            CapOffsets( &lo, &hi );
            float loW[3], hiW[3];
            Mad3( base, m_plane.normal, lo, loW );
            Mad3( base, m_plane.normal, hi, hiW );
            EmitRing( loW, m_plane.u, m_plane.v, m_radius, m_sides, 0.0f );
            if ( m_kind == KPRIM_CYLINDER )
                EmitRing( hiW, m_plane.u, m_plane.v, m_radius, m_sides, 0.0f );
            for ( int i = 0; i < m_sides; ++i )
            {
                const float a = 6.283185307f * (float)i / (float)m_sides;
                float p[3], q[3];
                for ( int k = 0; k < 3; ++k )
                {
                    p[k] = loW[k] + m_plane.u[k] * cosf( a ) * m_radius
                                  + m_plane.v[k] * sinf( a ) * m_radius;
                    q[k] = ( m_kind == KPRIM_CYLINDER )
                         ? ( hiW[k] + m_plane.u[k] * cosf( a ) * m_radius
                                    + m_plane.v[k] * sinf( a ) * m_radius )
                         : hiW[k];                 // the cone's apex
                }
                if ( !KiwiLines_Add( p, q ) )
                    return;
            }
        }

    private:
        // Box kinds share every path except BoxLoopUV's base interpretation.
        bool IsBox() const
        { return m_kind == KPRIM_BOX || m_kind == KPRIM_BOX_CENTER; }

        // The sphere has no height stage; everything else does.
        int FinalStage() const { return ( m_kind == KPRIM_SPHERE ) ? 1 : 2; }

        // Relabel rather than re-register field 0: SetFields would clear the typed
        // value before the current stage consumes it.
        void RelabelStageField()
        {
            const char *label = ( m_stage >= 2 ) ? "height"
                              : ( m_kind == KPRIM_BOX_CENTER ) ? "half-size"
                              : ( IsBox() ) ? "size" : "radius";
            KiwiNum_SetFieldLabel( 0, label );
        }

        void ClearNumeric()
        {
            // Reset would replace this command's multi-field table; clear entry only.
            KiwiNum_ClearEntry();
            m_hasNum   = false;
            m_numWorld = 0.0f;
        }

        bool LatchFromCursor()
        {
            ray_t ray;
            if ( !Pick_RayFromCursor( &ray ) )
                return false;
            float w[3];
            if ( !KiwiCon_RayPlaneBounded( m_plane, ray, w ) )
                return false;
            KiwiCon_WorldToPlane( m_plane, w, m_cur );
            KiwiCon_SnapUV( m_plane, m_cur );      // world-anchored lattice
            m_haveCur = true;
            return true;
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        // Bias the latch by current height so h=Dot3(rel,n)-m_heightStart is
        // continuous when the view gate reopens or relative mode resumes.
        void LatchHeightStart()
        {
            m_haveHeightStart = false;
            KiwiCon_PlaneToWorld( m_plane, m_p0, m_ref );
            if ( !KiwiCam_AxisPortrayable( m_plane.normal ) )
                return;                        // no usable mapping — try again next frame
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                return;
            float rel[3];
            Sub3( p, m_ref, rel );
            m_heightStart     = Dot3( rel, m_plane.normal ) - m_height;
            m_haveHeightStart = true;
        }

        bool BaseIsUsable() const
        {
            if ( IsBox() )
                return fabsf( m_cur[0] - m_p0[0] ) >= KPRIM_MIN_EXTENT
                    && fabsf( m_cur[1] - m_p0[1] ) >= KPRIM_MIN_EXTENT;
            const float d0 = m_cur[0] - m_p0[0], d1 = m_cur[1] - m_p0[1];
            const float r  = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
            return r >= KPRIM_MIN_EXTENT;
        }

        void BoxLoopUV( float out[8] ) const
        {
            // The two corners are m_p0 and (stage 1) the cursor / (stage 2) m_p1.
            const float *c1 = ( m_stage >= 2 ) ? m_p1 : m_cur;
            float u0, u1, v0, v1;
            if ( m_kind == KPRIM_BOX_CENTER )
            {
                // Centre box uses p0 +/- the drag's absolute per-axis half-extents.
                const float hu = fabsf( c1[0] - m_p0[0] );
                const float hv = fabsf( c1[1] - m_p0[1] );
                u0 = m_p0[0] - hu; u1 = m_p0[0] + hu;
                v0 = m_p0[1] - hv; v1 = m_p0[1] + hv;
            }
            else
            {
                u0 = ( m_p0[0] < c1[0] ) ? m_p0[0] : c1[0];
                u1 = ( m_p0[0] < c1[0] ) ? c1[0]   : m_p0[0];
                v0 = ( m_p0[1] < c1[1] ) ? m_p0[1] : c1[1];
                v1 = ( m_p0[1] < c1[1] ) ? c1[1]   : m_p0[1];
            }
            out[0] = u0; out[1] = v0;              // CCW as KiwiExtrude_BuildPrismDef expects
            out[2] = u1; out[3] = v0;              // second CCW corner
            out[4] = u1; out[5] = v1;
            out[6] = u0; out[7] = v1;
        }

        void CapOffsets( float *lo, float *hi ) const
        {
            if ( m_kind == KPRIM_SPHERE )
            {
                // A sphere's AABB is a CUBE centred on the click: the "height" is
                // the diameter, split either side of the plane.
                *lo = -m_radius;
                *hi =  m_radius;
                return;
            }
            const float h = ( m_stage >= 2 ) ? m_height : 0.0f;
            *lo = ( h >= 0.0f ) ? 0.0f : h;
            *hi = ( h >= 0.0f ) ? h    : 0.0f;
        }

        void RingPoint( const float uv[2], float offset, float out[3] ) const
        {
            float base[3];
            KiwiCon_PlaneToWorld( m_plane, uv, base );
            Mad3( base, m_plane.normal, offset, out );
        }

        void Recompute()
        {
            if ( m_stage == 1 )
            {
                if ( IsBox() )
                {
                    // Same scalar rule as the corner rect tool: one typed number
                    // sizes the base SQUARE, signed by the drag direction.
                    if ( m_hasNum )
                    {
                        const float su = ( m_cur[0] >= m_p0[0] ) ? 1.0f : -1.0f;
                        const float sv = ( m_cur[1] >= m_p0[1] ) ? 1.0f : -1.0f;
                        m_cur[0] = m_p0[0] + su * m_numWorld;
                        m_cur[1] = m_p0[1] + sv * m_numWorld;
                    }
                }
                else
                {
                    const float d0 = m_cur[0] - m_p0[0], d1 = m_cur[1] - m_p0[1];
                    m_radius = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
                }
            }
            else if ( m_stage == 2 )
            {
                // RayAxis becomes ill-conditioned when the view aligns with the
                // height axis. Hold the prior height and drop the latch until the
                // view becomes portrayable, then rebase without a jump.
                const bool canZ = KiwiCam_AxisPortrayable( m_plane.normal );
                if ( !canZ )
                {
                    // Force a rebase when the gate reopens.
                    m_haveHeightStart = false;
                }
                else if ( !m_haveHeightStart )
                {
                    // Rebase at the cursor while preserving m_height.
                    LatchHeightStart();
                }
                m_zBlocked = !canZ;

                // Ctrl uses absolute cursor height; releasing it re-latches so
                // relative motion resumes continuously.
                const bool absNow = KiwiExt_AbsoluteHeld();
                if ( !absNow && m_absPrev )
                    LatchHeightStart();
                m_absPrev  = absNow;
                m_absolute = absNow && canZ && m_haveHeightStart;

                float h = m_height;
                float rawAbs  = h;               // the cursor's own height, absolute
                bool  haveRaw = false;
                ray_t ray;
                float p[3];
                if ( canZ && m_haveHeightStart && CursorRay( &ray )
                  && KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                {
                    float rel[3];
                    Sub3( p, m_ref, rel );
                    // m_ref lies on the base plane, so this projection is absolute height.
                    rawAbs  = Dot3( rel, m_plane.normal );
                    haveRaw = true;
                    h = absNow ? rawAbs : ( rawAbs - m_heightStart );
                }
                // Typed height is independent of the view-axis mapping.
                if ( m_hasNum )
                {
                    h = m_numWorld;
                }
                else if ( m_absolute && haveRaw )
                {
                    // Creation gestures snap by default; Ctrl keeps raw cursor height.
                }
                else if ( haveRaw )
                {
                    // Ranked geometry/face targets precede the major-aware lattice;
                    // KEXT_SELF_SNAP_BAND prevents the base outline gluing height to zero.
                    h = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, h, nullptr );
                }
                else
                {
                    // Without a cursor mapping, retain and grid-quantise held height.
                    const float g = KiwiUnits_GridSpacingWorld();
                    if ( g > 0.0f )
                        h = floorf( h / g + 0.5f ) * g;
                }
                m_height = h;
            }

            m_invalid = !PlacementUsable();
            UpdateHud();
        }

        bool PlacementUsable() const
        {
            if ( m_stage < 1 )
                return true;                       // nothing placed yet is not "invalid"
            if ( IsBox() )
            {
                const float *c1 = ( m_stage >= 2 ) ? m_p1 : m_cur;
                if ( fabsf( c1[0] - m_p0[0] ) < KPRIM_MIN_EXTENT
                  || fabsf( c1[1] - m_p0[1] ) < KPRIM_MIN_EXTENT )
                    return false;
            }
            else if ( m_radius < KPRIM_MIN_EXTENT )
            {
                return false;
            }
            if ( m_stage >= 2 && fabsf( m_height ) < KPRIM_MIN_EXTENT )
                return false;
            return true;
        }

        // Name the height axis (plane normal), not the plane itself.
        const char *HeightAxisName() const
        {
            switch ( PlaneWorldAxis( m_plane ) )
            {
            case 2:  return "Z";
            case 1:  return "Y";
            case 0:  return "X";
            default: return "the working plane's normal";
            }
        }

        // Stage-zero HUD exposes the active working plane.
        const char *PlaneName() const
        {
            switch ( PlaneWorldAxis( m_plane ) )
            {
            case 2:  return "XY (flat)";
            case 1:  return "XZ (upright)";
            case 0:  return "YZ (upright)";
            default: return "face";
            }
        }

        void UpdateHud()
        {
            char a[32], b[32];
            // Patch mode has a fixed control grid and no collision or caps; omit sides.
            if ( m_patchMode )
            {
                if ( m_stage == 0 )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "cylinder [PATCH, experimental]  plane %s  ·  "
                               "click: centre  ·  P: back to a brush", PlaneName() );
                else if ( m_stage == 1 )
                {
                    KiwiUnits_Format( a, sizeof( a ), m_radius );
                    _snprintf( m_hud, sizeof( m_hud ),
                               "cylinder [PATCH]  r %s  ·  click: rim  ·  "
                               "no collision — add caulk  ·  P: brush", a );
                }
                else
                {
                    KiwiUnits_Format( a, sizeof( a ), m_radius );
                    KiwiUnits_Format( b, sizeof( b ), fabsf( m_height ) );
                    _snprintf( m_hud, sizeof( m_hud ),
                               "cylinder [PATCH]  r %s  h %s  ·  q3 curve, no caps, "
                               "no collision  ·  P: brush", a, b );
                }
                m_hud[sizeof( m_hud ) - 1] = '\0';
                return;
            }
            if ( m_stage == 0 )
            {
                if ( IsBox() )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "%s  plane %s  ·  click: %s  ·  Z: working plane",
                               LowerName(), PlaneName(),
                               ( m_kind == KPRIM_BOX_CENTER ) ? "centre of the base"
                                                              : "first corner" );
                else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "%s  %i sides  plane %s  ·  click: centre  ·  [ ] sides  ·  Z: plane",
                               LowerName(), m_sides, PlaneName() );
            }
            else if ( m_stage == 1 )
            {
                if ( IsBox() )
                {
                    KiwiUnits_Format( a, sizeof( a ), fabsf( m_cur[0] - m_p0[0] ) );
                    KiwiUnits_Format( b, sizeof( b ), fabsf( m_cur[1] - m_p0[1] ) );
                    if ( m_kind == KPRIM_BOX_CENTER )
                    {
                        // Report full base dimensions, twice the half-extent drag.
                        KiwiUnits_Format( a, sizeof( a ), fabsf( m_cur[0] - m_p0[0] ) * 2.0f );
                        KiwiUnits_Format( b, sizeof( b ), fabsf( m_cur[1] - m_p0[1] ) * 2.0f );
                        _snprintf( m_hud, sizeof( m_hud ),
                                   "centre box  base %s x %s  ·  click: a corner  ·  type = half-size",
                                   a, b );
                    }
                    else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "box  base %s x %s  ·  click: opposite corner  ·  type = square size",
                               a, b );
                }
                else if ( m_kind == KPRIM_SPHERE )
                {
                    KiwiUnits_Format( a, sizeof( a ), m_radius );
                    _snprintf( m_hud, sizeof( m_hud ),
                               "sphere  r %s  %i sides (%i faces)  ·  click: commit  ·  type = radius  ·  [ ] sides",
                               a, m_sides, m_sides * m_sides );
                }
                else
                {
                    KiwiUnits_Format( a, sizeof( a ), m_radius );
                    _snprintf( m_hud, sizeof( m_hud ),
                               "%s  r %s  %i sides  ·  click: radius  ·  type = radius  ·  [ ] sides",
                               LowerName(), a, m_sides );
                }
            }
            else
            {
                KiwiUnits_Format( a, sizeof( a ), m_height );
                // The blocked-view remedy replaces the redundant TOO THIN warning.
                if ( m_zBlocked )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "%s  height %s  ·  ORBIT to set height — this view looks "
                               "straight along %s  ·  or type = height",
                               LowerName(), a, HeightAxisName() );
                // Expose absolute mapping mode in the HUD.
                else if ( m_absolute )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "%s  height %s  %s·  CTRL: the cap rides the cursor  ·  "
                               "click/Enter: commit",
                               LowerName(), a, m_invalid ? "TOO THIN  " : "" );
                else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "%s  height %s  %s·  click/Enter: commit  ·  type = height",
                               LowerName(), a, m_invalid ? "TOO THIN  " : "" );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        const char *LowerName() const
        {
            switch ( m_kind )
            {
            case KPRIM_BOX:        return "box";
            case KPRIM_BOX_CENTER: return "centre box";
            case KPRIM_CYLINDER: return "cylinder";
            case KPRIM_SPHERE:   return "sphere";
            default:             return "cone";
            }
        }

        void Create()
        {
            if ( IsBox() )
            {
                CreateBox();
                return;
            }
            if ( m_kind == KPRIM_CYLINDER )
            {
                CreateCylinder();
                return;
            }
            CreateConeOrSphere();
        }

        // Box stays unlinked through rebuild/validation, so rejection is free.
        void CreateBox()
        {
            float loop[8];
            BoxLoopUV( loop );
            float lo, hi;
            CapOffsets( &lo, &hi );

            const char *why = "unknown";
            brush_t *def = KiwiExtrude_BuildPrismDef( m_plane, loop, 4, lo, hi, &why );
            if ( !def )
            {
                Sys_Printf( "Box: rejected — %s.\n", why ? why : "invalid geometry" );
                return;
            }
            KiwiValid_Rebuild( def );
            if ( !KiwiValid_CheckBrush( def, &why ) )
            {
                Brush_Free_R( def );               // still unlinked, refCount 0
                Sys_Printf( "Box: rejected — %s.\n", why ? why : "invalid geometry" );
                return;
            }

            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "create box" );
            KiwiExtrude_LandDef( def );
            Sys_Printf( "Box created.\n" );
            g_nUpdateBits = -1;
        }

        // Brush_MakeSided accepts the def, so cylinder validates before landing.
        void CreateCylinder()
        {
            // Keep patch construction separate from the ported brush path.
            if ( m_patchMode )
            {
                CreatePatchCylinder();
                return;
            }
            float mins[3], maxs[3];
            if ( !SolidAabb( mins, maxs ) )
                return;
            const int axis = PlaneWorldAxis( m_plane );
            if ( axis < 0 )
            {
                Sys_Printf( "Cylinder: construction plane is no longer axis-aligned.\n" );
                return;
            }

            brush_t *def = AllocBoxDef( mins, maxs );
            if ( !def )
                return;
            KiwiValid_Rebuild( def );              // Brush_MakeSided reads def->mins/maxs

            // snap=0 preserves the modern-grid radius; the ported tail still
            // rebuilds with bFull=1 and may apply legacy snapping.
            Brush_MakeSided( (int)(intptr_t)def, (unsigned int)m_sides, axis, 0 );

            const char *why = "unknown";
            if ( !KiwiValid_CheckBrush( def, &why ) )
            {
                Brush_Free_R( def );
                Sys_Printf( "Cylinder: rejected — %s.\n", why ? why : "invalid geometry" );
                return;
            }

            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "create cylinder" );
            KiwiExtrude_LandDef( def );
            Sys_Printf( "Cylinder created (%i sides).\n", m_sides );
            g_nUpdateBits = -1;
        }

        // Build the control net directly: Patch_BrushToMesh requires and deletes a
        // selected source brush and would replace the drawn plane/radius with AABB data.
        // The stock 9x3 net uses four quadratic quarter-arcs; odd handle columns
        // overshoot to r/cos(alpha/2), matching kiwi_patchfillet.cpp's ArcPoint.
        // Three height rows keep walls straight. Patches have no caps or collision.
        void CreatePatchCylinder()
        {
            float mins[3], maxs[3];
            if ( !SolidAabb( mins, maxs ) )
                return;

            float lo, hi;
            CapOffsets( &lo, &hi );
            if ( !( hi - lo > 1.0e-3f ) || !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Cylinder (patch): zero radius or height.\n" );
                return;
            }

            // Match Patch_BrushToMesh's faces[0] material source via an unlinked
            // throwaway box, then free it.
            patchMesh_material tex = {}, lm = {};
            bool haveMtl = false;
            {
                brush_t *probe = AllocBoxDef( mins, maxs );
                if ( probe )
                {
                    if ( probe->faces && probe->faceCount > 0 )
                    {
                        tex = *(patchMesh_material *)&probe->faces[0].mtldef[0].lyrMtl;
                        lm  = *(patchMesh_material *)&probe->faces[0].mtldef[1].lyrMtl;
                        haveMtl = true;
                    }
                    Brush_Free_R( probe );     // never linked: refCount 0
                }
            }

            patchMesh_t *p = MakeNewPatch();
            if ( !p )
            {
                Sys_Printf( "Cylinder (patch): out of memory.\n" );
                return;
            }

            const int spans = KPRIM_PATCH_SPANS;
            p->width    = spans * 2 + 1;          // 9
            p->height   = KPRIM_PATCH_ROWS;       // 3
            p->type     = PATCH_CYLINDER;
            p->contents = 0;
            p->flags    = 0;                      // MakeNewPatch does NOT seed this

            float base[3];
            KiwiCon_PlaneToWorld( m_plane, m_p0, base );

            const float alpha = 6.283185307f / (float)spans;   // per-span angle
            for ( int col = 0; col < p->width; ++col )
            {
                const float psi = 0.5f * alpha * (float)col;
                const float rad = ( col & 1 ) ? ( m_radius / cosf( alpha * 0.5f ) )
                                              : m_radius;
                const float ca = cosf( psi ), sa = sinf( psi );
                for ( int row = 0; row < KPRIM_PATCH_ROWS; ++row )
                {
                    const float f = (float)row / (float)( KPRIM_PATCH_ROWS - 1 );
                    const float off = lo + ( hi - lo ) * f;
                    for ( int k = 0; k < 3; ++k )
                        p->ctrl[col][row].xyz[k] =
                            base[k] + m_plane.normal[k] * off
                                    + rad * ( ca * m_plane.u[k] + sa * m_plane.v[k] );
                }
            }

            if ( haveMtl )
            {
                p->texture  = tex;
                p->lightmap = lm;
            }
            // Unrealized patch materials create selectable but invisible geometry.
            KiwiMtl_RealizePatch( p );

            // Empty copied/default channels can hide lightmap view and compile unlit;
            // ensuring them is a no-op for sound inherited channels.
            KiwiMtl_EnsurePatchChannels( p );      // kiwi_material.h:250

            Patch_KiwiFinishNew( p );

            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "create patch cylinder" );
            brush_t    *pdef = AddBrushForPatch( p, (entity_s *)world_entity->def );
            selbrush_t *inst = Brush_AddToList( pdef, world_entity );
            Brush_AddToList2( inst );

            Sys_Printf( "Cylinder (PATCH, experimental): a q3 curve, %i spans.  It has "
                        "NO COLLISION — add a caulk brush inside it — and no end caps "
                        "(stock CoD4 practice).  Press P to go back to a brush.\n",
                        spans );
            g_nUpdateBits = -1;
        }

        // Cone/sphere cores read selected_brushes, so land before cutting and undo
        // the stamped creation if post-cut validation fails.
        void CreateConeOrSphere()
        {
            float mins[3], maxs[3];
            if ( !SolidAabb( mins, maxs ) )
                return;

            brush_t *def = AllocBoxDef( mins, maxs );
            if ( !def )
                return;
            // Rebuild before landing: ported cores need mins/maxs, and failure is
            // still cheap while the def is unlinked (brush.cpp:1459-1463).
            KiwiValid_Rebuild( def );

            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( m_kind == KPRIM_CONE ? "create cone" : "create sphere" );
            KiwiExtrude_LandDef( def );            // links + selects; QE_SingleBrush now passes

            if ( m_kind == KPRIM_CONE )
                Brush_MakeSidedCone( m_sides );
            else
                Brush_MakeSidedSphere( m_sides );

            const char *why = "unknown";
            if ( !KiwiValid_CheckBrush( def, &why ) )
            {
                // Undo removes the stamped creation landed above.
                KiwiCmd_UndoCancel();
                Sys_Printf( "%s: rejected — %s.\n", Name(), why ? why : "invalid geometry" );
                g_nUpdateBits = -1;
                return;
            }
            Sys_Printf( "%s created (%i sides).\n", Name(), m_sides );
            g_nUpdateBits = -1;
        }

        // World AABB consumed by the ported primitive cores.
        bool SolidAabb( float mins[3], float maxs[3] )
        {
            float c[3];
            KiwiCon_PlaneToWorld( m_plane, m_p0, c );

            if ( m_kind == KPRIM_SPHERE )
            {
                for ( int k = 0; k < 3; ++k )
                {
                    mins[k] = c[k] - m_radius;
                    maxs[k] = c[k] + m_radius;
                }
                return true;
            }

            const int axis = PlaneWorldAxis( m_plane );
            if ( axis < 0 )
            {
                Sys_Printf( "%s: construction plane is not axis-aligned.\n", Name() );
                return false;
            }
            // A face-derived normal may point down a world axis, so derive the
            // signed endpoint before ordering its bounds.
            const float endAxis = c[axis] + m_plane.normal[axis] * m_height;
            for ( int k = 0; k < 3; ++k )
            {
                if ( k == axis )
                {
                    mins[k] = ( c[k] < endAxis ) ? c[k] : endAxis;
                    maxs[k] = ( c[k] < endAxis ) ? endAxis : c[k];
                }
                else
                {
                    mins[k] = c[k] - m_radius;
                    maxs[k] = c[k] + m_radius;
                }
            }
            if ( maxs[axis] - mins[axis] < KPRIM_MIN_EXTENT )
            {
                Sys_Printf( "%s: zero height — nothing created.\n", Name() );
                return false;
            }
            return true;
        }

        // Allocate an unlinked six-face box with the current material.
        brush_t *AllocBoxDef( float mins[3], float maxs[3] )
        {
            Ed_EnsureCurrentMaterial_Kiwi();
            brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, 0 );
            if ( !def )
            {
                Sys_Printf( "%s: brush allocation failed.\n", Name() );
                return 0;
            }
            // Brush_Create Com_Error()s on backwards bounds (brush.cpp:501-503).
            for ( int k = 0; k < 3; ++k )
            {
                if ( mins[k] <= maxs[k] )
                    continue;
                Brush_Free_R( def );
                Sys_Printf( "%s: backwards bounds — nothing created.\n", Name() );
                return 0;
            }
            Brush_Create( mins, maxs, def, 0 );
            return def;
        }

        primKind_t    m_kind;
        kconPlane_t   m_plane;
        int           m_stage    = 0;
        float         m_p0[2]    = { 0.0f, 0.0f };
        float         m_p1[2]    = { 0.0f, 0.0f };
        float         m_cur[2]   = { 0.0f, 0.0f };
        bool          m_haveCur  = false;
        float         m_radius   = 0.0f;
        float         m_height   = 0.0f;
        int           m_sides    = KPRIM_CYL_SIDES_DEF;
        bool          m_patchMode = false;   // cylinder only
        bool          m_hasNum   = false;
        float         m_numWorld = 0.0f;
        bool          m_invalid  = false;
        float         m_ref[3]   = { 0.0f, 0.0f, 0.0f };
        float         m_heightStart = 0.0f;
        bool          m_haveHeightStart = false;
        bool          m_zBlocked = false;     // height view gate
        // Auto-height requests a top-cap face-push handoff after creation.
        bool          m_autoHeight = false;
        // Height snapping keeps the ranked target, Ctrl edge, and current mode.
        snap_result_t m_snap;
        bool          m_absPrev  = false;
        bool          m_absolute = false;
        char          m_hud[192] = { 0 };
    };

    KiwiPrimitiveCommand s_box     ( KPRIM_BOX );
    KiwiPrimitiveCommand s_cylinder( KPRIM_CYLINDER );
    KiwiPrimitiveCommand s_sphere  ( KPRIM_SPHERE );
    KiwiPrimitiveCommand s_cone    ( KPRIM_CONE );
    KiwiPrimitiveCommand s_boxCenter( KPRIM_BOX_CENTER );   // centre-origin box
}

// Patch preference is lazy because the profile path is unavailable at static init.
static int s_patchMode = -1;        // -1 = not loaded yet

bool KiwiPrim_PatchMode()
{
    if ( s_patchMode < 0 )
        s_patchMode = Radiant_ProfileGetInt( "KiwiUX", "RoundToolPatch", 0 ) ? 1 : 0;
    return s_patchMode != 0;
}

void KiwiPrim_SetPatchMode( bool on )
{
    s_patchMode = on ? 1 : 0;
    Radiant_ProfileSetInt( "KiwiUX", "RoundToolPatch", s_patchMode );
}

void KiwiPrim_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiPrimitiveBox",      0, 0, KIWI_CMD_PRIM_BOX );
    Radiant_RegisterCommand( "KiwiPrimitiveCylinder", 0, 0, KIWI_CMD_PRIM_CYLINDER );
    Radiant_RegisterCommand( "KiwiPrimitiveSphere",   0, 0, KIWI_CMD_PRIM_SPHERE );
    Radiant_RegisterCommand( "KiwiPrimitiveCone",     0, 0, KIWI_CMD_PRIM_CONE );
    Radiant_RegisterCommand( "KiwiPrimitiveBoxCenter", 0, 0, KIWI_CMD_PRIM_BOX_CENTER );
}

KiwiEditorCommand *KiwiPrim_CommandForId( int commandId )
{
    switch ( commandId )
    {
    case KIWI_CMD_PRIM_BOX:        return &s_box;
    case KIWI_CMD_PRIM_BOX_CENTER: return &s_boxCenter;   // centre-origin box
    case KIWI_CMD_PRIM_CYLINDER: return &s_cylinder;
    case KIWI_CMD_PRIM_SPHERE:   return &s_sphere;
    case KIWI_CMD_PRIM_CONE:     return &s_cone;
    default:                     return 0;
    }
}

// "Solids" block in the shell's Construct panel.
void KiwiPrim_MenuItems()
{
    // ImGui hover state must be tested immediately after the corresponding button.
    struct row_t { const char *label; int id; const char *tip; };
    // Keep one row for every registered primitive kind.
    static const row_t KSOLIDS[5] =
    {
        { "Box",      KIWI_CMD_PRIM_BOX,      0 },
        { "Centre Box", KIWI_CMD_PRIM_BOX_CENTER,
          "Places the box from its CENTRE and half-extents rather than from a\n"
          "corner (Shift+V)." },
        { "Cylinder", KIWI_CMD_PRIM_CYLINDER,
          "Needs an axis-aligned construction plane (XY/XZ/YZ) — the ported\n"
          "primitive fixes its own axes." },
        { "Sphere",   KIWI_CMD_PRIM_SPHERE,   0 },
        { "Cone",     KIWI_CMD_PRIM_CONE,
          "Needs the XY construction plane — the ported primitive fixes its\n"
          "own axes." },
    };
    ImGui::TextDisabled( "Solids (land real brushes)" );
    for ( int i = 0; i < 5; ++i )
    {
        if ( i )
            ImGui::SameLine();
        ImGui::PushID( 940 + i );
        if ( ImGui::Button( KSOLIDS[i].label ) )
            Radiant_ExecCommand( (unsigned int)KSOLIDS[i].id );
        if ( KSOLIDS[i].tip && ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", KSOLIDS[i].tip );
        ImGui::PopID();
    }
}
