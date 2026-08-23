#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_patchfillet.cpp — ROUND Q implementation.  See kiwi_patchfillet.h for the
// fillet derivation (axis, tangent points, chamfer depth), the bezier control
// points and their error bound, the general-angle handling and the two refusals,
// the deliberate "patch lands at commit" deviation, the undo argument read out of
// undo.cpp, and the B-key context ruling.
//
// NEW code over the ported cores.  It computes no plane and no winding: the
// chamfer goes through kiwi_bevel.h's exported appender (which writes THREE
// PLANEPTS and lets Face_MakePlane do the plane), the rebuild goes through
// KiwiValid_Rebuild, and the patch is created by the ported allocator / material
// tail / linker in the ported order.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "winding.h"

#include "kiwi_patchfillet.h"
#include "kiwi_bevel.h"                 // the chamfer frame + appender (ROUND Q exports)
#include "kiwi_camera.h"                // ROUND AI, ITEM 2 — KCAM_RAYAXIS_MIN_DEN
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_lines.h"
#include "kiwi_material.h"              // ROUND T — R6, the patch material realize
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its DEFINITION) ──────────────
extern int          Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118
extern int          g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)

extern unsigned int Brush_RemoveFace( brush_t *b, unsigned int faceIndex );   // brush.cpp:345  0x471640
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );         // brush.cpp:669  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                        // brush.cpp:927  0x4765A0
extern void         Select_Deselect( int a1 );                                // select.cpp:1444 0x48E800 (int, NOT char — mangling)

// pmesh.cpp — the patch allocator, the KIWI-UX material/tessellation tail, and
// the symbiont-brush maker.  MakeNewPatch and AddBrushForPatch are the binary's
// own (0x437AC0 / 0x4386A0); Patch_KiwiFinishNew is this round's forwarder for
// the two file-static workers Patch_BrushToMesh runs between them.
extern patchMesh_t *MakeNewPatch();                                           // pmesh.cpp:136  0x437AC0
extern brush_t     *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );  // pmesh.cpp:840  0x4386A0
extern void         Patch_KiwiFinishNew( patchMesh_t *p );                    // pmesh.cpp:1558 (ROUND Q)
// ROUND AJ, ITEM 2 — the same tail with the naturalize scale taken from the
// PARENT FACE's texdef instead of the editor default (pmesh.cpp:1508).
extern void         Patch_KiwiFinishNewLike( patchMesh_t *p, const texdef_sub_t *srcTex );
// ROUND AM (mid-round directive) — the Surface Inspector's "Lmap" alignment, as a
// per-patch forwarder onto the ported Patch_Lightmap_Texturing_Sub (0x4397B0), so
// the fillet can run it inside its OWN undo bracket.  pmesh.cpp carries the whole
// argument for why the forwarder exists rather than a call to the public spelling.
extern void         Patch_KiwiLmapAlign( patchMesh_t *p );                    // pmesh.cpp (ROUND AM)
extern void         Patch_KiwiCapAlign( patchMesh_t *p );                     // pmesh.cpp (ROUND AN)
// ROUND AO, ITEM 2 — the ported post-mutation bookkeeping for a control-point
// edit (bounds -> Brush_RebuildBrush -> curveDef -> ++version).  Declared exactly
// as kiwi_transform.cpp:57 declares it, which is the other site that moves them.
extern void         Patch_Rebuild( patchMesh_t *p, char doBounds );           // pmesh.cpp:2137

// undo.cpp — the bracket head KiwiCmd_UndoBegin does not cover for an EDGE
// selection (kiwi_selection.h DESIGN NOTE 2: edges are not on selected_brushes).
extern void         Undo_AddBrush( entity_brush_s *pBrushInst );              // undo.cpp:494  0x45E680

// KIWI-UX (CLEANUP, B-28): FILE SCOPE, not block scope.  Round AI shipped a link
// error from a block-scope extern that MSVC mangled with its enclosing namespace;
// kiwi_uv.cpp carries the full account.  This is the declaration that used to sit
// inside KiwiPatchFillet_RegisterCommands.
extern bool         Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                             int commandId );                 // mainfrm.cpp:1340

namespace
{
    const float KPF_EPS       = 1.0e-4f;
    const float KPF_MATCH_TOL = 0.1f;      // the ported FindPoint dedup tolerance
    const float KPF_PI        = 3.14159265358979f;

    // KIWI-UX (CLEANUP, B-11): KPF_MAX_SPANS is the format bound expressed as
    // spans, exactly as KLOFT_MAX_CURVE_SEGS is.  Bind it so the two cannot part.
    static_assert( KPF_MAX_SPANS * 2 + 1 == KPATCH_MAX_WIDTH,
                   "KPF_MAX_SPANS must be (KPATCH_MAX_WIDTH - 1) / 2" );

    const float KPF_COL_OK [3] = { 0.55f, 0.95f, 0.80f };   // the arc preview
    const float KPF_COL_BAD[3] = { 1.00f, 0.30f, 0.25f };
    const float KPF_COL_EDGE[3]= { 0.45f, 0.62f, 0.72f };   // the edges being filleted

    // ── local predicate over the shared vec helpers (kiwi_vec.h) ────────────
    inline bool PointNear( const float *a, const float *b, float tol )
    {
        float d[3];
        Sub3( a, b, d );
        return fabsf( d[0] ) <= tol && fabsf( d[1] ) <= tol && fabsf( d[2] ) <= tol;
    }

    // KIWI-UX (CLEANUP, RayAxis): the local copy is gone — it was one of four
    // byte-identical bodies.  It is KiwiCam_RayAxis (kiwi_camera.h) now, beside
    // the KCAM_RAYAXIS_MIN_DEN gate every copy already cited.

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

    // KIWI-UX (CLEANUP, UndoCoverBrush): the local copy is gone — this was one
    // of five verbatim bodies.  It is KiwiCmd_UndoCoverBrush (kiwi_command.h)
    // now, beside the bracket whose blind spot it exists to fill.

    // ── shakeout E: the numeric FIELD tables.  STATIC storage — the numeric layer
    //    copies the struct but never the label.
    //
    // ── ROUND T: TWO TABLES, ONE PER MODE ───────────────────────────────────
    // CHAMFER carries a second, Tab-reachable ANGLE field — the user directive's
    // "allow angle adjustments".  FILLET carries only the radius, and the reason
    // is geometric rather than a shortcut: the fillet arc must be TANGENT TO BOTH
    // FACES, which pins its axis on the bisector (kiwi_patchfillet.h THE AXIS).
    // Tilt the plane and the tangent solution stops being a CIRCULAR arc and
    // becomes an ellipse, which the biquadratic-bezier construction in this file
    // is not derived for and would silently approximate wrongly.  So the bias is
    // REFUSED in fillet mode, out loud, rather than accepted and ignored.
    //
    // Tab: with two fields the numeric layer cycles them; with one it offers Tab
    // to the command, which does not use it (kiwi_split.h TAB ROUTING).  So Tab
    // reaches the bias exactly when there is a bias to reach.
    const kiwiNumField_t KPF_FIELDS_CHAMFER[2] = { { "depth",  KNUM_LENGTH, false },
                                                   { "bias",   KNUM_ANGLE,  false } };
    const kiwiNumField_t KPF_FIELDS_FILLET [1] = { { "radius", KNUM_LENGTH, false } };

    // The bias field's guard band: the chamfer plane must stay strictly between
    // the two faces, so |bias| is clamped this far inside the half-angle.
    const float KPF_BIAS_GUARD_DEG = 2.0f;

    // ═════════════════════════════════════════════════════════════════════════
    //  ONE FILLETED EDGE
    // ═════════════════════════════════════════════════════════════════════════
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

        // ROUND T: which way "positive bias" tilts.  kiwi_bevel.h defines the
        // rotation in the +v sense; whether +v points toward adj[0] or adj[1]
        // depends on the order MakeFrame happened to find them in, so the sign is
        // resolved ONCE, per unit, from the normals — and then "positive means
        // toward the second face" is true for every edge in the gesture.
        float biasSign;
        float biasLimit;            // radians; |bias| must stay under this
    };

    // The cross-section control point for column `col` of `u`, at radius `r`,
    // taken at the EDGE MIDPOINT.  kiwi_patchfillet.h derives both formulae.
    //   even col 2i    →  A + r·m(i·alpha)
    //   odd  col 2i+1  →  A + (r/cos(alpha/2))·m((i+0.5)·alpha)
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

    // ═════════════════════════════════════════════════════════════════════════
    //  §Q  FILLET EDGE (PATCH)
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiPatchFilletCommand : public KiwiEditorCommand
    {
    public:
        // ROUND T: ONE tool, two modes.  The name follows the mode so the HUD, the
        // hint strip and Repeat Last all say what is actually running.
        const char *Name() const override
        { return m_curve ? "Fillet Edge (patch)" : "Bevel Edge (chamfer)"; }
        bool CanExecute() override { return KiwiPatchFillet_CanFillet(); }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        // Never snap or pick the geometry the gesture is reshaping (kiwi_pick.h).
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        // ── ROUND T: the tool's own keys, on the prompt strip ────────────────
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            // ROUND AJ, ITEM 6: the BALL chip leads both sets.  With the tool parked
            // at entry it is the only thing that makes anything happen, so it is
            // the first thing the strip has to say.
            static const kiwiPrompt_t s_chamfer[] = {
                { "Ball", "Drag it — chamfer depth" },
                { "D",    "Curve it (patch fillet)" },
                { "Tab",  "Angle bias" },
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
                // Degrees are NOT a length: undo the numeric layer's unconditional
                // inches→world conversion to recover exactly what was typed, the
                // same thing kiwi_construct.cpp's bearing field and R's angle do
                // (kiwi_numeric.h "KIND IS A DISPLAY FACT").
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

        // ── ROUND T: D TOGGLES CHAMFER <-> FILLET, MID-GESTURE ──────────────
        // USER DIRECTIVE, verbatim: "The default bevel mode should be chamfer,
        // make it a Curve(Fillet) if (D) is pressed during the operation of the
        // tool."
        //
        // The SOLID does not jump across the toggle: the two modes drive the same
        // chamfer through different scalars, related by kiwi_patchfillet.h's
        // d = r(1-k²)/k, so the conversion is that identity read in whichever
        // direction the toggle went.  What changes is what the confirm LANDS (a
        // bare chamfer, or a chamfer plus the patch) and what the preview draws.
        //
        // The framework offers KeyDown to the active command before anything else
        // sees it (kiwi_command.cpp's ladder), so D is free here whatever it means
        // outside a gesture.
        bool KeyDown( int vk, unsigned mods ) override
        {
            if ( vk != 0x44 || mods )               // 'D', no modifiers
                return false;
            if ( m_units.empty() )
                return true;

            const filletUnit_t &drv = m_units[0];
            if ( !m_curve )
            {
                // chamfer -> fillet: r = d·k / (1 - k²), the inverse of the depth
                // formula.  A bias in force is DROPPED, with a reason: a tilted
                // plane has no tangent circular arc (see the field tables above).
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
            // The FIELD TABLE changes with the mode (two fields for the chamfer,
            // one for the fillet), so it has to be re-installed — and the typed
            // scalar meant the OTHER mode's quantity, so carrying it across would
            // silently re-interpret the user's number.
            //
            // KiwiNum_SetFields, NOT KiwiNum_Reset: Reset reinstalls the DEFAULT
            // single unnamed field and throws the command's own table away, which
            // kiwi_numeric.h calls out by name as the bug ClearEntry exists to make
            // unwritable.  SetFields clears the text and the focus as part of its
            // contract, which is exactly what a mode change wants.
            m_hasNum = false;
            {
                const kiwiNumField_t *f = 0;
                const int nf = NumericFields( &f );
                KiwiNum_SetFields( f, nf );
            }

            // ── AND RE-LATCH THE DRAG ORIGIN, or the conversion above is a lie ──
            // Recompute derives the scalar from the CURSOR every frame, as
            // `raw - m_start`, so without this the cursor would immediately
            // overwrite the converted value with the old one under a new meaning
            // and the solid WOULD jump.  Solving `raw - m_start == wanted` for
            // m_start is what makes the toggle continuous: the same cursor
            // position now produces the converted scalar, and moving from there
            // carries on smoothly in the new mode's units.
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

        // ═══════════════════════════════════════════════════════════════════
        //  ROUND AJ, ITEM 6 — NOTHING MOVES UNTIL THE BALL IS TAKEN HOLD OF
        // ═══════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: "in edge bevel mode, dont do operations with
        // the mouse automatically.  Add a lollipop that controls the amount of
        // deformation that must be grabbed."
        //
        // THE LOLLIPOP ALREADY EXISTED (round Y, item 7 — LollipopHandle above).
        // What did not is a GATE: the command opened HOT, so the very first
        // MouseMove after B ran Recompute, which mapped the cursor through RayAxis
        // and appended a chamfer face at whatever depth the cursor happened to
        // express.  The ball was drawn on geometry that had already deformed.
        //
        // TWO INDEPENDENT HALVES, and both are needed:
        //
        //   1. THE COMMAND PARKS AT ENTRY.  KiwiCmd_Pause from inside Begin() is
        //      legal and deliberate — KiwiCmd_Start sets `g_activeCommand = cmd`
        //      BEFORE calling Begin (kiwi_command.cpp:1260, "set BEFORE Begin:
        //      Begin may query Active()") and s_hot is already true, so the pause
        //      takes.  A PAUSED command is not fed MouseMove at all.
        //   2. THE COMMAND'S OWN GRAB GATE (m_grabbed).  Parking alone is not
        //      enough, because shakeout E's resume rule is deliberately the
        //      broadest one — ANY LMB press inside the camera image resumes — so a
        //      stray click anywhere would un-park the gesture and hand it back to
        //      the cursor.  With the gate, a resumed-but-ungrabbed command still
        //      holds its scalar: the ONLY things that move it are the ball and a
        //      typed number.  This is exactly the shape kiwi_transform.cpp's Move
        //      already uses (m_grabbed / GrabLive, raised by the same shared arm).
        //
        // KiwiCmd_HandleGrab (kiwi_command.h) does the rest for free: it resumes,
        // re-latches at the press pixel (Rebase → ReLatchFor, so the chamfer does
        // not jump by however far the cursor wandered since B), raises HandleGrab,
        // re-latches again and feeds frame one with a delta of exactly zero.
        // KiwiLollipop_Release then PAUSES the gesture again, so letting go parks
        // it.  RMB / Enter confirm and Esc cancel are untouched.
        void HandleGrab( bool held ) override
        {
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
            // ROUND AJ, ITEM 6: PARK, at zero deformation.  See HandleGrab above.
            KiwiCmd_Pause();
            // ROUND T: the gesture ALWAYS opens as a flat chamfer, which is the
            // directive's "the default bevel mode should be chamfer".
            Sys_Printf( "Bevel Edge: %i edge(s), parked at 0.  GRAB THE BALL on the "
                        "stem and drag to cut the chamfer, or just type a depth.  "
                        "D curves it into a patch fillet, Tab reaches the angle bias, "
                        "RMB / Enter confirms, Esc cancels.\n", (int)m_units.size() );
            return true;
        }

        void Commit() override
        {
            if ( m_invalid || m_depth < KPF_MIN_RADIUS )
            {
                // §19: never commit invalid geometry.  Strip the chamfers, then take
                // the CANCEL path so the gesture leaves nothing behind at all.
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
                // CHAMFER: the solid is already exactly right — Apply() has been
                // maintaining it live since the first mutation — so the commit is
                // the absence of a rollback.  This is what KiwiBevelCommand's
                // Commit did, and it is why the two commands could merge at all.
                Sys_Printf( "Bevelled %i edge(s) at %g%s.\n", (int)m_units.size(),
                            (double)m_depth,
                            ( fabsf( m_biasDeg ) > 1.0e-3f ) ? " (biased)" : "" );
                Reset();
                g_nUpdateBits = -1;
                return;
            }

            const int made = LandPatches();
            // ROUND AJ, ITEM 2: three patches per edge now — the arc plus the two
            // end caps that seal it against the chamfer face (see LandCaps).
            // KIWI-UX (CLEANUP, B-39): and the skips are named, in the shape
            // Gather() already uses in this file, so a count lower than 3x the edge
            // count has a reason attached rather than being read as a design choice.
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

        // ═══════════════════════════════════════════════════════════════════
        //  ROUND Y, ITEM 7 — THE BEVEL GETS THE LOLLIPOP
        // ═══════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: "Edge bevel mode needs a gizmo/lollipop.
        // Hard to tell when where it starts/beings and goes out of range."
        //
        // This is a one-degree-of-freedom gesture measured along a bisector —
        // exactly the class kiwi_lollipop.h was written for ("face push/pull,
        // region extrude and face extrude … the gestures with ONE degree of
        // freedom") — and it was the only one of the four without a handle.
        // Nothing is reinvented: answering LollipopHandle is the whole contract
        // (kiwi_command.h:608), and kiwi_lollipop.cpp then owns the ring, the
        // stem, the ball, the hit test, the grab and the gizmo stand-down.
        //
        // THE TWO RULES THE HEADER STATES, applied to a chamfer:
        //   * IT RIDES THE LIVE GEOMETRY.  The anchor is the DRIVING unit's
        //     chamfer-face midpoint AT THE CURRENT DEPTH — mid - biasedNormal *
        //     depth, which is the same point DrawWorld's bisector stub already
        //     ends at (`tip`, below).  So the ball sits on the face the drag is
        //     moving and cannot be swallowed by it.
        //   * IT POINTS THE WAY THE DRAG GOES.  The chamfer travels along
        //     -biasedNormal as the depth grows, so that is the direction; the
        //     depth is clamped non-negative in Recompute, so unlike a push/pull
        //     there is no sign to fold in.
        //
        // The DRIVING unit is m_units[0] — the same one LatchStart, ReLatchFor
        // and Recompute already measure against, so the handle and the number
        // can never disagree about which corner the gesture is about.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_units.empty() || !outAnchor || !outDir )
                return false;
            float nb[3];
            KiwiBevel_BiasedNormal( m_units[0].e, nb );
            for ( int k = 0; k < 3; ++k )
            {
                outAnchor[k] = m_units[0].e.mid[k] - nb[k] * m_depth;
                // ── KIWI-UX (ROUND AM, ITEM 6): THE STEM POINTS OUTWARD ─────
                // USER DIRECTIVE, verbatim: "the lollipop for the bevel command
                // needs to be on the other side (180 flip it)."
                //
                // The anchor is right and does not move — it is the chamfer
                // face's midpoint at the CURRENT depth, so the handle still rides
                // the live geometry (the first of kiwi_lollipop.h's two rules).
                // The DIRECTION was the second rule applied too literally: "it
                // points the way the drag goes" put the stem along -n, i.e. INTO
                // the solid, so kiwi_lollipop.cpp's ball — which sits at
                // anchor + dir * KLOL_STEM_PIX (42 px) — was 42 px inside the
                // brush being chamfered, buried in exactly the geometry the
                // gesture is cutting.  Outward puts it in the open air in front
                // of the chamfer face, which is where the user is looking.
                //
                // NOTHING ELSE READS THIS DIRECTION.  kiwi_lollipop.cpp uses it
                // for the tip, the perpendicular basis and the ball's hit test
                // only (BuildGeo); the depth itself comes from the command's own
                // cursor mapping, which is untouched.  So the gesture, its sign
                // and its numbers are exactly what they were — this moves a
                // handle and nothing else.
                outDir[k]    = nb[k];
            }
            return true;
        }

        // Taking hold of the ball must not make the chamfer jump by however far
        // the cursor wandered since B was pressed.  ReLatchFor is exactly that
        // re-latch and already exists for the D toggle: it sets m_start so THIS
        // cursor position means the value the shape currently has.
        void Rebase() override
        {
            if ( m_units.empty() )
                return;
            ReLatchFor( m_curve ? m_radius : m_depth );
        }

        void DrawWorld() override
        {
            if ( m_units.empty() )
                return;

            // The edges being filleted, so the user can see WHICH corners the
            // gesture owns even when the chamfer is hidden behind the brush.
            KiwiLines_Color( KPF_COL_EDGE[0], KPF_COL_EDGE[1], KPF_COL_EDGE[2] );
            for ( size_t i = 0; i < m_units.size(); ++i )
                if ( !KiwiLines_Add( m_units[i].e.e0, m_units[i].e.e1 ) )
                    return;

            if ( m_depth < KPF_MIN_RADIUS )
                return;

            // ── ROUND T: CHAMFER MODE DRAWS THE CHAMFER, NOT AN ARC ─────────
            // The chamfer FACE is live geometry and the ordinary brush draw already
            // shows it, so all this adds is the bisector stub that says how deep
            // the cut is and which way the bias has tilted it — KiwiBevelCommand's
            // own preview, moved here with the rest of it.
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

            // THE ARC, EVALUATED — this is the patch, not a sketch of it.  The
            // bezier is walked at the same control points LandPatches will write,
            // so what is on screen during the drag is what lands.
            const float *col = m_invalid ? KPF_COL_BAD : KPF_COL_OK;
            KiwiLines_Color( col[0], col[1], col[2] );
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                const filletUnit_t &u = m_units[i];
                float lo[3], hi[3];
                RowEnds( u, lo, hi );

                // One polyline per END of the patch, which is what reads as a
                // quarter-cylinder in wireframe without spending the whole budget.
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

        // ROUND AJ, ITEM 6: "the ball has never been held and no number has been
        // typed, and nothing has been cut" — the tool's rest state.
        bool Parked() const
        {
            return !m_grabbed && !m_hasNum && m_depth < KPF_MIN_RADIUS;
        }

        void Reset()
        {
            m_units.clear();
            m_radius    = 0.0f;
            m_depth     = 0.0f;
            m_curve     = false;        // ROUND T: chamfer is the default, every time
            m_biasDeg   = 0.0f;
            m_hasBias   = false;
            m_start     = 0.0f;
            m_haveStart = false;
            m_hasNum    = false;
            m_grabbed   = false;        // ROUND AJ, ITEM 6 — the grab gate
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

        // ── selection → units ───────────────────────────────────────────────
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

                // THE DEDUP (kiwi_bevel.cpp's rule, lifted): one physical edge is
                // TWO (face,edge) items and the two faces wind in opposite
                // directions, so segments are compared UNORDERED at the ported
                // 0.1-unit tolerance.
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

        // The arc's fixed part: k, phi, the plane basis, the span count and the
        // row direction.  Everything here is independent of the radius, so it is
        // computed ONCE from the baseline and never re-derived under the drag.
        //   0 = ok, 1 = too flat, 2 = too sharp, 3 = degenerate basis.
        int DeriveArc( filletUnit_t &u )
        {
            const brush_t *def = u.e.def;
            Copy3( def->faces[u.e.adj[0]].plane.normal, u.n1 );
            Copy3( def->faces[u.e.adj[1]].plane.normal, u.n2 );

            float c = Dot3( u.n1, u.n2 );
            if ( c >  1.0f ) c =  1.0f;
            if ( c < -1.0f ) c = -1.0f;
            u.phi = acosf( c );                       // between the OUTWARD normals

            // k = n·n1 = cos(phi/2) = sin(theta/2).  Read off the frame rather than
            // recomputed from c: n is already the normalised bisector, so this is
            // the same number with one fewer square root of round-off in it.
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

            // basis: a = n1, b = normalise(n2 - c·n1).  Both are ⟂ u (the edge lies
            // in both faces), so m(psi) sweeps the arc in the cross-section plane.
            Copy3( u.n1, u.basisA );
            for ( int k = 0; k < 3; ++k )
                u.basisB[k] = u.n2[k] - c * u.n1[k];
            if ( !Norm3( u.basisB ) )
                return 3;

            // SPANS.  Enough that no span exceeds KPF_SPAN_DEG of arc, capped by the
            // patch format's column ceiling.  See the error table in the header.
            const float phiDeg = u.phi * ( 180.0f / KPF_PI );
            int spans = (int)ceilf( phiDeg / KPF_SPAN_DEG );
            if ( spans < 1 )             spans = 1;
            if ( spans > KPF_MAX_SPANS )  spans = KPF_MAX_SPANS;
            u.spans = spans;

            // ROW DIRECTION.  A patch control point's normal is built from summed
            // cross products of neighbour directions in (col,row) index space, and
            // pmesh.cpp's s_curveNeighbors table walks those CLOCKWISE from +row —
            // which makes each term proportional to cross(dCol, dRow)
            // (Curve_ComputeNormals, pmesh.cpp:502/609: cr = dirs[n+1] x dirs[n]).
            // So the surface faces cross(dCol, dRow), and OUTWARD means that has to
            // agree with the bisector n.
            //
            // dCol at the middle of the arc is the tangent there,
            //     tau = -sin(phi/2)·a + cos(phi/2)·b,
            // and dRow is ±u.  Test the sign once and remember which way the rows
            // have to run; the patch writer honours it.
            float tau[3], nrm[3];
            const float sh = sinf( u.phi * 0.5f ), ch = cosf( u.phi * 0.5f );
            for ( int k = 0; k < 3; ++k )
                tau[k] = -sh * u.basisA[k] + ch * u.basisB[k];
            Cross3( tau, u.e.u, nrm );
            u.rowFlip = ( Dot3( nrm, u.e.n ) < 0.0f );

            // ROUND T: the bias frame.  n1 and n2 straddle the bisector in the
            // (n, v) plane, so exactly one of them has a positive v-component;
            // pointing the sign at n2 makes "+bias" mean "toward the second
            // face" on every edge regardless of the order they were found in.
            u.biasSign  = ( Dot3( u.n2, u.e.v ) >= 0.0f ) ? 1.0f : -1.0f;
            u.biasLimit = KiwiBevel_BiasLimit( u.e );
            return 0;
        }

        // ── the drag ────────────────────────────────────────────────────────
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

        // ROUND T: re-latch so THIS cursor position means `wanted`.  Recompute's
        // mapping is `s = raw - m_start`, so `m_start = raw - wanted`.  Used by the
        // D toggle, which changes what the scalar MEANS without wanting the shape
        // to move.  A cursor that cannot be resolved leaves the latch alone — the
        // next MouseMove re-derives it anyway.
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

            // ROUND Q's rule, the same one §25's bevel now follows: the RAW
            // closest-point scalar, full float, NOTHING quantises it.  A fillet
            // radius that could only take whole grid values would be the same
            // defect the directive is about, one command over.
            //
            // ROUND T: the scalar the drag produces is a DEPTH along the bisector,
            // and that is the number chamfer mode wants directly.  Fillet mode
            // reads the same scalar as a RADIUS, which is exactly what round Q did.
            float s = m_curve ? m_radius : m_depth;
            ray_t ray;
            float p[3];
            const filletUnit_t &drv = m_units[0];
            // ROUND AJ, ITEM 6: THE GATE.  The cursor mapping runs only while the
            // lollipop's ball is HELD.  Without a grab the scalar is whatever it
            // already was — zero at entry, or the last dragged / typed value — and
            // the whole gesture is inert.  Numeric entry (m_hasNum, below) is
            // deliberately OUTSIDE the gate: it never went through the cursor.
            if ( m_grabbed
              && m_haveStart && CursorRay( &ray ) && KiwiCam_RayAxis( ray, drv.e.mid, drv.e.n, p ) )
            {
                float rel[3];
                Sub3( p, drv.e.mid, rel );
                s = -Dot3( rel, drv.e.n ) - m_start;
            }

            if ( m_hasNum )
                s = m_numWorld;
            // ROUND AJ, ITEM 6: the geometry-snap arm is cursor-driven too (it
            // answers "what is under the pointer"), so it sits behind the same gate
            // as the drag.  Snapping the chamfer out to a wall is something you do
            // WHILE dragging, not something a parked tool does on its own.
            else if ( m_grabbed && m_snap.valid && KiwiSnap_IsGeometry( m_snap.type ) )
            {
                // Round UP TO the thing under the cursor: its depth along the
                // driving bisector.  In FILLET mode that depth is converted back to
                // a radius by inverting d = r(1-k²)/k; in CHAMFER mode the depth IS
                // the scalar and no conversion happens at all.
                //
                // ── KIWI-UX (ROUND AA, ITEM 5): THE DEPTH IS READ OFF THE PLANE ──
                // USER DIRECTIVE, verbatim: "the split tool is still vulnerable to
                // the variable flat face non-planar behavior that was fixed with
                // extrusions.  Fix this and in other spots too.  Make basically all
                // operations like the new extrusion, it works good."
                //
                // This was the last surviving copy of round Z's hand-rolled
                // `dot( snap.position - ref, axis )` on a gesture that is genuinely
                // one-axis: the drag itself is KiwiCam_RayAxis( ray, drv.e.mid, drv.e.n )
                // just above, and the latch uses the same axis.  Arm 6 (SNAP_FACE)
                // is live here — this is a drag command, not a plane-placement tool
                // (kiwi_snap.cpp gates arm 6 on toolActive), and PICKF_EXCLUDE_SELECTED
                // hides only the selected brush, so "chamfer this edge out to that
                // wall" reaches it — and SNAP_FACE's position is the SLIDING
                // ray-surface hit.  So the chamfer depth changed with where on the
                // target face the cursor happened to be.  KiwiSnap_AxisDepth is the
                // shared rule (§56.3): plane INTERSECT axis, one scalar per face.
                //
                // TWO THINGS TO KEEP STRAIGHT HERE.  (1) THE SIGN: this file's
                // convention is inward-positive, hence the leading `-` on the old
                // expression and on the drag above; AxisDepth answers along +axis,
                // so the negation moves outside it.  (2) THE REFUSAL: a
                // near-parallel face now returns false, and the caller contract is
                // "leave the distance alone" — which for this branch means keeping
                // the value the DRAG produced above, not falling through to zero.
                // That is why the assignment is inside the `if` instead of the
                // branch unconditionally overwriting `s` the way it used to.
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

        // ── ROUND T: push the user's bias onto every unit's frame ────────────
        // Per unit, because the clamp is per unit: the guard band is a fraction of
        // THAT corner's wedge, so a gesture spanning a 90° corner and a 150° one
        // biases each as far as it legally can rather than as far as the tightest
        // one allows.  Fillet mode forces 0 — the arc has no biased form (see the
        // field tables).
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

        // ── the per-frame rebuild (kiwi_patchfillet.h THE GESTURE) ──────────
        void Apply()
        {
            if ( m_depth < KPF_MIN_RADIUS )
            {
                if ( m_added )
                {
                    RemoveAll();
                    RebuildAll();
                }
                // ROUND AJ, ITEM 6: a PARKED tool is not an INVALID one.  Before the
                // ball has been taken hold of the depth is legitimately zero, and
                // painting the HUD red and shouting "depth too small" at a tool
                // that has not been asked to do anything yet would be a false
                // alarm on every single entry.  Once the user has grabbed (or
                // typed), zero means what it always meant.
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
                // ROUND T: ONE depth for both modes.  Fillet mode has already put
                // ChamferDepth(u, m_radius) into m_depth — but only for the DRIVING
                // unit, and k differs per corner, so the per-unit depth is still
                // derived here exactly as round Q did.
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
            // Reverse order: each def's appended faces are contiguous at the tail
            // in add order, so removing backwards always removes a tail face and
            // never shifts an index this loop still needs.
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

        // Edge selections are NOT on selected_brushes, so the bracket's
        // Undo_AddBrushList covers nothing — add each touched brush by hand,
        // BEFORE the first mutation (kiwi_command.h's protocol note).
        void OpenUndoForBrushes()
        {
            if ( m_undoOpen )
                return;
            // ROUND T: ONE literal for both modes — D can flip the mode AFTER the
            // record is open, and a name that lied about which half ran would be
            // worse than one that names the tool.  (Undo_GeneralStart stores the
            // POINTER, so it must be a literal either way.)
            KiwiCmd_UndoBegin( "bevel / fillet edge" );
            m_undoOpen = true;
            for ( size_t i = 0; i < m_units.size(); ++i )
                if ( DefFirstIndex( i ) == i )
                    KiwiCmd_UndoCoverBrush( m_units[i].e.node );
        }

        // ── the patch: where its rows sit along the edge ────────────────────
        // The CHAMFER FACE's own winding, projected onto the edge direction, so the
        // patch covers exactly the notch that was cut rather than the whole original
        // edge (they differ whenever the chamfer plane clips other geometry).
        // Falls back to the edge itself when the winding is not available.
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

        // ═══════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AM, ITEM 5) — THE HAIRLINE ON EACH SIDE
        // ═══════════════════════════════════════════════════════════════════
        // USER REPORT, verbatim: "The Curve is leaking a small gap on each side.
        // Try to fix the spacing here. so it fits great."
        //
        // WHERE THE SEAM IS, AND WHERE IT IS NOT.  The two candidate causes were
        // worked out on paper before anything moved:
        //
        //   (a) BEZIER SAG AT MID-SPAN.  Checked, and it is the wrong sign AND the
        //       wrong place.  A patch column triple (2i, 2i+1, 2i+2) is a QUADRATIC
        //       bezier, ArcPoint puts the odd handle at r/cos(alpha/2) — the tangent
        //       intersection, which is stock Radiant's own cylinder construction —
        //       and the resulting parabola's mid-span radius is
        //           r * (1 + cos^2(a/2)) / (2 cos(a/2))   >   r
        //       i.e. it bulges OUTWARD, never inward (equality only at a = 0, by
        //       AM-GM).  At KPF_SPAN_DEG the excess is a fraction of a percent of r
        //       and it is at MID-SPAN, not at the sides.  A gap "on each side" is
        //       not this, and widening the patch by a sagitta would make the mid of
        //       every span worse to fix an error that is not there.
        //
        //   (b) THE RAILS ARE NOT WHERE THE DERIVATION ASSUMED.  In the IDEAL
        //       geometry they are exact: column 0 sits at A + r*n1 with
        //       A = mid - n*(r/k), so its offset from the edge is r*(n1 - n/k),
        //       whose dot with n1 is -r*(k/k) + r = 0 (ON face 0's plane) and whose
        //       dot with n is -r/k + r*k = -r(1-k^2)/k = -ChamferDepth (ON the
        //       chamfer plane).  So in theory the rail lands exactly on the line
        //       where the chamfer meets its neighbour — the seam is closed by
        //       construction, and that is why three rounds of arc math could not
        //       find anything wrong with it.
        //
        //       THE ARC IS BUILT FROM THE IDEAL DEPTH.  The BRUSH's chamfer face is
        //       not: KiwiBevel_AppendFace lays three planepts down and the brush
        //       rebuild re-derives the plane from them (and snaps them when the
        //       preference says to), so the face that actually exists after the
        //       commit is a few thousandths away from the plane the radius was
        //       solved against.  A few thousandths at the rail IS a hairline, on
        //       BOTH sides, exactly as reported — and RowEnds already conceded the
        //       principle for the OTHER axis, reading the real winding to decide
        //       how far along the edge the patch runs while the cross-section kept
        //       trusting the ideal.
        //
        // SO THE FIX IS THE SAME RULE APPLIED TO THE CROSS-SECTION: at commit,
        // measure the rails off the geometry THAT EXISTS.  Each rail is the point
        // on the intersection LINE of the actual chamfer plane and the actual
        // adjacent plane nearest the ideal one, and the radius is re-solved from
        // it through the identity above,
        //       |rail - mid| = r * |n1 - n/k| = r * sqrt(1 - k^2) / k,
        // (the middle term expands to 1 - 2(n1.n)/k + 1/k^2 = 1/k^2 - 1).  The two
        // sides are solved independently and averaged, then the two rail COLUMNS
        // are written exactly rather than through ArcPoint — so the seam closes to
        // float precision on both sides even when the two disagree, and the
        // interior columns keep one symmetric arc.
        //
        // Returns false when anything is unavailable or degenerate, in which case
        // the caller uses the ideal radius and this round changes nothing.
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

                // The ideal rail this is correcting, as the seed point.
                float seed[3];
                ArcPoint( u, m_radius, ( side == 0 ) ? 0 : ( u.spans * 2 ), seed );

                // Project the seed onto plane_a AND plane_c at once: solve
                // seed + a*na + b*nc for the two plane equations.  The 2x2 Gram
                // matrix is [[1, g],[g, 1]] with g = na.nc, invertible whenever the
                // two planes are not parallel — which they are not, or there would
                // be no chamfer.
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

                // Strip the ALONG-EDGE component: the row loop adds the row's own
                // point back in, so the cross-section must carry none of its own
                // (this is the same split RowEnds owns the other half of).
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
            // A refit that disagrees with the drag by more than a hair is not a
            // rebuild artefact — it is a sign the assumptions above do not hold for
            // this corner (a re-planed face, a non-manifold neighbour).  Refuse it
            // and land the ideal arc rather than silently reshaping the fillet.
            if ( !( rFit > KPF_MIN_RADIUS )
              || fabsf( rFit - m_radius ) > m_radius * 0.05f )
                return false;
            if ( rOut )
                *rOut = rFit;
            return true;
        }

        // ROUND AM, ITEM 5: ONE cross-section rule, so the arc patch and the two
        // CAP patches cannot disagree about where the profile is.  They were two
        // copies of `ArcPoint( u, m_radius, col, cs )`; refitting one and not the
        // other would have closed the rail seam and opened an END seam in its
        // place, which is the same bug moved.
        void CrossSection( const filletUnit_t &u, float r, bool refit,
                           const float rail0[3], const float railN[3],
                           int col, int width, float *out ) const
        {
            if ( refit && col == 0 )                { Copy3( rail0, out ); return; }
            if ( refit && col == width - 1 )        { Copy3( railN, out ); return; }
            ArcPoint( u, r, col, out );
        }

        // The quadratic bezier chain evaluated at t in [0,1] over the WHOLE arc —
        // preview only.  Span s covers t in [s/spans, (s+1)/spans]; inside a span
        // the standard B(x) = (1-x)²P0 + 2x(1-x)P1 + x²P2 over the three control
        // points ArcPoint gives for columns 2s, 2s+1, 2s+2.
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

        // ═══════════════════════════════════════════════════════════════════
        //  THE COMMIT: one patch per filleted edge, INSIDE the open bracket
        //
        //  The creation sequence mirrors Patch_BrushToMesh (pmesh.cpp:1291-1461),
        //  which is the Curve→Bevel / Cylinder / End-cap backend, step for step:
        //     MakeNewPatch                     pmesh.cpp:136   (alloc + defaults)
        //     width/height/type                pmesh.cpp:1291-1293
        //     fill ctrl[col][row].xyz          pmesh.cpp:1295-1393
        //     inherit the source materials     pmesh.cpp:1395-1396
        //     Patch_KiwiFinishNew              pmesh.cpp:1490  (ROUND Q) == :1409-1432
        //     AddBrushForPatch(p, owner->def)  pmesh.cpp:1446   (bbox brush + LINK)
        //     Brush_AddToList(pdef, owner)     pmesh.cpp:1447   (instance + patch_t)
        //     land it                          pmesh.cpp:1448-1461
        //
        //  TWO DELIBERATE DIFFERENCES FROM THAT FUNCTION, both stated:
        //    1. it lands on selected_brushes (Brush_AddToList2) rather than being
        //       spliced into active_brushes and then Select_Brush'd.  That is
        //       kiwi_extrude.cpp's LandDef tail, the Ed_NewBrushDrag order, and it
        //       is what puts the new patch where Undo_EndBrushList will find it —
        //       which is the ONLY thing that makes undo remove it.  (The ported
        //       function's `if (&active_brushes == &selected_brushes)` arm is a
        //       comparison of two distinct globals and can never be true.)
        //    2. it does NOT Select_Delete the source.  Curve→Bevel CONSUMES the
        //       brush it was built from; this fillet keeps it — the whole point is
        //       a chamfered solid with a patch in the notch.
        // ═══════════════════════════════════════════════════════════════════
        int LandPatches()
        {
            // ── KIWI-UX (CLEANUP, B-20): THE RULE, STATED ONCE ──────────────
            // THIS is the site the other five point at.  DESELECT BEFORE anything
            // lands: a creating gesture opens its undo bracket over an EMPTY
            // selection, so the record covers exactly what the gesture creates and
            // nothing it merely started from.  kiwi_loft.cpp (x2), kiwi_bevel.cpp
            // and kiwi_primitive.cpp (x2) all follow it and all point here.
            //
            // The PRACTICE is the rule; the exact undo MECHANISM behind it is not
            // restated here, because the audit found the usual one-line telling of
            // it ("Undo_EndBrushList stamps everything on selected_brushes as
            // 'added by this record', so a pre-existing brush would be DELETED by
            // the undo") incomplete: Undo_AddBrush also stamps `ownerPrev`, and a
            // brush the record COVERED is restored despite the stamp.  The
            // stamp-vs-cover interaction is written up in RADIANT_KNOWN_ISSUES.
            // Do not relax a Select_Deselect on the strength of either reading.
            Select_Deselect( 1 );

            m_skips = landSkips_t();          // KIWI-UX (CLEANUP, B-39)

            int made = 0;
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                const filletUnit_t &u = m_units[i];
                if ( !Sel_BrushLive( u.e.node ) || u.faceIndex < 0 )
                {
                    ++m_skips.deadBrush;      // KIWI-UX (CLEANUP, B-39)
                    continue;
                }
                entity_s *owner = u.e.node->owner;
                if ( !owner || !owner->def )
                {
                    ++m_skips.deadBrush;      // KIWI-UX (CLEANUP, B-39)
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
                    ++m_skips.alloc;          // KIWI-UX (CLEANUP, B-39)
                    continue;
                }
                p->width  = u.spans * 2 + 1;      // <= KPATCH_MAX_WIDTH (CLEANUP, B-11)
                p->height = KPF_PATCH_ROWS;
                p->type   = PATCH_BEVEL;          // what this IS, and what the Curve
                                                  // menu's own quarter-cylinder uses

                // ── KIWI-UX (ROUND T): INHERIT FROM THE **SOURCE** FACE ──────
                // WAS: `src = &u.e.def->faces[u.faceIndex]` — the CHAMFER face this
                // gesture had just created.  That is one indirection too many: the
                // chamfer's own material came from kiwi_bevel's srcFace, which was
                // hardcoded to adj[0], so a caulk neighbour produced a caulk chamfer
                // and then a caulk PATCH — created, selectable, and drawing nothing.
                // That is the "fillets are being created invisible" report end to
                // end (the whole chain is written out in kiwi_material.h).
                //
                // NOW: the patch copies from `u.e.srcFace`, which kiwi_bevel.h R5
                // has already chosen to be an INHERITABLE face, and the copy is
                // REALIZED below (R6).  The chamfer wears the same material, so the
                // solid and the surface over it agree by construction rather than
                // by one copying the other.
                const int   srcFi = ( u.e.srcFace >= 0 && u.e.srcFace < u.e.def->faceCount )
                                  ? u.e.srcFace : u.faceIndex;
                const face_t *src = &u.e.def->faces[srcFi];

                // MakeNewPatch leaves `flags` at whatever operator new returned (it
                // sets contents but not flags — pmesh.cpp:136-157), which every
                // ported creator then copies onto the bbox brush's faces inside
                // AddBrushForPatch.  Seeded from the source face here, deliberately:
                // it is both the fix for that and the §26 "carry per-face material
                // through every new op" rule.
                p->contents = src->contents;
                p->flags    = src->toolflags;

                // The cross-section, translated to each row.  It is CONSTANT along
                // the edge (both faces contain the edge, so the wedge is the same at
                // every point of it), so three rows — both ends and the exact
                // midpoint — describe the surface with no approximation at all.
                // ── KIWI-UX (ROUND AM, ITEM 5): THE RAILS COME FROM THE BRUSH ──
                // The cross-section is re-solved against the chamfer face that
                // ACTUALLY exists, and the two rail columns are written exactly.
                // RefitRails carries the whole derivation and the refusal rule; a
                // false answer means the ideal arc lands, i.e. round AJ's shape.
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

                // Inherit the source face's material, exactly as Patch_BrushToMesh
                // inherits the source brush's (pmesh.cpp:1395-1396) — same cast,
                // same two layers.
                p->texture  = *(patchMesh_material *)&src->mtldef[0].lyrMtl;
                p->lightmap = *(patchMesh_material *)&src->mtldef[1].lyrMtl;

                // ── KIWI-UX (ROUND T): AND **REALIZE** IT.  R6, THE OTHER HALF ──
                // A MaterialDef copied by value carries {lyrMtl, radMtl} as
                // POINTERS, and lyrMtl can be a DEGENERATE handle (the name at
                // offset 0, layerCount == 0) minted by MakeDegenerateLayerMtl while
                // the renderer was down or a prefab was loading (materialdef.cpp:72
                // / :103).  Copy one of those onto a patch and the draw path does
                // this, silently:
                //     Patch_BuildInstanceVisuals (pmesh.cpp:9903)
                //         MaterialDef_11(&p->texture) == 0        (zero layers)
                //         -> visCount = 0, visArray = NULL
                //     DrawPatches (pmesh.cpp:9985) -> `return false` before it emits
                // i.e. THE PATCH IS CREATED AND NEVER DRAWN.  Patch_BuildInstanceVisuals
                // does call Materialdef_Realize itself, but only on the CURRENT EDIT
                // LAYER's channel and only once the renderer is up — realizing here,
                // on all three channels, at creation time, is the belt that makes
                // the inherited material a REAL material before anything asks.
                KiwiMtl_RealizePatch( p );

                // ── …AND A COPIED CHANNEL CAN BE EMPTY, NOT ONLY UNREALIZED ──
                // The two lines above copy the source face's channel 0 and 1
                // POINTER PAIRS verbatim, so a source whose lightmap channel is
                // missing or unnamed hands this patch the same hole — and a patch
                // with a dead lightmap channel is invisible in the Shift+L render
                // method AND compiles with no baked light or sun shadow at all.
                // The fallback is MakeNewPatch's own default triple, so a repaired
                // patch is exactly what a fresh one would have carried.  No-op on
                // a sound copy, which is every ordinary edge.
                KiwiMtl_EnsurePatchChannels( p );   // kiwi_material.h:250

                // ── KIWI-UX (ROUND AJ, ITEM 2): NATURALIZE AT THE PARENT'S SCALE ──
                // USER DIRECTIVE, verbatim: "Also the Texture should be fixed so it
                // looks natural with the parent solid edge area."  The MATERIAL was
                // already inherited (round T, above); what was not was the texel
                // DENSITY — Patch_KiwiFinishNew hardcoded the editor's default
                // sample size, so a patch grown out of a rescaled wall came out at a
                // different repeat length and the join read as a texture change.
                // The full derivation is on Patch_KiwiFinishNewLike (pmesh.cpp).
                Patch_KiwiFinishNewLike( p, &src->mtldef[0].mat_texDef );

                // ── KIWI-UX (ROUND AM): …AND THEN "Lmap" ────────────────────
                // USER DIRECTIVE, verbatim: "Please set the texture alignment to
                // 'lmap' when doing a fillet curve (for all parts).  It's the
                // setting that makes it lineup perfect."
                //
                // LAST, deliberately: this is the same order the Surface
                // Inspector produces when a user presses Natural and then Lmap,
                // and Lmap is the one the directive says wins.  FinishNewLike is
                // still run first and still matters — it is what seeds layers 1
                // and 2 and the patch's stored sample size, which the lmap pass
                // does not touch, and the parent-density naturalize it does is
                // what layer 0 falls back to if the edit layer is not 0.
                //
                brush_t    *pdef = AddBrushForPatch( p, (entity_s *)owner->def );
                // ROUND AN refinement — USER DIRECTIVE, verbatim: "the main
                // surface needs the 'CAP' button pressed in texture inspector.
                // Do this automatically please."  The ARC gets CAP (the Surface
                // Inspector button's own primitive, button-default 1.0 scale);
                // the two END CAPS in LandCaps keep round AM's lmap.
                //
                // ── ROUND AS (CRASH FIX): AFTER AddBrushForPatch, not before ──
                // USER STACK: Patch_GetAxisFace AV, `b was nullptr` — b is
                // p->pSymbiot.  CAP's reference face IS a face of the symbiont
                // brush (Patch_GetAxisFace / PMESH_03 both read p->pSymbiot),
                // and AddBrushForPatch is what creates it (pmesh.cpp:869).
                // Round AM's lmap primitive never read the symbiont, so the
                // pre-AddBrush position was harmless until AN swapped the arc
                // to CAP.  KIWI-UX (CLEANUP, B-1): the caps in LandCaps call
                // Patch_KiwiLmapAlign BEFORE their AddBrushForPatch (:1731 /
                // :1733), which is safe ONLY because lmap does not read
                // p->pSymbiot (pmesh.cpp:2875-2880 forwards to
                // Patch_Lightmap_Texturing_Sub).  Any CAP-class alignment added
                // there must go BELOW the AddBrushForPatch.
                Patch_KiwiCapAlign( p );
                selbrush_t *inst = Brush_AddToList( pdef, owner );
                Brush_AddToList2( inst );         // → selected_brushes, where the
                                                  // bracket TAIL will stamp it
                ++made;

                // ── KIWI-UX (ROUND AJ, ITEM 2): SEAL THE ENDS ────────────────
                made += LandCaps( u, src, owner, rUse, refit, rail0, railN );
            }

            // The typed selection named EDGES of brushes that have just grown a
            // face; the legacy side now holds the new patches.  Resync so nothing
            // downstream reads a stale winding pointer or a dead edge index.
            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();
            return made;
        }

        // ═══════════════════════════════════════════════════════════════════
        //  ROUND AJ, ITEM 2 — THE THICKEN, AND WHY IT IS TWO PATCHES AND NOT FIVE
        // ═══════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: "When using the Bevel mode on a solid's edge,
        // in fillet mode (with curve), the curve should automatically use the
        // thicken operation (built-in) to fill up the gap between the underlying
        // brush chamfer."
        //
        // WHAT THE GAP ACTUALLY IS, derived rather than eyeballed.  The arc's centre
        // is A = mid - n·(r/k) and its tangent points are T = A + r·n1 and
        // A + r·n2.  The chamfer plane is n·(x - mid) = -d with d = r(1-k²)/k, and
        //     n·(T1 - mid) = -r/k + r·(n·n1) = -r/k + r·k = -r(1-k²)/k = -d,
        // so BOTH TANGENT POINTS LIE EXACTLY ON THE CHAMFER PLANE — and, being the
        // arc's endpoints, they are also on the two original faces, i.e. on the two
        // long edges of the chamfer face's own winding.  The arc therefore meets the
        // solid exactly along both of its long rails and bulges outward between
        // them (its middle sits at r(1-k)/k, nearer the edge than d).  The enclosed
        // volume is a lens whose top is the arc, whose bottom is the chamfer FACE —
        // a real solid surface, already there, already wearing the same material —
        // and which is OPEN ONLY AT ITS TWO ENDS.
        //
        // STOCK THICKEN (Patch_Thicken, pmesh.cpp:7411) makes an offset copy plus up
        // to FOUR seam strips.  Here the offset copy is the brush's own chamfer face
        // and two of the four seams are those tangent rails, so the only seams that
        // are not already solid are seam C and seam D — the `width x 3` end strips.
        // This is that pair, built the way pmesh.cpp:7529-7581 builds them: source
        // profile on row 0, offset profile on row 2, MIDPOINT on row 1.
        //
        // THE CAP IS EXACTLY PLANAR.  Every arc control point is translated along
        // the edge direction u to reach the end, and the offset is a projection
        // ALONG n — and n ⟂ u by construction (KiwiBevel_MakeFrame re-orthogonalises
        // u against n) — so all six-to-thirty control points of a cap lie in one
        // plane perpendicular to u.  That makes the winding test below exact rather
        // than approximate: the cross product of any two independent in-plane
        // directions is parallel to ±u, so comparing it against the outward
        // direction decides the row order with no tolerance at all.  (Stock thicken
        // has to call patchInvert2 on exactly one of its two end seams for the same
        // reason; that function is file-static in pmesh.cpp, and choosing the row
        // ORDER up front is the same act without reaching for it.)
        int LandCaps( const filletUnit_t &u, const face_t *src, entity_s *owner,
                      float rUse, bool refit, const float rail0[3], const float railN[3] )
        {
            float lo[3], hi[3];
            RowEnds( u, lo, hi );

            // The chamfer plane, as a point + the frame's bisector normal.  Fillet
            // mode forces bias 0 (ApplyBias), so e.n IS the chamfer face's normal.
            //
            // ROUND AM, ITEM 5: when the rails were refitted, the plane the cap
            // projects onto has to be the SAME one they were fitted to — the face
            // that actually exists — or the cap's bottom row lands on the ideal
            // plane the arc no longer touches and the hairline simply moves to the
            // ends.  `planePt` is any point of that face's plane; the projection
            // below uses u.e.n, which fillet mode has already forced to be its
            // normal (bias 0), so only the OFFSET is being corrected here.
            const float d = ChamferDepth( u, rUse );
            float planePt[3];
            Mad3( u.e.mid, u.e.n, -d, planePt );
            if ( refit )
                Copy3( rail0, planePt );         // a rail IS on the real chamfer plane

            int made = 0;
            for ( int end = 0; end < 2; ++end )
            {
                const float *at = end ? hi : lo;

                // OUTWARD is the way this end faces: along the edge, away from the
                // middle.  A degenerate half (a chamfer winding that collapsed to
                // one side of the midpoint) has no direction and gets no cap.
                float outward[3];
                Sub3( at, u.e.mid, outward );
                if ( !Norm3( outward ) )
                    continue;

                float off[3];
                Sub3( at, u.e.mid, off );

                const int w = u.spans * 2 + 1;
                if ( w < KPATCH_MIN_WIDTH || w > KPATCH_MAX_WIDTH )   // KIWI-UX (CLEANUP, B-11)
                {
                    ++m_skips.capRange;       // KIWI-UX (CLEANUP, B-39)
                    continue;
                }

                // The two profiles, before the row order is decided.
                // KIWI-UX (CLEANUP, B-11): sized BY the bound the line above
                // tests, so the array cannot be left behind if it moves.
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

                // Row order.  dCol runs first control point to last — that is the
                // chord between the two TANGENT POINTS, which both lie in the
                // chamfer plane, so dCol is perpendicular to n.  dRow runs the arc
                // to its projection, i.e. along n.  The surface faces
                // cross(dCol, dRow) (Curve_ComputeNormals' sense — the same
                // derivation DeriveArc's rowFlip is written against), and since the
                // two are exactly perpendicular that cross product is exactly ±u.
                //
                // dRow IS TAKEN AT THE MIDDLE COLUMN, not at column 0: the arc is
                // TANGENT to the chamfer plane's edges, so at col 0 and col w-1 the
                // point and its projection are the SAME POINT and the difference is
                // zero.  The middle column is where the separation is greatest.
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
                    ++m_skips.alloc;          // KIWI-UX (CLEANUP, B-39)
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

                // The SAME material and the SAME texel density as the arc it caps —
                // it is the same surface turning a corner, and R6's realize applies
                // for the identical reason it applies over there.
                p->texture  = *(patchMesh_material *)&src->mtldef[0].lyrMtl;
                p->lightmap = *(patchMesh_material *)&src->mtldef[1].lyrMtl;
                KiwiMtl_RealizePatch( p );
                // The cap copies the same two channels the arc does, so it needs
                // the same guard for the same reason — see the arc's own call.
                KiwiMtl_EnsurePatchChannels( p );   // kiwi_material.h:250
                Patch_KiwiFinishNewLike( p, &src->mtldef[0].mat_texDef );
                Patch_KiwiLmapAlign( p );         // ROUND AM — "for all parts"

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

            // ROUND AJ, ITEM 6: the PARKED line.  It names the handle by the thing
            // the user can see (the ball on the stem) rather than by "drag", which
            // is precisely the verb that no longer works on its own.
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
                // ROUND T: the bias is reported as the number the user typed AND as
                // the number that was actually applied, because the per-corner clamp
                // can differ from the request and a silent clamp is a lie.
                const float applied = m_units[0].e.bias * m_units[0].biasSign
                                    * ( 180.0f / KPF_PI );
                char bias[48] = { 0 };
                if ( fabsf( m_biasDeg ) > 1.0e-3f || m_hasBias )
                {
                    char appliedText[32];
                    _snprintf( bias, sizeof( bias ), "  bias %s deg%s",
                               KiwiFmt_Num( appliedText, sizeof( appliedText ), applied, 4 ),
                               ( fabsf( applied - m_biasDeg ) > 0.05f ) ? " (clamped)" : "" );
                }
                _snprintf( m_hud, sizeof( m_hud ),
                           "bevel  %i edge(s)  d %s%s  (D: curve it)",
                           (int)m_units.size(), b, bias );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // KIWI-UX (CLEANUP, B-39): the landing paths' skip reasons, counted.  The
        // commit line reports "%i patch(es)" and the user has no way to tell a
        // count lower than 3x the edge count from a design decision, so every bare
        // `continue` in LandPatches / LandCaps now leaves a mark here.  Zeroed at
        // the top of LandPatches; LandCaps adds to the same set.  Reporting only —
        // no early-out changed.
        struct landSkips_t
        {
            int deadBrush = 0;   // the unit's brush or its owner went away mid-gesture
            int capRange  = 0;   // a cap width outside the patch format's 3..15 columns
            int alloc     = 0;   // MakeNewPatch returned NULL
        };
        landSkips_t               m_skips;

        std::vector<filletUnit_t> m_units;
        float                     m_radius    = 0.0f;
        // ROUND T: the merged tool's own state.  m_depth is the chamfer depth in
        // BOTH modes (fillet mode derives it from m_radius), m_curve says which
        // mode is running, m_biasDeg is the user's requested angle in DEGREES.
        float                     m_depth     = 0.0f;
        bool                      m_curve     = false;
        float                     m_biasDeg   = 0.0f;
        bool                      m_hasBias   = false;
        float                     m_start     = 0.0f;
        bool                      m_haveStart = false;
        bool                      m_hasNum    = false;
        // ROUND AJ, ITEM 6: the lollipop's ball is HELD.  Raised by the framework's
        // one shared handle-grab arm (KiwiCmd_HandleGrab), which kiwi_lollipop.cpp
        // calls on the ball's press and KiwiCmd_HandleRelease clears on its release.
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

// ─── §3 canExecute ───────────────────────────────────────────────────────────
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

// ─── the B dispatch (kiwi_patchfillet.h THE B KEY) ───────────────────────────
// KIWI-UX (CLEANUP, B-31): the round-T bevel/fillet merge is written up ONCE, in
// kiwi_bevel.h.  Bare B on a brush edge reaches this command, which starts as a
// flat chamfer and becomes a fillet on D.
int KiwiPatchFillet_ContextB( int commandId )
{
    if ( commandId != KIWI_CMD_FILLET_CURVE )
        return commandId;
    return KiwiPatchFillet_CanFillet() ? KIWI_CMD_FILLET_EDGE : commandId;
}

// ─── registration ────────────────────────────────────────────────────────────
void KiwiPatchFillet_RegisterCommands()
{
    // UNBOUND: B reaches it through KiwiPatchFillet_ContextB, so a second row on
    // 0x42 would be a second answer to the same question.  The palette and the
    // command list still list it by name and can run it directly.
    Radiant_RegisterCommand( "KiwiFilletEdgePatch", 0, 0, KIWI_CMD_FILLET_EDGE );
}

KiwiEditorCommand *KiwiPatchFillet_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_FILLET_EDGE ) ? &s_patchFillet : 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AO, ITEM 2) — THE FILLETS FOLLOW THE SURFACE
// ═════════════════════════════════════════════════════════════════════════════
// kiwi_patchfillet.h carries the whole design: why the association is GEOMETRIC
// rather than stored, the one rule, what it refuses and what it never attempts.
namespace
{
    // ON-PLANE tolerance.  0.01 world units — the editor's own on-plane epsilon
    // (named KBOOL_ONPLANE_EPS in kiwi_boolean.h; KVALID_PLANE_DIST is the same
    // order).  Kept local so this file's tolerance stays its own.  It has to be at
    // least this loose because round AM's finding is that a brush's plane is
    // re-derived from SNAPPED planepts and sits thousandths off the ideal the
    // patch was solved against.
    const float KFIL_ONPLANE_EPS = 0.01f;

    // IN-WINDING tolerance, and it is deliberately LOOSER than the on-plane one.
    // This is the test that stops a NEIGHBOURING brush's fillet being grabbed
    // because it happens to share an infinite plane with the face being pushed;
    // it only has to separate "over this face" from "over some other brush", so
    // half a grid unit of slack costs nothing and covers a rail that sits exactly
    // on the winding's boundary (which every fillet rail does, by construction).
    const float KFIL_INWINDING_EPS = 0.5f;

    // A brush's bounds, grown, against a patch's control hull — the cheap
    // rejection before any per-point work.
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

    // "Is `pt` on this face's plane AND over its winding."  The winding test is
    // the standard convex in-polygon walk: for every edge of the winding, the
    // point must be on the inward side of the edge's plane, where the inward
    // direction is `faceNormal x edgeDir`.  A winding is convex by construction
    // here (it is a half-space intersection), so no ear clip and no winding order
    // assumption beyond the one the editor already guarantees.
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
            // inward = n x e for a CCW winding about n; the sign is fixed below by
            // testing it against the winding's own centroid rather than assumed,
            // so this is order-agnostic.
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

    // The face as it stood BEFORE the push.  The caller has already moved the
    // planepts by the time this runs (the §19 gate has to pass first), so the
    // face's LIVE plane is the new one and the test plane is rebuilt from the
    // arguments rather than read back off the brush.
    const face_t *mf = &def->faces[movedFace];
    face_t before = *mf;                          // a VALUE copy: the winding pointer
                                                  // is borrowed, never freed here
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
            if ( p->width < 3 || p->height < 2
              || p->width > 16 || p->height > 16 )
                continue;
            if ( !PatchNearBrush( p, def, 1.0f ) )
                continue;

            // ── THE ONE RULE, PART 1: which control points are on the plane ──
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
                // Nothing of this patch is on the moved face — but it may still be
                // a fillet on this brush whose CROSS-SECTION the move cuts, which
                // is the case the header says is refused rather than approximated.
                // Detected as "some control points are on the plane but they do not
                // form whole rows".
                bool partial = false;
                for ( int c = 0; c < p->width && !partial; ++c )
                    for ( int r = 0; r < p->height && !partial; ++r )
                        partial = PointOnFace( &before, p->ctrl[c][r].xyz );
                if ( partial )
                    ++skipped;
                continue;
            }

            // ── PART 2: cover it for undo, then move it ─────────────────────
            // The caller's bracket head cloned `selected_brushes`; a patch this
            // gesture happens to touch is not in general on that list, so nothing
            // cloned it and freeing/mutating it without this would make Ctrl+Z
            // restore the brush and leave the fillet where the push put it.  This
            // is kiwi_boolean.cpp's tool-cover rule at a different site, verbatim
            // in shape: Undo_AddBrush INSIDE the already-open record.
            Undo_AddBrush( (entity_brush_s *)b->def );

            for ( int r = 0; r < p->height; ++r )
            {
                if ( !onRow[r] )
                    continue;
                for ( int c = 0; c < p->width; ++c )
                    for ( int k = 0; k < 3; ++k )
                        p->ctrl[c][r].xyz[k] += delta[k];
            }

            // ── PART 3: the interior rows follow ────────────────────────────
            // KPF_PATCH_ROWS's invariant is "both ends and the exact midpoint", and
            // an end that moved while the midpoint did not would leave the patch
            // straight but non-uniformly parameterised — a texture stretch on a
            // surface the whole fillet work exists to make line up.  Interior rows
            // are re-interpolated linearly between the two END rows, which is
            // exactly how LandPatches wrote them.
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

            // The ported post-mutation bookkeeping, ONCE per patch: bounds
            // recompute -> Brush_RebuildBrush -> curveDef re-tessellate ->
            // ++version.  `Patch_Rebuild( p, 1 )` is exactly what
            // Patch_UpdateSelected_0 (pmesh.cpp 0x43D800) runs after translating
            // control points, and it is what kiwi_transform.cpp:2823 already calls
            // at the one other site that moves them.  NOT `bDirty`: that field
            // means "this patch carries an explicit sample size" and gates `size`
            // in Patch_Write (pmesh.cpp:1146/:1418) — setting it here would put a
            // stale number in the .map.
            Patch_Rebuild( p, 1 );
            ++moved;
        }
    }

    if ( outSkipped )
        *outSkipped = skipped;
    return moved;
}
