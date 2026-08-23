#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_dupe.cpp — RADIANT_UX_DESIGN §25 mirror + arrays.  See kiwi_dupe.h for the
// "mirror is already ported" finding, the Clone_Selection reading behind the
// arrays and the ONE-undo-record proof.
//
// NEW code over the ported cores.  Every copy is made by Clone_Selection and every
// copy is transformed by Select_Move / Select_RotateAxis +
// Select_ApplyMatrix_SelectedBrushes — this file owns the input mapping, the
// preview and the undo bracket, nothing else.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s

#include "kiwi_dupe.h"
#include "kiwi_csg.h"                 // KIWI-UX (CLEANUP, B-10): KiwiCsg_BrushUsable
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_conselect.h"        // ROUND K — Shift+D over brush EDGES
#include "kiwi_construct.h"        // ROUND AA, ITEM 8 — the array's line vector
#include "kiwi_grid.h"
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_transform.h"        // ROUND AA, ITEM 8 — KiwiXform_PivotOverride
#include "kiwi_units.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

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
// KIWI-UX (CLEANUP, B-28): FILE SCOPE, not block scope.  Round AI shipped a link
// error from a block-scope extern that MSVC mangled with its enclosing namespace;
// kiwi_uv.cpp carries the full account.  This is the declaration that used to sit
// inside KiwiDupe_RegisterCommands.
extern bool      Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                          int commandId );              // mainfrm.cpp:1340

// ROUND J — the CREATION undo bracket for Duplicate (see kiwi_dupe.h).  Same
// four entry points KiwiCmd_UndoBegin/Commit use (kiwi_command.cpp:49-53), minus
// Undo_AddBrushList, which is exactly the difference.
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
    // ROUND AA, ITEM 8: the picked array line.  Warm, so it never reads as one more
    // ghost — it is the INPUT the ghosts were derived from, not another copy.
    const float KDUP_COL_LINE[3]  = { 1.00f, 0.78f, 0.22f };


    // ── shakeout E: the numeric FIELD table (kiwi_command.h NumericFields) ──
    // STATIC storage — the numeric layer copies the struct but never the label.
    const kiwiNumField_t KARR_FIELDS[1] = { { "count", KNUM_COUNT, false } };

    // ── KIWI-UX (ROUND AA, ITEM 8): the LINEAR table gains a READ-ONLY spacing ─
    // The design decision below is that the COUNT decides the SPACING, so the
    // spacing is a DERIVED quantity the user never types — and a derived quantity
    // the user cannot see is a derived quantity the user cannot trust.  readOnly is
    // exactly the flag for that (kiwi_numeric.h:69-74): the bubble shows the row,
    // Tab skips it, and NumericFieldChanged therefore never fires for field 1 — so
    // the base's "forward field 0 to NumericChanged" body (kiwi_command.h:512-516)
    // is still the whole truth and this command still overrides only NumericChanged.
    //
    // RADIAL keeps its ONE field, byte for byte.  Its step is an angle it already
    // prints in the HUD, and widening a table costs a re-verification of a grammar
    // this item is not touching.
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

    // ── KIWI-UX (ROUND AA, ITEM 8): THE CONSTRUCTION SEGMENT UNDER A PIXEL ───
    // USER REPORT, verbatim: "The linear array tool needs to accept a line to go
    // across.  Doing it by hand is really hard and pivot (V) support needs to be
    // there as well."
    //
    // KIWI-UX (CLEANUP, PickLineAt): the twenty-line scan under this was a
    // DELIBERATE second copy of kiwi_split.cpp's PickLineAt, argued on the
    // grounds that the two tools want different PAYLOADS.  They do — and the
    // payloads are all that differed, so the SCAN moved to
    // KiwiCon_PickSegmentAt (kiwi_construct.h) and this keeps only its own.
    // The "why not KiwiConSel_PickAt" reasoning moved with it.
    bool PickConstructionSegment( int imgX, int imgY, float *outA, float *outB,
                                  int *outObj, int *outSeg )
    {
        return KiwiCon_PickSegmentAt( imgX, imgY, outA, outB, outObj, outSeg, 0 );
    }

    // A brush instance Clone_Selection will actually copy (its own two skips).
    // KIWI-UX (CLEANUP, B-10): the same four tests kiwi_csg.cpp / kiwi_autobool.cpp
    // ran, written with the eclass null-check folded into the owner conjunction —
    // the same truth table.  KiwiCsg_BrushUsable (kiwi_csg.h) is the one spelling.
    inline bool Cloneable( const selbrush_t *b ) { return KiwiCsg_BrushUsable( b ); }

    int CloneableSelectedCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( Cloneable( b ) )
                ++n;
        return n;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  The two array commands (one class, two modes — they share everything but
    //  the per-step transform and the drag mapping).
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiArrayCommand : public KiwiEditorCommand
    {
    public:
        explicit KiwiArrayCommand( bool radial ) : m_radial( radial ) {}

        const char *Name() const override
        {
            return m_radial ? "Array (Radial)" : "Array (Linear)";
        }
        bool CanExecute() override { return KiwiDupe_CanArray(); }

        // KIWI-UX (shakeout E): ONE named COUNT field.  The kind is what stops the
        // bubble printing "12 in" for a copy count (kiwi_numeric.h — kind drives
        // formatting only; the value the command receives is unchanged).
        int NumericFields( const kiwiNumField_t **out ) const override
        {
            // ROUND AA, ITEM 8: linear also shows the DERIVED spacing (read-only).
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
            // ROUND AA, ITEM 8: field 1 is the linear spacing, in RAW WORLD UNITS —
            // which is what KNUM_LENGTH is documented to want (kiwi_command.h:528-532),
            // and what the step vector already is.
            if ( field == 1 && !m_radial )
            {
                *out = Len3( m_offset );
                return true;
            }
            return false;
        }

        // ── KIWI-UX (ROUND AA, ITEM 8): the keys/clicks this command invents ────
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

        // ── KIWI-UX (ROUND AA, ITEM 8): THE LINE PICK ───────────────────────────
        // USER REPORT, verbatim: "The linear array tool needs to accept a line to go
        // across.  Doing it by hand is really hard and pivot (V) support needs to be
        // there as well."
        //
        // WHY PressIntercept AND NOT WantsClicks.  WantsClicks changes a command's
        // WHOLE grammar: it opts out of pausing entirely (kiwi_command.cpp:1549-1558
        // takes the multi-click branch ABOVE the HOT→PAUSED arm) and every press
        // becomes a placed point.  The linear array's existing hand-drag mode is
        // shakeout E's HOT/PAUSED gesture — cursor sets the step, LMB parks it,
        // RMB/Enter confirms, LMB resumes — and this item's brief is that that mode
        // survives "exactly as it is".  Flipping WantsClicks would silently delete
        // the park, which is a regression dressed as a feature.
        //
        // PressIntercept is the hook that exists for precisely this shape, and
        // kiwi_transform.cpp:423-427 already wrote the argument out for the movable
        // pivot: it lets ONE press mean something else "without turning these into
        // WantsClicks multi-click tools — which they are not, and which would cost
        // them the whole HOT/PAUSED grammar shakeout E gave them."  It is the FIRST
        // rung of KiwiCmd_MouseButton (kiwi_command.cpp:1492), above the resume arm,
        // with the press pixel already latched (:1489-1491).
        //
        // THE VETO IS CONDITIONAL, which is the whole reason this works: the press is
        // consumed ONLY when a construction segment is actually under it.  A press on
        // empty space returns false and falls straight through to the park/resume
        // arms it always did, so the two grammars never contend for the same pixel.
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
                Sys_Printf( "Array: select at least one ordinary brush "
                            "(patches and fixed-size entities cannot be cloned).\n" );
                return false;
            }

            // The source bboxes drive the ghost preview.  Capped, and the union is
            // kept so the preview can degrade to one box for the whole selection.
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

            // ── KIWI-UX (ROUND AA, ITEM 8): THE SESSION PIVOT WINS ──────────
            // USER REPORT, verbatim: "…and pivot (V) support needs to be there as
            // well."
            //
            // WHAT V ALREADY IS.  The pivot is not a per-command mode — it is ONE
            // SESSION pivot, deliberately (kiwi_transform.h:319-322: Plasticity's is
            // per-command and "the directive explicitly asks for reuse, so KIWI keeps
            // ONE session pivot in memory").  It is placed with V inside Move or
            // Rotate and it SURVIVES that command's commit, because the reset rule is
            // the SELECTION SIGNATURE and committing a move leaves the same items
            // selected (kiwi_transform.h:325-331).  So "place the pivot, then run the
            // array" already carries the pivot across, and the array's whole job is
            // to READ it instead of overwriting it with the centroid.  That is why
            // this item adds no V key of its own: the placement machinery
            // (BeginPivot/CommitPivot) is file-local to kiwi_transform.cpp and a
            // second copy of it here would be a second pivot, which is exactly the
            // thing that header refused to build.
            //
            // KiwiXform_PivotOverride is SELF-EXPIRING — it re-tests the signature on
            // every read and clears itself when the selection moved on
            // (kiwi_transform.cpp:226-242) — so a stale pivot from some older
            // selection can never reach this line.
            //
            // LATCHED ONCE, like the centroid it replaces, for the reason kiwi_dupe.h
            // gives: m_ref is the radial rotation centre, the drag plane's origin and
            // the bubble anchor, and a reference point that moved mid-gesture would
            // move all three under the user.
            //
            // WHAT THE PIVOT DOES, EXACTLY, so the two modes are not oversold:
            //   RADIAL — it IS the rotation centre (Perform's rot_around[0]).  This is
            //            the headline: "revolve these around THAT corner" was simply
            //            not expressible before.
            //   LINEAR — it is the origin the step is MEASURED FROM.  The drag plane
            //            passes through it (LatchStart / Recompute), so the step is
            //            read at the pivot's depth rather than the centroid's, and
            //            with a line picked it decides WHICH END of the line the
            //            array runs away from (AdoptLine's endpoint ordering).
            // It does NOT teleport the originals onto the pivot.  That was considered
            // and rejected twice over: this item's brief says the copies are
            // "distributed FROM THE SELECTION", and moving the originals would make
            // this a gesture that MODIFIES existing brushes, which needs the
            // Undo_AddBrushList half of the bracket Perform deliberately does not open
            // (kiwi_dupe.h's undo note).  Silently changing the undo semantics of the
            // array to make a pivot read prettier is not a trade worth taking.
            m_pivoted = KiwiXform_PivotOverride( m_ref );
            if ( !m_pivoted )
                Select_GetMid( m_ref );

            // The offset plane's normal, latched for the same reason
            // kiwi_transform.cpp latches it: camera navigation stays live during a
            // modal command, and a live vpn would swing the offset plane.
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
            // A COUNT is not a length: undo the numeric layer's inches→world
            // conversion to recover exactly what the user typed (the same thing
            // kiwi_transform.cpp's R and S do for degrees and factors).
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

            // ── ROUND AA, ITEM 8: THE PICKED LINE, HIGHLIGHTED ───────────────
            // Drawn FIRST, out of the same batch the framework opened for this
            // command (kiwi_command.cpp:1646 KiwiLines_Begin( 288, 2 ) — DrawWorld
            // never opens its own, which is why there is no Begin/Flush here), so
            // the ghost budget below sees the segments this already spent and
            // coarsens itself if it has to.  A picked line is 7 segments: the span
            // plus a 3-axis tick at each end, and the ORIGIN end gets the bigger
            // tick so the direction the array runs is readable without the HUD.
            //
            // Only the COLOUR distinguishes it — KiwiLines_Begin fixes ONE width per
            // batch, so a thicker line would need a second batch and would then draw
            // over/under everything else instead of with it.
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
            // kiwi_lines.h TRAP 1: declare what fits, never spill.  A box is 12
            // segments, an axis cross is 3.
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
            // ROUND AA, ITEM 8: the line and the pivot are per-GESTURE, so they die
            // with it.  (The SESSION pivot itself is untouched — this only forgets
            // that this run of the array read one.)
            m_lineHave  = false;
            m_lineObj   = -1;
            m_lineSeg   = -1;
            m_lineLen   = 0.0f;
            m_linePixHave = false;
            m_pivoted   = false;
        }

        // ── KIWI-UX (ROUND AA, ITEM 8): ADOPT A PICKED SEGMENT AS THE VECTOR ────
        // THE DESIGN DECISION THIS ITEM HAD TO MAKE, stated once and here: with a
        // line picked, THE COUNT DECIDES THE SPACING.  The copies SPAN the line —
        // spacing = length / (count - 1) — rather than a spacing field deciding how
        // many copies fit.
        //
        // PLASTICITY DECIDES THIS, it is not a coin-flip.  Their rectangular array
        // is built on exactly this rule:
        //   * RectangularArrayCommand.ts:40 — the user picks an ENDPOINT and the
        //     factory is fed `array.step1 = step1.length() / (array.num1 - 1)`,
        //     where step1 is (picked point - centroid).  Length over count-1, i.e.
        //     the copies span the picked extent.  Line :35 even names the prompt
        //     "Select endpoint 1", which is the same act as clicking a line here.
        //   * ArrayFactory.ts:143-146 — `distance1` is the SPAN, and its setter is
        //     `step1 = distance1 / (num1 - 1)`; the getter inverts it.  Spacing is a
        //     derived quantity in their model, not a stored one.
        //   * ArrayFactory.ts:119-134 — RectangularArrayFactory's default mode is
        //     `'extent'` (:120), and in that mode changing num1 RE-DERIVES step1 to
        //     preserve distance1 (:130-134).  The alternative they also ship,
        //     `'spacing'`, is the non-default.  So "count decides spacing, extent is
        //     held" is Plasticity's default answer to this exact question.
        // It is also the answer the user's own phrasing asks for — "a line to go
        // ACROSS" is a span, not a direction hint — and the one that fits the tool as
        // built, whose single editable field is already `count` (KARR_FIELDS).  The
        // derived spacing is not hidden: it is the read-only field 1 and it is in the
        // HUD.
        //
        // ENDPOINT ORDER: the array runs AWAY FROM THE REFERENCE POINT.  A segment
        // has no inherent direction, and the store's point order is an artifact of
        // how the line was drawn, so using it raw would send the array backwards
        // through the selection about half the time — for no reason the user could
        // see or predict.  Taking the end NEARER m_ref as the origin makes the rule
        // "it goes the way the line points, from your side of it", and it is the
        // second place the pivot earns its keep: put the pivot on the far side of the
        // selection and the same line arrays the other way.
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

        // The hand-aim release (kiwi_dupe.h KARR_LINE_BREAK_PIXELS has the argument
        // for the deadzone).
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
            // REBASE, DO NOT RE-LATCH.  The hand-drag maps the cursor against
            // m_start, which was latched before the line was ever picked, so simply
            // handing control back would snap the step by however far the user
            // travelled to reach the line.  Re-latching plainly (m_start = cursor)
            // is no better: it makes the offset ZERO at the instant of release, so
            // the whole preview collapses on the frame the user was only trying to
            // adjust it.  Instead m_start is placed so that THIS cursor position
            // maps to the step the line was already giving — the array is unchanged
            // on the release frame and hand motion adjusts from there.  That is the
            // same "pick up from the cursor without a jump" discipline
            // KiwiCmd_MouseButton's resume arm keeps (kiwi_command.cpp:1530-1537).
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

        // ROUND AA, ITEM 8: latch m_start such that the CURRENT cursor maps to the
        // CURRENT m_offset, so handing the step back to the hand-drag is a no-op on
        // the frame it happens.  Falls back to the plain latch when the cursor ray
        // misses the plane (grazing view), which is the pre-existing behaviour.
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
                // ── ROUND AA, ITEM 8: THE TWO SOURCES, ARBITRATED ────────────
                // The picked line and the hand-drag both answer "what is the step",
                // so they are tested in that order and exactly one of them writes
                // m_offset on any given frame — they can never fight over it.  The
                // hand-drag arm below is UNCHANGED, byte for byte; it has simply
                // moved inside an else.
                ReleaseLineIfHandAimed();

                if ( m_lineHave )
                {
                    // COUNT DECIDES SPACING — AdoptLine has the derivation and the
                    // Plasticity citations.  m_count is already clamped to
                    // [KARR_MIN_COUNT, KARR_MAX_COUNT] above and Reset() only ever
                    // seeds it from KARR_DEF_LINEAR (3), so span >= 1 already and
                    // this cannot divide by zero; the floor below is belt-and-braces
                    // against a future default that forgets.
                    int span = m_count - 1;
                    if ( span < 1 )
                        span = 1;
                    const float step = m_lineLen / (float)span;
                    for ( int k = 0; k < 3; ++k )
                        m_offset[k] = m_lineDir[k] * step;

                    // NO GRID SNAP HERE, deliberately.  The hand-drag snaps its delta
                    // because the delta is a raw cursor reading with nothing else
                    // holding it; a picked line is already exact geometry the user
                    // authored, and rounding the derived spacing to the grid would
                    // mean the copies NO LONGER SPAN THE LINE — which is the one
                    // thing this mode promises.  The line is the constraint, so it
                    // wins over the grid, exactly as a geometry snap already wins
                    // over the grid in the arm below.
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

            // Radial: rotate the source box's CENTRE about the pivot on Z and keep
            // the extents.  A rotated box is not axis-aligned, so this is a
            // deliberately approximate ghost — it shows WHERE each copy goes, which
            // is what the preview is for.
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

        // ROUND AA, ITEM 8: a 3-axis cross at `c`, half-extent `r`.  Same shape the
        // coarse ghost fallback above already draws, kept as its own helper because
        // the line highlight wants it at two different sizes.
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

        // KIWI-UX (CLEANUP, BoxEdges): the corner expansion and the 12-edge table
        // are KiwiBox_Corners / KIWI_BOX_EDGE in kiwi_lines.h now — this was one
        // of three verbatim copies of the same box.
        static bool DrawBox( const float *mins, const float *maxs )
        {
            float v[KIWI_BOX_CORNERS][3];
            KiwiBox_Corners( mins, maxs, v );
            for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
                if ( !KiwiLines_Add( v[KIWI_BOX_EDGE[e][0]], v[KIWI_BOX_EDGE[e][1]] ) )
                    return false;
            return true;
        }

        // ── the commit ──────────────────────────────────────────────────────
        void Perform()
        {
            // The originals, as they are RIGHT NOW.  They must be back in
            // selected_brushes at KiwiCmd_UndoCommit time (kiwi_dupe.h's undo note).
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
                // Clone the CURRENT selection (the originals on step 0, then copy
                // i-1), so the cumulative transform lands copy i at i × step
                // without ever re-selecting the originals mid-loop.
                Clone_Selection( grid_sizes[g_qeglobals.d_gridsize] );
                if ( selected_brushes.next == &selected_brushes )
                {
                    Sys_Printf( "Array: nothing cloneable — stopped after %i copy(ies).\n", i );
                    break;
                }

                if ( m_radial )
                {
                    // The canonical ported transform pattern (mainfrm.cpp
                    // Radiant_RotateSelection): pivot in rot_around[0], matrix built
                    // by Select_RotateAxis, applied by Select_ApplyMatrix.
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
                    // The clone/paste tail the classic handler runs
                    // (Cmd_OnSelectionClone, mainfrm.cpp 0x425480): refresh the 2D
                    // back-face-cull hint from the face materials.
                    sub_47B940( b->def );
                }
            }

            // Final selection = the originals PLUS every copy.  Built in the typed
            // layer and pushed down through its ONE crossing, so the legacy lists
            // are only ever driven through Select_Deselect / Select_Brush.
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
                // ROUND AA, ITEM 8: name the LINE in the report, so a mapper reading
                // the console after the fact can tell a spanned array from a
                // hand-aimed one that happened to land on the same numbers.
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
            // ROUND AA, ITEM 8: the HUD has to NAME THE MODE IN FORCE.  With two
            // sources for the step and a reference point that is sometimes the pivot
            // and sometimes the centroid, a line that only shows numbers leaves the
            // user guessing which of four states produced them — and the whole
            // complaint this item answers is that setting the step by hand is opaque.
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
                // LINE MODE prints the SPAN and the derived SPACING, because those
                // are the two numbers the count/length rule relates and the user can
                // only check the rule if it can see both.
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

        // ── ROUND AA, ITEM 8 ────────────────────────────────────────────────
        // m_ref came from the session pivot rather than Select_GetMid.  Kept as its
        // own flag rather than re-asking KiwiXform_PivotOverride every frame, for
        // the reason kiwi_transform.cpp:3231 gives for the same flag: the query is
        // self-expiring, so a later read could answer differently and the HUD would
        // start describing a reference point the gesture is not using.
        bool          m_pivoted    = false;
        // The picked construction segment, if one is the array vector.  m_lineA is
        // the end NEARER m_ref (AdoptLine's ordering rule), m_lineDir is unit.
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

// ─── §3 canExecute ───────────────────────────────────────────────────────────
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
    // ROUND K: the palette predicate has to cover BOTH arms of the dispatch below,
    // or Shift+D greys out on a pure edge selection — which is exactly the
    // selection the new arm exists for.
    return CloneableSelectedCount() > 0 || KiwiConSel_CanDuplicateEdges();
}

// ─── ROUND J: DUPLICATE (Shift+D) ────────────────────────────────────────────
// The Plasticity source, the two ported halves and the CREATION undo bracket are
// all written out in kiwi_dupe.h.  This is the assembly and nothing else.
bool KiwiDupe_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned)KIWI_CMD_DUPLICATE )
        return false;

    // ── KIWI-UX (ROUND K): THE EDGE ARM ─────────────────────────────────────
    // USER DIRECTIVE: "When pressing Shift-D, while having edges of a solid(brush)
    // selected, it should create new lines in place of those edges."
    //
    // THE DISPATCH RULE, in one line: ANY SEL_EDGE item in the typed selection
    // sends the press to the edge arm; everything else keeps round J's clone.
    //
    // Why "any", rather than "the dominant kind is EDGE" or "only edges":
    //   * a MIXED selection cannot be cloned in the edge sense anyway — the
    //     ported Clone_Selection works on whole brushes and would clone the owner
    //     solids of the selected edges, which is emphatically not what "duplicate
    //     these edges" means;
    //   * edge selection is a deliberate, mode-2 act (a brush is not promoted onto
    //     it since shakeout D's promotion removal), so a selection containing an
    //     edge is a selection the user built edge-first;
    //   * the two arms are then mutually exclusive and there is no third case to
    //     remember.
    // Tested BEFORE the cloneable count, because a pure edge selection has
    // CloneableSelectedCount() == 0 (edges are not brushes) and would otherwise be
    // refused with a message about patches.
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
        Sys_Printf( "Duplicate: nothing cloneable is selected (patches and "
                    "fixed-size entity brushes go through Copy/Paste).\n" );
        return true;
    }
    if ( KiwiCmd_Active() )
    {
        // Never stomp a live gesture — the same guard KiwiCmd_AfterPaste keeps
        // (kiwi_command.cpp:594).
        Sys_Printf( "Duplicate: finish the current command first.\n" );
        return true;
    }

    // THE CREATION BRACKET (kiwi_dupe.h): no Undo_AddBrushList, because a
    // duplicate modifies nothing that already exists.  Undo_EndBrushList then
    // stamps exactly the copies, and Undo_Undo removes them.
    Undo_ClearRedo();
    Undo_GeneralStart( "duplicate" );        // stores the POINTER — literals only

    Clone_Selection( grid_sizes[g_qeglobals.d_gridsize] );

    // Cmd_OnSelectionClone's own tail (mainfrm.cpp:2976-2979), replicated: the
    // 2D back-face-cull hint is re-derived from the face materials on every brush
    // def in both display lists.
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        sub_47B940( i->def );
    for ( selbrush_t *j = active_brushes.next;   j != &active_brushes;   j = j->next )
        sub_47B940( j->def );

    Undo_EndBrushList( &selected_brushes );
    Undo_End();

    // The typed selection must be re-derived from the legacy lists before anything
    // observes it — Clone_Selection went through the legacy funnels only.
    Sel_InvalidateFromLegacy();

    Sys_Printf( "Duplicated the selection.\n" );
    g_nUpdateBits = -1;

    // …and hand off into Move PAUSED, which is DuplicateCommand.ts's last line
    // (`this.editor.enqueue(new MoveCommand(this.editor), false)`).  The classic
    // Paste / Clone tail is the same call, so there is one handoff in the editor.
    KiwiCmd_AfterPaste();
    return true;
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiDupe_RegisterCommands()
{
    // Unbound: the CLASSIC-profile bindings.  The ARRAYS still claim no key; the
    // modern profile gives Shift+D to DUPLICATE (round J), which is what
    // Plasticity binds it to (default-keymap.ts:280 `shift-d` ->
    // `command:duplicate`) and is the muscle memory the old note below was
    // reaching for.
    //
    // THAT NOTE WAS ALSO WRONG ABOUT THE TABLE and is corrected here rather than
    // deleted, because the error is the exact kind this file's audits exist to
    // prevent: it claimed "0x44 appears only as mods 4 = Select Inside", when vk
    // 0x44's real occupancy is mods 0 CameraUp 33055 (mainfrm.cpp:1034) · mods 1
    // RotateZ 32961 (:1078) · mods 5 MakeDetail 33042 (:1088) · mods 7
    // DropVertices 33213 (:1077).  Shift+D was TAKEN, and the modern profile
    // displaces RotateZ to Shift+Alt+D (mods 3, free) with the house two-step —
    // the full chain is in kiwi_keymap.h.
    //   Array (Radial) — still unbound; Ctrl+Alt+D (mods 6) is the free candidate.
    // The mirror trio needs no row: 32956/57/58 are already in the table
    // (mainfrm.cpp's compiled-in defaults) under FlipX/FlipY/FlipZ.
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

// ─── the "Duplicate" block in the shell's panel window ──────────────────────
void KiwiDupe_MenuItems()
{
    ImGui::SeparatorText( "Duplicate" );

    const bool canMirror = KiwiDupe_CanMirror();
    const bool canArray  = KiwiDupe_CanArray();

    // KIWI-UX (CLEANUP, B-21): the block tooltip is collected from EVERY button in the
    // block.  ImGui::IsItemHovered() after the loop refers to the LAST item submitted,
    // so a tip meant for X/Y/Z only ever appeared over Z.
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

    // ROUND J: Duplicate sits at the top of the block because it is the one a
    // mapper reaches for constantly, and because it is the plain form of what the
    // two arrays do N times.
    ImGui::BeginDisabled( !KiwiDupe_CanDuplicate() );
    if ( ImGui::Button( "Duplicate (and move)" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_DUPLICATE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Clones the selection and enters Move, paused —\n"
                           "drag a gizmo handle or type a distance, then RMB/Enter." );

    bool arrayHover = false;               // KIWI-UX (CLEANUP, B-21), as above
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
            ImGui::SetTooltip( "Select at least one ordinary brush.\n"
                               "Patches and fixed-size entities cannot be cloned\n"
                               "by the ported Clone_Selection." );
        else
            // ROUND AA, ITEM 8: the two new inputs are worth one line each here,
            // because neither is discoverable from the button.
            ImGui::SetTooltip( "Linear: click a CONSTRUCTION LINE to array across it\n"
                               "(the count spans the line), or drag to set the step\n"
                               "by hand.  Typed digits are always the count.\n"
                               "Both: a session PIVOT (V, placed in Move/Rotate) is\n"
                               "used as the reference point instead of the selection\n"
                               "mid — for Radial that is the axis it revolves about." );
    }
}
