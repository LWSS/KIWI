#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_transform.cpp — RADIANT_UX_DESIGN §13 / §20 / §21 / §22 implementation.
// See kiwi_transform.h for the three rules (apply-from-baseline, one undo record
// per gesture, reuse the ported cores), the cursor mapping and the numeric rules.
//
// NEW code over the ported cores.  Everything that touches geometry goes through
// a ported mutator; this file owns input mapping, constraint state, the baseline
// and the validity gate.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (texture / lightmap lock)

#include "kiwi_transform.h"
#include "kiwi_boxselect.h"          // ROUND K — the ONE click grammar (IdlePressReselect)
#include "kiwi_camera.h"             // KiwiCam_WorldPerPixel (the pivot marker's scale)
#include "kiwi_command.h"
#include "kiwi_conselect.h"          // shakeout F — the construction move arm
#include "kiwi_extrude.h"            // ROUND X — KEXT_SELF_SNAP_BAND (the ONE self-snap rule)
// KIWI-UX (ROUND BL, ITEM 4): the object move's quantiser is KiwiSnap_LatticeAxis
// now (one lattice rule, majors included), so this file makes no KiwiGrid_ call at
// all.  The include stays because the header is what documents the lattice this
// file's comments argue about, and dropping it would be a build change for a
// documentation reason.
#include "kiwi_grid.h"
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_patchfillet.h"        // ROUND AO, ITEM 2 — KiwiFillet_CarryOnPlaneMove
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int   Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp
extern camera_s *Ed_Camera();                                          // camwnd.cpp
extern void  CamWnd_BuildMatrix();                                     // camwnd.cpp 0x403470
extern int   g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)

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

// brush.cpp // KIWI-UX forwarders for the two file-static texture-lock halves.
extern void  Ed_FaceTexLockSave( float *saveBuf, face_t *face );
extern void  Ed_FaceTexLockReproject( face_t *face, const float *saveBuf, const byte *lockFlags );

// undo.cpp — the bracket head KiwiCmd_UndoBegin does not cover for face selections.
extern void  Undo_AddBrush( entity_brush_s *pBrushInst );              // undo.cpp:494  (0x45E680)
extern void  Undo_AddEntity( int a1 );                                 // undo.cpp:601  (0x45E8B0)
// SHAKEOUT G — the push-through delete drives the CLASSIC delete core.  The
// entity variant is what Cmd_OnSelectionDelete (mainfrm.cpp) feeds for the same
// reason: Select_Delete frees an owner entity left with no brushes.
extern void  Undo_AddEntity_W( entity_s *a1 );                         // undo.cpp:633
extern void  Select_Deselect( int bAlsoFreeFaces );                    // select.cpp:1445 (0x48E800)
extern void  Select_Brush( selbrush_t *brush, char some_overwrite,
                           char bStatus, char center_grid_on_selection ); // select.cpp:884
extern void  Select_Delete();                                          // select.cpp:1521 (0x48E760)

extern bool  ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );   // imgui_shell.cpp

namespace
{
    // ── tuning (spec §13's numbers, in one place) ───────────────────────────
    // KIWI-UX (CLEANUP, A-35): RULE — R has NO degrees-per-pixel constant.  A
    // rotation comes only from a grabbed ring (KiwiRotateCommand::Recompute); a
    // free mapping would yaw on every mouse move after R was pressed.
    const float KX_SCALE_PER_PIXEL = 0.005f;   // S
    const float KX_SCALE_MIN       = 0.01f;    // S clamp
    const float KX_ANGLE_STEP      = 5.0f;     // R snap increment, degrees
    // KIWI-UX (CLEANUP, A-26): defined FROM the exported KXPUSH_EPS
    // (kiwi_transform.h), which kiwi_extrude.cpp's HUD reads for the same
    // push-versus-delete comparison.  Same value it has always been.
    const float KX_EPS             = KXPUSH_EPS;

    enum { KX_MAX_EDGE_FACES = 8 };            // faces one brush edge may touch

    // Axis accent colours (§18: X red, Y green, Z blue — the same language as the
    // §17 world axes, so a constraint reads instantly).
    const float KX_AXIS_COL[3][3] =
    {
        { 1.00f, 0.35f, 0.35f },
        { 0.40f, 1.00f, 0.45f },
        { 0.45f, 0.60f, 1.00f },
    };
    const float KX_RUBBER_COL[3] = { 1.00f, 0.80f, 0.25f };
    const float KX_BAD_COL[3]    = { 1.00f, 0.30f, 0.25f };

    enum constraint_t { CON_FREE = 0, CON_AXIS, CON_PLANE };

    // ── shakeout E: the numeric FIELD tables (kiwi_command.h NumericFields) ──
    // STATIC storage, because the numeric layer copies the structs but never the
    // label strings.  One field each in v1:
    //   G  LENGTH only.  An "angle" field for a move would have to mean "the
    //      bearing of the move direction in some plane", and G's unconstrained
    //      direction rule is already the ground-plane projection of the mouse
    //      (kiwi_transform.h NUMERIC ENTRY) — a second, differently-defined
    //      bearing on top of that is how a grammar stops being predictable.  Type
    //      an axis key, then a length: that is the exact, one-keystroke answer.
    //   R  ANGLE — the one command where the user's "cycle to the angle" ask has
    //      an unambiguous meaning, and it is already what its scalar IS.
    //   S  FACTOR.
    const kiwiNumField_t KXF_MOVE  [1] = { { "length", KNUM_LENGTH, false } };
    const kiwiNumField_t KXF_ROTATE[1] = { { "angle",  KNUM_ANGLE,  false } };
    const kiwiNumField_t KXF_SCALE [1] = { { "factor", KNUM_FACTOR, false } };

    // ── local predicate over the shared vec helpers (kiwi_vec.h) ────────────
    inline bool PointNear( const float *a, const float *b, float tol )
    {
        float d[3];
        Sub3( a, b, d );
        return fabsf( d[0] ) <= tol && fabsf( d[1] ) <= tol && fabsf( d[2] ) <= tol;
    }

    // ── SHAKEOUT G's push-through threshold, at NAMESPACE scope (ROUND Q) ────
    // Was a private static of KiwiXformCommand.  It is lifted out unchanged so the
    // exported KiwiXform_PushFaceOnce below and the E-extrude's negative arm read
    // the SAME number the interactive push/pull reads — one delete rule in the
    // editor, not two that drift (the duplicate-function hazard, RADIANT_KNOWN_
    // ISSUES).  Walks every winding point of the WHOLE brush, not just this face's,
    // which is the point: the far side is on some other face.
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

    // ═════════════════════════════════════════════════════════════════════════
    //  SHAKEOUT G — THE SESSION PIVOT (kiwi_transform.h THE MOVABLE PIVOT).
    //
    //  ONE point, in memory, shared by Move and Rotate, dropped when the selection
    //  SIGNATURE changes.  Deliberately file-static rather than a member of either
    //  command: the whole point of the directive is that it OUTLIVES the gesture
    //  that placed it ("save the pivot spot in memory for reuse soon after"), and a
    //  member would die with the command object's Reset.
    //
    //  IT IS NEVER WRITTEN TO DISK.  No Radiant_ProfileSetInt, no ini key, no
    //  sidecar — a pivot is a scratch value about the thing you are editing right
    //  now, and restoring one from a previous session would silently aim the next
    //  rotate at a point the user has no memory of choosing.
    // ═════════════════════════════════════════════════════════════════════════
    bool     s_pivotHave   = false;
    float    s_pivotPos[3] = { 0.0f, 0.0f, 0.0f };
    unsigned s_pivotSig    = 0;

    // The SELECTION SIGNATURE the reset rule keys on.  Order-independent (a mix of
    // per-item hashes, combined with +) so a re-sync that rebuilds the same set in a
    // different order does NOT drop the pivot, which is exactly the case
    // Sel_Generation() cannot distinguish.  Construction items are counted too, so
    // a pivot placed for a construction move dies with that selection as well.
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

    // The pivot, if one is in force for the CURRENT selection.  Self-expiring: the
    // signature is re-tested on every read, so nothing has to hook the selection
    // funnels to invalidate it.
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

    // ── cursor mapping primitives (kiwi_transform.h "CURSOR MAPPING") ───────
    // ray ∩ plane(point, normal).  False when the ray is (near) parallel.
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

    // KIWI-UX (CLEANUP, RayAxis): the local copy is gone - it was one of four
    // byte-identical bodies, and THIS file's was the one the other three
    // mirrored.  It moved verbatim to KiwiCam_RayAxis (kiwi_camera.h), beside
    // the KCAM_RAYAXIS_MIN_DEN gate every copy already cited.

    // ── geometry readers ────────────────────────────────────────────────────
    winding_t *WindingOf( const selbrush_t *b, int faceIndex )
    {
        if ( !b || !b->def || !b->def->faces )
            return 0;
        if ( faceIndex < 0 || faceIndex >= b->def->faceCount )
            return 0;
        return b->def->faces[faceIndex].w;
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

    // KIWI-UX (CLEANUP, A-13 / A-14 / C-49): EdgeEnds and VertexPos were copies
    // three and four of two resolutions that now live in kiwi_selection.h as
    // Sel_EdgeEnds / Sel_ItemWorldPos (the latter carries A-14's named
    // KIWI_PATCH_MAX_DIM instead of the bare 16 this copy had).
    //
    // THE checkLive ARGUMENT, per site: the four SELECTION-LOOP callers pass
    // false because each has ALREADY run Sel_BrushLive on the same item one line
    // above, and asking twice would put a second display-list walk inside a
    // per-item loop — the exact cost DominantKind's own checkLive parameter
    // exists to avoid (see :330 below).  The two ACTIVE-ITEM callers that never
    // tested liveness at all (the G reference point and the S/R vertex anchor)
    // take the default true: one walk per gesture start, and it closes a real
    // deref-a-freed-node hole.

    // ── the selection's dominant kind (spec §13, mixed selections) ──────────
    // objects > faces > edges > verts.  Returns false for an empty/unusable one.
    //
    // `checkLive` is false on the canExecute path: the palette re-asks every open
    // frame, and Sel_BrushLive is a display-list walk, so an O(selection * brushes)
    // sweep per frame would be a real cost on a big map for a question whose only
    // consequence is greying a row.  KiwiSel() is rebuilt from the legacy lists
    // whenever they change, so a non-live item there is already a rarity; Begin()
    // pays for the real check, which is the one that matters.
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
        // Whole-selection ops drive `selected_brushes` directly, so THAT is the
        // condition to test — not the typed selection, which can name faces whose
        // brushes are deliberately absent from the legacy list.
        return selected_brushes.next != &selected_brushes;
    }

    // KIWI-UX (CLEANUP, UndoCoverBrush): the local copy is gone — this was one
    // of five verbatim bodies.  It is KiwiCmd_UndoCoverBrush (kiwi_command.h)
    // now, beside the bracket whose blind spot it exists to fill.

    // ═════════════════════════════════════════════════════════════════════════
    //  The shared modal base: constraint state, numeric state, HUD, undo latch.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiXformBase : public KiwiEditorCommand
    {
    public:
        // A transform must never snap the geometry it is dragging to itself, and
        // must never pick it either (kiwi_pick.h PICKF_EXCLUDE_SELECTED).
        //
        // …EXCEPT WHILE PLACING THE PIVOT (shakeout G).  The user's own words are
        // "it lets you move the pivot with the mouse (supports snapping)", and the
        // points they will reach for FIRST are corners of the very solid the
        // gesture is about — a pivot that could not land on the thing being rotated
        // would be useless.  So the exclusion is lifted for the duration of the
        // placement and restored the moment it ends.
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

        // ── SHAKEOUT G: the LMB press that PLACES the pivot ──────────────────
        // The framework offers a press here BEFORE its own pause / resume arms see
        // it (kiwi_command.h PressIntercept), which is the only way a click can
        // mean something other than "park the gesture" without turning these into
        // WantsClicks multi-click tools — which they are not, and which would cost
        // them the whole HOT/PAUSED grammar shakeout E gave them.
        bool PressIntercept( int imgX, int imgY ) override
        {
            (void)imgX; (void)imgY;
            if ( !m_pivotPlacing )
                return false;
            CommitPivot();
            return true;
        }

    protected:
        // ── SHAKEOUT G: the movable pivot, the half both commands share ──────
        // Only the ANCHOR each command feeds it to differs (Move: the constraint
        // reference m_ref; Rotate: the rotation centre m_pivot), so the placement
        // gesture, the key, the snap tracking and the marker all live here.

        // V toggles placement; Esc leaves placement before it can reach the
        // framework's cancel rung.  True = consumed.
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
                RefreshHud();                      // …and drop the PIVOT line
                g_nUpdateBits |= 1;
                return true;
            }
            return false;
        }

        void BeginPivot()
        {
            m_pivotPlacing = true;
            // ── KIWI-UX (ROUND Z, ITEM 4): SEED FROM THE **LIVE** ANCHOR ─────
            // Was `PivotActive( m_pivotWip )` first, falling back to PivotAnchor().
            // Both were wrong once the gesture had travelled: the SESSION pivot is
            // the position stored at the last commit and PivotAnchor() used to be
            // the BASE reference, so a V pressed half way through a move put the
            // placement marker back where the move STARTED.  PivotAnchor() is now
            // the live one (KiwiMoveCommand), and asking the command is also the
            // only reading that is right when no session pivot exists at all —
            // which is the common case, since V is how one gets placed.
            Copy3( PivotAnchor(), m_pivotWip );
            m_pivotWipHave = true;
            // A PAUSED gesture never receives MouseMove (kiwi_command.cpp), so the
            // pivot could not follow the cursor while parked.  Resuming is free
            // here: since shakeout G neither Move nor Rotate changes any geometry
            // from a bare mouse move, so being HOT with nothing grabbed is a
            // no-op state.
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
            RefreshHud();                          // ApplyPivot already did, belt-and-braces
            g_nUpdateBits = -1;
        }

        // The cursor's world point while placing.  THE WHOLE SNAP RESULT IS USED,
        // including type == SNAP_NONE: kiwi_snap.h is explicit that "no snap" is an
        // ANSWER and not a failure — with `valid` true and the type NONE, `position`
        // is the raw surface hit, or the ray ∩ Z=0 ground point, or a point down the
        // ray.  That is exactly "anywhere in 3D, off the solid", and it means the
        // pivot uses the same one point every other command in this layer does
        // rather than a second, differently-defined free placement.
        //
        // The view-facing-plane fallback below only runs when the snap layer could
        // not answer at all (a ray it refused to build).
        //
        // True = this MouseMove belonged to the pivot and the command must not run
        // its own Recompute.
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

        // A small, deliberately DIFFERENT marker from the snap cross: three short
        // axis ticks through the point plus a screen-facing diamond around it, so
        // "the gizmo is not where it usually is" reads at a glance.  `wip` true =
        // the placement is live (brighter, and it is the only thing on screen the
        // cursor is driving).
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

        // Re-run the command's own HUD line.  Both commands keep UpdateHud private
        // (it reads state this base cannot see), so the base reaches it through one
        // forwarder rather than by hoisting the whole HUD up here.
        virtual void RefreshHud() {}

        // Where the pivot marker would sit with no override — the command's own
        // natural anchor.  Overridden by Move and Rotate.
        virtual const float *PivotAnchor() const { return s_pivotPos; }
        // Adopt a freshly placed pivot into whatever this command calls its anchor.
        virtual void ApplyPivot( const float p[3] ) { (void)p; }
        // S DOES NOT TAKE V (kiwi_transform.h says why in full: it has no handle
        // set, so it is still a free drag and a pivot for it would be an anchor for
        // a mapping the user cannot see).  Plasticity does bind
        // `keyboard:scale:pivot` (default-keymap.ts:159); KIWI's scale gets it when
        // scale gets handles.
        virtual bool SupportsPivot() const { return false; }

    public:
        // The one thing the outside world asks about a live placement (the flag
        // itself stays protected — kiwi_gizmo.cpp must not be able to set it).
        bool PivotPlacingNow() const { return m_pivotPlacing; }

    protected:
        bool  m_pivotPlacing = false;
        bool  m_pivotWipHave = false;
        float m_pivotWip[3]  = { 0.0f, 0.0f, 0.0f };

        // Cursor ray for the frame, from wherever the framework last saw the
        // cursor over the camera image.  False when it is not over it.
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

        // Degrees / factors are NOT lengths: undo the numeric layer's
        // inches→world conversion to recover exactly what the user typed.
        float NumRaw() const { return Units_ToDisplay( m_numWorld ); }

        // Snapping is live unless CTRL suppressed it (kiwi_snap.h arm 0).
        // KIWI-UX (CLEANUP, SnapActive): one spelling, KiwiSnap_Active in
        // kiwi_snap.h — this member is the accessor over THIS command's result.
        bool SnapActive() const { return KiwiSnap_Active( m_snap ); }

        void OpenUndo( const char *literalOp )
        {
            if ( m_undoOpen )
                return;
            KiwiCmd_UndoBegin( literalOp );     // string LITERAL — stored by pointer
            m_undoOpen = true;
        }

        // Axis-lock key handling shared by all three commands.  X/Y/Z lock, a
        // second press of the SAME axis releases (v1), Shift+X/Y/Z plane-lock.
        // `allowPlane` is false for R and S, where a plane lock has no meaning.
        bool HandleAxisKey( int vk, unsigned mods, bool allowPlane )
        {
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

        // Re-latch so locking never JUMPS: the delta accumulated so far becomes
        // the new constraint's base, and the mapping restarts from the cursor's
        // current position under the new constraint.
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

        // KIWI-UX (CLEANUP, A-42): the buffer is the CALLER'S.  This used to return
        // a pointer to a function-local static, which is correct only while no one
        // format string uses it twice — an invisible failure the moment one does.
        const char *ConstraintText( char *buf, size_t n ) const
        {
            if ( m_con == CON_AXIS )  { _snprintf( buf, n, "axis %s",  AxisName( m_axis ) ); buf[n - 1] = '\0'; return buf; }
            if ( m_con == CON_PLANE ) { _snprintf( buf, n, "plane %s", AxisName( m_axis ) ); buf[n - 1] = '\0'; return buf; }
            return "free";
        }

        // The constraint accent line through `origin`, plus the start→now rubber
        // band.  Both live inside the framework's 64-segment batch.
        void DrawConstraint( const float *origin, const float *from, const float *to )
        {
            camera_s *c = Ed_Camera();
            if ( c->width < 1 || c->height < 1 )
                return;

            if ( m_con == CON_AXIS || m_con == CON_PLANE )
            {
                // Scaled to the viewing distance so it reads at any zoom without
                // becoming an infinite line across the map.
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
                    // A plane lock draws the TWO free axes, so "the plane
                    // perpendicular to X" reads as "Y and Z are free".
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

            // The rubber band, skipped when it would be a zero-length segment
            // (R and S have no translation to show, so they pass from == to).
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

    // ═════════════════════════════════════════════════════════════════════════
    //  G — the context-aware move (§13 table, §20 face, §21 edge, §22 vertex).
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiMoveCommand : public KiwiXformBase
    {
    public:
        const char *Name() const override { return "Move"; }
        bool CanExecute() override { return KiwiXform_CanMove(); }

        // ── shakeout E: field table, live value, bubble anchor, resume ───────
        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KXF_MOVE; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            // A FACE push keeps its SIGN — "-64 in" is the difference between
            // pulling a wall in and pushing it out, and the bubble is the only
            // place that reads at a glance.  Everything else reports the
            // magnitude of the 3D delta, which is what "length" means for a move
            // that may be running on any of three axes at once.
            *out = ( m_kind == SEL_FACE ) ? m_scalar : Len3( m_total );
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 )
                return false;
            // WHERE THE GEOMETRY IS NOW, not where the gesture started: m_ref is
            // the LATCHED reference point and m_total / m_scalar is how far it has
            // travelled since.  (The gizmo anchors on m_ref alone on purpose —
            // kiwi_transform.h — but a value bubble has to sit on the thing whose
            // value it is showing.)
            if ( m_kind == SEL_FACE )
                for ( int k = 0; k < 3; ++k ) out3[k] = m_ref[k] + m_pushDir[k] * m_scalar;
            else
                for ( int k = 0; k < 3; ++k ) out3[k] = m_ref[k] + m_total[k];
            return true;
        }

        void Rebase() override
        {
            // OnConstraintChanged IS the re-latch this needs, exactly: it folds the
            // accumulated total into the constraint's base and re-latches the
            // mapping start at the cursor's CURRENT position.  That is the same
            // "locking never jumps" guarantee (kiwi_transform.h CURSOR MAPPING),
            // applied to the PAUSED → HOT edge instead of to an axis key.
            OnConstraintChanged();
        }

        // ── SHAKEOUT G: the GRAB GATE (kiwi_transform.h THE MOVE GRAB GATE) ──
        // The framework's shared handle-grab arm calls this on the press and
        // release edges of ANY handle — a gizmo arrow, a plane corner, the centre
        // square, the lollipop's ball (kiwi_command.h THE ONE HANDLE-GRAB ENTRY).
        // The re-latch on the PRESS edge is the same OnConstraintChanged an axis
        // key runs, and it is what stops the geometry jumping by however far the
        // cursor idled between pressing G and taking hold of a handle.
        //
        // ── KIWI-UX (ROUND L): THE GRAB-FRESHNESS LATCH ──────────────────────
        // Re-latching the MAPPING is not enough on its own, because the mapping is
        // not the only thing Recompute reads.  The SNAP arm is ABSOLUTE — it sets
        // `total = snapPos - ref`, dragging the reference point ONTO whatever the
        // snap layer answered — and the grid arm re-quantises the accumulated
        // total.  Neither is a delta from anything, so neither is zeroed by a
        // re-latch, and both fired on the very first Recompute after a grab.
        //
        // So the grab pixel is latched here too, and the whole cursor-driven half
        // of Recompute is held until the cursor LEAVES it (GrabLive below).  One
        // pixel of real movement is all it takes to release the freeze, so nothing
        // a user can perceive is lost — and until then, a press on a handle moves
        // nothing at all, whatever is underneath it.  That is the directive:
        // "It should ONLY move with gizmo drag, no pre existing mouse offset."
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

        // ROUND L: raised by the shared arm for every handle this command owns.
        void HandleGrab( bool held ) override { NoteGrab( held ); }

        // KIWI-UX (ROUND BK, ITEM 6b): "is a handle actually held right now?",
        // published on the same terms as PivotPlacingNow — KiwiXform_WantsSnapDots
        // asks it and nothing outside may SET the flag.  `m_grabbed` rather than
        // GrabLive(): the dots are guidance and must appear on the press, not one
        // pixel of travel later.
        bool HandleHeld() const { return m_grabbed; }

        // The DRIVE face's own normal, for the gizmo's fourth arrow.  False unless
        // this gesture is actually pushing faces.
        bool PushDir( float *out3 ) const
        {
            if ( m_kind != SEL_FACE || m_faces.empty() || m_construct )
                return false;
            Copy3( m_driveNormal, out3 );
            return true;
        }

        // ── ROUND K: THE LOLLIPOP (kiwi_lollipop.h) ─────────────────────────
        // A FACE gesture is the one-degree-of-freedom case, and the one the user
        // asked for by name.  Everything else — objects, edges, vertices,
        // construction — keeps the three-arrow gizmo, which is the right handle for
        // a three-axis move.
        //
        // THE ANCHOR IS THE LIVE ONE.  m_ref is the LATCHED centroid and m_scalar is
        // how far the push has travelled since, so `m_ref + m_pushDir * m_scalar` is
        // where the face is on THIS frame — which is exactly the directive's "it
        // should move with the face so it doesn't get buried after a grab".  (The
        // gizmo deliberately anchors on m_ref alone — kiwi_transform.h — because a
        // translate gizmo that crawls with the geometry it is dragging is a gizmo
        // that runs away from the cursor.  A one-axis handle has no such problem.)
        //
        // THE DIRECTION CARRIES THE SIGN.  A negative push moves the face INTO the
        // solid; a stem that kept pointing along the outward normal would then be
        // sticking out of the opposite face with its ball inside the brush.  Flipping
        // with the sign keeps the stem on the side the user is dragging toward, which
        // is the other half of "stay external to the face".
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_kind != SEL_FACE || m_faces.empty() || m_construct )
                return false;
            if ( !outAnchor || !outDir )
                return false;
            // The DELETE state draws the untouched baseline (DrawWorld), so the
            // handle must sit on the baseline too rather than on a face that has
            // been pushed out the far side of its own brush.
            const float s = m_deleting ? 0.0f : m_scalar;
            for ( int k = 0; k < 3; ++k )
            {
                outAnchor[k] = m_ref[k] + m_pushDir[k] * s;
                outDir[k]    = ( s < 0.0f ) ? -m_pushDir[k] : m_pushDir[k];
            }
            return true;
        }

        // (The ball IS the move grab: ONE gate, whichever handle opened it — that
        //  is now HandleGrab above, raised by the shared arm for the ball, the
        //  arrows, the plane corners and the centre square alike.)

        // ── ROUND K: "left clicking is disabled" ────────────────────────────
        // USER REPORT, verbatim: "When clicking a face, left clicking is disabled.
        // It shouldn't be disabled.  I should be able to click multiple faces while
        // holding shift or move to another face without de-selecting first.  Fix."
        //
        // THE EXACT RULE TABLE THIS IMPLEMENTS (the framework has already
        // established PAUSED, and the lollipop's ball has already had first refusal
        // on the press — kiwi_viewport.cpp):
        //
        //   state                     press           result
        //   ------------------------- --------------- ---------------------------
        //   auto-entered face push,   plain LMB       CANCEL, then the ORDINARY
        //   PAUSED, nothing applied                   click grammar at that pixel
        //                                             (which re-selects whatever is
        //                                             there and, in mode 3 on a
        //                                             face, auto-enters again)
        //   …same                     Shift+LMB       CANCEL, ADD that face to the
        //                                             selection, RE-ENTER the push
        //                                             over the enlarged set
        //   …same                     LMB on nothing  CANCEL + deselect (the plain
        //                                             click grammar does both)
        //   anything applied /        any LMB         UNCHANGED: shakeout E's
        //   grabbed / typed                           resume-on-any-press
        //   not a face gesture        any LMB         UNCHANGED
        //
        // WHY "CANCEL AND RESTART" IS INDISTINGUISHABLE FROM "KEEP THE COMMAND".
        // The gate is `nothing applied`, and shakeout G proved what that means:
        // Begin never mutates and never opens a bracket, ApplyFaces returns before
        // OpenUndoForBrushes while the scalar is still zero, and KiwiCmd_MouseMove
        // refuses to reach the command at all while PAUSED.  So Cancel here is
        // RestoreAll over an untouched brush plus KiwiCmd_UndoCancel over a bracket
        // that was never opened — provably no geometry change and provably no undo
        // record.  Restarting then re-latches over the new face set, which is the
        // only thing "keep the command" could have meant anyway.
        //
        // Delegating to KiwiBox_ClickSelectAt rather than re-picking here is what
        // keeps ONE click grammar in the editor: construction lines, regions and
        // brush geometry all arbitrate in one place, and this rung inherits every
        // rule they agree on for free — including region selection, which a
        // hand-rolled face-only pick here would have silently swallowed.
        // ── ROUND N: THE SAME GATE, ASKED WITHOUT A PRESS ───────────────────
        // The key funnel needs the identical question the press rung asks — "is
        // this an auto-entered face gesture that has applied nothing?" — so that a
        // FACE-CONTEXT VERB (Ctrl+R, Z, E, J, C, Q) can cancel it and run over the
        // face selection it is holding, instead of being swallowed by
        // KiwiCmd_KeyDown's catch-all rung.  kiwi_command.h PreemptIdle has the
        // whole report and the whole argument; this is one predicate, factored so
        // the two callers can never drift apart.
        bool PreemptIdle() const override { return IdleUnmovedFace(); }

        // ══════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND BO, ITEM 3) — THE SNAP CONTEXT, AND THE ONE EXCEPTION
        // ══════════════════════════════════════════════════════════════════
        // Round Z's asymmetry is gone: this command is a TRANSFORM in every arm —
        // object move, edge/vertex drag, gizmo axis drag AND the face push/pull —
        // so all of them are RAW until Ctrl is held.  USER DIRECTIVE, verbatim:
        // *"with dragging, there is no snapping unless ctrl is held.  Respect
        // that."*  The BN occlusion-gated ladder the user praised is not watered
        // down by this; it is what Ctrl now switches ON.
        //
        // THE ONE EXCEPTION, and it is the whole reason SnapContext has a third
        // value: PIVOT PLACEMENT (V).  The pivot exists to be put exactly on a
        // corner — "able to snap corners together easily by placing a pivot" is the
        // directive that created it — so a raw pivot placement would be a feature
        // with no purpose.  It snaps always, Ctrl included; the contract page says
        // so out loud so it does not read as the old inconsistency coming back.
        kiwiSnapCtx_t SnapContext() const override
        { return m_pivotPlacing ? KSNAPCTX_ALWAYS : KSNAPCTX_TRANSFORM; }

        // ── KIWI-UX (ROUND Z, ITEM 1): G / R / S MAY TAKE THIS OVER ─────────
        // kiwi_command.h CanSwapTo carries the argument and the Plasticity cites.
        //
        // THE ONE REFUSAL is a FACE push that has already moved: Commit deselects
        // on that path (round K's after-confirm deselect), so committing it to run
        // R would hand the new verb an EMPTY selection.  An unmoved face gesture is
        // fine — it cancels, and Cancel never clears anything.
        //
        // The CONSTRUCTION arm is refused outright: its undo record is a store
        // snapshot taken by KiwiConSel_MoveBegin (kiwi_conselect.h), and no
        // transform in the swap list acts on construction geometry, so a swap could
        // only end in a committed snapshot and a verb that declines.
        //
        // …and never mid-PIVOT-PLACEMENT: V owns the keyboard for those frames and
        // the placement's own Esc / V grammar must not be shot out from under it.
        bool CanSwapTo( int commandId ) const override
        {
            (void)commandId;
            if ( m_pivotPlacing || m_construct )
                return false;
            if ( m_kind == SEL_FACE && GestureMoved() )
                return false;
            return true;
        }

        // "Would a commit keep anything?"  m_undoOpen is the definitive one (the
        // bracket is opened by the FIRST real mutation — Apply / ApplyFaces); the
        // rest catch the states that are about to mutate on the next frame.
        bool GestureMoved() const override
        {
            if ( m_undoOpen || m_deleting || m_hasNum )
                return true;
            if ( m_kind == SEL_FACE )
                return fabsf( m_scalar ) > KX_EPS;
            return Len3( m_total ) > KX_EPS;
        }

        // "A FACE gesture that has changed nothing yet."  Every clause is one of
        // the ways this command can already have done work: an open bracket, the
        // push-through-delete state, a held handle, a typed distance, or a non-zero
        // push.  With all five clear, both Cancel and a takeover are provably
        // record-free and geometry-free (the proof is in the block above).
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

            // The additive case: an additive click never auto-enters (that is
            // kiwi_boxselect.cpp's rule, and it is right — a selection-building
            // gesture must not start a command every time it grows).  So the push is
            // re-entered here, over the set the click just enlarged.
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

        // ── one face the gesture pushes (§20) ───────────────────────────────
        struct faceUnit_t
        {
            selbrush_t *node;
            brush_t    *def;
            int         faceIndex;
            float       normal[3];      // baseline outward normal
            float       basePts[9];     // baseline planepts
            // The baseline TEXDEF block too (face_t.mtldef, 4 MaterialDefs).
            // Texture lock REWRITES the texdef, so without this the lock would
            // chain frame to frame — reprojecting an already-reprojected texdef —
            // and a drag out-and-back would not return the texture to where it
            // started.  With it, every frame does exactly one Save/move/Reproject
            // over the ORIGINAL state, which is what apply-from-baseline means for
            // a face (kiwi_transform.h rule 1).
            byte        baseMtl[sizeof( MaterialDef ) * 4];

            // ── SHAKEOUT G: how far this face can be pushed IN before the brush
            // stops existing.  USER DIRECTIVE, verbatim: "Also allow the extrusion
            // mode to completely delete a part of a brush by pushing it all the way
            // off."
            //
            // `depth` is the brush's THICKNESS along this face's baseline outward
            // normal: max over every winding point p of ( d0 - n·p ), where
            // d0 = n·planepts[0].  Every point of a convex solid is on the inside
            // (n·p <= d0), so depth >= 0 and it is exactly the distance the plane
            // would have to travel inward to reach the farthest vertex.  Measured
            // ONCE, at Begin, from the untouched geometry — measuring it per frame
            // would read a brush the gesture has already deformed.
            float       depth;
            bool        doomed;         // this frame's push annihilates the brush
        };

        // ── one UNIQUE world edge and the faces that share it (§21) ─────────
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

        // ── one vertex the gesture drags (§22) ──────────────────────────────
        struct vertUnit_t
        {
            selbrush_t *node;
            brush_t    *def;
            bool        patchPoint;
            int         col, row;       // patch control point
            float       basePos[3];     // baseline world position
            float       curPos[3];      // where the ported solver actually left it
        };

        bool Begin() override
        {
            Reset();

            if ( !DominantKind( &m_kind ) )
            {
                // ── KIWI-UX (shakeout F): the CONSTRUCTION arm ────────────────
                // G on a PURE construction selection moves construction geometry.
                // "Pure" is free here: DominantKind returning false already means
                // the typed selection is empty, so this arm is only ever reached
                // when there is nothing brush-side to move and G can never be
                // ambiguous about which of the two selections it acts on.
                //
                // v1 IS WHOLE-OBJECT ONLY: a KCONSEL_POINT or KCONSEL_SEGMENT item
                // still drags its whole object.  Per-point editing needs the store
                // to grow a point-level edit AND the drawing tools to agree about
                // what a moved point means for a PARAMETRIC circle/arc, which is a
                // bigger change than a shakeout round — logged in
                // RADIANT_KNOWN_ISSUES rather than half-done here.
                if ( KiwiConSel_CanMove() && KiwiConSel_MoveBegin( m_ref ) )
                {
                    m_construct = true;
                    m_kind      = SEL_OBJECT;   // the free/axis/plane cursor mapping
                                                // is the whole-object one, unchanged
                    // ── KIWI-UX (ROUND AP, ITEM 2): THE PIVOT TAKES CONTROL HERE TOO ──
                    // USER DIRECTIVE, verbatim: "Fix this so the pivot point takes
                    // control in all cases and doesn't try to fight and teleport."
                    //
                    // This arm returns BEFORE the shakeout-G line further down
                    // (`m_pivotOverridden = PivotActive( m_ref )`), so a construction
                    // move was the one Move arm that ignored a placed pivot outright:
                    // m_ref stayed the anchor centroid, the gizmo drew on the centroid,
                    // the geometry-snap arm dragged the CENTROID onto the target, and V
                    // did nothing until the user happened to press it mid-gesture
                    // (ApplyPivot, which does work here).  "In all cases" means this
                    // arm asks the same question the brush arms ask, at the same time
                    // and with the same one-latch rule: read AFTER MoveBegin has
                    // written the natural anchor, so the natural one is still what a
                    // pivot-less gesture uses and is still there the moment the pivot
                    // expires (PivotActive self-expires on a selection change, and its
                    // signature already counts construction items — SelectionSignature).
                    //
                    // ONE LATCH, and it holds for the whole gesture: m_ref is written
                    // exactly once, here, and only ever read afterwards.  That is what
                    // makes the round-Z invariant `live == m_ref + m_total` true for
                    // this arm as well, which in turn is what the absolute snap arms
                    // (`total = snapped - m_ref`) require in order to be idempotent
                    // rather than compounding.
                    m_pivotOverridden = PivotActive( m_ref );
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

            // KIWI-UX (shakeout G): the SESSION PIVOT overrides the reference point
            // this gesture maps against — the constraint anchor, the gizmo origin
            // and the axis/plane-lock origin are all m_ref, which is precisely "the
            // point of pivot" the directive asks to be able to move.  It is read
            // AFTER the per-kind Begin so the natural anchor is still computed (and
            // still used the moment the pivot expires).
            m_pivotOverridden = PivotActive( m_ref );

            // The movement plane's normal is LATCHED here — see kiwi_transform.h.
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
            if ( TrackPivot( snap ) )         // shakeout G: V-placement owns the move
                return;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            if ( HandlePivotKey( vk ) )       // shakeout G: V / Esc-while-placing
                return true;
            return HandleAxisKey( vk, mods, true );
        }

        // ── shakeout G: the pivot hooks (KiwiXformBase) ─────────────────────
        bool SupportsPivot() const override { return true; }

        // ── KIWI-UX (ROUND Z, ITEM 4): THE ANCHOR IS THE **LIVE** ONE ───────
        // Was `return m_ref`, the BASE reference point.  Every reader of this
        // wanted "where is the pivot right now": BeginPivot seeds the placement
        // marker with it, and a marker that starts 101 inches behind the geometry
        // is the same bug the gizmo had.  LiveAnchor is the one definition of
        // "now" and this forwards to it rather than keeping a second answer.
        const float *PivotAnchor() const override
        {
            LiveAnchor( m_liveRef );
            return m_liveRef;
        }
        void RefreshHud() override { UpdateHud(); }

        // ── KIWI-UX (ROUND Z, ITEM 4): THE PIVOT WAS COUNTED TWICE ──────────
        // USER REPORT, verbatim: "I just set the pivot to the top center of this
        // cylinder and it does this.  It's wrong.  The gizmo needs to ride exactly
        // where I placed the pivot(V) so I can snap how I want it to snap easier."
        // — with the gizmo drawn ~101.5 inches above the cylinder and the value
        // bubble reading 101.5 in, i.e. the offset WAS the gesture's own travel.
        //
        // ROOT CAUSE, and it is one line.  The gesture's ONE invariant is
        //     live position of the reference point  ==  m_ref + m_total
        // (LiveAnchor, the gizmo through KiwiXform_ActivePivot, the commit ride,
        // the marker in DrawWorld, and the grid arm's `snap( m_ref + total )` all
        // read it that way).  `p` here is a point picked under the cursor, i.e. a
        // CURRENT world position — it already contains m_total.  Writing it into
        // m_ref made the invariant read `p + m_total`, so everything that draws or
        // rides the pivot was off by exactly what the move had applied so far.
        // Placing a pivot before any travel hid it completely, which is why it
        // survived rounds L and T: at m_total == 0 the two are the same point.
        //
        // THE FIX IS AT THE SOURCE — subtract what has already been applied, so
        // the BASE that satisfies the invariant is stored.  Nothing downstream
        // changes and nothing is re-offset at draw time:
        //   * LiveAnchor / the gizmo  → m_ref + m_total == p            (drawn ON it)
        //   * the geometry-snap arm   → total = snapPos - m_ref, so the live anchor
        //                               lands EXACTLY on the target (Recompute)
        //   * Commit's PivotRide      → PivotStore( m_ref + m_total ) == where it is
        //   * RecomputeFace's absolute grid arm reads m_ref[axis] as "the plane's
        //     BASELINE position", which the face form below preserves: p is on the
        //     pushed plane, so p - pushDir*scalar is on the baseline one.
        void ApplyPivot( const float p[3] ) override
        {
            if ( m_kind == SEL_FACE )
                Mad3( p, m_pushDir, -( m_deleting ? 0.0f : m_scalar ), m_ref );
            else
                Sub3( p, m_total, m_ref );
            m_pivotOverridden = true;         // ROUND L — the ride below keys on it
            // Re-latch, exactly as an axis key does: the mapping origin moved, and
            // the accumulated total must survive that without the geometry jumping.
            OnConstraintChanged();
            UpdateHud();
        }

        // ── KIWI-UX (ROUND L): THE ANCHOR RIDES THE APPLIED TRANSLATION ──────
        // USER REPORT, verbatim: "The pivot point is broken when moving a box
        // because the gizmo doesn't move with the object.  Fix this.  I should be
        // able to snap corners together easily by placing a pivot and moving it."
        //
        // m_ref is LATCHED for the whole gesture and MUST stay latched: it is the
        // origin every cursor mapping measures from (MapCursor), so moving it
        // mid-drag would move the mapping under the drag.  What was wrong was that
        // the gizmo was ALSO drawn on it, so the handles stayed nailed to the
        // gesture's starting point while the geometry walked away from them — and
        // on the next grab they were still there, nowhere near the corner the
        // pivot was placed on.
        //
        // So the DISPLAY anchor is `m_ref + whatever has been applied`, which is
        // where the pivot physically is right now, while the MAPPING anchor stays
        // m_ref.  Two different questions, two different answers, one latch.
        // (kiwi_transform.h's shakeout-D note argued a translate gizmo must NOT
        // crawl with its geometry; that was written for a gizmo whose mapping
        // origin and draw origin were the same value.  With them separated the
        // arrows travel WITH the cursor rather than away from it, which is what
        // every other modeller does and what the directive asks for.)
        void LiveAnchor( float *out3 ) const
        {
            if ( m_kind == SEL_FACE )
                Mad3( m_ref, m_pushDir, m_deleting ? 0.0f : m_scalar, out3 );
            else
                Add3( m_ref, m_total, out3 );
        }

        // KIWI-UX (CLEANUP, A-35): RULE — the snap query is asked AT THE CURSOR
        // (kiwi_command.h's SnapQueryAnchor default); this command installs no
        // override.  Do not redirect it at the pivot: the mapping already resolves
        // `total = snapPos - m_ref`, so pointing at a corner puts the pivot there.

        // The vector the SESSION PIVOT rode this gesture, or nothing.  Only a
        // WHOLE-SELECTION translation carries the pivot with it: a face push, an
        // edge move and a vertex drag RESHAPE the solid, and there is no one vector
        // the anchor travelled — the corner the pivot sits on may not have moved at
        // all.  Those leave the pivot exactly where the user put it.
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
            // KIWI-UX (shakeout G): PUSH-THROUGH DELETE.  Tested FIRST, above the
            // invalid arm, because the delete state is deliberately NOT an invalid
            // one — the geometry on screen is the untouched baseline and what the
            // gesture is asking for is a removal, not a reshape.
            if ( m_deleting )
            {
                CommitDelete();
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            if ( m_invalid )
            {
                // §19: never commit invalid geometry.  Restore, then take the
                // CANCEL path so the gesture leaves nothing behind at all
                // (KiwiCmd_UndoCommit no-ops once the bracket is closed here).
                RestoreAll();
                KiwiCmd_UndoCancel();
                m_undoOpen = false;
                Sys_Printf( "Move: cancelled — %s.\n", m_why ? m_why : "invalid geometry" );
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            // KIWI-UX (shakeout F): the construction arm KEEPS its store snapshot
            // — that snapshot IS this gesture's undo record (kiwi_conselect.h).
            if ( m_construct )
            {
                // ── KIWI-UX (ROUND AP, ITEM 2): AND THE PIVOT RIDES IT ───────
                // The round-L ride below is unreachable from here (this branch
                // returns first), so without this a pivot placed for a construction
                // move stayed at the position the lines USED to be at and the next
                // G started with the anchor detached from the geometry again — the
                // second half of "the pivot point takes control in all cases".  A
                // construction move IS a whole-selection translation, which is
                // exactly PivotRide's precondition (m_kind is SEL_OBJECT on this
                // arm by construction, set in Begin).  Written the same way and in
                // the same order as the brush copy: BEFORE Reset, which forgets
                // m_total, and nothing here deselects.
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

            // DELIBERATE DIVERGENCE from the ported vertex drag, applied ONCE at
            // commit (never per frame): Brush_MoveVertex does not call
            // Brush_BuildWindings, so it leaves def->[mins,maxs] describing the
            // brush as it was BEFORE the drag — every consumer of the bbox
            // (Select_GetBounds, the filters, culling) then sees stale bounds until
            // something else happens to rebuild.  The planepts the solver wrote are
            // self-consistent, so one rebuild here refreshes the box without
            // changing the geometry.  Face/edge/object paths already rebuild inside
            // their own apply.
            // ── ROUND K: AFTER-CONFIRM DESELECT ──────────────────────────────
            // USER DIRECTIVE, verbatim: "After confirming an action, the part should
            // be de-selected as well."  A confirmed FACE push/pull ends with nothing
            // selected: the face the gesture was about is where the user just left
            // it, and keeping it selected means the very next click in the viewport
            // re-enters push/pull on a face nobody asked to touch again.
            //
            // FACES ONLY, and deliberately so.  An OBJECT / EDGE / VERTEX move ends
            // with the thing still selected because a mapper places, looks, and
            // nudges again — that is the gesture's whole rhythm, and it is what
            // every classic drag in this editor does.  The EXTRUDES keep their own
            // rule (kiwi_extrude.cpp): they land a NEW brush and that brush stays
            // selected, because the flow's result is the new body.
            const bool deselectAfter = ( m_kind == SEL_FACE && !m_construct );

            // ── KIWI-UX (ROUND L): THE PIVOT RIDES THE COMMITTED TRANSLATION ──
            // Placed BEFORE Reset (which forgets m_total) and before the deselect
            // (PivotStore re-takes the SELECTION SIGNATURE, and a cleared selection
            // would stamp the pivot with a signature that expires immediately).
            // The deselect only ever runs on the FACE path, which PivotRide refuses
            // anyway, so the two can never fight; the order is belt and braces.
            //
            // This is what makes "place a pivot on a corner and keep going" work:
            // the pivot is the constraint anchor, the gizmo origin AND the snap
            // reference (Recompute's geometry-snap arm drags m_ref onto the target),
            // so after a commit all three have to be where that corner NOW is.
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
            if ( deselectAfter )                 // ROUND K — see the note above
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
            // KIWI-UX (shakeout G): the pivot marker outranks everything else in
            // this batch — while a placement is live it IS the gesture, and the
            // constraint accent would only be describing an anchor that is moving.
            if ( m_pivotPlacing )
            {
                DrawPivotMarker( m_pivotWip, true );
                return;
            }

            // KIWI-UX (shakeout G): the DELETE preview.  Dim red outlines of exactly
            // the brushes a confirm would remove, over their UNTOUCHED geometry (the
            // push itself is rolled back the moment the state is entered), so the
            // user sees what is about to go rather than a mangled solid.
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
            // ROUND L: the marker sits on `now`, the LIVE anchor, for the same
            // reason the gizmo does — it is a picture of where the pivot IS, and a
            // pivot left behind at the gesture's start is exactly the bug.
            if ( m_pivotOverridden )
                DrawPivotMarker( now, false );
        }

        // KIWI-UX (shakeout A, §14): the gizmo's one reach into this command —
        // apply the grabbed handle's constraint through the SAME SetConstraint the
        // X/Y/Z keys use, so the re-latch that stops a mid-gesture lock from
        // jumping applies to a handle grab too.  See KiwiXform_PresetMoveConstraint.
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
        // ─── lifecycle ──────────────────────────────────────────────────────
        void Reset()
        {
            m_construct    = false;
            m_faces.clear();
            m_edges.clear();
            m_verts.clear();
            m_base.clear();
            m_haveMapStart = false;
            m_axisWarned   = false;    // ROUND AI, ITEM 2
            m_invalid      = false;
            m_why          = 0;
            m_hasNum       = false;
            m_grabbed      = false;    // shakeout G: the grab gate
            m_grabFresh    = false;    // ROUND L: the grab-freshness freeze
            m_deleting     = false;    // shakeout G: the push-through-delete state
            m_pivotPlacing = false;
            m_pivotOverridden = false; // ROUND L: is m_ref the session pivot?
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

        // KIWI-UX (CLEANUP, A-30): the ONE liveness sweep of the frame.  Walks each
        // unit exactly once, stamps the answer, and reports whether every unit was
        // live — the same question, and the same answer, AllLive gave here before.
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

        // The stamp's lifetime, on EVERY exit path out of Recompute (it has several).
        struct liveScope_t
        {
            KiwiMoveCommand *c;
            explicit liveScope_t( KiwiMoveCommand *cmd ) : c( cmd ) {}
            ~liveScope_t() { c->m_liveStamped = false; }
        };

        // One unit's liveness: the stamp inside a Recompute, the direct display-list
        // test everywhere else.  Same answer either way; only the cost differs.
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

        // Snapshot one brush def once, however many items of the gesture name it.
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
            // KIWI-UX (shakeout F): construction geometry has no brush baseline and
            // no ported undo bracket — its cancel is popping the store snapshot
            // MoveBegin pushed, which is an exact restore AND drops the record.
            if ( m_construct )
            {
                KiwiConSel_MoveCancel();
                return;
            }

            // The texdef half of the face baseline: KiwiValid_Restore only knows
            // about planepts / control points, and texture lock rewrote the texdef.
            for ( size_t i = 0; i < m_faces.size(); ++i )
            {
                faceUnit_t &u = m_faces[i];
                if ( !FaceLive( i ) || !u.def->faces      // KIWI-UX (CLEANUP, A-30)
                  || u.faceIndex >= u.def->faceCount )
                    continue;
                memcpy( &u.def->faces[u.faceIndex].mtldef[0], u.baseMtl, sizeof( u.baseMtl ) );
            }

            for ( size_t i = 0; i < m_base.size(); ++i )
            {
                // Never write through a node the map has since freed.
                if ( !BaseNodeLive( m_base[i].def ) )
                    continue;
                KiwiValid_Restore( m_base[i] );
            }
            if ( m_kind == SEL_OBJECT && m_undoOpen )
            {
                // Objects are moved through Select_Move, not through planepts, so
                // the roll-back is the inverse translate rather than a baseline
                // rewrite.  Exact: the residual chain always sums to m_applied.
                const float back[3] = { -m_applied[0], -m_applied[1], -m_applied[2] };
                if ( Len3( back ) > KX_EPS && SelectionHasObjects() )
                    Select_Move( back, 0 );
                m_applied[0] = m_applied[1] = m_applied[2] = 0.0f;
            }
        }

        bool BaseNodeLive( const brush_t *def ) const
        {
            // KIWI-UX (CLEANUP, A-30): the terminal test is the frame's stamp when
            // there is one, so a rollback is O(base x units) rather than
            // O(base x units x brushes).
            for ( size_t i = 0; i < m_faces.size(); ++i )
                if ( m_faces[i].def == def ) return FaceLive( i );
            for ( size_t i = 0; i < m_edges.size(); ++i )
                if ( m_edges[i].def == def ) return EdgeLive( i );
            for ( size_t i = 0; i < m_verts.size(); ++i )
                if ( m_verts[i].def == def ) return VertLive( i );
            return false;
        }

        // ─── Begin, per context ─────────────────────────────────────────────
        bool BeginObjects()
        {
            if ( !SelectionHasObjects() )
            {
                Sys_Printf( "Move: no whole objects are selected.\n" );
                return false;
            }
            // Reference = the active object's own centre when there is one, else
            // the selection centre (spec: "the moved active-item position, or
            // selection-mid when no active").  Select_GetTrueMid, not
            // Select_GetMid: the latter floor-snaps to the LEGACY grid, which
            // would bias every snap by up to one legacy cell.
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
                // SHAKEOUT G — the push-through-delete threshold (faceUnit_t::depth).
                // ROUND Q hoisted the body to namespace scope, so the E-extrude's
                // negative arm measures the brush with this exact ruler.
                u.depth  = FaceDepthAlong( def, u.normal, u.basePts );  // KIWI-UX (CLEANUP, A-41)
                u.doomed = false;
                m_faces.push_back( u );
                AddBaseline( def );
            }
            if ( m_faces.empty() )
            {
                Sys_Printf( "Move: no pushable faces in the selection.\n" );
                return false;
            }

            // The ACTIVE face drives the scalar; everything else follows it along
            // its own normal (§20's multi-face rule).
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

                // ── THE DEDUP (RADIANT_KNOWN_ISSUES "UX overhaul" debt) ──────
                // One physical brush edge is shared by two faces, so an edge-mode
                // pick/marquee yields TWO (face,edge) items naming the SAME world
                // segment.  Solving each independently would write the same faces'
                // planepts twice with different anchors.  Segments are compared
                // UNORDERED (the two faces wind in opposite directions, so the
                // endpoints arrive swapped) at the same 0.1-unit tolerance the
                // ported FindPoint/SetupVertexSelection dedup uses.
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

                // v1 LIMIT, stated rather than faked: two selected edges that share
                // a face are ILL-POSED for this solver.  Each unit rewrites its
                // adjacent faces' planepts from "moved edge + one RETAINED point",
                // and a face with two moving edges has no retained point that
                // stays put — whichever unit wrote last would win and the other
                // edge would not move at all.  The second unit is dropped with a
                // message instead (a proper simultaneous solve belongs with the
                // Phase-5 modeling work).
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

            // Reference = the active edge's midpoint, else the mean midpoint.
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

        // Every face of the brush whose winding contains BOTH endpoints, with the
        // retained third point: the winding vertex NOT on the edge that is
        // FARTHEST from the edge line (spec §21 "the one farthest from the edge
        // line for stability" — a near-collinear third point makes the re-solved
        // plane numerically worthless).
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
                    // Perpendicular distance from the edge LINE.
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
                    // Same control point twice = the same drag; skip the duplicate.
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
                    // ── THE VERTEX FAN-OUT (kiwi_selection.h DESIGN NOTE 3) ──
                    // One world corner is N distinct (face,vert) items.  The
                    // PORTED solver Brush_MoveVertex finds every face of the brush
                    // that touches the world position itself, so the fan-out is
                    // ALREADY handled inside it — all this layer must do is not
                    // drive the same (brush, position) twice.
                    bool dup = false;
                    for ( size_t k = 0; k < m_verts.size() && !dup; ++k )
                        dup = ( m_verts[k].def == u.def && !m_verts[k].patchPoint
                             && PointNear( m_verts[k].basePos, u.basePos, 0.1f ) );
                    if ( dup )
                        continue;
                    // NO planept baseline for this path: Brush_MoveVertex splits and
                    // collapses windings, so it CHANGES faceCount — a 9-floats-per-face
                    // copy would no longer describe the brush.  Roll-back for brush
                    // vertices is the undo bracket (and the solver's own revert-on-
                    // non-convex), which is why this path never calls RestoreAll's
                    // baseline arm.
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

        // ─── ROUND L: the grab-freshness freeze (see NoteGrab) ──────────────
        // A grab is "fresh" from the press until the cursor leaves the press pixel.
        // While it is, NOTHING cursor-driven contributes: not the delta mapping,
        // not the geometry snap, not the grid quantiser.  One pixel of travel ends
        // it for good (until the next grab).
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

        // ─── mapping ────────────────────────────────────────────────────────
        // The cursor's world position under the CURRENT constraint.
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
                // ── KIWI-UX (ROUND AI, ITEM 2): SAY WHY, ONCE ───────────────
                // The only way MapCursor fails on a one-axis constraint is the
                // sample gate in RayAxis above — the drag axis is within ~14
                // degrees of the view direction, where the closest-point solve is
                // amplified by 1/sin^2 and returns garbage.  Before this round it
                // returned that garbage; now it refuses, and a silent refusal on
                // the single most-used gesture in the editor (push a wall face
                // while looking at the wall) would be worse than the bug.  The
                // gesture is NOT cancelled — release, orbit a little and grab
                // again, or type the distance.
                //
                // The VIEW gate is re-asked here purely to CLASSIFY the failure:
                // MapCursor also fails when the cursor is not over the camera image
                // at all (CursorRay), and that must stay silent.  Only a genuinely
                // end-on drag axis earns the sentence.
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
            // Carry the accumulated delta into the new constraint, then re-latch
            // the mapping at the cursor's current position: the geometry does not
            // move at the instant the key is pressed.
            if ( m_kind == SEL_FACE )
            {
                // A face gesture re-aims its push direction instead: default = the
                // driving face's own normal, an axis lock = that world axis.
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
                // KIWI-UX (ROUND BL, ITEM 4): every GRAB and every constraint change
                // re-arms the pivot rebase.  It is consumed on the first frame the
                // grab is LIVE (Recompute), not here: the cursor has not necessarily
                // moved yet, and a rebase on the press edge would move geometry on a
                // press — the exact thing the round-L grab-freshness latch exists to
                // forbid ("It should ONLY move with gizmo drag, no pre existing
                // mouse offset").
                m_pivotRebase = true;
            }
            LatchMapStart();
        }

        // KIWI-UX (ROUND BL, ITEM 4): "does THIS gesture own world axis k?" — the
        // predicate three separate loops in Recompute spelled out by hand, and which
        // the rebase and the lattice pass added this round both need as well.  A
        // fourth hand-rolled copy is how the axis sets drift apart.
        bool OwnsAxis( int k ) const
        {
            return ( m_con == CON_FREE )
                || ( m_con == CON_AXIS  && k == m_axis )
                || ( m_con == CON_PLANE && k != m_axis );
        }

        // Project a delta onto the active constraint.
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

        // The direction a bare typed scalar means (kiwi_transform.h NUMERIC ENTRY).
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
            if ( m_con == CON_PLANE )
                d[m_axis] = 0.0f;
            else
                d[2] = 0.0f;                    // project on the ground plane (Z=0)
            if ( !Norm3( d ) )
            {
                out[0] = 1.0f; out[1] = out[2] = 0.0f;    // documented fallback: +X
                return true;
            }
            Copy3( d, out );
            return true;
        }

        // ─── the per-frame total ────────────────────────────────────────────
        //
        // ── KIWI-UX (SHAKEOUT G): G MOVES NOTHING UNTIL A HANDLE IS HELD ─────
        //
        // USER REPORT, verbatim: "When selecting an object and pressing G (move),
        // it moves with the mouse.  It should only move with the gizmo."
        //
        // BEFORE.  The cursor mapping ran on every MouseMove, unconditionally:
        //     float p[3];
        //     if ( m_haveMapStart && MapCursor( p ) )
        //     { Sub3( p, m_mapStart, d ); Add3( m_lockBase, d, total ); }
        // m_mapStart was latched in Begin() from wherever the cursor happened to
        // be, so the FIRST pixel of travel after pressing G translated the whole
        // selection — with no handle ever touched, and (unconstrained) along a
        // camera-facing plane the user had not chosen.  The gizmo's arrows only
        // ever narrowed that drag to an axis; they were never what caused it.
        //
        // AFTER.  Exactly two things can change the total, and both are explicit:
        //     * a HELD MOVE HANDLE feeding the cursor mapping (m_grabbed, set by
        //       HandleGrab from the framework's shared handle-grab arm),
        //     * a TYPED value (the numeric layer, m_hasNum).
        // With nothing held the total keeps its latched value: the mapping is
        // skipped, and so is the SNAP arm — a snap that fired while nothing was
        // held would drag the reference point onto a snap target, i.e. it would be
        // the same bug wearing a different hat.
        //
        // ROUND L extends that to the GRAB EDGE ITSELF (GrabLive rather than
        // m_grabbed): for as long as the cursor has not left the pixel the handle
        // was pressed at, the snap arm is skipped too — it is an ABSOLUTE mapping
        // and a re-latch cannot zero it, so it was the one thing that could still
        // move geometry on the frame a handle was taken hold of.
        //
        // The typed arm is deliberately NOT gated.  "Type a value" is the other
        // half of the affordance the idle HUD advertises, and a number the user
        // spelled out is as explicit an act as a grab.
        void Recompute() override
        {
            // KIWI-UX (CLEANUP, A-30): the frame's ONE liveness sweep, taken here —
            // where AllLive used to run — and dropped again on every exit path.
            liveScope_t liveThisFrame( this );
            if ( !StampLiveness() )
            {
                // The map freed something under us: abandon the gesture rather
                // than write through a dangling node.  Cancel() skips dead nodes.
                Sys_Printf( "Move: selection changed under the gesture — cancelled.\n" );
                KiwiCmd_Cancel();
                return;
            }

            if ( !m_haveMapStart )
                LatchMapStart();

            AgeGrab();                       // ROUND L: has the cursor left the grab pixel?

            if ( m_kind == SEL_FACE )
            {
                RecomputeFace();
                return;
            }

            float total[3] = { m_total[0], m_total[1], m_total[2] };
            float p[3];
            if ( GrabLive() && m_haveMapStart && MapCursor( p ) )
            {
                // ══════════════════════════════════════════════════════════════
                //  KIWI-UX (ROUND BL, ITEM 4) — THE PIVOT RIDES THE CURSOR
                // ══════════════════════════════════════════════════════════════
                // USER REPORT, verbatim: *"You need to keep the pivot point in line
                // with the cursor like this and snap it."*  Round BK answered the
                // same report ("the center of the gizmo should always track my
                // mouse") INSIDE THE SNAP ARM — KiwiSnap_AreaMagnet — and that arm
                // is downstream of this mapping, which is where the offset lives.
                //
                // THE MAPPING IS RELATIVE and always has been: `d = p - m_mapStart`
                // moves the selection by the cursor's TRAVEL since the grab, so
                // whatever gap there was between the cursor and the pivot at the
                // instant of the grab is carried, unchanged, for the whole drag.
                // Grab a gizmo arrow 50 px out along its shaft, or grab a big
                // cylinder anywhere but its centre, and the pivot is 50 px (or half
                // a cylinder) away from the crosshair for ever after.  That is the
                // user's screenshot, and it is also why "snap to the major grid
                // line" felt impossible: every snap band in this command is measured
                // around THE PIVOT, so the user was aiming a cursor that was not the
                // thing being snapped.  Nothing was wrong with the magnet; it was
                // being aimed with the wrong hand.
                //
                // THE REBASE, ONCE PER GRAB.  On the first LIVE frame of a grab the
                // components this constraint OWNS are re-based so that the pivot
                // sits exactly at the cursor's constrained projection; the delta
                // math below is then untouched, so round AN's "a constraint owns the
                // delta of THIS grab, the base rides through" still holds exactly —
                // the off-constraint components of m_lockBase are not written here,
                // and neither is anything else.  Round Z's invariant
                // (`live == m_ref + m_total`) is what MAKES the rebase expressible:
                // wanting pivot == p is wanting total == p - m_ref.
                //
                // MapCursor IS the constrained projection, for every constraint:
                // KiwiCam_RayAxis onto the locked axis through m_ref (CON_AXIS), the
                // ray/plane hit for CON_PLANE, and the camera-facing move plane for
                // CON_FREE.  So "the pivot tracks the cursor's constrained
                // projection" needs no new geometry — only the one-time rebase.
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
                // ── KIWI-UX (ROUND AN, ITEM 2): CONSTRAIN THE DELTA, NOT THE TOTAL ──
                // USER REPORT, verbatim: "I should be able to drag the X arrow, then
                // drag the Y arrow, and so on... Currently when changing arrows it
                // resets the operation."
                //
                // OnConstraintChanged folds the accumulated travel into m_lockBase at
                // every handle change precisely so a new constraint continues from
                // where the old one left off — and the next line here used to be
                // `Add3(lockBase, d, total); Constrain(total)`, which zeroed the
                // OFF-AXIS components of the CARRIED BASE along with the delta's.
                // Grab Y after dragging X and X's travel was wiped on the first
                // move: the fold and the constrain cancelled each other out.  A
                // constraint owns the delta of THIS grab; the base rides through.
                Constrain( d );
                Add3( m_lockBase, d, total );
            }
            // Published BEFORE the numeric branch: NumericDirection reads the live
            // mouse direction out of m_total, so it must be this frame's, not the
            // previous frame's.
            Copy3( total, m_total );

            // KIWI-UX (ROUND BL, ITEM 4): the HUD's major-line lamp is recomputed
            // from scratch every frame, HERE, so a typed value or a released grab
            // cannot leave it lit (the lattice pass below only ever sets it).
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
                    // Drag the reference point ONTO the snap target.  KIWI-UX
                    // (ROUND L): m_ref IS the session pivot whenever one is placed
                    // (Begin's PivotActive override), so "the reference point" and
                    // "the pivot" are the same point BY CONSTRUCTION and placing a
                    // pivot on a corner is what makes that corner the thing that
                    // snaps.  The ride below keeps that true across a commit.
                    //
                    // ── KIWI-UX (ROUND AA, ITEM 5): UNDER AN AXIS LOCK, A FACE IS
                    //    A PLANE HERE TOO ──────────────────────────────────────
                    // USER DIRECTIVE, verbatim: "the split tool is still vulnerable
                    // to the variable flat face non-planar behavior that was fixed
                    // with extrusions.  Fix this and in other spots too.  Make
                    // basically all operations like the new extrusion, it works
                    // good."
                    //
                    // With CON_AXIS in force this gesture IS a one-axis gesture:
                    // Constrain() zeroes the two off-axis components, so the answer
                    // is exactly dot( snap.position - m_ref, e_axis ) — round Z's
                    // hand-rolled pattern written in component form, and carrying
                    // round Z's bug with it.  SNAP_FACE's position is the sliding
                    // ray-surface hit (kiwi_snap.cpp arm 6 writes the raw Test_Ray
                    // point), so "X-lock this brush until it meets that wall" gave a
                    // different offset for every pixel of the same flat wall, and a
                    // wall EDGE-ON to the lock axis gave pure cursor noise instead
                    // of no answer.  KiwiSnap_AxisDepth is the shared rule (§56.3):
                    // target plane INTERSECT gesture axis, one scalar for the whole
                    // face, point candidates still projected, near-parallel refused.
                    //
                    // ONLY under CON_AXIS.  CON_PLANE is 2 DOF and CON_FREE is 3;
                    // AxisDepth returns ONE scalar and cannot answer either, and
                    // "drag the reference point onto that vertex" is the right
                    // question there — so those keep the projection verbatim.
                    //
                    // A REFUSAL LEAVES THE DISTANCE ALONE, which for this command
                    // means keeping the latched total rather than snapping to zero
                    // (the caller contract in kiwi_snap.h, and what the three
                    // round-Z consumers do).
                    // ══════════════════════════════════════════════════════════
                    //  KIWI-UX (ROUND BK, ITEM 6a + 6c) — AN AREA HIT IS A
                    //                                     MAGNET, NOT A TELEPORT.
                    // ══════════════════════════════════════════════════════════
                    // USER REPORTS, verbatim: *"I can no longer snap to the grid
                    // when moving with the gizmo."* and *"the center of the gizmo
                    // should always track my mouse."*
                    //
                    // ONE root cause, two symptoms, and it is a RANKING rather than
                    // a regression in this function: kiwi_snap.cpp's arm 6 answers
                    // SNAP_FACE for ANY surface under the cursor and returns before
                    // arm 9 (the grid) can, so in a built scene the `else` branch
                    // below — the whole grid quantiser — is unreachable, and this
                    // branch drags the selection onto whatever plane the ray landed
                    // on instead of tracking the cursor.  kiwi_snap.h carries the
                    // full derivation and the reason the NAMED arms keep their
                    // absolute behaviour.
                    //
                    // SO SNAP_FACE — and only SNAP_FACE — is applied through
                    // KiwiSnap_AreaMagnet (it wins only within a screen-space band
                    // of the cursor's own answer).  KIWI-UX (ROUND BL, ITEM 4): the
                    // grid half of round BK's answer used to be a second loop at the
                    // bottom of THIS branch, which meant a drag only met the lattice
                    // when the ray happened to hit a surface.  It is the unified
                    // lattice pass below now, outside this `if`, so every arm reaches
                    // it — and the pivot it snaps is under the cursor.
                    const bool areaOnly = ( m_snap.type == SNAP_FACE );
                    bool axisDepthDone = false;
                    if ( m_con == CON_AXIS )
                    {
                        float ax[3] = { 0.0f, 0.0f, 0.0f };
                        ax[m_axis] = 1.0f;
                        float t = 0.0f;
                        if ( KiwiSnap_AxisDepth( m_snap, m_ref, ax, &t ) )
                        {
                            // ROUND AN, ITEM 2: the snap is absolute ALONG THE LOCKED
                            // AXIS ONLY — the off-axis components carry the previous
                            // grabs' travel (m_lockBase, already sitting in `total`)
                            // and an axis lock does not own them.  Writing all three
                            // was the "teleports back to the start" on chained grabs.
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
                        // ROUND AN, ITEM 2: absolute only on the OWNED axes.  A
                        // plane lock owns the two in-plane components; the third is
                        // the carried base.  CON_FREE owns all three (a named target
                        // replaces everything — unchanged behavior).
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

                    // KIWI-UX (ROUND BL, ITEM 4): the grid pass that used to sit here
                    // — round BK's, and reachable only from this areaOnly arm — is
                    // now the UNIFIED lattice pass below, which every arm reaches.
                }

                // ══════════════════════════════════════════════════════════════
                //  KIWI-UX (ROUND BL, ITEM 4) — THE PIVOT'S ABSOLUTE LATTICE LOCK
                // ══════════════════════════════════════════════════════════════
                // USER REPORT, verbatim: *"It's still not possible to snap to the
                // major grid lines while using a tool.  This makes it really hard to
                // align things."*
                //
                // WHERE THE GRID WAS, BEFORE THIS ROUND.  Two separate arms, and
                // which one a drag got was decided by what the ray happened to hit:
                //   * with a surface under the cursor the query answers SNAP_FACE
                //     (kiwi_snap.cpp arm 6), and round BK's grid loop ran INSIDE the
                //     geometry branch, gated on `areaOnly`;
                //   * with nothing under the cursor it answers SNAP_GRID and a
                //     COMPLETELY DIFFERENT quantiser ran in the `else` branch —
                //     KiwiGrid_Snap, which rounds to the nearest CELL and has never
                //     heard of a MAJOR line.
                // So "can I land on the red line" had two answers in one gesture,
                // one of which was structurally no, and both were aimed at a pivot
                // that was not under the cursor (see the rebase above).
                //
                // ONE PASS NOW, on the axes this constraint owns, always absolute
                // (round P's rule: the moved PIVOT lands on the lattice, so an
                // off-grid start does not drag its offset along), with the MAJOR
                // preference from KiwiSnap_LatticeAxis in both modes.  `hard` is the
                // only thing the arm decides: the grid arm quantises (there is
                // nothing else it could mean), the area arm keeps its capture band
                // so free dragging over a surface still feels continuous.
                //
                // A NAMED GEOMETRY TARGET IS EXEMPT, unchanged: a vertex, an edge
                // midpoint, a face centre, a construction endpoint or an
                // intersection is a place the user pointed at and must never be
                // rounded off it.  SNAP_FACE names nothing (it is wherever the ray
                // landed), which is why it is on the lattice side of this line —
                // exactly the distinction round BK drew for the magnet.
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

            Copy3( total, m_total );
            Apply();
            UpdateHud();
        }

        void RecomputeFace()
        {
            float dist = m_scalar;
            float p[3];
            // KIWI-UX (ROUND BN, ITEM 6): the major lamp is this frame's answer here
            // too — same rule, same place in the flow, as the object move's reset.
            m_majorLock = false;
            // ── KIWI-UX (ROUND BP, ITEM 2): CTRL = ABSOLUTE (kiwi_extrude.h) ──
            // The third of the three one-axis gestures, on exactly the same terms as
            // the two extrudes.  MapCursor's SEL_FACE arm already hands back
            // `dot( cursorOnAxis - m_ref, m_pushDir )` — the pushed plane's own
            // position measured from the face it started on — so ABSOLUTE is that
            // value with the `- m_scalarStart` rebase term dropped.  Only the CTRL-UP
            // edge re-latches, so releasing Ctrl never moves the face.
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
            // SHAKEOUT G: the grab gate, exactly as above.  The face push is driven
            // by the gizmo's NORMAL ARROW (or any of its three world arrows, which
            // axis-lock the push), never by bare cursor travel.  ROUND L: …and not
            // until the cursor has left the grab pixel either (GrabLive).
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
                // ── KIWI-UX (ROUND BT): THE FULL LADDER (kiwi_extrude.h) ──────
                // The third of the four one-axis gestures, on exactly the same terms.
                // Round BP's nearest-value contest is gone — the hard lattice is never
                // more than half a cell from the cursor, so it beat every real face on
                // a fine grid and the push *"only snapped to the grid"*.  Geometry
                // ranks first (and is aim-gated by the ladder's own pixel radii), a
                // face plane is a magnet, the lattice with its majors is the fallback.
                dist = KiwiExt_LadderDepth( m_snap, m_ref, m_pushDir, rawAbs,
                                            &m_majorLock );
            }
            else if ( GrabLive() && SnapActive() )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // ── KIWI-UX (ROUND Z, ITEM 3): A FACE IS A PLANE ────────
                    // Was `dot( snapPos - ref, pushDir )`, which for arm 6's
                    // sliding ray-surface hit gave a different depth for every
                    // pixel of the SAME flat roof.  KiwiSnap_AxisDepth
                    // (kiwi_snap.h) intersects the target face's PLANE with this
                    // gesture's axis instead — one scalar for the whole face —
                    // and keeps the exact position of every POINT candidate.
                    // False = an edge-on face with no answer: leave `dist` alone.
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_pushDir, &sd ) )
                    {
                    // ── KIWI-UX (ROUND X, ITEM 4a): NOT TO SELF ─────────────
                    // USER DIRECTIVE: "When extruding, dont allow snapping to self,
                    // it's just an annoyance fix."  A push is measured from the
                    // face's own plane and everything the face is made of lies on
                    // it, so every one of those candidates resolves to scalar 0 —
                    // "do not push" — and they are the ones nearest the cursor for
                    // the whole first part of the drag.  PICKF_EXCLUDE_SELECTED
                    // (this command's PickFlags) hides the owner BRUSH from the pick
                    // arms; it cannot hide a construction line drawn on the face or
                    // an axis guide through its centre, and this rule covers every
                    // arm at once.  Full argument on KEXT_SELF_SNAP_BAND
                    // (kiwi_extrude.h) — one band, shared by all three one-axis
                    // gestures so they cannot drift apart.  ROUND Z, ITEM 2: it
                    // still applies — Ctrl turns the ranked query ON here, it does
                    // not turn the self-snap rule off.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            dist = sd;
                    }
                }
                else
                {
                    // §6 numeric hygiene: a face pushed along an axis-aligned
                    // normal must land exactly on the grid, so the SCALAR is what
                    // gets quantised — never the cursor point.
                    //
                    // ── ROUND P: …AND ON AN AXIS-ALIGNED NORMAL IT IS ABSOLUTE ──
                    // Same directive as the object move above, and the same defect:
                    // quantising the DISTANCE moved a face that started 3 units off
                    // the grid in clean multiples and left it 3 units off forever.
                    // When the push direction IS a world axis there is a well-defined
                    // absolute answer — the moved plane's position along that axis —
                    // so that is what is snapped:
                    //     pos  = ref[axis] + sign * dist       (|pushDir[axis]| == 1)
                    //     dist = ( snap(pos) - ref[axis] ) * sign
                    // On a SLANTED normal there is no single axis to be on the grid
                    // of — the plane would have to land on a lattice of its own — so
                    // that case keeps the delta quantisation, deliberately and
                    // narrowly.  (m_ref is the DRIVE face's winding centre, which is
                    // ON the plane being pushed, so its coordinate along an
                    // axis-aligned normal IS the plane's position.)
                    //
                    // ── KIWI-UX (ROUND BN, ITEM 6): ONE LATTICE, MAJORS INCLUDED ──
                    // USER DIRECTIVE, verbatim: *"Holding Ctrl should allow snapping
                    // to the major lines of the grid as well… (in this case, while
                    // extruding)."*  Everything the sixteen lines this replaces did —
                    // find the world axis, snap the ABSOLUTE coordinate on one,
                    // quantise the DELTA otherwise — is what KiwiSnap_LatticeAxis
                    // already does, and round BL taught THAT function the major
                    // preference for the object move.  The face push was the last
                    // owned-axis gesture still running its own copy of half the rule,
                    // and it is the gesture with the strongest claim on landing
                    // exactly on a red line.  Same numbers on a minor, majors added.
                    bool major = false;
                    dist = KiwiSnap_LatticeAxis( dist, m_ref, m_pushDir, true, &major );
                    if ( major )
                        m_majorLock = true;
                }
            }
            // KIWI-UX (ROUND BO, ITEM 3): ROUND AG'S LIGHT GRID MAGNET IS GONE.
            // USER REPORT, verbatim: *"the grid snapping is better, but now it's
            // impossible to get fine details."*  This was the third arm — a
            // capture band that pulled the push onto the lattice even with the
            // ranked query suppressed — and it is exactly what made a Ctrl-free
            // drag unable to stop between two lattice values.  "No snapping unless
            // Ctrl" has to mean NO snapping, so the raw mapped `dist` now stands.
            // The lattice (with its majors) is still one Ctrl away, on the arm
            // above.

            m_scalar = dist;

            // ── KIWI-UX (shakeout G): PUSH-THROUGH DELETE ────────────────────
            // USER DIRECTIVE: "allow the extrusion mode to completely delete a part
            // of a brush by pushing it all the way off."
            //
            // The plane's own travel along its baseline normal is
            // m_scalar * dot( pushDirection, faceNormal ) — the two are the same
            // vector unless an axis lock is in force, in which case the whole set
            // shares one world direction and each face's plane moves by its own
            // projection of it.  Once that travel is INWARD by more than the
            // brush's thickness along that normal (faceUnit_t::depth), the
            // half-space intersection is empty: there is no brush left to reshape.
            //
            // Without this the state was reported as an ordinary §19 rejection —
            // red HUD, edit rolled back, and no way to say "yes, remove it", which
            // is exactly what the user was trying to do.  Pulling back below the
            // threshold restores the ordinary push, because the flag is recomputed
            // from scratch every frame and the geometry is re-applied from the
            // untouched baseline.
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
                // Put the geometry back FIRST: the preview for this state is the
                // brush as it still is, outlined in red, not a half-collapsed one.
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

        // ── KIWI-UX (shakeout G): the push-through delete, committed ─────────
        //
        // ORDER, and why each step is where it is (read out of select.cpp /
        // mainfrm.cpp, not assumed):
        //
        //  1. RestoreAll — the doomed brushes go back to their baseline planepts
        //     before anything clones them, so what undo restores is the brush the
        //     user started with rather than a half-pushed one.  (In the delete state
        //     the geometry is already restored; this is the idempotent belt.)
        //  2. OpenUndoForBrushes — the bracket HEAD, which must run BEFORE the first
        //     mutation (kiwi_command.h).  It is Undo_ClearRedo + Undo_GeneralStart +
        //     Undo_AddBrushList(&selected_brushes) plus one Undo_AddBrush per
        //     touched brush, because a FACE selection is not on selected_brushes
        //     (kiwi_selection.h DESIGN NOTE 2).  Those per-brush clones ARE what
        //     Undo_Undo's Phase 4 re-links, i.e. what brings the brush back.
        //     It may already be open from an earlier valid push in the same
        //     gesture — that is correct and deliberate: ONE gesture, ONE record.
        //  3. Undo_AddEntity_W per owner — Cmd_OnSelectionDelete (mainfrm.cpp) does
        //     exactly this, because Select_Delete additionally FREES an owner entity
        //     left with no brushes (select.cpp:1520-1523) and that has to be
        //     restorable too.
        //  4. Select_Deselect(1) then Select_Brush per doomed node — Select_Delete
        //     takes NO arguments; it deletes whatever is on selected_brushes
        //     (select.cpp:1504-1526).  Driving the legacy selection through its own
        //     funnels is how every other verb in this layer reaches a ported core.
        //  5. Select_Delete() — the classic core, unmodified.
        //  6. Sel_Clear on the typed selection: the faces it named no longer exist.
        //  The bracket TAIL is the framework's (KiwiCmd_UndoCommit): Undo_EndBrushList
        //  over a now-EMPTY selected_brushes stamps nothing, which is the right shape
        //  for a pure removal — nothing new was added, so nothing needs the
        //  "remove on undo" stamp.
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

            // DESELECT BEFORE THE HEAD, the kiwi_extrude.h rule: everything
            // Undo_AddBrushList clones must still be on the list at COMMIT, or its
            // clone is restored alongside the live original and the brush DOUBLES.
            // Here the list is emptied by Select_Delete, so the head must find it
            // empty.  In practice it already is — a FACE selection puts nothing on
            // selected_brushes (kiwi_selection.h DESIGN NOTE 2) and a mixed
            // selection would have resolved to SEL_OBJECT, never reaching this path
            // — but "in practice" is not a guarantee and this costs one call.
            Select_Deselect( 1 );

            OpenUndoForBrushes();                       // head (may already be open)
            // `(entity_s *)node->owner->def` — the entity DEF, cast, which is the
            // EXACT argument Cmd_OnSelectionDelete feeds (mainfrm.cpp) and the thing
            // Undo_AddEntity_W compares against `(entity_s *)world_entity->def`
            // (undo.cpp:618).  NOT brush_t::owner, which is the entity INSTANCE.
            // AFTER the brushes, because undo.cpp:539 warns when brushes are added
            // to a record that already carries entities.
            for ( size_t i = 0; i < doomed.size(); ++i )
                if ( doomed[i]->owner && doomed[i]->owner->def )
                    Undo_AddEntity_W( (entity_s *)doomed[i]->owner->def );

            for ( size_t i = 0; i < doomed.size(); ++i )
                Select_Brush( doomed[i], 0, 0, 0 );

            Select_Delete();                            // the CLASSIC core
            Sel_Clear( KiwiSel() );

            Sys_Printf( "Move: pushed through — %i brush(es) deleted.\n", (int)doomed.size() );
        }

        // ─── apply ──────────────────────────────────────────────────────────
        void Apply()
        {
            // KIWI-UX (shakeout F): the construction arm applies from the BASELINE
            // (an ABSOLUTE delta — kiwi_transform.h rule 1), never incrementally.
            if ( m_construct )
            {
                KiwiConSel_MoveApply( m_total );
                m_invalid = false;
                m_why     = 0;
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

        // §20 — each face from ITS OWN baseline planepts, wrapped in the ported
        // texture-lock bracket, exactly as Brush_Move does it (brush.cpp 0x47ba40).
        void ApplyFaces()
        {
            if ( m_faces.empty() )
                return;
            // KIWI-UX (shakeout G): THE FIRST-MUTATION GUARD, made real.
            // kiwi_transform.h rule 2 says the bracket opens at the first ACTUAL
            // mutation, and ApplyObjects has always honoured that (its `Len3(d) <=
            // KX_EPS` early-out).  This path did not: it opened the bracket and then
            // wrote the baseline planepts back over themselves, so a face gesture
            // that was started and escaped WITHOUT MOVING left an empty undo record
            // behind.  That is now the common case — shakeout G makes clicking a
            // face in Face mode auto-enter this very command (kiwi_boxselect.cpp) —
            // so a zero push must cost nothing.  Once the bracket IS open every
            // frame still runs, because the baseline rewrite is how a drag returns
            // to zero.
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

                // Multi-face: the SAME scalar, each along its OWN normal (§20) —
                // except under an axis lock, where the whole set shares one world
                // direction (that is what the lock means).
                float dir[3];
                if ( m_con == CON_AXIS ) Copy3( m_pushDir, dir );
                else                     Copy3( u.normal,  dir );

                // Back to the ORIGINAL geometry AND the ORIGINAL texdef, then run
                // Brush_Move's exact bracket once over it.  Face_MakePlane first
                // because Face_TexLock_Save builds the world tex matrix from
                // face->plane.normal, which must describe the baseline planepts we
                // just restored — not last frame's rebuilt plane.
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

        // §21 — every adjacent face's plane re-solved from the MOVED edge plus its
        // retained third point.  The three points are written as the face's
        // planepts, so the ported Face_MakePlane (inside Brush_BuildWindings) is
        // still the only thing that computes a plane.
        //
        // NO TEXTURE LOCK here, deliberately: §20 asks for the lock on face
        // push/pull (a pure plane TRANSLATE, which is what Face_TexLock_Reproject's
        // LU solve is built for), and the ported edge/vertex drag path
        // (MoveSelection → Brush_MoveVertex) does not lock either.  An edge move
        // REORIENTS the adjacent planes, and reprojecting a rotating plane every
        // frame is a texture-behaviour change this phase has no oracle for.
        void ApplyEdges()
        {
            if ( m_edges.empty() )
                return;
            // KIWI-UX (shakeout G): the same first-mutation guard as ApplyFaces —
            // an edge gesture that never moved must not leave an undo record.
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

                    // Face_MakePlane's normal is cross(p0-p1, p2-p1); pick the
                    // winding order that reproduces the BASELINE outward normal,
                    // or the face would flip and the brush turn inside out.
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

        // §22 — brush verts through the PORTED solver, patch points from baseline.
        void ApplyVerts()
        {
            if ( m_verts.empty() )
                return;
            // KIWI-UX (shakeout G): the same first-mutation guard.  The per-vertex
            // residual test below already skipped the WORK for a zero delta; this
            // skips the BRACKET too, which is what rule 2 actually asks for.
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
                    // From the baseline, so a wandering drag never accumulates.
                    Add3( u.basePos, m_total, pm->ctrl[u.col][u.row].xyz );
                    Copy3( pm->ctrl[u.col][u.row].xyz, u.curPos );
                    touchedPatch = true;
                    continue;
                }

                // Brush_MoveVertex re-triangulates, so it can only be driven
                // INCREMENTALLY: hand it the residual from where it actually left
                // the vertex last time, and adopt its `end` (it grid-snaps
                // internally — see the deviation note below) as the new truth.
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
                // ONE Patch_Rebuild per patch, however many of its control points
                // the gesture moved — the rebuild re-tessellates the whole mesh,
                // so doing it per point would cost N full tessellations a frame.
                // This IS the ported post-mutation bookkeeping: Patch_Rebuild(def,1)
                // is exactly what Patch_UpdateSelected_0 (pmesh.cpp 0x43D800) runs
                // after translating the queued control points (bounds recompute →
                // Brush_RebuildBrush → curveDef re-tessellate → ++version).
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

        // Face/edge selections are NOT on selected_brushes, so the bracket's
        // Undo_AddBrushList covers nothing — add each touched brush by hand,
        // BEFORE the first mutation (kiwi_command.h's protocol note).
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

        // §19: rebuild, check, and roll the baseline back when the check fails.
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

        // §19 for the vertex path, which is DIFFERENT from the face/edge path and
        // deliberately so:
        //   * BRUSH VERTS — the gate is Brush_MoveVertex's own.  It clamps the drag
        //     against the neighbour planes, tests Brush_Convex, and REVERTS every
        //     move-face itself when the result is not convex (brush.cpp "Phase 2d"),
        //     returning 0.  Invalid geometry therefore never goes live, and this
        //     layer must not second-guess it: the solver does not call
        //     Brush_BuildWindings, so def->[mins,maxs] are stale, and the triangles
        //     its split/collapse pass emits would trip V4/V5/V6 on healthy brushes.
        //     Only V7-on-stale-bounds would be left, which is worse than nothing —
        //     so the brush-vertex arm runs NO extra check at all.
        //   * PATCH POINTS — Patch_Rebuild does update the bounds, and a runaway
        //     control-point drag is exactly what V7 is for.
        // Either way this only RAISES the flag; there is no planept baseline to roll
        // back to, so commit-while-invalid takes the cancel path and the undo
        // bracket performs the exact restore.
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

        // ─── HUD ────────────────────────────────────────────────────────────
        void UpdateHud()
        {
            const char *what = m_construct               ? "construction"
                             : ( m_kind == SEL_OBJECT ) ? "objects"
                             : ( m_kind == SEL_FACE   ) ? "faces"
                             : ( m_kind == SEL_EDGE   ) ? "edges" : "verts";

            // KIWI-UX (shakeout G): the pivot placement owns the line while it runs.
            if ( m_pivotPlacing )
            {
                UpdatePivotHud();
                return;
            }

            // KIWI-UX (shakeout G): the DELETE warning.  Deliberately not routed
            // through m_invalid — HudInvalid() drives the red numeric HUD and
            // "Commit takes the cancel path", and this state is the opposite of
            // that: confirming is meaningful and is what the user asked for.
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

            // KIWI-UX (shakeout G): with the free drag gone, "started but nothing
            // held" is the state G now OPENS in, so the HUD has to name the two
            // things that can move anything.  Without this G reads as broken.
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
                // KIWI-UX (ROUND BN, ITEM 6): the face push gets the same MAJOR lamp
                // the object move has had since round BL — it is the same lattice
                // now, so it must be the same readout.
                // ── KIWI-UX (ROUND BP, ITEM 2): ABSOLUTE SHOWS BOTH NUMBERS ─────
                // Position first (that is what the cursor is aiming at while Ctrl is
                // down), delta in parentheses so the familiar reading survives.  On a
                // slanted push direction there is no world coordinate to name, so the
                // absolute label falls back to the distance and says so.
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
            // KIWI-UX (ROUND BL, ITEM 4): say when a MAJOR grid line took the pivot.
            // The lock is otherwise only legible as "the number went round", which is
            // exactly the feedback the user said was missing ("really hard to align
            // things") — the snap accents show WHERE, this says WHAT.
            SetHud( "%s  %s  %s, %s, %s%s", what, ConstraintText( cbuf, sizeof( cbuf ) ),
                    bx, by, bz, m_majorLock ? "  ·  MAJOR GRID" : "" );
        }

        // ─── state ──────────────────────────────────────────────────────────
        sel_kind_t m_kind = SEL_OBJECT;
        // KIWI-UX (shakeout F): this gesture is moving CONSTRUCTION objects, not
        // brush geometry.  NOT a fifth sel_kind_t — sel_kind_t is the typed
        // selection's vocabulary and construction geometry is deliberately outside
        // it (kiwi_construct.h scope ruling 1) — so it is a flag that redirects
        // Apply / RestoreAll / Commit and leaves the cursor mapping alone.
        bool       m_construct = false;

        std::vector<faceUnit_t>      m_faces;
        std::vector<edgeUnit_t>      m_edges;
        std::vector<vertUnit_t>      m_verts;
        std::vector<kiwiBaseBrush_t> m_base;

        // ── KIWI-UX (CLEANUP, A-30): THE PER-FRAME LIVENESS STAMP ───────────
        // Sel_BrushLive is a display-list walk, which is exactly why DominantKind
        // takes a `checkLive` parameter rather than sweeping every palette frame.
        // The same sweep used to run per unit, per Recompute, and again per
        // baseline entry inside RestoreAll — O(base x units x brushes) on a big
        // map, on every mouse-move frame of a live drag.
        //
        // So it is asked ONCE, at the TOP of Recompute (exactly where AllLive ran
        // before, so the observation point does not move), and stamped into three
        // byte vectors parallel to the unit vectors above.  The stamp is valid ONLY
        // for the duration of that Recompute call (liveScope_t clears it on every
        // exit path), so RestoreAll and BaseNodeLive read it when they are reached
        // THROUGH Recompute and fall back to the direct Sel_BrushLive test when
        // they are reached from Cancel or Commit — where no stamp was taken and a
        // stale one could put a write through a freed node.
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
        // ROUND BP, ITEM 2: the Ctrl-absolute pair — m_absPrev is the edge detector
        // (the rebase happens on CTRL UP only) and m_absolute is this frame's mode.
        bool  m_absPrev     = false;
        bool  m_absolute    = false;
        float m_scalarBase  = 0.0f;
        float m_scalarStart = 0.0f;
        bool  m_haveMapStart = false;
        bool  m_axisWarned   = false;   // ROUND AI, ITEM 2 — say it once per refusal run
        // KIWI-UX (shakeout G): a move HANDLE is held.  The whole cursor mapping is
        // gated on it — see Recompute.
        bool  m_grabbed      = false;
        // KIWI-UX (ROUND L): the grab-freshness freeze and the pixel it measures
        // against (NoteGrab / AgeGrab / GrabLive), plus whether m_ref is the SESSION
        // PIVOT rather than this kind's natural anchor (PivotRide / LiveAnchor).
        bool  m_grabFresh    = false;
        int   m_grabPixX     = 0;
        int   m_grabPixY     = 0;
        bool  m_pivotOverridden = false;
        // KIWI-UX (ROUND Z, ITEM 4): scratch for PivotAnchor(), which must hand back
        // a POINTER to the LIVE anchor and has nowhere else to compute it.  mutable
        // because PivotAnchor is const and the value is derived, never state.
        mutable float m_liveRef[3] = { 0.0f, 0.0f, 0.0f };
        // KIWI-UX (shakeout G): this frame's push annihilates at least one brush, so
        // a confirm DELETES rather than reshapes.  See RecomputeFace / CommitDelete.
        bool  m_deleting     = false;
        // ── KIWI-UX (ROUND BL, ITEM 4) ──────────────────────────────────────
        // m_pivotRebase: armed by every grab / constraint change, consumed on the
        // first LIVE frame, where it re-bases the owned components so the pivot sits
        // at the cursor's constrained projection (Recompute).
        // m_majorLock:   this frame's lattice pass landed an owned axis on a MAJOR
        // grid line.  HUD only — the geometry is already where it says it is.
        bool  m_pivotRebase  = false;
        bool  m_majorLock    = false;
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  R — whole-selection rotate (§13).
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiRotateCommand : public KiwiXformBase
    {
    public:
        const char *Name() const override { return "Rotate"; }
        bool CanExecute() override { return KiwiXform_CanRotate(); }

        // ── shakeout E: field table, live value, bubble anchor, resume ───────
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
            // KIWI-UX (shakeout G): NOTHING TO RE-LATCH.  This used to re-derive a
            // pixel origin from the current angle, because the free drag mapped
            // horizontal travel to degrees and resuming a paused rotate would
            // otherwise re-apply every pixel the cursor wandered while parked.
            // With the free drag removed (Recompute below) the angle is latched by
            // definition: nothing but a held ring or a typed value can move it, and
            // neither has a cursor-relative origin to re-seat.
        }

        bool Begin() override
        {
            m_deg = m_applied = 0.0f;
            m_axis = 2;                       // Z default (spec §13)
            m_con  = CON_AXIS;                // R is ALWAYS about an axis
            m_hasNum   = false;
            m_invalid  = false;
            m_undoOpen = false;
            m_ringActive = false;             // KIWI-UX (shakeout D): no ring held yet
            m_ringDeg    = 0.0f;
            m_ringBase   = 0.0f;              // KIWI-UX (ROUND L)
            m_ringFresh  = false;

            if ( !SelectionHasObjects() )
            {
                Sys_Printf( "Rotate: no whole objects are selected.\n" );
                return false;
            }
            // The pivot is LATCHED: Select_GetMid re-reads the selection bounds, so
            // calling it per increment would let the pivot crawl as the geometry
            // turns.  The ported rotate-mode nudge latches g_vRotateOrigin the same
            // way (select.cpp NudgeSelection_Apply).
            Select_GetMid( m_pivot );
            // KIWI-UX (shakeout G): …and the SESSION PIVOT overrides it outright.
            // m_pivot is fed to ApplyDelta as rot_around[0] and Select_RotateAxis
            // fills only rows 1..3 (select.cpp:2360-2372 identity-inits `m` =
            // &(*rot_around)[1]), so row 0 IS the rotation centre and overriding it
            // here is the whole plumbing — nothing downstream needs to change.
            m_pivotOverridden = PivotActive( m_pivot );
            m_pivotPlacing    = false;
            UpdateHud();
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            if ( TrackPivot( snap ) )         // shakeout G: V-placement owns the move
                return;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            if ( HandlePivotKey( vk ) )       // shakeout G: V / Esc-while-placing
                return true;
            return HandleAxisKey( vk, mods, false );
        }

        // ── shakeout G: the pivot hooks (KiwiXformBase) ─────────────────────
        bool SupportsPivot() const override { return true; }
        const float *PivotAnchor() const override { return m_pivot; }
        void RefreshHud() override { UpdateHud(); }
        void ApplyPivot( const float p[3] ) override
        {
            // Moving the CENTRE of a rotation that has already turned something is a
            // composition of two different rotations, and the HUD only claims one.
            // Same rule (and same fix) as re-aiming the axis in SetConstraint: undo
            // what the old centre applied, then adopt the new one from zero.
            ApplyDelta( -m_applied );
            m_applied = 0.0f;
            m_deg     = 0.0f;
            Copy3( p, m_pivot );
            m_pivotOverridden = true;
            UpdateHud();
        }

        // ── KIWI-UX (ROUND Z, ITEM 1): G / R / S MAY TAKE THIS OVER ─────────
        // (kiwi_command.h CanSwapTo.)  R has no deselect-on-commit rule and no
        // construction arm, so the only refusal is a live pivot placement — V owns
        // the keyboard for those frames.
        //
        // NOTE that ApplyPivot above already ZEROES the angle when the centre
        // moves, so a rotate's "has it moved" question is the plain one: no
        // double-count of the kind ITEM 4 fixed on the move can arise here.
        bool CanSwapTo( int commandId ) const override
        { (void)commandId; return !m_pivotPlacing; }

        bool GestureMoved() const override
        { return m_undoOpen || m_hasNum || fabsf( m_deg ) > KX_EPS; }

        void Commit() override
        { m_undoOpen = false; m_pivotPlacing = false; g_nUpdateBits = -1; }

        void Cancel() override
        {
            ApplyDelta( -m_applied );         // exact inverse; the bracket also restores
            m_applied = 0.0f;
            m_undoOpen = false;
            m_pivotPlacing = false;
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

        // ── KIWI-UX (shakeout D): the rotate-ring handoff ───────────────────
        // See kiwi_transform.h for why the ring feeds a DEGREE SCALAR rather
        // than driving the rotate itself.  Both are no-ops unless this command
        // is the active one — KiwiXform_* guard that on the way in.
        const float *Pivot() const { return m_pivot; }

        void PresetAxis( int axis )
        {
            if ( axis < 0 || axis > 2 )
                return;
            SetConstraint( CON_AXIS, axis );
            UpdateHud();
        }

        // ── ROUND L: the ring grab, and why the feed changed meaning ─────────
        // The shared handle-grab arm raises this for a rotate ring exactly as it
        // does for a move handle (kiwi_command.h).  R has no cursor mapping to
        // re-latch — Rebase() is deliberately empty — so the only thing a ring grab
        // has to latch is the ANGLE THE RING'S SWEEP IS ADDED TO.
        void HandleGrab( bool held ) override
        {
            if ( !held )
                return;
            m_ringBase  = m_deg;              // the sweep is measured FROM here
            m_ringFresh = true;               // …and the 5° snap waits for real travel
        }

        void RingFeed( bool active, float sweepDegrees )
        {
            // KIWI-UX (shakeout G): the shakeout-E Rebase() on the ring-release
            // edge is gone with the pixel mapping it existed to re-seat.  A release
            // now simply stops feeding: `deg` keeps whatever the ring last swept
            // (Recompute latches m_deg), which is exactly what "releasing pauses
            // the wip rotate" means.
            //
            // KIWI-UX (ROUND L): `sweepDegrees` is the sweep SINCE THE GRAB, not an
            // absolute angle.  It used to be absolute, and kiwi_gizmo.cpp zeroed its
            // accumulator on every grab — so taking hold of a ring a second time fed
            // 0 and ApplyDelta( 0 - m_applied ) spun the selection back to square
            // one.  The base is latched by HandleGrab above and added here, so frame
            // one of any grab feeds exactly the angle that is already applied.
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
            // Re-aiming the axis must undo what the OLD axis applied, or the
            // gesture would be a composition of two rotations instead of the one
            // the HUD claims (apply-from-baseline, rule 1).
            ApplyDelta( -m_applied );
            m_applied = 0.0f;
            m_axis    = axis;
            Recompute();
        }

        // ── KIWI-UX (SHAKEOUT G): R ROTATES NOTHING UNTIL A RING IS GRABBED ───
        //
        // USER REPORT, verbatim: "The rotate tool is bugged.  It needs to not do
        // any rotation at all unless the gizmo is being dragged.  Currently it does
        // a yaw spin with any mouse movement after pressing R."
        //
        // BEFORE.  Two angle sources, and the fallback was always live:
        //     if ( m_ringActive )                       deg = m_ringDeg;
        //     else if ( m_haveStartX && CursorPixels( &x, &y ) )
        //                                               deg = (x - m_startX) * 0.5f;
        // m_startX was latched in Begin() from wherever the cursor happened to be,
        // and MouseMove calls Recompute every frame — so the FIRST pixel of mouse
        // travel after pressing R applied half a degree about Z, with no handle
        // ever touched and no way to hold still enough to avoid it.
        //
        // AFTER.  `deg` is LATCHED (it starts at 0 and keeps its value); only two
        // things can change it, and both are explicit acts:
        //     * a HELD ring feeding its swept angle (KiwiXform_FeedRotateDegrees),
        //     * a TYPED value (the numeric layer).
        // Mouse movement with nothing held is therefore a true no-op: Recompute
        // still runs (the HUD and the snap state refresh), ApplyDelta gets a delta
        // of exactly zero and returns before it can open an undo bracket.
        //
        // The 5° snap now applies only to a ring drag.  Typed degrees were already
        // exempt, and re-snapping a LATCHED angle would be a no-op at best and, for
        // an angle a ring left off-increment because snapping was suppressed, an
        // unrequested quantisation of a value the user is no longer touching.
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
            if ( !SelectionHasObjects() )
                return;
            OpenUndo( "rotate selection" );

            // The canonical ported pattern (mainfrm.cpp Radiant_RotateSelection /
            // select.cpp NudgeSelection_Apply): pivot into row 0, Select_RotateAxis
            // fills rows 1..3, then the whole-selection apply.
            float rot_around[4][3];
            Copy3( m_pivot, rot_around[0] );
            Select_RotateAxis( m_axis, delta, (float (*)[4][3])rot_around );
            Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], delta, 0 );
            m_applied += delta;
            g_nUpdateBits = -1;
        }

        void UpdateHud()
        {
            // KIWI-UX (shakeout G): the pivot placement owns the line while it runs.
            if ( m_pivotPlacing )
            {
                UpdatePivotHud();
                return;
            }
            // KIWI-UX (shakeout G): with the free drag gone, "nothing has happened
            // yet" is a real and common state, and the HUD has to say what to do
            // about it — otherwise R now looks broken in the opposite direction.
            if ( !m_ringActive && !m_hasNum && fabsf( m_deg ) <= KX_EPS )
            {
                SetHud( "objects  axis %s  grab a ring / type degrees%s", AxisName( m_axis ),
                        m_pivotOverridden ? "  [pivot moved]" : "  (V moves the pivot)" );
                return;
            }
            SetHud( "objects  axis %s  %.1f deg%s", AxisName( m_axis ), (double)m_deg,
                    m_pivotOverridden ? "  [pivot moved]" : "" );
        }

        float m_pivot[3] = { 0.0f, 0.0f, 0.0f };
        float m_deg      = 0.0f;
        float m_applied  = 0.0f;
        bool  m_ringActive = false;      // KIWI-UX (shakeout D): a ring is held
        float m_ringDeg    = 0.0f;       // …and this is the angle it wants
        // KIWI-UX (ROUND L): the angle the ring's SWEEP is added to, latched on the
        // grab edge, plus the "no travel yet" freeze that holds the 5° snap off.
        float m_ringBase   = 0.0f;
        bool  m_ringFresh  = false;
        // KIWI-UX (shakeout G): the rotation centre is the SESSION PIVOT, not
        // Select_GetMid's.  Kept as its own flag rather than re-asking PivotActive
        // per frame so a pivot placed mid-gesture (which stores it AND sets this) is
        // drawn immediately, before any selection re-signature can be observed.
        bool  m_pivotOverridden = false;
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  S — whole-selection scale (§13).
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiScaleCommand : public KiwiXformBase
    {
    public:
        const char *Name() const override { return "Scale"; }
        bool CanExecute() override { return KiwiXform_CanScale(); }

        // ── shakeout E: field table, live value, bubble anchor, resume ───────
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
            // Same re-latch as R, against S's own 1 + dx*k mapping.
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

            if ( !SelectionHasObjects() )
            {
                Sys_Printf( "Scale: no whole objects are selected.\n" );
                return false;
            }
            // Reported for the HUD only — Select_Scale computes its OWN pivot with
            // Select_GetMid on every call (select.cpp 0x48FDC0), and that is left
            // alone rather than second-guessed.  See the DEVIATION note in the
            // final report: a per-call pivot means a long scale gesture can crawl
            // by up to one legacy grid cell.
            Select_GetMid( m_pivot );
            UpdateHud();
            return true;
        }

        // ── KIWI-UX (ROUND Z, ITEM 1): G / R / S MAY TAKE THIS OVER ─────────
        // (kiwi_command.h CanSwapTo.)  S has no pivot placement, no construction
        // arm and no deselect-on-commit, so it can always yield.
        bool CanSwapTo( int commandId ) const override
        { (void)commandId; return true; }

        bool GestureMoved() const override
        { return m_undoOpen || m_hasNum || fabsf( m_factor - 1.0f ) > KX_EPS; }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            // KIWI-UX (CLEANUP, A-23): S has no snap arm — the mapping is a pure
            // 1 + dx*k in pixels (kiwi_transform.h), so the base's m_snap is not
            // latched here; nothing in this class would read it.
            (void)pick;
            (void)snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            return HandleAxisKey( vk, mods, false );
        }

        void Commit() override { m_undoOpen = false; g_nUpdateBits = -1; }

        void Cancel() override
        {
            ApplyWanted( 1.0f, 1.0f, 1.0f );
            m_undoOpen = false;
            g_nUpdateBits = -1;
        }

        void DrawWorld() override
        {
            DrawConstraint( m_pivot, m_pivot, m_pivot );
        }

    protected:
        void SetConstraint( constraint_t con, int axis ) override
        {
            // HandleAxisKey already decided lock-vs-release; CON_FREE arrives on the
            // second press of the SAME axis and means "back to uniform".  (A plane
            // lock has no meaning for a scale, so CON_PLANE is simply ignored.)
            if ( con == CON_FREE )
            {
                m_con = CON_FREE;
                Recompute();
                return;
            }
            if ( con != CON_AXIS )
                return;
            m_con  = CON_AXIS;
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
                for ( int k = 0; k < 3; ++k )
                    if ( k != m_axis )
                        want[k] = 1.0f;

            ApplyWanted( want[0], want[1], want[2] );
            UpdateHud();
        }

    private:
        // Select_Scale multiplies, so the residual is a RATIO — that is what keeps
        // the gesture apply-from-baseline instead of compounding.
        void ApplyWanted( float sx, float sy, float sz )
        {
            const float want[3] = { sx, sy, sz };
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
            Select_Scale( ratio[0], ratio[1], ratio[2] );
            m_applied[0] = want[0]; m_applied[1] = want[1]; m_applied[2] = want[2];
            g_nUpdateBits = -1;
        }

        void UpdateHud()
        {
            if ( m_con == CON_AXIS )
                SetHud( "objects  axis %s  x%.3f", AxisName( m_axis ), (double)m_factor );
            else
                SetHud( "objects  uniform  x%.3f", (double)m_factor );
        }

        float m_pivot[3]   = { 0.0f, 0.0f, 0.0f };
        float m_factor     = 1.0f;
        float m_applied[3] = { 1.0f, 1.0f, 1.0f };
        int   m_startX     = 0;
        int   m_dummyY     = 0;
        bool  m_haveStartX = false;
    };

    KiwiMoveCommand   s_move;
    KiwiRotateCommand s_rotate;
    KiwiScaleCommand  s_scale;
}

// ─── §3 canExecute predicates ────────────────────────────────────────────────
bool KiwiXform_CanMove()
{
    sel_kind_t k;
    if ( DominantKind( &k, false ) )       // palette-rate: skip the liveness walk
        return true;
    // KIWI-UX (shakeout F): G is also live on a PURE construction selection.
    return KiwiConSel_CanMove();
}

bool KiwiXform_CanRotate()
{
    return SelectionHasObjects();
}

bool KiwiXform_CanScale()
{
    return SelectionHasObjects();
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiXform_RegisterCommands()
{
    // Unbound here: these are the CLASSIC-profile bindings, and the modern profile
    // (kiwi_keymap.cpp) is what puts them on G / R / S.
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

// ─── §14 gizmo handoff (shakeout A) ─────────────────────────────────────────
bool KiwiXform_DominantKind( sel_kind_t *out )
{
    if ( !out )
        return false;
    return DominantKind( out );                 // liveness-checked (the Begin-rate path)
}

void KiwiXform_PresetMoveConstraint( int con, int axis )
{
    // Guarded, not asserted: the ONLY legal caller is the gizmo, immediately after
    // a successful KiwiCmd_Start(KIWI_CMD_MOVE).  If anything else is active the
    // request is silently dropped rather than aimed at the wrong command.
    if ( KiwiCmd_Active() != &s_move )
        return;
    s_move.PresetConstraint( con, axis );
}

// ─── shakeout D: the gizmos as modal chrome (kiwi_transform.h) ───────────────
// Every one of these is the SAME identity guard KiwiXform_PresetMoveConstraint
// already used, so the gizmo can never aim a preset at the wrong command and can
// never read a pivot that belongs to a gesture that has ended.
bool KiwiXform_IsMoveActive()
{
    return KiwiCmd_Active() == &s_move;
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
        // KIWI-UX (ROUND L): the LIVE anchor, i.e. the latched reference point PLUS
        // whatever this gesture has applied so far — "the gizmo doesn't move with
        // the object.  Fix this."  The MAPPING still measures from the latched
        // m_ref (KiwiMoveCommand::MapCursor); only the drawing and hit-testing
        // anchor rides.  See KiwiMoveCommand::LiveAnchor for the full argument.
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

// ─── shakeout G: the movable pivot ──────────────────────────────────────────
// KIWI-UX (CLEANUP, A-35): RULE — the grab gate is raised ONLY through the
// framework's KiwiEditorCommand::HandleGrab, the one arm every handle shares; no
// file-specific entry point for one command's gate.
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

// ── KIWI-UX (ROUND BK, ITEM 6b): "WHERE COULD I SNAP TO?" ───────────────────
// USER DIRECTIVE, verbatim: *"Also when hunting for a pivot point, the obvious
// spots (centers, corners, points, midways, etc.) need to have a black dot to show
// where they are."*
//
// The dots already exist and already enumerate EXACTLY the snap set
// (KiwiSnap_DrawFaceAccents, kiwi_snap.cpp — winding corners = arm 2, edge
// midpoints = arm 4, the face centroid = arm 4b).  What they did not have was a
// reason to be drawn during a TRANSFORM: their gate is `cmd->WantsClicks()`, which
// is the placement tools and nothing else, and round Y's note says why that gate
// is deliberately tight ("forty dots on every face during every drag gesture").
//
// So the gate is widened by exactly the two states the directive names, and the
// predicate lives HERE because both are this file's own state:
//   * PIVOT PLACEMENT (V) — "hunting for a pivot point", word for word;
//   * a HELD HANDLE — the gizmo drag itself, where the snap query is live and the
//     user is aiming at a corner to land on.
// A PARKED gesture with nothing held draws none, so the editor is not permanently
// speckled the way round Y refused to make it.
bool KiwiXform_WantsSnapDots()
{
    if ( KiwiCmd_Active() == &s_move )
        return s_move.PivotPlacingNow() || s_move.HandleHeld();
    // ROTATE has no HandleHeld twin: its grab latch is the RING drag, which lives
    // in KiwiRotateCommand's own state and aims at an ANGLE, not at a point — so
    // only its pivot placement earns the dots.
    if ( KiwiCmd_Active() == &s_rotate )
        return s_rotate.PivotPlacingNow();
    return false;
}

bool KiwiXform_PivotPlacing()
{
    // The two commands that can be placing one are the two that support it, and
    // both keep the flag in the shared base — but the flag is protected, so the
    // question is answered through the same identity guard rather than by widening
    // the class's interface for a bool.
    if ( KiwiCmd_Active() == &s_move )
        return s_move.PivotPlacingNow();
    if ( KiwiCmd_Active() == &s_rotate )
        return s_rotate.PivotPlacingNow();
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND Q — THE FACE PUSH, EXPORTED AS A ONE-SHOT (kiwi_transform.h §Q)
//
//  USER DIRECTIVE, verbatim: "Make it so that I can un-extrude faces entirely to
//  DESTROY them.  This is allowed in plasticity."
//
//  E on a face grows a NEW BODY, and it refused a negative distance outright
//  (kiwi_extrude.cpp Commit: "distance must be positive").  That refusal is what
//  the directive is about: pulling the lollipop back through the brush went RED
//  and confirming did nothing, so there was no way to un-extrude anything.
//  Negative E now means "carve the SOURCE face inward", which is push/pull —
//  including push/pull's DELETE state once the carve annihilates the solid.
//
//  It is wired here rather than reimplemented over there ON PURPOSE.  The delete
//  rule (FaceDepthAlong), the §19 gate, the texture-lock bracket and the exact
//  Select_Deselect → head → Undo_AddEntity_W → Select_Brush → Select_Delete order
//  are all subtle and all already correct in KiwiMoveCommand; a second copy in
//  kiwi_extrude.cpp is precisely the duplicate-function drift this codebase keeps
//  getting bitten by.  This is that code, factored, with the interactive command
//  left untouched.
//
//  DIFFERENCES FROM THE INTERACTIVE PUSH, stated:
//    * ONE face, not a set — the E gesture has exactly one source face.
//    * No baseline vector, because there is no per-frame re-apply: the caller
//      previews and this runs once, at commit, from the face as it stands.
//    * The undo bracket is opened HERE, around the whole thing, and closed by the
//      framework's KiwiCmd_UndoCommit — the same shape every other commit uses.
// ═════════════════════════════════════════════════════════════════════════════
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

    // The baseline this call rolls back to on rejection — planepts AND the texdef
    // block, exactly the pair KiwiMoveCommand::faceUnit_t carries and for exactly
    // the same reason (the texture lock REWRITES the texdef, so restoring the
    // points alone would leave a reprojected texture behind).
    float normal[3];
    float basePts[9];
    byte  baseMtl[sizeof( MaterialDef ) * 4];
    Copy3( f->plane.normal, normal );
    memcpy( basePts, &f->planepts[0][0], sizeof( basePts ) );
    memcpy( baseMtl, &f->mtldef[0],      sizeof( baseMtl ) );

    // ── the DELETE arm, tested FIRST ────────────────────────────────────────
    // Same threshold as the interactive push (RecomputeFace): the plane's travel
    // along its own baseline normal is `dist`, and once that is inward by more
    // than the brush's thickness along that normal the half-space intersection is
    // empty — there is no brush left to reshape, only one to remove.
    const float depth = FaceDepthAlong( def, normal, basePts );
    if ( depth > KX_EPS && dist <= -depth + KX_EPS )
    {
        // The CommitDelete order, mirrored step for step (its own numbered note
        // above spells out why each step is where it is).  The one difference is
        // that nothing has been applied yet, so there is no RestoreAll to run.
        Select_Deselect( 1 );                     // the list must be EMPTY at head

        KiwiCmd_UndoBegin( undoOp );              // Undo_ClearRedo + GeneralStart +
                                                  // AddBrushList(&selected_brushes)
        KiwiCmd_UndoCoverBrush( node );                   // …which covers nothing for a FACE
                                                  // selection, so the brush by hand
        // AFTER the brush: undo.cpp warns when brushes are added to a record that
        // already carries entities.  `node->owner->def` is the entity DEF, cast —
        // the EXACT argument Cmd_OnSelectionDelete feeds and the thing
        // Undo_AddEntity_W compares against `(entity_s *)world_entity->def`.
        if ( node->owner && node->owner->def )
            Undo_AddEntity_W( (entity_s *)node->owner->def );

        Select_Brush( node, 0, 0, 0 );
        Select_Delete();                          // the CLASSIC core, unmodified
        Sel_Clear( KiwiSel() );

        *outWhy = "pushed through";
        g_nUpdateBits = -1;
        return KXPUSH_DELETED;
    }

    // ── the ordinary carve/push ─────────────────────────────────────────────
    KiwiCmd_UndoBegin( undoOp );
    KiwiCmd_UndoCoverBrush( node );          // the same per-brush cover OpenUndoForBrushes
                                     // adds, because a FACE selection is not on
                                     // selected_brushes (kiwi_selection.h NOTE 2)

    byte lockFlags[3];
    lockFlags[0] = (byte)( g_PrefsDlg->m_bTextureLock  != 0 );
    lockFlags[1] = (byte)( g_PrefsDlg->m_bLightmapLock != 0 );
    lockFlags[2] = 1;
    float saveBuf[19];

    // Brush_Move's exact bracket, once, over the baseline (ApplyFaces' body for a
    // single face): Face_MakePlane FIRST, because Face_TexLock_Save builds the
    // world tex matrix from face->plane.normal and that must describe the points
    // being pushed, not a previous rebuild's.
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
        // §19: never leave a rejected edit live.  The record is CANCELLED rather
        // than committed, so a refused carve leaves nothing on the undo stack —
        // the same shape KiwiMoveCommand::Commit's invalid arm takes.
        memcpy( &f->mtldef[0],      baseMtl, sizeof( baseMtl ) );
        memcpy( &f->planepts[0][0], basePts, sizeof( basePts ) );
        KiwiValid_Rebuild( def );
        KiwiCmd_UndoCancel();
        *outWhy = why ? why : "invalid geometry";
        g_nUpdateBits = -1;
        return KXPUSH_FAILED;
    }

    // ── KIWI-UX (ROUND AO, ITEM 2): THE FILLETS FOLLOW THE SURFACE ──────────
    // USER REPORT, verbatim: "When extending a surface that has fillets, the
    // fillets also need to stretch with the surface."
    //
    // AFTER the §19 gate, so a REFUSED push never moves a patch, and INSIDE the
    // bracket opened above, so the brush and its fillets are one undo record.
    // kiwi_patchfillet.h carries the association design (geometric, not stored —
    // it is the only one that is still correct after a map reload) and the one
    // case it refuses rather than approximating.
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
