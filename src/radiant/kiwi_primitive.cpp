#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_primitive.cpp — RADIANT_UX_DESIGN §16b implementation.  See
// kiwi_primitive.h for the gesture, the two landing orders and exactly what the
// ported primitives impose on this file.
//
// NEW code over the ported cores.  Every brush that reaches the map is written
// either by §23's audited prism writer (Box) or by the ported
// Brush_MakeSided{,Cone,Sphere} (Cylinder / Cone / Sphere).  No ring math and no
// winding rule is re-implemented here.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s

#include "kiwi_primitive.h"
#include "kiwi_camera.h"            // ROUND AI, ITEM 2 - KiwiCam_AxisPortrayable
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_extrude.h"
#include "kiwi_lines.h"
#include "kiwi_material.h"          // ROUND AG, ITEM 8 - KiwiMtl_RealizePatch
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"          // ROUND BK, ITEM 5 - Sel_* / KiwiSel_SetModeMask
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <imgui/imgui.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int       Sys_Printf( const char *fmt, ... );                    // win_qe3.cpp
extern int       g_nUpdateBits;                                         // 0x25D5A74 (mainfrm.cpp)
// KIWI-UX (CLEANUP, B-34): `planeptsSrc` is the definition's own parameter name
// (brush.cpp:463), copied verbatim as the house rule requires.  It is misleading:
// every caller in this layer passes g_qeglobals.random_texture_stuff, i.e. the
// MATERIAL-DEF source, not plane points.
extern brush_t  *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls ); // brush.cpp:465 (0x4751e0)
extern void      Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:510 (0x475300)
extern void      Brush_Free_R( brush_t *def );                          // brush.cpp:706 (0x475af0)
extern void      Select_Deselect( int bAlsoFreeFaces );                 // select.cpp:1444 (0x48E800)
extern bool      Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
// KIWI-UX (CLEANUP, B-28): FILE SCOPE, not block scope.  Round AI shipped a link
// error from a block-scope extern that MSVC mangled with its enclosing namespace;
// kiwi_uv.cpp carries the full account.  This is the declaration that used to sit
// inside KiwiPrim_RegisterCommands.
extern void      Radiant_ExecCommand( unsigned int cmdId );             // mainfrm.cpp:4054
// KIWI-UX (ROUND BK, ITEM 5): the height sign is "toward the camera", so the
// auto-height arm needs the view axis.  `camera_s` comes from mainfrm.h, included
// above; the accessor never returns NULL (contract at camwnd.cpp:154-160).
extern camera_s *Ed_Camera();                                           // camwnd.cpp:161

// The three ported primitives.  Signatures copied from their definitions, not
// from the call sites: Brush_MakeSided's first parameter really is an `int`
// carrying the brush_t* (brush.cpp:3401, faithful to 0x4731E0).
extern void      Brush_MakeSided( int a1, unsigned int sides, int axis, char snap ); // brush.cpp:3405 (0x4731E0)
extern void      Brush_MakeSidedCone( int sides );                      // brush.cpp:3653 (0x47BC10)
extern void      Brush_MakeSidedSphere( int sides );                    // brush.cpp:3728 (0x47BE90)

// xywnd.cpp:1563 // KIWI-UX forwarder (see kiwi_extrude.h).
extern void      Ed_EnsureCurrentMaterial_Kiwi();
// entity.cpp 0x25D5B30 - worldspawn (same extern brush.cpp:39 / camwnd.cpp:65 use).
extern entity_s *world_entity;

// ── ROUND AG, ITEM 8: the EXPERIMENTAL PATCH MODE's entry points ───────────
// Every one copied from its DEFINITION, and the same set kiwi_patchfillet.cpp
// declares in its own extern block (that file is the precedent this follows in
// full — see the long note at MakePatchCylinder).
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );        // brush.cpp:669  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                       // brush.cpp:927  0x4765A0
extern patchMesh_t *MakeNewPatch();                                          // pmesh.cpp:136  0x437AC0
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );  // pmesh.cpp:840  0x4386A0
extern void         Patch_KiwiFinishNew( patchMesh_t *p );                    // pmesh.cpp:1558 (ROUND Q)
// radiant_registry.cpp:31 / :44 — the preference pair the whole KIWI layer uses.
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
        // ── KIWI-UX (ROUND AF, ITEM 8): THE CENTRE BOX ──────────────────────
        // USER DIRECTIVE, verbatim: "Add an option to the Box command that allows
        // it to be a 'Center' box instead of the current 'Corner box' behavior."
        //
        // Plasticity ships the two as SEPARATE COMMANDS, not as a mode inside one:
        // `CornerBoxCommand` (BoxCommand.ts:60) and `CenterBoxCommand`
        // (BoxCommand.ts:155), bound `"shift-c": "command:corner-box"` and
        // `"shift-v": "command:center-box"` (default-keymap.ts:288-289).  KIWI
        // follows that exactly, and it costs almost nothing: a centre box differs
        // from a corner box in ONE function (BoxLoopUV) — the first click is the
        // CENTRE of the base rect and the drag is its HALF-EXTENTS — so this is a
        // fifth `primKind_t` over the same command class rather than a fifth
        // command class.  Every other `m_kind == KPRIM_BOX` test in this file was
        // widened to `IsBox()` in the same change, which is why the two share all
        // of the staging, the numeric field, the preview and the creation path.
        //
        // Shift+V was ALREADY RESERVED FOR IT.  Shakeout F put the centre RECT
        // there as a placeholder and said why in as many words: "KIWI HAS NO
        // CENTRE-BOX primitive… Binding Rectangle (CENTRE) here instead keeps the
        // corner/centre PAIRING the neighbouring keys teach… and means the chord
        // will not have to move when the centre box does ship" (kiwi_keymap.cpp).
        // It has shipped; the chord is claimed as promised and the centre rect
        // moves to the free Alt+V (see the audit in kiwi_keymap.cpp).
        KPRIM_BOX_CENTER
    };


    // KIWI-UX (CLEANUP, RayAxis): the local copy is gone — it was one of four
    // byte-identical bodies.  It is KiwiCam_RayAxis (kiwi_camera.h) now, beside
    // the KCAM_RAYAXIS_MIN_DEN gate every copy already cited.

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

    // ── shakeout E: the numeric FIELD tables (kiwi_command.h NumericFields) ──
    // STATIC storage — the numeric layer copies the structs but never the labels.
    // Field 0's label here is a PLACEHOLDER: the command relabels it per stage
    // through KiwiNum_SetFieldLabel, which is why the table can be shared by all
    // four kinds.  The pointers below must stay valid for the whole gesture, so
    // they are string literals too.
    const kiwiNumField_t KPRIM_FIELDS_BOX[1] =
    { { "size", KNUM_LENGTH, false } };
    const kiwiNumField_t KPRIM_FIELDS_RING[2] =
    { { "radius", KNUM_LENGTH, false },
      { "sides",  KNUM_COUNT,  false } };

    // ═════════════════════════════════════════════════════════════════════════
    //  §16b the modal command.  One class, four kinds — the four gestures differ
    //  only in "is stage 1 a corner or a radius" and in what the commit calls,
    //  and four near-identical classes is how those two facts drift apart.
    // ═════════════════════════════════════════════════════════════════════════
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

        // A primitive PLACES points, so a click must never auto-commit
        // (kiwi_command.h's WantsClicks contract).
        bool WantsClicks() const override { return true; }
        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        // ── shakeout E: PER-STAGE fields ────────────────────────────────────
        // Field 0 is the stage's own length ("size" / "radius" → "height"); it is
        // RELABELLED as the stage advances (KiwiNum_SetFieldLabel) rather than
        // re-registered, so whatever is typed survives the rename.  Field 1 is the
        // SIDE COUNT on everything but the box — previously only reachable through
        // `[` / `]`, and "12 sides" is a number a user wants to state, not nudge.
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
                // A COUNT is not a length: undo the numeric layer's inches→world
                // conversion to recover exactly what was typed (kiwi_numeric.h
                // "KIND IS A DISPLAY FACT"; kiwi_dupe.cpp does the same).
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
            m_zBlocked = false;                // ROUND AI, ITEM 2
            m_autoHeight = false;              // ROUND BK, ITEM 5
            // KIWI-UX (ROUND BT): these commands are file-static singletons, so the
            // Ctrl edge detector has to start each gesture cleared or a gesture that
            // ENDED with Ctrl down would hand the next one a phantom CTRL-UP rebase.
            m_snap     = snap_result_t();
            m_absPrev  = false;
            m_absolute = false;
            m_hud[0]   = '\0';
            m_sides    = ( m_kind == KPRIM_SPHERE ) ? KPRIM_SPH_SIDES_DEF : KPRIM_CYL_SIDES_DEF;
            // ROUND AG, ITEM 8: seeded from the remembered preference, so "I work
            // in patches" is a decision the mapper makes once rather than once per
            // cylinder — the same rule round AF's side count follows.
            m_patchMode = ( m_kind == KPRIM_CYLINDER ) && KiwiPrim_PatchMode();

            KiwiCon_AutoPlaneForTool();
            m_plane = KiwiCon_ActivePlane();

            // Refuse EARLY and LOUDLY when the ported core cannot express what the
            // plane asks for, rather than landing something that points elsewhere.
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

            // ── KIWI-UX (ROUND K): THE BOX-CREATION FIX, HALF ONE ───────────────
            // Announce the gesture as a PLANE PLACEMENT, which is what makes
            // kiwi_snap.cpp's arm 8 (SNAP_CPLANE — ray ∩ the working plane,
            // grid-snapped in plane) run at all, and what suppresses the area snap
            // that would silently drag a base corner off the plane.  Without it the
            // snap answered on the world Z=0 ground grid and the base rectangle
            // could not grow at all on a VERTICAL working plane — the full root
            // cause is written out on KiwiCon_SetPlanePlacement (kiwi_construct.h).
            // Cleared in Commit() AND Cancel(), and by the early return above (the
            // flag is not set until here, so a refused Begin leaves it alone).
            KiwiCon_SetPlanePlacement( true );

            LatchFromCursor();
            RelabelStageField();               // shakeout E: "size" / "radius"
            UpdateHud();
            Sys_Printf( "%s: click to place, Esc cancels.\n", Name() );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            // KIWI-UX (ROUND BT): kept for the HEIGHT stage's ladder as well as the
            // base stages' own arm below — the answer was previously discarded the
            // moment the base was placed, which is why the height could only ever
            // find the grid (kiwi_extrude.h KiwiExt_LadderDepth).
            m_snap = snap;
            if ( m_stage < 2 )
            {
                // ── KIWI-UX (ROUND K): THE BOX-CREATION FIX, HALF TWO ───────────
                // Stages 0 and 1 live ON the construction plane.  A GEOMETRY snap
                // (a corner, a midpoint, a crossing, a face) is a target the user
                // aimed at on purpose and is projected onto the plane exactly as
                // before; ANYTHING ELSE is resolved by casting the cursor ray at the
                // working plane HERE, rather than by projecting whatever the snap
                // layer's fallback arm happened to answer with.
                //
                // WHY BOTH, when half one already makes the snap answer on the
                // plane: this is the load-bearing half.  It makes the rubber band
                // track the cursor on ANY plane at ANY camera angle even if the snap
                // query refuses (behind the eye, no camera size yet, a future arm
                // that answers somewhere else) — i.e. it removes the dependency that
                // caused the bug instead of only fixing today's instance of it.  It
                // is also exactly what Plasticity's point picker does: geometry
                // snaps win, and the construction plane is the fallback that always
                // has an answer.
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
                        // ROUND R: §17 spacing, in plane, on the WORLD-ANCHORED
                        // lattice (kiwi_construct.h KiwiCon_SnapUV).
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
                // Nothing usable this frame: KEEP the last point rather than
                // collapsing the preview (an edge-on plane is a transient state the
                // user orbits out of, not an error).
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

        // ── ROUND K: the tool's own keys, for the bottom-left prompt strip ──
        // Same contract as the drawing tools' (kiwi_command.h HudPrompts): STATIC
        // storage, and only the keys this command invents for itself.
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
            // ── KIWI-UX (ROUND K): Z CYCLES THE WORKING PLANE, at stage 0 ───────
            // USER REPORT: "Creating a 3d brush vertically is impossible right now,
            // any camera angle creates it horizontally."  Half one and half two of
            // the fix (above) make a vertical plane WORK; this is what makes one
            // REACHABLE without leaving the gesture — while a modal command runs the
            // framework swallows every unconsumed key, so the §16 construction-plane
            // commands (Ctrl-free 34011..34013) cannot be pressed mid-Box.
            //
            // Z rather than a new letter because the drawing tools already spend Z on
            // "go vertical" (kiwi_construct.cpp's Z lock), so the key already means
            // "stop being flat" everywhere a placement gesture is live.  The cycle is
            // XY -> XZ -> YZ -> XY, i.e. exactly KiwiCon_SetPlaneAxis's 2 / 1 / 0.
            //
            // STAGE 0 ONLY, and refused with a message afterwards: the base corner is
            // already committed to a plane by then, and silently re-seating the plane
            // under it would move a point the user placed.
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

            // `[` / `]` adjust the side count (kiwi_primitive.h SIDE COUNTS).  The
            // box has no ring, so it leaves the keys alone — and while any modal
            // command runs the framework swallows unconsumed keys anyway, so the
            // modern grid-spacing bindings can never fire mid-gesture either way.
            // ROUND AG, ITEM 8: `P` toggles the EXPERIMENTAL PATCH MODE, mid
            // gesture, and the choice is REMEMBERED across sessions (the
            // "RoundToolPatch" preference) exactly as the round-AF side count is.
            // Only the cylinder offers it: the cone and the sphere have no stock
            // patch spelling this could be faithful to (Patch_BrushToMesh's cone
            // arm collapses a ring to a point and its hemisphere type has no
            // creation path at all), and the box is not round.
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
                RelabelStageField();           // shakeout E: field 0 is "height" now
                LatchHeightStart();
                Recompute();
                // KIWI-UX (ROUND BK, ITEM 5): the height stage is USELESS in this
                // view — finish the solid instead of parking in it.  See
                // AutoHeightIfBlocked; returning false COMMITS.
                if ( AutoHeightIfBlocked() )
                    return false;
                return true;
            }
            // ── ROUND AI, ITEM 2: DO NOT COMMIT A HEIGHT THE VIEW COULD NOT SET ──
            // Without this the click falls through to Commit(), which sees
            // m_invalid (height 0 < KPRIM_MIN_EXTENT) and prints "degenerate
            // placement — nothing created" — technically correct and completely
            // unhelpful, because it does not say that the CAMERA is the problem.
            // A typed height is exempt: it never went through the cursor mapping.
            if ( m_zBlocked && !m_hasNum )
            {
                Sys_Printf( "%s: this view looks straight along %s, so the cursor "
                            "cannot express a height — orbit away from the "
                            "top/bottom lock, or type one.\n", Name(), HeightAxisName() );
                return true;                      // stay in the gesture
            }
            return false;                         // the height click COMMITS
        }

        // ══════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AQ, ITEM 8) — ENTER FINISHES THE STAGE IT IS IN.
        // ══════════════════════════════════════════════════════════════════════
        // The framework calls this ONLY with a complete typed value pending and
        // ONLY from the Enter rung (kiwi_command.h AdvanceStage carries the rule).
        // It is the SAME stage-advance body Click() runs, minus the two things
        // Click() needs that Enter does not:
        //
        //   * `m_haveCur`.  A typed radius does not need a cursor at all — the
        //     number IS the answer, which is exactly what round AI's m_zBlocked
        //     exemption at :561 already recognises for the height stage.
        //   * the FINAL stage.  Enter there means confirm and must stay meaning
        //     confirm, so this returns false and the framework commits.  That is
        //     also why the sphere (FinalStage()==1) advances only out of stage 0.
        //
        // Recompute() has ALREADY folded the typed number into m_p1 / m_radius by
        // the time this runs (NumericFieldChanged -> Recompute fires on every
        // keystroke), so the advance is reading a base the number already sized —
        // which is why ClearNumeric() below cannot lose it.  The stage that
        // follows re-labels its field and re-latches its own origin, i.e. the
        // mapping is rebased, exactly as it is on a click.
        bool AdvanceStage() override
        {
            if ( m_stage >= FinalStage() )
                return false;                     // last stage: Enter still commits

            if ( m_stage == 0 )
            {
                // The base point is the cursor's if there is one, otherwise
                // wherever the tool already latched (Begin seeds both).
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

            // stage 1 -> 2 (box/cylinder/cone only; the sphere never gets here
            // because FinalStage()==1 was refused above).
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
            // KIWI-UX (ROUND BK, ITEM 5): the same completion Click() takes.
            // `false` here means "Enter commits", which is exactly what is wanted
            // once the height has been supplied.
            if ( AutoHeightIfBlocked() )
                return false;
            return true;
        }

        // ══════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND BK, ITEM 5) — 5 FT, AND THE LOLLIPOP ALREADY ARMED.
        // ══════════════════════════════════════════════════════════════════════
        // The directive and the units are on KPRIM_AUTO_HEIGHT (kiwi_primitive.h).
        // Called at the instant the HEIGHT STAGE is entered, and only then — the
        // gate it reads (`m_zBlocked`, round AI ITEM 2) is recomputed on every
        // Recompute, so if the user orbits mid-gesture the ordinary height drag is
        // live again and this never runs.
        //
        //   * NOTHING when the view CAN portray the axis — the normal two-stage
        //     gesture is untouched, and so is every non-height primitive (the
        //     sphere never reaches stage 2, FinalStage()==1).
        //   * NOTHING when a height has been TYPED: a number works at any angle
        //     (Recompute's own exemption) and it outranks a default.
        //   * OTHERWISE the height becomes KPRIM_AUTO_HEIGHT, signed so the solid
        //     grows TOWARD THE CAMERA — the top cap is then "the one that was
        //     facing the locked camera", which is the face the directive wants the
        //     lollipop on.  `m_invalid` is re-derived (Recompute already ran with
        //     height 0 and would otherwise leave Commit refusing a degenerate
        //     placement), and the caller commits.
        //
        // Returns true when it supplied a height, i.e. "commit now".
        bool AutoHeightIfBlocked()
        {
            if ( !m_zBlocked || m_hasNum || m_stage < 2 )
                return false;

            const camera_s *c = Ed_Camera();
            // The plane normal points one way; the camera looks along vpn.  The cap
            // facing the camera is the one on the -vpn side, so the height takes the
            // sign that puts it there.  A dead-on tie cannot happen: m_zBlocked IS
            // "the view axis and this normal are within KiwiCam_AxisPortrayable's
            // cone", so the dot product is large.
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
            KiwiCon_SetPlanePlacement( false );        // ROUND K — see Begin()
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
            // KIWI-UX (ROUND BK, ITEM 5): …and only for the auto-height path, hand
            // the new solid's top cap to the face push.  AFTER Create, because
            // that is what puts the brush on the map.
            if ( m_autoHeight )
                ArmTopFacePush();
        }

        // ── the arming half (ROUND BK, ITEM 5) ──────────────────────────────
        // The lollipop is not a thing that can be "turned on": it is drawn whenever
        // the ACTIVE COMMAND wants one (kiwi_lollipop.h KiwiLollipop_Wanted), and a
        // Move over a FACE selection is what wants one.  So arming it is exactly
        // the state a face CLICK produces — the shakeout-G auto-enter
        // (kiwi_boxselect.cpp ClickSelect): mode FACE, that face selected, Move
        // started and PAUSED so nothing follows the cursor until the ball is
        // grabbed.  This reproduces that state and nothing else; there is no second
        // code path and no second undo shape.
        void ArmTopFacePush()
        {
            // The landed brush.  Create() runs Select_Deselect(1) immediately
            // before Brush_AddToList2, so `selected_brushes` holds exactly what
            // this gesture just made (kiwi_extrude.cpp LandDef :521-527).
            selbrush_t *inst = ( selected_brushes.next != &selected_brushes )
                             ? selected_brushes.next : nullptr;
            if ( !inst || !inst->def || inst->patch || !inst->def->faces )
                return;                       // a patch cylinder has no face to push

            // The cap facing the camera: the face whose outward normal is closest to
            // the direction the height went.  `m_height`'s sign already encodes that
            // (AutoHeightIfBlocked), so the wanted direction is normal * sign.
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

            // …and the Move itself through the SHAKEOUT-G HANDOFF, which exists for
            // exactly this: KiwiCmd_Start cannot be called from inside Commit()
            // (the bracket close would land on the new command and KiwiNum_Reset
            // would throw away its field table), so the request is parked and
            // drained at the very end of KiwiCmd_Commit.  PAUSED, so the lollipop
            // is up and nothing follows the cursor until the ball is grabbed —
            // the same state a face click produces.
            KiwiCmd_StartDeferred( KIWI_CMD_MOVE, /*paused*/ true );
        }

        void Cancel() override
        {
            KiwiCon_SetPlanePlacement( false );        // ROUND K — see Begin()
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
        // ROUND AF, ITEM 8: the two box kinds are the SAME tool everywhere except
        // BoxLoopUV.  This is the predicate every widened test now asks.
        bool IsBox() const
        { return m_kind == KPRIM_BOX || m_kind == KPRIM_BOX_CENTER; }

        // The sphere has no height stage; everything else does.
        int FinalStage() const { return ( m_kind == KPRIM_SPHERE ) ? 1 : 2; }

        // A typed value belongs to ONE stage: the radius you typed must not
        // silently become the height when the stage advances.  The framework
        // resets the buffer per COMMAND (kiwi_command.cpp KiwiCmd_Start), which is
        // the right granularity for a one-scalar gesture and the wrong one for a
        // staged tool, so a staged tool clears it itself.
        // KIWI-UX (shakeout E): field 0 carries the CURRENT stage's name, so the
        // HUD tag and the bubble's secondary rows read "height" once the base is
        // down.  Relabelled, not re-registered — SetFields would clear the entry.
        void RelabelStageField()
        {
            const char *label = ( m_stage >= 2 ) ? "height"
                              : ( m_kind == KPRIM_BOX_CENTER ) ? "half-size"
                              : ( IsBox() ) ? "size" : "radius";
            KiwiNum_SetFieldLabel( 0, label );
        }

        void ClearNumeric()
        {
            // KIWI-UX (shakeout E): KiwiNum_ClearEntry, NOT KiwiNum_Reset — Reset
            // reinstalls the DEFAULT single field and would throw this command's
            // own "radius / height / sides" table away half way through the
            // gesture (kiwi_numeric.h).
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
            KiwiCon_SnapUV( m_plane, m_cur );      // ROUND R: world-anchored lattice
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

        // ── ROUND AI, ITEM 2: THIS IS A REBASE, NOT A RESET ─────────────────
        // `m_heightStart` is biased by the CURRENT `m_height`, so
        //     h = Dot3(rel, n) - m_heightStart
        // evaluates to exactly `m_height` at the instant of the latch.  At stage
        // entry `m_height` is 0 and this is bit-identical to what it always did;
        // mid-gesture (the view gate re-opening) it is what makes the height
        // "start moving from where it was" instead of snapping back to zero — the
        // round-L grab-rebase discipline, applied to a view change.
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
                // ── ROUND AF, ITEM 8 — THE ONLY LINE THAT DIFFERS ───────────
                // m_p0 is the CENTRE of the base rect and the drag is its HALF-
                // extents, so the rect is p0 +/- |c1 - p0| per axis.  Everything
                // downstream — the winding order, the prism build, the height
                // stage, the numeric field, the preview — is unchanged, which is
                // the whole reason this is a kind and not a command.
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
            out[0] = u0; out[1] = v0;              // CCW in plane space, the winding
            out[2] = u1; out[3] = v0;              // KiwiExtrude_BuildPrismDef expects
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
                // ── KIWI-UX (ROUND AI, ITEM 2): THE VIEW GATE ───────────────
                // USER REPORT, verbatim: "when creating a box (or other shape)
                // with the camera perfectly aligned to TOP, when it's time to do
                // the Height(Z), i move the camera and the height is already set
                // to a huge negative number.  This needs to be fixed and it should
                // be zero until I move my mouse in a way that's able to portray Z
                // movement.  It's impossible to portray Z movement while at
                // top/bottom camera lock."
                //
                // The whole derivation is on KiwiCam_AxisPortrayable
                // (kiwi_camera.h).  Short form: the height stage maps the cursor
                // through RayAxis, whose gain is 1/sin^2(angle between the plane
                // normal and the ray) — so at a top view it solves a degenerate
                // system and returns a point hundreds of units up or down the Z
                // line.  That number was then quantised, stored in m_height and
                // sat there; orbiting away did not CREATE the -140 yd, it merely
                // made the already-latched value visible.
                //
                // WHILE THE VIEW CANNOT PORTRAY THE AXIS THE HEIGHT IS HELD.  Not
                // reset — held, so a value the user set from a workable angle
                // survives an orbit through the top.  At stage entry it is 0,
                // which is what the report asks for.
                const bool canZ = KiwiCam_AxisPortrayable( m_plane.normal );
                if ( !canZ )
                {
                    // Drop the latch so the gate re-OPENING re-bases (below)
                    // instead of applying an accumulated phantom delta.
                    m_haveHeightStart = false;
                }
                else if ( !m_haveHeightStart )
                {
                    // REBASE at the current cursor, biased to preserve m_height.
                    LatchHeightStart();
                }
                m_zBlocked = !canZ;

                // ── KIWI-UX (ROUND BT): CTRL = ABSOLUTE, HERE TOO ───────────────
                // The FOURTH member of the extrude family (kiwi_extrude.h): this
                // stage pulls a cap along one axis exactly as the three others do,
                // and round BP's claim that it was "covered by the lattice half" was
                // never true — everything below used to be `floorf( h/g + 0.5 )*g`
                // with no modifier and no ladder at all.  Same two transitions as the
                // siblings: CTRL DOWN is the feature and lets the cap jump to the
                // cursor's own height, CTRL UP re-latches `m_heightStart` so relative
                // resumes from where absolute left it and NOTHING moves.
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
                    // `m_ref` is the base point ON the plane (LatchHeightStart), so
                    // this projection IS the cap's height above the base — the
                    // "absolute reading" round BP said a primitive did not have.
                    rawAbs  = Dot3( rel, m_plane.normal );
                    haveRaw = true;
                    h = absNow ? rawAbs : ( rawAbs - m_heightStart );
                }
                // A TYPED HEIGHT WORKS AT ANY ANGLE, gate or no gate — it never
                // went through the cursor mapping in the first place.
                if ( m_hasNum )
                {
                    h = m_numWorld;
                }
                else if ( m_absolute && haveRaw )
                {
                    // ── AND THE SNAP HALF IS THIS COMMAND'S OWN CONTRACT ────────
                    // A primitive is a CREATION gesture, so round BO makes snapping
                    // its DEFAULT and Ctrl the release (kiwi_command.h SnapContext) —
                    // the opposite of the three transform siblings.  Composed rather
                    // than overridden: with Ctrl held the cap rides the cursor
                    // EXACTLY, unquantised, which is the directive's *"match … exactly
                    // where my mouse is"* and is also what "snapping freed" has to
                    // mean.  The ladder runs in the ENGAGED state below.
                }
                else if ( haveRaw )
                {
                    // ── THE FULL LADDER, ENGAGED (kiwi_extrude.h) ───────────────
                    // Replaces the blind `floorf( h/g + 0.5 )*g`: a corner or an edge
                    // the user is pointing at now takes the height, a face plane is a
                    // magnet, and the lattice — absolute on a world axis and aware of
                    // the MAJOR lines, neither of which the old quantiser was — is the
                    // fallback.  Note the source-plane refusal comes free with it
                    // (KEXT_SELF_SNAP_BAND): the base outline's own corners sit at
                    // height 0 and can no longer glue the cap shut.
                    h = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, h, nullptr );
                }
                else
                {
                    // No cursor mapping this frame (the view gate, or a degenerate
                    // ray): the held height stands, quantised exactly as before.
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

        // ROUND AI, ITEM 2: the name of the axis the HEIGHT stage pulls along —
        // the plane NORMAL, not the plane.  "orbit, this view looks straight along
        // Z" is actionable; "…straight down XY (flat)" is not.
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

        // ROUND K: the WORKING PLANE's name, so stage 0 can say which one is in
        // force — the whole "it always comes out horizontal" report was a user who
        // had no way to see, or change, the plane the base was going onto.
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
            // ── ROUND AG, ITEM 8: PATCH MODE OWNS THE WHOLE STRIP ───────────
            // It is EXPERIMENTAL and it produces something with different rules
            // (no collision, no caps, a fixed control grid), so it says all three
            // at every stage rather than adding a chip to a brush-shaped sentence
            // and hoping.  The `sides` count is deliberately absent: it is not the
            // knob here — kiwi_primitive.h THE CONTROL GRID IS FIXED.
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
                        // The base is TWICE the drag: report what will be built, not
                        // what the cursor has travelled.
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
                // ROUND AI, ITEM 2: the gate names its own remedy.  It REPLACES the
                // "TOO THIN" chip rather than stacking with it — a height of 0 is
                // of course too thin, and saying so as well would bury the one
                // sentence that tells the user what to do.
                if ( m_zBlocked )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "%s  height %s  ·  ORBIT to set height — this view looks "
                               "straight along %s  ·  or type = height",
                               LowerName(), a, HeightAxisName() );
                // KIWI-UX (ROUND BT): the ABSOLUTE badge, for the same reason its
                // three siblings carry one — a mapping mode the user cannot see is a
                // mapping mode they will not trust (kiwi_extrude.h).
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

        // ── the commit ──────────────────────────────────────────────────────
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

        // BOX — §23's writer, unchanged: build, rebuild, gate, and only then land.
        // A rejection frees an UNLINKED def and touches nothing else.
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

            // KIWI-UX (CLEANUP, B-20): deselect before landing — the rule and its
            // reasons are stated once, at kiwi_patchfillet.cpp's LandPatches.
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "create box" );
            KiwiExtrude_LandDef( def );
            Sys_Printf( "Box created.\n" );
            g_nUpdateBits = -1;
        }

        // CYLINDER — the ported Brush_MakeSided takes the def POINTER, so the
        // build-gate-then-land order survives intact.
        void CreateCylinder()
        {
            // ROUND AG, ITEM 8: the experimental fork.  Everything below is the
            // unchanged brush path; the patch path is its own function so neither
            // can grow a branch the other has to reason about.
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

            // snap 0, not the classic dialog's 1: the placement is already on the
            // modern grid (§17) and flooring every ring point to an integer would
            // quantise the radius the user just drew.  (Brush_MakeSided's own tail
            // still rebuilds with bFull 1 — see kiwi_primitive.h, logged.)
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

        // ═══════════════════════════════════════════════════════════════════
        //  ROUND AG, ITEM 8 — THE EXPERIMENTAL PATCH MODE
        // ═══════════════════════════════════════════════════════════════════
        // USER REPORT + PROPOSAL, verbatim: a boolean'd cylinder arch comes out as
        // "a fan of sliver faces" and "texturing becomes hell"; "In normal cod4,
        // they use patches for curves.  Maybe you could add an experimental patch
        // hybrid option for the circle/cylinder (any round) tools?  What do you
        // think?"
        //
        // YES, AND IT IS THE AUTHENTIC ANSWER RATHER THAN A NEW IDEA.  Stock
        // Radiant's own Curve > Cylinder is exactly this — mainfrm.cpp:3831
        // `Cmd_OnCurvePatchtube` calls `Patch_BrushToMesh( 0, 0, 0, 0 )`
        // (pmesh.cpp:1281), which throws the box away and leaves a 9x3
        // PATCH_CYLINDER.  A CoD4 mapper's round geometry IS patches; the faceted
        // brush cylinder is what this editor had, not what the format wants.
        //
        // ── WHY IT IS NOT Patch_BrushToMesh ─────────────────────────────────
        // Three hard reasons, and kiwi_patchfillet.cpp reached the same conclusion
        // for its own quarter-cylinders (kiwi_patchfillet.cpp:1145-1155):
        //   1. it gates on QE_SingleBrush() — exactly one brush SELECTED — so it
        //      cannot be driven from a tool that has not landed anything;
        //   2. it derives the ring from `def->mins/maxs` only, i.e. from an
        //      axis-aligned box, so the radius and the working plane the user just
        //      drew would be discarded;
        //   3. it ends with Select_Delete(), destroying the source.
        // So this reproduces the SEQUENCE (MakeNewPatch -> fill ctrl -> materials
        // -> KiwiMtl_RealizePatch -> Patch_KiwiFinishNew -> AddBrushForPatch ->
        // Brush_AddToList -> Brush_AddToList2) and supplies its own control net.
        //
        // ── THE CONTROL NET, AND THE OVERSHOOT ──────────────────────────────
        // A quadratic bezier column pair per quarter arc: width = spans*2 + 1 with
        // EVEN columns on the circle at radius r and ODD (handle) columns pushed
        // out to r / cos(alpha/2), alpha = the per-span angle.  That overshoot is
        // what makes the quadratic interpolate a true circular arc, it is
        // kiwi_patchfillet.cpp's own ArcPoint rule (:206-220), and at the 4-span
        // 90-degree case it evaluates to r*sqrt(2) — which is exactly what
        // Patch_BrushToMesh's corner construction produces, so the two agree.
        //
        // SPANS ARE FIXED AT 4 (a 9-wide grid, one column per 90 degrees), NOT
        // m_sides.  The patch format's control grid is ctrl[16][16] and
        // Patch_GenericMesh refuses a width outside 3..15 (pmesh.cpp:1550), so 7
        // spans is the absolute ceiling and 4 is the stock cylinder.  A patch's
        // smoothness is a TESSELLATION property, not a control-point count — that
        // is the entire point of using one — so `sides` is simply not the knob
        // here.  The HUD says so.
        //
        // HEIGHT is 3 rows (bottom / middle / top), the stock cylinder's own
        // layout; the middle row is the midpoint, which keeps the walls straight.
        //
        // NO CAPS, PER STOCK CoD4 PRACTICE, and no collision either: a patch is a
        // render surface.  The hint says so on every creation.
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

            // The MATERIAL, taken the way Patch_BrushToMesh takes it (pmesh.cpp:
            // 1395-1396: the source brush's faces[0]).  A throwaway box def built
            // by the ordinary path is the only thing in this tool that knows what
            // the current material is, so it is built, read and freed.
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
            // Required whether or not the material was inherited — see the long
            // note at kiwi_patchfillet.cpp:1242-1257: an unrealized MaterialDef
            // gives a patch that is created, selectable and NEVER DRAWN.
            KiwiMtl_RealizePatch( p );

            // ...and a COPIED channel can be empty as easily as it can be
            // unrealized: the probe's own channels come from Brush_Create, but
            // `haveMtl` false leaves both slots at MakeNewPatch's defaults and a
            // damaged source leaves them at the source's damage.  A patch with a
            // dead lightmap channel is invisible in Shift+L and compiles unlit
            // (kiwi_material.h "the three channels").  No-op when the copy was
            // sound, which is the normal case.
            KiwiMtl_EnsurePatchChannels( p );      // kiwi_material.h:250

            Patch_KiwiFinishNew( p );

            // KIWI-UX (CLEANUP, B-20): deselect before landing — the rule and its
            // reasons are stated once, at kiwi_patchfillet.cpp's LandPatches.
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

        // CONE / SPHERE — both ported cores gate on QE_SingleBrush() and read
        // selected_brushes.next->def, so the brush has to be LIVE AND SELECTED
        // before they can cut it.  Land first, cut, then gate; a failed gate is
        // rolled back through the undo bracket (kiwi_primitive.h).
        void CreateConeOrSphere()
        {
            float mins[3], maxs[3];
            if ( !SolidAabb( mins, maxs ) )
                return;

            brush_t *def = AllocBoxDef( mins, maxs );
            if ( !def )
                return;
            // Rebuilt BEFORE landing, matching kiwi_extrude.cpp's order: the ported
            // cores read def->mins/maxs, which only Brush_BuildWindings sets
            // (brush.cpp:1459-1463), and doing it while the def is still unlinked
            // keeps the one step that CAN fail cheaply outside the bracket.
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
                // Undo_EndBrushList stamped this brush; Undo_Undo removes every
                // brush carrying the stamp, which is exactly "undo a creation"
                // (kiwi_extrude.h UNDO, read out of undo.cpp).
                KiwiCmd_UndoCancel();
                Sys_Printf( "%s: rejected — %s.\n", Name(), why ? why : "invalid geometry" );
                g_nUpdateBits = -1;
                return;
            }
            Sys_Printf( "%s created (%i sides).\n", Name(), m_sides );
            g_nUpdateBits = -1;
        }

        // The world AABB the ported primitives cut.  False (with a message) when
        // the placement cannot make one.
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
            // The height runs along plane.normal, whose world axis may point the
            // NEGATIVE way (an XY plane flipped by "from face"), so the endpoint is
            // computed rather than assumed.
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

        // The ported creator's head, exactly as kiwi_extrude.cpp uses it: current
        // material, six-face box def over [mins, maxs], still UNLINKED.
        brush_t *AllocBoxDef( float mins[3], float maxs[3] )
        {
            Ed_EnsureCurrentMaterial_Kiwi();
            brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, 0 );
            if ( !def )
            {
                Sys_Printf( "%s: brush allocation failed.\n", Name() );
                return 0;
            }
            // Brush_Create Com_Error()s on a backwards box, so the guard is here
            // rather than in it (brush.cpp:501-503).
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
        bool          m_patchMode = false;   // ROUND AG, ITEM 8 (cylinder only)
        bool          m_hasNum   = false;
        float         m_numWorld = 0.0f;
        bool          m_invalid  = false;
        float         m_ref[3]   = { 0.0f, 0.0f, 0.0f };
        float         m_heightStart = 0.0f;
        bool          m_haveHeightStart = false;
        bool          m_zBlocked = false;     // ROUND AI, ITEM 2 — the view gate
        // KIWI-UX (ROUND BK, ITEM 5): this gesture's height was supplied by
        // AutoHeightIfBlocked rather than by the cursor, so Commit hands the new
        // solid's top cap to the face push.  Reset in Begin(); never persisted.
        bool          m_autoHeight = false;
        // ── KIWI-UX (ROUND BT): the height stage joins the extrude family ───
        // (kiwi_extrude.h).  `m_snap` is this frame's ranked answer — the stage-2
        // ladder needs it and MouseMove used to throw it away past stage 1;
        // `m_absPrev` is the Ctrl edge detector (the rebase is on CTRL UP only) and
        // `m_absolute` is this frame's mode, read by the HUD.
        snap_result_t m_snap;
        bool          m_absPrev  = false;
        bool          m_absolute = false;
        char          m_hud[192] = { 0 };
    };

    KiwiPrimitiveCommand s_box     ( KPRIM_BOX );
    KiwiPrimitiveCommand s_cylinder( KPRIM_CYLINDER );
    KiwiPrimitiveCommand s_sphere  ( KPRIM_SPHERE );
    KiwiPrimitiveCommand s_cone    ( KPRIM_CONE );
    KiwiPrimitiveCommand s_boxCenter( KPRIM_BOX_CENTER );   // ROUND AF, ITEM 8
}

// ─── registration + lookup ───────────────────────────────────────────────────
// ─── ROUND AG, ITEM 8: the remembered patch-mode preference ──────────────────
// Same shape as KiwiCon_ToolSides (kiwi_construct.cpp:4230): lazily loaded on
// first read, because the profile path is not ready at static-init time.
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
    // Unbound in both profiles, like every other §16b creator: Shift+A (the add
    // menu) is the one key this round claims, and it lists all four.
    Radiant_RegisterCommand( "KiwiPrimitiveBox",      0, 0, KIWI_CMD_PRIM_BOX );
    Radiant_RegisterCommand( "KiwiPrimitiveCylinder", 0, 0, KIWI_CMD_PRIM_CYLINDER );
    Radiant_RegisterCommand( "KiwiPrimitiveSphere",   0, 0, KIWI_CMD_PRIM_SPHERE );
    Radiant_RegisterCommand( "KiwiPrimitiveCone",     0, 0, KIWI_CMD_PRIM_CONE );
    // ROUND AF, ITEM 8 — the centre box.  Registered UNBOUND here (the classic
    // profile row); kiwi_keymap.cpp claims Shift+V for it, which shakeout F
    // reserved for exactly this command.
    Radiant_RegisterCommand( "KiwiPrimitiveBoxCenter", 0, 0, KIWI_CMD_PRIM_BOX_CENTER );
}

KiwiEditorCommand *KiwiPrim_CommandForId( int commandId )
{
    switch ( commandId )
    {
    case KIWI_CMD_PRIM_BOX:        return &s_box;
    case KIWI_CMD_PRIM_BOX_CENTER: return &s_boxCenter;   // ROUND AF, ITEM 8
    case KIWI_CMD_PRIM_CYLINDER: return &s_cylinder;
    case KIWI_CMD_PRIM_SPHERE:   return &s_sphere;
    case KIWI_CMD_PRIM_CONE:     return &s_cone;
    default:                     return 0;
    }
}

// ─── the "Solids" block in the shell's panel window ──────────────────────────
void KiwiPrim_MenuItems()
{
    // KIWI-UX (CLEANUP, B-21): the tip belongs to the BUTTON it describes.  It used to
    // sit after the loop, where ImGui::IsItemHovered() refers to the LAST item
    // submitted — so a tip about Cylinder AND Cone only ever appeared over Cone.  It is
    // per-row now, inside the loop, and each row carries its own text (0 = no tip).
    struct row_t { const char *label; int id; const char *tip; };
    // KIWI-UX (CLEANUP, B-36): FIVE kinds, five buttons.  The centre box was
    // registered, dispatched and instantiated but reachable only by Shift+V or the
    // palette, so this block offered four of the five.  It is a table row and
    // nothing else — the dispatch below already handles every id in the table.
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
