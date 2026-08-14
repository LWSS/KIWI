#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_boolean.cpp — ROUND L implementation.  See kiwi_boolean.h for the
// Plasticity findings (every one cited to a file:line), the four deliberate
// deviations, the N-plane subtract, the §19 all-or-nothing policy and the undo
// ordering.
//
// NEW code over the ported cores.  It computes no plane math of its own: every
// cut runs through KiwiSplit_DefByPlane (kiwi_split.h) over the ported
// Brush_SplitBrushByFace, and the union runs the CLASSIC CSG-merge handler.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // ROUND AA, ITEM 2 — camera_s (the eye-orient)
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_boolean.h"
#include "kiwi_boxselect.h"         // ROUND AA, ITEM 9 — KiwiBox_CollectBrushes
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_split.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

// ── ported entry points (each verified against its DEFINITION) ──────────────
extern camera_s   *Ed_Camera();                                               // camwnd.cpp:153
extern int         Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern int         g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)
extern selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner );          // brush.cpp:656  (0x475980)
extern void        Brush_AddToList2( selbrush_t *b );                         // brush.cpp:910  (0x4765a0)
extern void        Brush_Free( selbrush_t *b );                               // brush.cpp:982  (0x475ba0)
extern void        Select_Deselect( int bAlsoFreeFaces );                     // select.cpp:1428 (0x48E800)
extern void        Select_Brush( selbrush_t *brush, char some_overwrite,
                                 char bStatus, char center_grid_on_selection ); // select.cpp:884
extern void        Radiant_ExecCommand( unsigned int cmdId );                 // mainfrm.cpp:3949
// ROUND N: the tool is consumed by a difference, and the tool is NOT on
// selected_brushes (it is picked with PICKF_EXCLUDE_SELECTED), so the bracket
// head's Undo_AddBrushList never cloned it.  These are what cover it by hand —
// the same pair kiwi_transform.cpp:355 UndoCoverBrush uses, and in the same order
// (the entity first; undo.cpp warns when brushes precede their entity).
extern void        Undo_AddBrush( entity_brush_s *pBrushInst );                // undo.cpp:494  (0x45e680)
extern void        Undo_AddEntity( int a1 );                                   // undo.cpp:601  (0x45e8a0)

// The fill pair, the same two kiwi_region.cpp (:88 / :90-94) and kiwi_split.cpp
// (:59 / :60-64) use.  R_AddCmdSetMaterialColor comes from r_rendercmds.h.
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    // ── the CLASSIC CSG-merge command id ────────────────────────────────────
    // mainfrm.cpp:5331 `case 32927: Cmd_OnSelectionCsgmerge();`.  The SAME id
    // kiwi_csg.cpp fronts and kiwi_join.cpp's face arm dispatches, so a union from
    // Q, from J and from the CSG panel are one handler with one undo record.
    const int KBOOL_ID_MERGE = 32927;

    // Plasticity's phantom hues (BooleanFactory.ts:203-205 / :351-392): red for the
    // DIFFERENCE tool, and its blue-for-targets is a green here because KIWI's
    // selection tint is already blue and a preview has to be distinguishable FROM
    // the selection, not the same colour as it.  Its 0.1 alpha reads as nothing
    // over a lit wall at KIWI's brightness, so the alpha is 0.22 — the value
    // kiwi_region.cpp chose for exactly this reason and kiwi_split.cpp reuses.
    const float KBOOL_DIFF_RGBA [4] = { 1.00f, 0.16f, 0.16f, 0.22f };   // the tool, carving
    const float KBOOL_UNION_RGBA[4] = { 0.25f, 1.00f, 0.45f, 0.22f };   // both, combining
    const float KBOOL_DIFF_LINE [3] = { 1.00f, 0.35f, 0.30f };
    const float KBOOL_UNION_LINE[3] = { 0.40f, 1.00f, 0.55f };
    // The same yellow every grabbable / clickable thing in this layer uses
    // (kiwi_gizmo.cpp KGZ_HOT, kiwi_lollipop.cpp KLOL_COL_BALL, kiwi_split.cpp
    // KSPLIT_HOT_COL) — "this is what the click will take".
    const float KBOOL_HOT_LINE  [3] = { 1.00f, 0.90f, 0.30f };

    // How many solids the translucent fill covers before it gives up and leaves
    // the rest as outlines.  A fill is one R_AddRenderCmdDrawTris PER FACE, so an
    // unbounded multi-target union could put hundreds of draws in one frame for a
    // picture that is already legible from the outlines.  The TOOL is always
    // filled — it is the operand the preview is about.
    //
    // ── KIWI-UX (ROUND AF, ITEM 3): 8 -> 32, AND SPENT NEWEST-FIRST ──────────
    // USER REPORT, verbatim: "When adding a bunch of objects to a boolean (windows
    // on a building), After about 10-12, the red previewer stopped working on newly
    // selected diff tools.  I still clicked all of them and the operation worked as
    // expected, but the previewer always broke."
    //
    // TWO separate caps produced that, and this is the cheaper one: with the fill
    // spent in PICK ORDER, tools 0..7 were tinted and every one after that was not
    // — so the solid the user had just clicked was precisely the one that showed
    // nothing.  Reversing the spend is the actual fix (the newest operand is the one
    // being looked at); raising the number to 32 is what makes the common case never
    // reach the degrade at all.  Punching a facade full of windows is a
    // ~20-30-operand gesture, 32 boxes is ~192 R_AddRenderCmdDrawTris in a frame,
    // and the camera pass already emits one line batch per brush for hundreds of
    // brushes.  Past 32 the operand is still OUTLINED and the console says so ONCE.
    const int KBOOL_MAX_FILL = 32;

    // ROUND AF, ITEM 3: headroom left in the shared line batch for the snap marker
    // KiwiCmd_DrawWorld emits AFTER this command's DrawWorld returns, plus the
    // colour-run change the hover outline costs.  Mirrors the 96 of slack the
    // framework's own 288 was sized with (kiwi_command.cpp).
    const int KBOOL_LINE_HEADROOM = 96;

    inline void Copy3( const float *a, float *o ) { o[0]=a[0]; o[1]=a[1]; o[2]=a[2]; }

    // A brush instance this verb may legally consume.  The same two tests every
    // other CSG path in this layer runs (kiwi_csg.cpp CsgUsable, kiwi_split.cpp
    // Splittable), which are in turn the ported cores' own: csg.cpp:572's
    // validation loop refuses patches and fixed-size entities outright.
    bool Usable( const selbrush_t *b )
    {
        if ( !b || !b->def || b->patch )
            return false;
        const entity_s *owner = b->owner;
        if ( !owner || !owner->def )
            return false;
        const entity_s *ownerDef = owner->def;
        if ( !ownerDef->eclass || ownerDef->eclass->fixedsize )
            return false;
        return b->def->faceCount >= 4;
    }

    // Cheap bounds rejection: two brushes that do not overlap cannot interact, and
    // the carve below would spend N splits discovering it.  Brush_BuildWindings
    // keeps def->[mins,maxs] current, so this is a read and not a computation.
    bool BoundsOverlap( const brush_t *a, const brush_t *b )
    {
        for ( int k = 0; k < 3; ++k )
            if ( a->mins[k] > b->maxs[k] || a->maxs[k] < b->mins[k] )
                return false;
        return true;
    }

    void FreeDefs( std::vector<brush_t *> *defs )
    {
        for ( size_t i = 0; i < defs->size(); ++i )
            KiwiSplit_FreeUnlandedDef( ( *defs )[i] );
        defs->clear();
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  THE N-PLANE SUBTRACT (kiwi_boolean.h DIFFERENCE).
    // ═════════════════════════════════════════════════════════════════════════
    enum carveResult_t
    {
        KBOOL_CARVED = 0,   // outPieces holds the surviving parts, none landed yet
        KBOOL_MISS,         // the two solids do not intersect: nothing to do, no error
        KBOOL_CONSUMED,     // the input lies wholly INSIDE the tool: zero pieces survive
        KBOOL_REFUSED       // §19 said no
    };

    // `target` is NEVER mutated: Brush_SplitBrushByFace clones twice and only reads
    // the input, so every def in `outPieces` and every intermediate remainder is a
    // fresh, UNLANDED clone.  A refusal therefore frees everything it made and
    // leaves the map exactly as it found it.
    //
    // ── KIWI-UX (ROUND T): WHAT THE HOLE'S WALLS ARE TEXTURED WITH ──────────
    // Every carve plane goes through KiwiSplit_DefByPlane, which since round T
    // seeds its template face from kiwi_material.h's rules instead of stamping
    // caulk.  Those rules are given `remainder` — a descendant of the TARGET —
    // so the answer is kiwi_material.h R4, stated positively: **the hole belongs
    // to the solid it was cut into, never to the tool.**  The tool is scaffolding
    // and is consumed by the commit; carrying its material into the target would
    // texture a hole with whatever the mapper happened to have on the cutter.
    // Nothing here changes to get that — it falls out of the splitter being
    // handed the target's own def, which it always was.
    //
    // ── KIWI-UX (ROUND AA, ITEM 9): THIS IS NOW THE ONE-TOOL WORKER ─────────
    // It was `CarveTarget` verbatim until this round and its body is UNCHANGED
    // except for one thing: the "everything ended up inside the tool" tail used to
    // return KBOOL_REFUSED with the §19 message.  It now returns KBOOL_CONSUMED
    // and says nothing, because in a MULTI-TOOL cascade that verdict is no longer
    // final — it is a statement about ONE INTERMEDIATE PIECE against ONE tool, and
    // a fragment that a later crack happens to swallow whole is not a brush the
    // user asked to delete, it is scaffolding that never existed.  CarveTarget
    // below is what decides, once, whether the TARGET disappeared; with a single
    // tool the target is the only piece, so the empty-set refusal it raises there
    // is byte-for-byte the pre-round-AA outcome, message included.
    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AO, ITEM 3) — A SLIVER IS NOT A REFUSAL
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "With this new update, the boolean tool got worse and
    // i can't diff an arch into a square pyramid anymore.  Try to make it more
    // robust."
    //
    // WHAT WAS ACTUALLY WRONG — and it is NOT a regression in this file, which has
    // not changed a line of carve since round AA (round AF touched only the
    // PREVIEW's two budgets; rounds AL/AM/AN did not touch this file at all, and
    // the only AL edit to kiwi_split.cpp was the fill NORMAL, TRAP 4).  The defect
    // is older and this shape is simply the one that exposes it.
    //
    // `KiwiSplit_DefByPlane` is ALL-OR-NOTHING: a §19 failure on EITHER half frees
    // BOTH and returns false, and this loop turned that into KBOOL_REFUSED for the
    // WHOLE TARGET.  But the two halves do not have the same standing here:
    //
    //   * the FRONT half is a PIECE THAT WOULD BE LANDED.  A sliver front (V3
    //     "face collapsed", V4 "zero-area face" under KVALID_MIN_FACE_AREA = 0.1
    //     square units, V6 "planes crossed" under KVALID_MIN_THICKNESS = 0.01) is
    //     a fragment of essentially zero volume that no mapper asked for and none
    //     could select afterwards.  DROP IT and carry on: the volume lost is below
    //     the gate that rejected it, by definition.
    //   * the BACK half is the REMAINDER, which is `target ∩ (inside planes 0..f)`
    //     and is DISCARDED at the end of the subtract by construction.  The true
    //     intersection is a SUBSET of it, so a sliver remainder proves that what
    //     this tool would remove is itself below the gate — i.e. the two solids
    //     effectively MISS.  Leave the target alone; that is the honest answer and
    //     it is also the one that does not consume the tool over a graze.
    //
    // A SQUARE PYRAMID IS THE WORST CASE FOR THE OLD RULE.  It has an apex where
    // four sloped planes meet at a point and four sloped edges where they meet in
    // pairs, so the outside slices an arch's many planes cut off it are wedges that
    // taper to nothing — and ONE of them under 0.1 square units anywhere in the
    // cascade threw away the entire carve and printed "one brush left untouched —
    // zero-area face".  With a box target the same tool never produces a taper and
    // the same boolean "works", which is exactly the shape-dependence the report
    // describes.
    //
    // WHAT IS NOT RELAXED: §19 itself.  Nothing that fails the gate is ever landed;
    // the change is only in what a failure MEANS for the rest of the gesture.  A
    // degenerate cut plane, or the ported core producing neither half, is still a
    // REFUSAL and still leaves the target untouched.
    //
    // The drops are COUNTED, not silent — a mapper who loses a fragment is told.
    // A plain int rather than another out-parameter through four call sites: the
    // carve is single-threaded, synchronous and reentrancy-free, and each commit
    // path zeroes it immediately before its own loop.
    int s_carveSliverDrops = 0;

    // ── KIWI-UX (ROUND AQ, ITEM 7): THE PER-REFUSAL ACCOUNT ─────────────────
    // Same rationale, same lifetime and the same single-threaded, synchronous
    // guarantee as s_carveSliverDrops above: each commit path zeroes these
    // immediately before its own loop (KiwiBool_ResetCarveReport).
    //   s_carveRefusals  — how many tool x piece cuts were skipped this operation
    //   s_carveMissWhy   — the FIRST "these two do not intersect" reason seen, kept
    //                      so an operation that changes nothing can say WHY instead
    //                      of only that it did nothing.
    enum { KBOOL_REFUSE_REPORT_MAX = 8 };
    int  s_carveRefusals   = 0;
    char s_carveMissWhy[192] = { 0 };
    // ── KIWI-UX (ROUND AR, ITEM 2): INTERMEDIATES CARRIED PAST §19 ──────────
    // How many running-intersection halves were carried on despite a §19 refusal
    // (kiwi_split.h §19 IS A GATE ON WHAT GETS LANDED).  Counted for the same
    // reason s_carveSliverDrops is: the mapper is told when the boolean had to
    // work around something, even though nothing invalid was landed.
    int  s_carveCarried    = 0;
    // KiwiBool_WouldCarve is a DRY RUN and the auto-bool asks it every frame the
    // cursor moves, so the reporting must be silenced around it or one hover would
    // be a hundred console lines a second.  Set/cleared by that function alone.
    bool s_carveQuiet      = false;

    void KiwiBool_ResetCarveReport()
    {
        s_carveSliverDrops = 0;
        s_carveRefusals    = 0;
        s_carveCarried     = 0;              // KIWI-UX (ROUND AR, ITEM 2)
        s_carveMissWhy[0]  = '\0';
    }

    carveResult_t CarveByOneTool( brush_t *target, const brush_t *tool,
                                  std::vector<brush_t *> *outPieces, const char **why )
    {
        outPieces->clear();
        *why = "unknown";

        if ( !BoundsOverlap( target, tool ) )
        {
            *why = "their bounding boxes do not overlap";   // KIWI-UX (AQ, ITEM 7)
            return KBOOL_MISS;
        }

        brush_t *remainder = target;
        bool     ownRemainder = false;      // true once `remainder` is ours to free

        for ( int f = 0; f < tool->faceCount; ++f )
        {
            const face_t &tf = tool->faces[f];

            brush_t        *front = 0, *back = 0;
            kiwiSplitHalf_t fs = KSPLIT_HALF_NONE, bs = KSPLIT_HALF_NONE;
            // ══════════════════════════════════════════════════════════════════
            //  KIWI-UX (ROUND AR, ITEM 2) — THE INTERMEDIATE IS NOT A LANDING
            // ══════════════════════════════════════════════════════════════════
            // USER REPORT, verbatim: *"why does this bool diff fail? … The only
            // different is I joined the polyline on the 2nd one.  When i bool it
            // into the pyramid it fails!"*
            //
            // `KiwiSplit_DefByPlaneCarve` differs from the Parts entry in exactly
            // one place: a BACK half that fails §19 but still has real thickness in
            // all three directions is HANDED BACK instead of freed.  The back half
            // is `remainder ∩ tool-plane f` — the running intersection — and the
            // tail of this very loop FREES it without ever landing it, so holding
            // it to the map's landing gate was refusing an intermediate for how it
            // would look if it were a brush, which it never becomes.  kiwi_split.h
            // carries the full argument and the KSPLIT_CARRY_EXTENT number.
            //
            // WHY IT IS THE ARCH REPORT.  An extruded arch is a fifteen-plus-plane
            // convex prism whose side planes are near-tangent neighbours.  Cut a
            // pyramid with it and the running intersection collects a zero-area
            // face (V4) or a coincident plane pair (V5) at SOME plane, essentially
            // every time — with a completely healthy volume.  Under the old rule
            // that single verdict returned KBOOL_MISS for the only piece of the
            // only target, `owned` stayed false, and the whole difference was a
            // silent no-op: the pyramid uncarved with the white tool sitting in it.
            //
            // WHAT IS NOT RELAXED: §19 on anything LANDED.  The FRONT halves — the
            // pieces that become brushes — are gated exactly as before, and a
            // genuinely grazing back half (any extent under KSPLIT_CARRY_EXTENT) is
            // still round AO's stop.
            bool backRefused = false;
            if ( !KiwiSplit_DefByPlaneCarve( remainder, tf.planepts[0], tf.planepts[1],
                                             tf.planepts[2], &front, &back,
                                             &fs, &bs, &backRefused, why ) )
            {
                // Nothing was allocated by that call, whatever went wrong.
                FreeDefs( outPieces );
                if ( ownRemainder )
                    KiwiSplit_FreeUnlandedDef( remainder );
                return KBOOL_REFUSED;
            }

            if ( bs != KSPLIT_HALF_OK )
            {
                // TWO STATES, ONE VERDICT (see the note above).
                //   NONE   — everything is on the OUTSIDE of this tool plane, so the
                //            remainder shares no volume with the tool at all.
                //   SLIVER — the running intersection has collapsed below §19, and
                //            the true intersection is a subset of it.
                // Either way the two solids miss and the target must be left alone.
                if ( front )
                    KiwiSplit_FreeUnlandedDef( front );
                FreeDefs( outPieces );
                if ( ownRemainder )
                    KiwiSplit_FreeUnlandedDef( remainder );
                // KIWI-UX (ROUND AQ, ITEM 7): NAME WHICH OF THE TWO IT WAS, and at
                // which plane.  "No intersection" and "the intersection collapsed
                // under a near-tangent cut" look identical from outside this
                // function and have completely different fixes — the first means
                // move the tool, the second means the tool grazes the target and
                // §19 is refusing the shard.  An arch's curved top is a fan of
                // near-tangent planes, so the second is exactly the verdict the
                // arch-into-box report needs distinguished.
                // The reason string is copied out FIRST: `*why` is an out-parameter
                // reused across calls and must never alias the buffer being written.
                char gateWhy[96];
                _snprintf( gateWhy, sizeof( gateWhy ), "%s", *why ? *why : "invalid geometry" );
                gateWhy[sizeof( gateWhy ) - 1] = '\0';
                static char s_missWhy[160];
                _snprintf( s_missWhy, sizeof( s_missWhy ),
                           ( bs == KSPLIT_HALF_NONE )
                             ? "no shared volume — tool plane %i leaves the target "
                               "entirely outside it"
                             : "the shared volume collapsed below the validity gate "
                               "at tool plane %i (a near-tangent cut: %s)",
                           f, gateWhy );
                s_missWhy[sizeof( s_missWhy ) - 1] = '\0';
                *why = s_missWhy;
                return KBOOL_MISS;
            }

            // KIWI-UX (ROUND AR, ITEM 2): the carry is NEVER silent.  Nothing
            // invalid was landed, but the mapper is told that the hole around this
            // plane may not be exact — and told WHICH gate, because that is the one
            // fact that separates "the tool is fine and §19 was fussy about an
            // intermediate" from "the tool is genuinely degenerate there".
            if ( backRefused )
            {
                ++s_carveCarried;
                if ( !s_carveQuiet && s_carveCarried <= KBOOL_REFUSE_REPORT_MAX )
                    Sys_Printf( "Boolean: tool plane %i left the running intersection "
                                "outside the validity gate (%s) — carried on, because "
                                "that piece is an intermediate and is never landed.\n",
                                f, ( *why && **why ) ? *why : "invalid geometry" );
            }

            if ( fs == KSPLIT_HALF_NONE )
            {
                // Everything is INSIDE this half-space: this plane carves nothing.
                // `back` is a re-clone of the same volume, so drop it and carry the
                // remainder we already have to the next plane.
                KiwiSplit_FreeUnlandedDef( back );
                continue;
            }

            if ( fs == KSPLIT_HALF_SLIVER )
            {
                // The outside slice is below the §19 gate: it was freed by the
                // splitter and it is NOT a piece.  The remainder still advances —
                // the volume forfeited is smaller than the gate that rejected it.
                ++s_carveSliverDrops;
            }
            else
            {
                outPieces->push_back( front );      // outside this plane = it survives
            }

            if ( ownRemainder )
                KiwiSplit_FreeUnlandedDef( remainder );
            remainder    = back;                    // still possibly inside the tool
            ownRemainder = true;
        }

        // Whatever is left after every plane IS target ∩ tool — the volume the
        // difference removes.  Discarded, and never landed for even one frame.
        if ( ownRemainder )
            KiwiSplit_FreeUnlandedDef( remainder );

        if ( outPieces->empty() )
        {
            // Every plane kept the whole remainder on its inside, i.e. the input
            // lies entirely within this tool.  Whether that is an ERROR is not this
            // function's call any more — see the CONSUMED note above.
            return KBOOL_CONSUMED;
        }
        return KBOOL_CARVED;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND AA, ITEM 9 — THE CASCADE: N TOOLS, ONE TARGET
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "the difference command needs to accept multiple
    // shift-clicked brushes.  For example, if I'm making a sidewalk and I want to
    // boolean each crack, I have to do it 1 by 1 current, that's awful.  Also
    // support shift box select."
    //
    // THE SHAPE.  Carve the target by tool 0; feed every SURVIVING PIECE through
    // tool 1; feed every survivor of THAT through tool 2, and so on.  That is the
    // definition of `target - (t0 ∪ t1 ∪ … )` for convex tools and it needs no
    // union of the tools to be computed — which is the whole reason it is done this
    // way, because a union of two cracks that do not touch is not a convex brush
    // and this editor has no non-convex brush to hold it in.
    //
    // OWNERSHIP, which is the only subtle part.  `target` is NEVER ours and is
    // never mutated (CarveByOneTool clones); every def CarveByOneTool hands back is
    // a fresh UNLANDED clone that we own.  So the working set has exactly two
    // states, and the invariant `!owned  <=>  cur == { target }` holds throughout:
    //   * `owned == false`: nothing has cut anything yet, so `cur` is the single
    //     borrowed `target` pointer and NOTHING in it may be freed;
    //   * `owned == true`: every element is a clone we made and must free on any
    //     path that does not hand it to the caller.
    // The transition is one-way and happens the first time a tool changes anything,
    // at which point `cur` held exactly one element (the borrowed target) which was
    // replaced wholesale — so there is never a mixed set to reason about.
    //
    // ALL-OR-NOTHING PER TARGET (kiwi_boolean.h §19) IS UNCHANGED: a REFUSAL at any
    // stage frees the whole working set and leaves the target on the map untouched.
    // A tool that MISSES a piece is not a refusal — it leaves that piece alone and
    // the cascade carries on, which is exactly the sidewalk case (each crack meets
    // one fragment and misses the rest).
    carveResult_t CarveTarget( brush_t *target, brush_t *const *tools, int toolCount,
                               std::vector<brush_t *> *outPieces, const char **why )
    {
        outPieces->clear();
        *why = "unknown";
        if ( !target || !tools || toolCount <= 0 )
            return KBOOL_MISS;

        std::vector<brush_t *> cur;
        bool                   owned = false;      // see the OWNERSHIP note above
        cur.push_back( target );

        for ( int t = 0; t < toolCount; ++t )
        {
            const brush_t *tool = tools[t];
            if ( !tool )
                continue;

            std::vector<brush_t *> next;
            bool                   changed = false;

            for ( size_t p = 0; p < cur.size(); ++p )
            {
                std::vector<brush_t *> made;
                const carveResult_t r = CarveByOneTool( cur[p], tool, &made, why );

                if ( r == KBOOL_REFUSED )
                {
                    // ══════════════════════════════════════════════════════════
                    //  KIWI-UX (ROUND AQ, ITEM 7) — ONE BAD PLANE NO LONGER
                    //                               THROWS AWAY THE WHOLE CARVE.
                    // ══════════════════════════════════════════════════════════
                    // USER REPORT, verbatim: "I'm having trouble dragging arches
                    // into boxes.  This needs to work.  The boolean tool needs to
                    // be more robust."
                    //
                    // An extruded ARCH is not one tool.  A concave region extrudes
                    // as a CONVEX DECOMPOSITION (kiwi_region.h ConvexPieces), so an
                    // arch arrives here as a fan of many thin wedges whose tops are
                    // near-tangent to each other and to the box face they are being
                    // cut into.  Under the old rule ANY one of those wedges hitting
                    // a hard refusal — a degenerate cut plane, or the ported core
                    // producing neither half — abandoned the ENTIRE difference and
                    // left the box whole.  That is the reported failure: the arch
                    // "does nothing", and it does nothing more often the more
                    // segments the arch has, i.e. exactly the arches worth cutting.
                    //
                    // Round AO already made this argument for SLIVERS and kept
                    // hard refusals all-or-nothing.  This round extends it to the
                    // hard refusal, and it is safe for the same reason: a refusal
                    // means CarveByOneTool CHANGED NOTHING and freed everything it
                    // made, so the piece in hand is exactly as it arrived.  Keeping
                    // it is therefore identical to that tool having MISSED it — the
                    // only difference is that a miss is silent and this is not.
                    //
                    // WHAT IS NOT RELAXED: §19.  Nothing invalid is landed; the
                    // hole is simply incomplete where a plane could not be applied,
                    // and the console says which tool, which piece and which gate
                    // so the mapper can nudge that one wedge instead of guessing at
                    // the whole gesture.  Bounded to KBOOL_REFUSE_REPORT_MAX lines
                    // per operation so a 40-segment arch cannot flood the console.
                    ++s_carveRefusals;
                    if ( s_carveQuiet )
                        { /* dry run — count only */ }
                    else if ( s_carveRefusals <= KBOOL_REFUSE_REPORT_MAX )
                        Sys_Printf( "Boolean: tool %i could not cut piece %i — %s.  "
                                    "That cut was skipped; the rest of the "
                                    "difference still ran.\n",
                                    t, (int)p, ( *why && **why ) ? *why : "invalid geometry" );
                    else if ( s_carveRefusals == KBOOL_REFUSE_REPORT_MAX + 1 )
                        Sys_Printf( "Boolean: (further per-cut refusals suppressed "
                                    "for this operation)\n" );
                    next.push_back( cur[p] );      // survives whole; ownership unchanged
                    continue;
                }
                if ( r == KBOOL_MISS )
                {
                    // KIWI-UX (ROUND AQ, ITEM 7): a miss is only worth a line when
                    // the WHOLE operation comes to nothing, and the caller decides
                    // that — so the reason is latched rather than printed, and the
                    // FIRST one is kept (it is the one nearest the user's intent:
                    // the tool they aimed first).
                    if ( !s_carveMissWhy[0] && *why && **why )
                    {
                        _snprintf( s_carveMissWhy, sizeof( s_carveMissWhy ),
                                   "tool %i vs piece %i: %s", t, (int)p, *why );
                        s_carveMissWhy[sizeof( s_carveMissWhy ) - 1] = '\0';
                    }
                    next.push_back( cur[p] );      // survives whole; ownership unchanged
                    continue;
                }

                // CARVED or CONSUMED: this piece is replaced by `made` (possibly by
                // nothing at all).  The input piece is ours to free only once the
                // set is ours — on the very first cut it is still the map's target.
                changed = true;
                if ( owned )
                    KiwiSplit_FreeUnlandedDef( cur[p] );
                for ( size_t k = 0; k < made.size(); ++k )
                    next.push_back( made[k] );
            }

            cur.swap( next );
            if ( changed )
                owned = true;
        }

        if ( !owned )
        {
            // Every tool missed.  `cur` is still the borrowed target and the map is
            // exactly as it was — the same verdict a single missing tool gave.
            // KIWI-UX (ROUND AQ, ITEM 7): hand the latched reason up so the
            // caller's "left untouched" line can name the gate instead of leaving
            // the mapper to guess between "not overlapping" and "grazing".
            if ( s_carveMissWhy[0] )
                *why = s_carveMissWhy;
            return KBOOL_MISS;
        }
        if ( cur.empty() )
        {
            // The tools between them swallowed the target whole.  kiwi_boolean.h
            // states the policy and why it is not "delete it"; the message is the
            // pre-round-AA one with the count generalised.
            *why = "it is entirely inside the tool(s) (a difference would remove it "
                   "completely) — delete it directly if that is what you want";
            return KBOOL_REFUSED;
        }
        outPieces->swap( cur );
        return KBOOL_CARVED;
    }

    // ── the translucent operand fill ────────────────────────────────────────
    // One convex face winding = one triangle FAN from vertex 0, which is exact for
    // a convex polygon.  Its own MATERIAL_COLOR bracket, the ported selected-face
    // fill's (camwnd.cpp 0x408106; kiwi_region.cpp:1000/:1085 spells it out):
    // neutral so the per-vertex colour drives the draw, white again afterwards.
    const int KBOOL_FILL_MAX_PTS = 64;      // a convex brush face never needs more

    void FillBrush( const brush_t *def, const float rgba[4] )
    {
        if ( !def || !def->faces )
            return;
        const camera_s *cam = Ed_Camera();   // ROUND AA, ITEM 2 — the orient reference
        if ( !cam )
            return;

        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        static float    s_xyzw  [KBOOL_FILL_MAX_PTS][4];
        static float    s_normal[KBOOL_FILL_MAX_PTS][3];
        static float    s_st    [KBOOL_FILL_MAX_PTS][2];
        static float    s_color [KBOOL_FILL_MAX_PTS];
        static uint16_t s_idx   [( KBOOL_FILL_MAX_PTS - 2 ) * 3];

        R_AddCmdSetMaterialColor( s_neutral );
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 3 || w->numpoints > KBOOL_FILL_MAX_PTS )
                continue;
            const int n = w->numpoints;
            for ( int i = 0; i < n; ++i )
            {
                s_xyzw[i][0] = w->p[i][0];
                s_xyzw[i][1] = w->p[i][1];
                s_xyzw[i][2] = w->p[i][2];
                s_xyzw[i][3] = 1.0f;
                Copy3( def->faces[f].plane.normal, s_normal[i] );
                s_st[i][0] = 0.0f;
                s_st[i][1] = 0.0f;
                s_color[i] = packedAsFloat;
            }
            const int tris = n - 2;
            for ( int t = 0; t < tris; ++t )
            {
                s_idx[t * 3 + 0] = 0;
                s_idx[t * 3 + 1] = (uint16_t)( t + 1 );
                s_idx[t * 3 + 2] = (uint16_t)( t + 2 );
            }
            // KIWI-UX (ROUND AA, ITEM 2): every face fan was wound from its own
            // outward plane, so only the half of each operand brush that fronted
            // the eye filled and the translucent solid read as a half-shell.
            // kiwi_lines.h TRAP 3.
            KiwiTris_OrientToEye( &s_xyzw[0][0], 4, s_idx, tris * 3, cam->origin );
            R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                    (short)( tris * 3 ), s_idx, (short)n,
                                    s_xyzw, s_normal, s_color, s_st );
        }
        R_AddCmdSetMaterialColor( s_white );
    }

    // Every face winding's boundary, inside the FRAMEWORK's shared batch
    // (kiwi_command.cpp KiwiCmd_DrawWorld).  A box is 24 segments; KiwiLines_Add
    // returning false is the budget speaking and stops the emission rather than
    // silently wrapping.
    //
    // ROUND AF, ITEM 3: callers now RESERVE before they call (OutlineCost below),
    // so a false return here is the last line of defence and no longer the normal
    // way the budget is discovered.
    bool OutlineBrush( const brush_t *def )
    {
        if ( !def || !def->faces )
            return true;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;
            for ( int p = 0; p < w->numpoints; ++p )
                if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                    return false;
        }
        return true;
    }

    // ROUND AF, ITEM 3: EXACTLY what OutlineBrush is about to spend, by the same
    // walk and the same three skip conditions.  Measured rather than estimated on
    // purpose — a cylinder tool is not 24 segments and a "24 per solid" guess would
    // reintroduce the same class of cliff one shape further along.
    int OutlineCost( const brush_t *def )
    {
        if ( !def || !def->faces )
            return 0;
        int n = 0;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;
            n += w->numpoints;
        }
        return n;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  Q — BOOLEAN.  Selected solids are the TARGETS, clicked solids are the TOOLS.
    //  (ROUND AA, ITEM 9 made the tool a SET; it was one solid until then.)
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiBooleanCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Boolean"; }
        bool CanExecute() override { return KiwiBool_CanBoolean(); }

        // ── KIWI-UX (ROUND AA, ITEM 9): A CLICK TOOL IN *BOTH* STAGES ───────
        // It used to be `m_stage == KBOOL_PICK_TOOL`: the click NAMED the one tool
        // solid and from then on the command inherited the shakeout-E confirm flow,
        // where a stray click merely parks the gesture.
        //
        // USER REPORT, verbatim: "the difference command needs to accept multiple
        // shift-clicked brushes.  For example, if I'm making a sidewalk and I want
        // to boolean each crack, I have to do it 1 by 1 current, that's awful."
        //
        // A SECOND tool can only be named by a SECOND CLICK, and the preview goes
        // live the moment the FIRST one is named — which is the point of the live
        // preview and is not something to give up to keep the stages tidy.  So the
        // clicks never stop being clicks.  What that costs is the park-on-click
        // behaviour (KiwiCmd_Pause returns early for a click tool,
        // kiwi_command.cpp), and the boolean has no drag to park: nothing about it
        // follows the cursor except the HOVER, which is exactly what must keep
        // working so the next solid can be aimed at.  RMB / Enter still commits and
        // Esc still walks back — neither goes through this rung.
        bool WantsClicks() const override { return true; }

        // ROUND AA, ITEM 9: …and a Shift+LMB DRAG is a marquee that ADDS TOOLS.
        // Offered in both stages for the same reason the clicks are.
        bool WantsMarquee() const override { return true; }

        // No scalar to type.  Declaring none also leaves Tab free, which this
        // command does not use but costs nothing to keep consistent with the other
        // modelling verbs.
        int NumericFields( const kiwiNumField_t **out ) const override
        { (void)out; return 0; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        // The tool must never be picked out of the geometry the verb is about to
        // carve — that is what makes "click ANOTHER solid" unambiguous.
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            // ROUND AA, ITEM 9: the additive grammar is advertised in BOTH stages,
            // because it is the same grammar in both — and an Esc in the live stage
            // no longer means "drop the tool", it means "drop the LAST one", which
            // a user cannot guess from a chip that says otherwise.
            static const kiwiPrompt_t s_pick[] = {
                { "LMB",       "Pick the other solid" },
                { "Shift+LMB", "Add another solid" },
                { "Shift+Drag","Box-add solids" },
            };
            static const kiwiPrompt_t s_live[] = {
                { "Q",         "Difference / Union" },
                { "Shift+LMB", "Add another solid" },
                { "Shift+Drag","Box-add solids" },
                { "Esc",       "Drop the last solid" },
            };
            if ( m_stage == KBOOL_PICK_TOOL )
            {
                *out = s_pick;
                return (int)( sizeof( s_pick ) / sizeof( s_pick[0] ) );
            }
            *out = s_live;
            return (int)( sizeof( s_live ) / sizeof( s_live[0] ) );
        }

        bool Begin() override
        {
            Reset();

            // ── KIWI-UX (ROUND AO, ITEM 3): SAY WHAT WAS SKIPPED, AND WHY ────
            // Round AM made a PATCH clickable in FACE mode (kiwi_pick.cpp:641-667:
            // a patch has no faces, so in mode 3 the patch itself is the surface).
            // That is right for texturing and it has one side effect here: a curve
            // now ENTERS `selected_brushes` where before the click bounced, so a
            // mapper who rubber-bands "the arch" can be holding patches, `Usable`
            // drops every one of them (csg.cpp:572's own validation refuses
            // patches: a patch is a render surface with no volume, so there is
            // nothing to carve and nothing to carve WITH), and until now it did so
            // in complete silence.  A skipped operand that says nothing is
            // indistinguishable from a broken command.
            int skippedPatches = 0;
            for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            {
                if ( Usable( b ) )
                    m_targets.push_back( b );
                else if ( b->patch )
                    ++skippedPatches;
            }
            if ( skippedPatches > 0 )
                Sys_Printf( "Boolean: %i curve/patch(es) in the selection were "
                            "skipped — a patch is a render surface with no volume, "
                            "so it can neither be carved nor carve.\n",
                            skippedPatches );
            if ( m_targets.empty() )
            {
                Sys_Printf( "Boolean: select at least one solid first (patches and "
                            "fixed-size entities cannot be booleaned).\n" );
                return false;
            }

            m_stage = KBOOL_PICK_TOOL;
            UpdateHud();
            Sys_Printf( "Boolean: click the OTHER solid to use as the tool — "
                        "Shift+click or Shift+drag a box to add more tools "
                        "(Esc cancels).\n" );
            return true;
        }

        // THE FRAMEWORK'S PICK IS NOT USABLE HERE, for the reason kiwi_matchface.cpp
        // states in full: KiwiCmd_MouseMove builds it with the user's CURRENT
        // SELECTION MODE, and in Point / Edge / Face mode that resolves to a vertex,
        // an edge or a face — never the whole solid this verb needs.  So the ray is
        // re-cast under an OBJECT-only mask, with the command's own exclude flag so
        // the targets can never answer.
        //
        // ── KIWI-UX (ROUND AA, ITEM 9): IT NOW TRACKS IN BOTH STAGES ────────
        // It used to return immediately once a tool was chosen ("the tool is
        // chosen; nothing tracks").  With a tool SET there is always a next solid
        // to aim at, so the hover has to stay alive for the whole gesture — it is
        // what tells the user which brush the next Shift+click will take, and it is
        // what Click() reads.
        //
        // THE FILTER DELIBERATELY ADMITS BRUSHES THAT ARE ALREADY TOOLS.  A
        // Shift+click on an existing tool TOGGLES IT BACK OFF (the additive
        // grammar's other half, the same as the marquee's and the click grammar's),
        // and a brush that could not be hovered could not be un-picked.  Targets
        // stay excluded, in the pick flags and again here.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick; (void)snap;

            selbrush_t *was = m_hover;
            m_hover = 0;
            m_hoverIsPatch = false;             // ROUND AO, ITEM 3

            int   x, y;
            ray_t ray;
            if ( KiwiCmd_LastCursor( &x, &y ) && Pick_RayFromImagePos( x, y, &ray ) )
            {
                const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT, PICKF_EXCLUDE_SELECTED );
                if ( hit.valid && hit.item.brush && Sel_BrushLive( hit.item.brush )
                  && Usable( hit.item.brush ) && !IsTarget( hit.item.brush ) )
                    m_hover = hit.item.brush;
                // ROUND AO, ITEM 3: remember WHY the hover refused, so the click can
                // name it.  "That is not a usable solid" is true of a patch, a
                // fixed-size entity and empty space alike, and a mapper aiming at an
                // arch built out of curves has no way to tell which one they hit.
                else if ( hit.valid && hit.item.brush && Sel_BrushLive( hit.item.brush )
                       && hit.item.brush->patch )
                    m_hoverIsPatch = true;
            }
            if ( m_hover != was )
                g_nUpdateBits |= 1;
            UpdateHud();
        }

        // The first click NAMES A TOOL and enters DIFFERENCE — Plasticity's own
        // default (BooleanFactory.ts:272) and the directive's ("it automatically
        // enters difference mode").  Returning true keeps the gesture running: this
        // command is never committed by a click, only by RMB / Enter.
        //
        // ── KIWI-UX (ROUND AA, ITEM 9): PLAIN REPLACES, SHIFT ACCUMULATES ───
        // USER REPORT, verbatim: "the difference command needs to accept multiple
        // shift-clicked brushes.  For example, if I'm making a sidewalk and I want
        // to boolean each crack, I have to do it 1 by 1 current, that's awful."
        //
        // Exactly the grammar the rest of the editor already has, so there is
        // nothing new to learn: PLAIN click = "the tool is THAT one" (the whole set
        // is replaced), SHIFT click = "…and that one too", SHIFT click on something
        // already in the set = take it back out.  The modifier comes from
        // KiwiCmd_LastShift, which is the press's own flag latched beside its pixel
        // (kiwi_command.h says why it is a latch and not an argument here).
        //
        // THE OP IS NOT RESET BY A LATER CLICK.  Entering the live stage picks
        // DIFFERENCE, as it always did; once the user has pressed Q, adding or
        // replacing a tool must not silently flip the verb back under them.
        bool Click() override
        {
            if ( !m_hover )
            {
                if ( m_hoverIsPatch )           // ROUND AO, ITEM 3
                    Sys_Printf( "Boolean: that is a curve/patch — a patch is a render "
                                "surface with no volume and cannot cut.  Use the "
                                "caulk brush inside it as the tool.\n" );
                else
                    Sys_Printf( "Boolean: that is not a usable solid — click another "
                                "one, or press Esc.\n" );
                return true;
            }

            const bool add = KiwiCmd_LastShift();
            if ( add )
            {
                for ( size_t i = 0; i < m_tools.size(); ++i )
                {
                    if ( m_tools[i] != m_hover )
                        continue;
                    m_tools.erase( m_tools.begin() + i );
                    if ( m_tools.empty() )
                        m_stage = KBOOL_PICK_TOOL;
                    UpdateHud();
                    Sys_Printf( "Boolean: solid removed — %i tool(s).\n",
                                (int)m_tools.size() );
                    g_nUpdateBits = -1;
                    return true;
                }
                m_tools.push_back( m_hover );
            }
            else
            {
                m_tools.clear();
                m_tools.push_back( m_hover );
            }

            if ( m_stage == KBOOL_PICK_TOOL )
            {
                m_op    = KBOOL_DIFFERENCE;
                m_stage = KBOOL_LIVE;
                Sys_Printf( "Boolean: DIFFERENCE — Shift+click adds more tools, Q "
                            "switches to union, RMB / Enter applies, Esc drops the "
                            "last tool.\n" );
            }
            else
            {
                Sys_Printf( "Boolean: %i tool(s).\n", (int)m_tools.size() );
            }
            UpdateHud();
            g_nUpdateBits = -1;
            return true;
        }

        // ── KIWI-UX (ROUND AA, ITEM 9): "Also support shift box select." ────
        // The rect's brush pass, run through the SAME two filters the hover runs
        // (Usable, and never a target), and then through the same duplicate test
        // the Shift+click path uses — a box drawn over a solid that is already a
        // tool must not enter it twice, or it would be carved with twice and freed
        // twice.  It ADDS ONLY: a marquee is an additive gesture by definition here
        // (the rung is Shift-gated at the dispatch site, kiwi_command.h
        // WantsMarquee), so it never clears the set and never toggles anything off.
        //
        // Entering the live stage from a marquee follows the click's rule exactly:
        // the op is picked once, on the transition, and never re-picked.
        void Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift ) override
        {
            (void)shift;                        // Shift-only rung: it is always true

            // A generous cap that is still a fixed frame: a marquee over a whole map
            // could otherwise name thousands of solids, and every one of them costs
            // a full N-plane carve against every target at commit time.
            const int   KBOOL_MARQUEE_MAX = 256;
            selbrush_t *found[KBOOL_MARQUEE_MAX];
            const int n = KiwiBox_CollectBrushes( x0, y0, x1, y1, crossing,
                                                  found, KBOOL_MARQUEE_MAX );

            int added = 0;
            for ( int i = 0; i < n; ++i )
            {
                selbrush_t *b = found[i];
                if ( !Sel_BrushLive( b ) || !Usable( b ) || IsTarget( b ) || IsTool( b ) )
                    continue;
                m_tools.push_back( b );
                ++added;
            }

            if ( !added )
            {
                Sys_Printf( "Boolean: nothing usable in that box — %i tool(s).\n",
                            (int)m_tools.size() );
                return;
            }

            if ( m_stage == KBOOL_PICK_TOOL )
            {
                m_op    = KBOOL_DIFFERENCE;
                m_stage = KBOOL_LIVE;
            }
            UpdateHud();
            Sys_Printf( "Boolean: %i solid(s) added — %i tool(s).\n",
                        added, (int)m_tools.size() );
            g_nUpdateBits = -1;
        }

        // Q toggles the operation while the gesture is live; Esc walks the stages
        // back before the framework's cancel rung sees the key; Enter in stage 1 is
        // consumed with a nudge, because there is no tool to apply anything with.
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk == 0x51 && m_stage == KBOOL_LIVE )       // 'Q'
            {
                m_op = ( m_op == KBOOL_DIFFERENCE ) ? KBOOL_UNION : KBOOL_DIFFERENCE;
                UpdateHud();
                Sys_Printf( "Boolean: %s.\n", ( m_op == KBOOL_UNION )
                            ? "UNION — the solids become one (must stay convex)"
                            : "DIFFERENCE — the tools carve the selection" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x1B && m_stage == KBOOL_LIVE )       // VK_ESCAPE
            {
                // ── KIWI-UX (ROUND AA, ITEM 9): Esc POPS, IT NO LONGER CLEARS ──
                // It used to drop THE tool, because there was only ever one.  With
                // a set, "undo that last pick" is what the key is reached for — a
                // mapper who has shift-clicked eight cracks and mis-aimed the ninth
                // wants the ninth back, not to start over.  Clearing the whole set
                // is still one Esc per tool away, and the LAST of those lands in
                // exactly the state a single Esc used to (stage 1, nothing picked);
                // one more cancels the command, as it always did.
                if ( m_tools.size() > 1 )
                {
                    m_tools.pop_back();
                    UpdateHud();
                    Sys_Printf( "Boolean: last solid dropped — %i tool(s).\n",
                                (int)m_tools.size() );
                    g_nUpdateBits = -1;
                    return true;
                }
                m_stage = KBOOL_PICK_TOOL;
                m_tools.clear();
                m_hover = 0;
                // A live-stage click COULD have parked the gesture before this round
                // (stage 2 was not a click tool then), and a PAUSED command receives
                // no MouseMove — the solid hover would be dead and the next click
                // would be eaten as a resume.  Kept, and now belt and braces: this
                // command WantsClicks in both stages since ITEM 9, so KiwiCmd_Pause
                // refuses it outright and there is nothing left to resume from.
                KiwiCmd_Resume();
                UpdateHud();
                Sys_Printf( "Boolean: tool dropped — click another solid (Esc again "
                            "cancels).\n" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x0D && m_stage == KBOOL_PICK_TOOL )  // VK_RETURN
            {
                Sys_Printf( "Boolean: click the other solid first.\n" );
                return true;
            }
            return false;
        }

        // PREVIEW.  DIFFERENCE draws the TOOL red and translucent over the targets
        // (Plasticity's phantom_red for the difference tool, BooleanFactory.ts:203);
        // UNION tints BOTH operands green, because a union has no carver and no
        // carved — the two are the same kind of thing.  Targets are outlined rather
        // than tinted in difference mode: they already carry the §18 selection tint,
        // and a second wash over it would only say "still selected".
        //
        // ── KIWI-UX (ROUND AA, ITEM 9): TWO SHARED BUDGETS, N TOOLS ─────────
        // Both the fill and the outline used to be sized by "one tool plus the
        // targets", and a tool SET breaks both assumptions:
        //
        //   * FILLS.  KBOOL_MAX_FILL (8) is a whole-frame budget, not a per-role
        //     one, because a fill costs one R_AddRenderCmdDrawTris PER FACE.  The
        //     TOOLS spend it FIRST — they are the operand the preview is about and
        //     the thing the user is actively building — and a union's targets take
        //     whatever is left.  Past the budget an operand is still OUTLINED, so
        //     nothing goes invisible; it just stops being tinted.
        //
        //   * OUTLINES.  The 192-segment line batch is shared with everything else
        //     the frame draws (kiwi_command.cpp KiwiCmd_DrawWorld) and a box costs
        //     24 segments, so it holds about EIGHT solids.  There is no per-role cap
        //     here because KiwiLines_Add already reports the budget honestly:
        //     OutlineBrush returns false the moment it cannot fit another segment
        //     and this bails.  The ORDER is what matters, and it is tools first for
        //     the same reason the fills are.
        //
        // The HOVER outline is drawn in BOTH stages now (it used to be stage 1
        // only), because in the live stage it is what says which solid the next
        // Shift+click will take.  It is drawn LAST and only when the hovered solid
        // is not already a tool, so it never overpaints an operand's own colour.
        //
        // ═════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AF, ITEM 3) — THE PREVIEW THAT DIED AT TWELVE TOOLS
        // ═════════════════════════════════════════════════════════════════════
        // USER REPORT, verbatim: "When adding a bunch of objects to a boolean
        // (windows on a building), After about 10-12, the red previewer stopped
        // working on newly selected diff tools.  I still clicked all of them and the
        // operation worked as expected, but the previewer always broke."
        //
        // BOTH round-AA budgets above were sized against ONE tool and both fired,
        // in pick order, so the operand that vanished was always the newest one:
        //   * the FILL cliff at 8 (KBOOL_MAX_FILL), and
        //   * the OUTLINE cliff at 288 / 24 = TWELVE (the shared line batch), which
        //     is the count the report names to the brush.
        // Worse, the outline arm `return`ed on the first refusal, which also killed
        // the HOVER outline — so past twelve tools the editor stopped saying what
        // the next click would even take.
        //
        // THREE CHANGES, and none of them is "cap it and hope":
        //   1. THE BATCH SCALES.  LineBudget() below asks the framework for exactly
        //      what this state costs, up to KCMD_LINE_BUDGET_MAX (1536) — ~60 boxes,
        //      which is past any hand-built boolean.
        //   2. NEWEST FIRST, both budgets.  Whatever the ceiling eventually drops is
        //      then the OLDEST operand, which is the one already understood, instead
        //      of the one the user is looking at.
        //   3. IT SAYS SO.  A degrade prints ONE console line per gesture naming the
        //      counts, and the hover outline is RESERVED for out of the budget so it
        //      can never be the thing that falls off the end.
        void DrawWorld() override
        {
            const bool hoverOn = ( m_hover && Sel_BrushLive( m_hover )
                                && !IsTool( m_hover ) && m_hover->def );
            const int  reserve = hoverOn ? OutlineCost( m_hover->def ) : 0;

            int fillDropped = 0;
            int lineDropped = 0;

            if ( m_stage == KBOOL_LIVE && !m_tools.empty() )
            {
                int         filled = 0;
                const bool  uni  = ( m_op == KBOOL_UNION );
                const float *fill = uni ? KBOOL_UNION_RGBA : KBOOL_DIFF_RGBA;
                const float *line = uni ? KBOOL_UNION_LINE : KBOOL_DIFF_LINE;

                // NEWEST FIRST — see change 2 above.  `i` walks down from size(),
                // so `i - 1` is the tool the last click added.
                for ( size_t i = m_tools.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_tools[i] ) )
                        continue;
                    if ( filled >= KBOOL_MAX_FILL ) { ++fillDropped; continue; }
                    FillBrush( m_tools[i]->def, fill );
                    ++filled;
                }
                if ( uni )
                {
                    for ( size_t i = m_targets.size(); i-- > 0; )
                    {
                        if ( !Sel_BrushLive( m_targets[i] ) )
                            continue;
                        if ( filled >= KBOOL_MAX_FILL ) { ++fillDropped; continue; }
                        FillBrush( m_targets[i]->def, fill );
                        ++filled;
                    }
                }

                KiwiLines_Color( line[0], line[1], line[2] );
                for ( size_t i = m_tools.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_tools[i] ) || !m_tools[i]->def )
                        continue;
                    // RESERVE, do not discover.  Stopping BEFORE the batch is dry is
                    // what keeps the hover outline drawable and what makes the drop
                    // countable instead of silent.
                    if ( KiwiLines_Remaining() - reserve < OutlineCost( m_tools[i]->def ) )
                        { ++lineDropped; continue; }
                    OutlineBrush( m_tools[i]->def );
                }
                for ( size_t i = m_targets.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_targets[i] ) || !m_targets[i]->def )
                        continue;
                    if ( KiwiLines_Remaining() - reserve < OutlineCost( m_targets[i]->def ) )
                        { ++lineDropped; continue; }
                    OutlineBrush( m_targets[i]->def );
                }
            }

            if ( hoverOn )
            {
                KiwiLines_Color( KBOOL_HOT_LINE[0], KBOOL_HOT_LINE[1], KBOOL_HOT_LINE[2] );
                OutlineBrush( m_hover->def );
            }

            NoteDegraded( fillDropped, lineDropped );
        }

        // ROUND AF, ITEM 3: what this gesture's overlay costs RIGHT NOW.  The
        // framework takes max( this, KCMD_LINE_BUDGET ) clamped to
        // KCMD_LINE_BUDGET_MAX (kiwi_command.cpp KiwiCmd_DrawWorld), so a one-tool
        // boolean is byte-for-byte the batch it always had and a 40-window facade
        // gets the batch IT needs instead of the batch a two-operand gesture needed.
        //
        // Measured, not estimated: OutlineCost walks the same windings OutlineBrush
        // will.  It runs once per frame over a set of tens, which is nothing next to
        // the fills it sits beside.
        int LineBudget() const override
        {
            int n = KBOOL_LINE_HEADROOM;
            if ( m_stage == KBOOL_LIVE )
            {
                for ( size_t i = 0; i < m_tools.size(); ++i )
                    if ( Sel_BrushLive( m_tools[i] ) )
                        n += OutlineCost( m_tools[i]->def );
                for ( size_t i = 0; i < m_targets.size(); ++i )
                    if ( Sel_BrushLive( m_targets[i] ) )
                        n += OutlineCost( m_targets[i]->def );
            }
            if ( m_hover && Sel_BrushLive( m_hover ) )
                n += OutlineCost( m_hover->def );
            return n;
        }

        void Commit() override
        {
            if ( m_stage != KBOOL_LIVE || m_tools.empty() )
            {
                Sys_Printf( "Boolean: no tool solid was picked — nothing was done.\n" );
                Reset();
                return;
            }
            if ( m_op == KBOOL_UNION )
                DoUnion();
            else
                DoDifference();
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Nothing to restore: the preview is a DRAW, every carve is built out of
            // unlanded defs inside Commit, and no bracket exists until then.
            Reset();
            g_nUpdateBits |= 1;
        }

    private:
        void Reset()
        {
            m_targets.clear();
            m_tools.clear();
            m_stage  = KBOOL_PICK_TOOL;
            m_op     = KBOOL_DIFFERENCE;
            m_hover  = 0;
            m_hud[0] = '\0';
            m_notedDegrade = false;     // ROUND AF, ITEM 3
        }

        bool IsTarget( const selbrush_t *b ) const
        {
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( m_targets[i] == b )
                    return true;
            return false;
        }

        // ROUND AA, ITEM 9.  The set is small (a handful of cracks) and is walked
        // per click and per marquee hit, so a linear scan is the right shape — the
        // same one IsTarget has always been.
        bool IsTool( const selbrush_t *b ) const
        {
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( m_tools[i] == b )
                    return true;
            return false;
        }

        // ── KIWI-UX (ROUND AF, ITEM 3): NO SILENT TRUNCATION ────────────────
        // The round-AA rule this round exists to enforce: a preview that stops
        // drawing part of what the command will DO must say so.  ONE line per
        // gesture — this is a per-FRAME call site, so a latch is not an optimisation
        // here, it is the difference between a note and a 60 Hz spam.  Reset() clears
        // it, so a fresh boolean can report again; a gesture that grows further past
        // the ceiling does not re-print, because the sentence it would print is the
        // same one and the state it describes has not changed KIND.
        void NoteDegraded( int fillDropped, int lineDropped )
        {
            if ( ( !fillDropped && !lineDropped ) || m_notedDegrade )
                return;
            m_notedDegrade = true;
            Sys_Printf( "Boolean: %i operand(s) are outlined but not tinted and %i "
                        "are not drawn at all — the preview budget is full.  The "
                        "NEWEST operands are the ones you can see; every operand in "
                        "the set is still applied on commit.\n",
                        fillDropped, lineDropped );
        }

        // ── DIFFERENCE: build every carve first, land nothing until they are all
        //    built and gated (kiwi_boolean.h UNDO).
        void DoDifference()
        {
            struct carve_t
            {
                selbrush_t            *node;
                std::vector<brush_t *> pieces;
            };
            std::vector<carve_t> carves;
            int missed  = 0;
            int refused = 0;

            // ── ROUND AA, ITEM 9: the tool SET, resolved ONCE ────────────────
            // Every liveness test the tools need happens HERE and nowhere else, so
            // the carve loop below is a pure computation over a fixed array and can
            // never trip over a node that died between two targets.  A tool whose
            // instance is dead is silently dropped rather than being an error: the
            // gesture spans arbitrary user time and this is the same courtesy the
            // target loop has always extended to its own list.
            std::vector<selbrush_t *> tools;
            std::vector<brush_t *>    toolDefs;
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( Sel_BrushLive( m_tools[i] ) && m_tools[i]->def )
                {
                    tools.push_back( m_tools[i] );
                    toolDefs.push_back( m_tools[i]->def );
                }
            if ( toolDefs.empty() )
            {
                Sys_Printf( "Boolean: no tool solid is still there — nothing was "
                            "done.\n" );
                return;
            }

            // ROUND AO, ITEM 3: zeroed immediately before the loop that can raise
            // it, so the count reported below is this gesture's and nothing else's.
            // KIWI-UX (ROUND AQ, ITEM 7): …and so are the refusal count and the
            // latched miss reason, which is what KiwiBool_ResetCarveReport is.
            KiwiBool_ResetCarveReport();

            for ( size_t i = 0; i < m_targets.size(); ++i )
            {
                selbrush_t *node = m_targets[i];
                if ( !Sel_BrushLive( node ) || IsTool( node ) )
                    continue;

                // Built IN PLACE so the piece vector is never copied and never has
                // two owners; CarveTarget leaves it empty on every refusal, so a
                // pop_back cannot strand a def.
                carves.push_back( carve_t() );
                carves.back().node = node;

                const char *why = "unknown";
                const carveResult_t r = CarveTarget( node->def, &toolDefs[0],
                                                     (int)toolDefs.size(),
                                                     &carves.back().pieces, &why );
                if ( r == KBOOL_CARVED )
                    continue;

                carves.pop_back();
                if ( r == KBOOL_MISS )
                {
                    ++missed;
                    // KIWI-UX (ROUND AQ, ITEM 7): say WHY it missed.  "did not meet
                    // the tool" was the entire diagnosis a mapper got, and for an
                    // arch it is the wrong one nine times out of ten — the tool DOES
                    // meet the box, it grazes it, and the gate that refused the
                    // shard is the thing worth naming.
                    Sys_Printf( "Boolean: one brush left untouched — %s.\n",
                                ( why && *why ) ? why : "the tool(s) did not reach it" );
                }
                else
                {
                    ++refused;
                    Sys_Printf( "Boolean: one brush left untouched — %s.\n",
                                why ? why : "invalid geometry" );
                }
            }

            if ( carves.empty() )
            {
                // NO BRACKET AT ALL when nothing changed — an empty undo record is a
                // Ctrl+Z that does nothing (kiwi_command.h rule).
                Sys_Printf( "Boolean: nothing was carved (%i brush(es) did not meet "
                            "the tool(s), %i refused).\n", missed, refused );
                return;
            }

            // ROUND AO, ITEM 3: a dropped sliver is BELOW §19 and therefore below
            // anything the mapper could have selected, but it is still geometry that
            // was thrown away and the editor says so rather than swallowing it.
            if ( s_carveSliverDrops > 0 )
                Sys_Printf( "Boolean: %i fragment(s) came out below the validity gate "
                            "(zero-area / collapsed) and were dropped rather than "
                            "refusing the whole carve.\n", s_carveSliverDrops );
            // KIWI-UX (ROUND AQ, ITEM 7): the tally for the per-cut skips, so the
            // summary line exists even when every individual line was suppressed.
            if ( s_carveRefusals > 0 )
                Sys_Printf( "Boolean: %i individual cut(s) were refused and skipped — "
                            "the hole may be incomplete where those planes fell.\n",
                            s_carveRefusals );
            // KIWI-UX (ROUND AR, ITEM 2): the carried intermediates.  Nothing
            // invalid was landed; this says the carve had to work through a §19
            // verdict on a piece that is never a brush.
            if ( s_carveCarried > 0 )
                Sys_Printf( "Boolean: %i running intersection(s) were outside the "
                            "validity gate and were carried on anyway (they are "
                            "intermediates and are never landed).\n", s_carveCarried );

            // The typed selection is about to name freed brushes, so it is dropped
            // BEFORE the mutation — kiwi_join.cpp's own rule, for the same reason.
            // The LEGACY list is left alone: it is what the bracket head clones.
            Sel_Clear( KiwiSel() );

            KiwiCmd_UndoBegin( "boolean difference" );

            // ── KIWI-UX (ROUND N): THE TOOL IS CONSUMED TOO ──────────────────
            // USER DIRECTIVE, verbatim: "When doing a difference command, it needs
            // to delete the original solid(Brush) along with the 'tool' solid.
            // Same with boolean, it should leave only 1 (if it doesn't already)."
            //
            // This OVERRULES round L's deliberate keep-the-tool deviation
            // (kiwi_boolean.h argued a reusable cutter was worth more than the
            // Plasticity behaviour).  The user has the editor; the user decides.
            //
            // COVERING IT FOR UNDO IS THE WHOLE SUBTLETY, and it is why the cover
            // is here rather than the Brush_Free being tacked onto the loop below.
            // The bracket head is Undo_AddBrushList( &selected_brushes )
            // (KiwiCmd_UndoBegin), and the tool is by construction NOT on that list:
            // it is picked with PICKF_EXCLUDE_SELECTED and refused if IsTarget.  So
            // nothing cloned it, and freeing it without this would make Ctrl+Z bring
            // the carved brushes back and leave the tool gone forever.  Undo_AddBrush
            // inside the open record is exactly the ported cover
            // (kiwi_transform.cpp:355 UndoCoverBrush, mirroring Undo_AddBrushList's
            // own per-element body) and Undo_Undo's phase 4 relinks the clone.
            //
            // The IsTarget guard is belt and braces: a tool that somehow WAS on the
            // selection is already cloned by the head, and covering it twice would
            // restore two copies of one brush.
            //
            // ROUND AA, ITEM 9: the cover is now a LOOP over the whole tool set.
            // Every clause above holds per tool and for the same reason — each one
            // was picked with PICKF_EXCLUDE_SELECTED (or through the marquee, which
            // filters IsTarget itself), so none of them is on the list the bracket
            // head cloned, and each is about to be freed.  `tools` was already
            // liveness-checked at the top of this function, so nothing here re-tests.
            for ( size_t t = 0; t < tools.size(); ++t )
            {
                if ( IsTarget( tools[t] ) )
                    continue;
                entity_s *owner = tools[t]->def->owner;
                if ( owner && owner->eclass && owner->eclass->fixedsize )
                    Undo_AddEntity( (int)(intptr_t)owner );
                Undo_AddBrush( (entity_brush_s *)tools[t]->def );
            }

            int landed = 0;
            for ( size_t c = 0; c < carves.size(); ++c )
            {
                carve_t &cv = carves[c];
                // ORDER (kiwi_split.h UNDO): land every piece FIRST, then free the
                // source, so the owner entity never transiently drops to zero
                // brushes.  The pieces carry the source's owner already —
                // Brush_SplitBrushByFace linked them to it.
                for ( size_t p = 0; p < cv.pieces.size(); ++p )
                {
                    selbrush_t *inst = Brush_AddToList( cv.pieces[p], cv.node->owner );
                    if ( inst->next || inst->prev )
                        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                    Brush_AddToList2( inst );
                    ++landed;
                }
                Brush_Free( cv.node );
            }

            // ORDER: the tools go LAST, after EVERY piece of EVERY target is landed
            // — the same rule the per-target loop follows one line up, and for the
            // same reason (an owner entity must never transiently run out of
            // brushes; a tool can share an owner with a target).  ROUND AA, ITEM 9
            // widens it from one free to a loop and the ordering argument is exactly
            // why the loop is HERE rather than folded into the landing loop above:
            // with several tools sharing one entity with several targets, freeing
            // any tool early is precisely the transient the rule forbids.
            //
            // Freed with Brush_Free, the INSTANCE free, exactly as the sources are:
            // Select_Delete would additionally free an owner entity that ran out of
            // brushes, which is a second, unasked-for edit and is not what "delete
            // the tool" means.
            for ( size_t t = 0; t < tools.size(); ++t )
                Brush_Free( tools[t] );
            m_tools.clear();

            Sys_Printf( "Boolean: difference — %i brush(es) carved into %i, and %i "
                        "tool(s) were consumed (%i missed, %i refused).\n",
                        (int)carves.size(), landed, (int)tools.size(),
                        missed, refused );
        }

        // ── UNION: the CLASSIC handler, over target(s) + tool ───────────────
        // This file owns NO bracket and NO geometry here.  CSG_Merge's contract is
        // "merge everything on selected_brushes" and it takes no arguments
        // (csg.cpp:572-579), so the only decision is what is selected when it runs;
        // driving that through the ported funnels and then dispatching the classic
        // id is kiwi_join.cpp's pattern verbatim, and it keeps ONE merge handler,
        // ONE undo record and ONE console report in the editor.
        //
        // EVERY REFUSAL IS CSG_Merge's OWN and is printed by it — fixed-size
        // entities, patches, brushes from different entities, and the big one: a
        // union whose hull is NOT CONVEX (Brush_MergeList returns NULL and the
        // originals are re-added, csg.cpp:620+).  Nothing here fakes a non-convex
        // union; a classic brush cannot be one.
        //
        // ROUND AA, ITEM 9: EVERY tool is selected, not just one, and the "is there
        // anything to union WITH" guard counts targets that are not tools — with a
        // set, `m_targets[i] != m_tool` was one comparison where N are needed, and a
        // single tool that also happened to be a target would have made the guard
        // pass on a selection of one brush.
        void DoUnion()
        {
            int live = 0;
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) && !IsTool( m_targets[i] ) )
                    ++live;
            if ( live < 1 )
            {
                Sys_Printf( "Boolean: nothing left to union with.\n" );
                return;
            }

            int tools = 0;
            Sel_Clear( KiwiSel() );
            Select_Deselect( 1 );
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) && !IsTool( m_targets[i] ) )
                    Select_Brush( m_targets[i], 0, 0, 0 );
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( Sel_BrushLive( m_tools[i] ) )
                {
                    Select_Brush( m_tools[i], 0, 0, 0 );
                    ++tools;
                }

            Sys_Printf( "Boolean: union of %i brush(es) + %i tool(s)...\n",
                        live, tools );
            Radiant_ExecCommand( (unsigned int)KBOOL_ID_MERGE );
        }

        void UpdateHud()
        {
            if ( m_stage == KBOOL_PICK_TOOL )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "boolean  %i solid(s) selected  %s", (int)m_targets.size(),
                           m_hover ? "click this solid to use as the tool"
                                   : "click the other solid" );
            }
            else
            {
                // ROUND AA, ITEM 9: "+ 1 tool" became "+ N tool(s)" — the count IS
                // the feedback for an accumulating gesture, and without it a
                // Shift+click that missed and one that landed look identical.
                _snprintf( m_hud, sizeof( m_hud ),
                           "boolean  %s  %i target(s) + %i tool(s)  (Q switches)",
                           ( m_op == KBOOL_UNION ) ? "UNION" : "DIFFERENCE",
                           (int)m_targets.size(), (int)m_tools.size() );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        enum stage_t { KBOOL_PICK_TOOL = 0, KBOOL_LIVE };
        enum op_t    { KBOOL_DIFFERENCE = 0, KBOOL_UNION };

        std::vector<selbrush_t *> m_targets;
        // ROUND AA, ITEM 9: `selbrush_t *m_tool` became a SET.  Order is the pick
        // order and it matters twice: Esc pops the LAST one, and the carve cascade
        // applies them in this order (which cannot change the RESULT — a difference
        // by t0 then t1 is the same volume as t1 then t0 — but does decide how the
        // surviving fragments are cut apart, and pick order is the one the mapper
        // can predict).
        std::vector<selbrush_t *> m_tools;
        selbrush_t *m_hover = 0;
        // ROUND AO, ITEM 3: the hover's REFUSAL REASON, latched beside it, so the
        // click can name a patch instead of saying "not a usable solid".
        bool        m_hoverIsPatch = false;
        stage_t     m_stage = KBOOL_PICK_TOOL;
        op_t        m_op    = KBOOL_DIFFERENCE;
        // ROUND AF, ITEM 3: the one-shot "the preview is degraded" latch.
        bool        m_notedDegrade = false;
        char        m_hud[160] = { 0 };
    };

    KiwiBooleanCommand s_boolean;
}

// ─── §3 canExecute ───────────────────────────────────────────────────────────
bool KiwiBool_CanBoolean()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( Usable( b ) )
            return true;
    return false;
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiBool_RegisterCommands()
{
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    Radiant_RegisterCommand( "KiwiBoolean", 0, 0, KIWI_CMD_BOOLEAN );
}

KiwiEditorCommand *KiwiBool_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_BOOLEAN )
        return &s_boolean;
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AF, ITEM 2 — THE CASCADE, LENT TO THE REGION EXTRUDE
// ═════════════════════════════════════════════════════════════════════════════
// See kiwi_boolean.h for the contract (who owns the tool, who owns the bracket)
// and for why this is the SAME difference rather than a second one.  Everything
// below is a driver over CarveTarget; not one line of the carve is re-derived.
namespace
{
    // Every CSG-usable brush on either display list, resolved ONCE, exactly as
    // DoDifference resolves its tool set — so the carve loop is a pure computation
    // over a fixed array and can never trip over a node freed between two targets.
    void CollectCarveTargets( const brush_t *toolDef, std::vector<selbrush_t *> *out )
    {
        out->clear();
        selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
        for ( int L = 0; L < 2; ++L )
        {
            selbrush_t *head = lists[L];
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !Usable( b ) || !b->def )
                    continue;
                if ( !BoundsOverlap( b->def, toolDef ) )
                    continue;
                out->push_back( b );
            }
        }
    }
}

bool KiwiBool_PointInSolid( const float p[3] )
{
    if ( !p )
        return false;
    selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
    for ( int L = 0; L < 2; ++L )
    {
        selbrush_t *head = lists[L];
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            const brush_t *def = b->def;
            if ( !Usable( b ) || !def || !def->faces )
                continue;
            // Bounds first: eight compares instead of faceCount plane evaluations
            // for the overwhelming majority that cannot contain the point.
            if ( p[0] < def->mins[0] || p[0] > def->maxs[0]
              || p[1] < def->mins[1] || p[1] > def->maxs[1]
              || p[2] < def->mins[2] || p[2] > def->maxs[2] )
                continue;
            bool inside = true;
            for ( int f = 0; f < def->faceCount && inside; ++f )
            {
                const plane_t &pl = def->faces[f].plane;
                // The editor's own convention: outward normals, interior at
                // n.p <= dist (kiwi_validity.h V6 states it in those words).  The
                // epsilon admits a point exactly ON a face, which is what a region
                // drawn on that face and pushed inward produces.
                const float d = pl.normal[0] * p[0] + pl.normal[1] * p[1]
                              + pl.normal[2] * p[2] - pl.dist;
                if ( d > 0.01f )
                    inside = false;
            }
            if ( inside )
                return true;
        }
    }
    return false;
}

bool KiwiBool_WouldCarve( brush_t *toolDef )
{
    if ( !toolDef )
        return false;
    std::vector<selbrush_t *> targets;
    CollectCarveTargets( toolDef, &targets );
    // KIWI-UX (ROUND AQ, ITEM 7): the round's per-cut reporting must not speak for
    // a DRY RUN — this function is asked once per hovered frame.
    struct quietScope_t { quietScope_t()  { s_carveQuiet = true;  }
                          ~quietScope_t() { s_carveQuiet = false; } } quiet;
    for ( size_t i = 0; i < targets.size(); ++i )
    {
        std::vector<brush_t *> pieces;
        const char *why = "unknown";
        brush_t *const tools[1] = { toolDef };
        const carveResult_t r = CarveTarget( targets[i]->def, tools, 1, &pieces, &why );
        // The DRY RUN allocates: CarveTarget clones, because that is how a
        // plane-defined brush is asked "what would be left".  Everything it made is
        // freed right here — nothing is landed and nothing is linked, so this is
        // exactly the "rejection is free" property kiwi_extrude.h relies on.
        FreeDefs( &pieces );
        if ( r == KBOOL_CARVED )
            return true;
    }
    return false;
}

int KiwiBool_DifferenceByDef( brush_t *toolDef, int *outPieces,
                              int *outMissed, int *outRefused )
{
    if ( outPieces )  *outPieces  = 0;
    if ( outMissed )  *outMissed  = 0;
    if ( outRefused ) *outRefused = 0;
    if ( !toolDef )
        return 0;

    struct carve_t
    {
        selbrush_t            *node;
        std::vector<brush_t *> pieces;
    };
    std::vector<carve_t>      carves;
    std::vector<selbrush_t *> targets;
    CollectCarveTargets( toolDef, &targets );

    int missed  = 0;
    int refused = 0;

    // BUILD EVERYTHING FIRST, land nothing — kiwi_boolean.h's UNDO rule, and the
    // reason a refusal here costs the map nothing.
    //
    // ROUND AO, ITEM 3: the cave carve gets the sliver tolerance for free (it is
    // the same CarveTarget), and its own zero + report so the two entry points
    // never report each other's drops.  KIWI-UX (ROUND AQ, ITEM 7): and the same
    // is true of the per-cut refusal account.
    KiwiBool_ResetCarveReport();

    for ( size_t i = 0; i < targets.size(); ++i )
    {
        selbrush_t *node = targets[i];
        if ( !Sel_BrushLive( node ) )
            continue;

        carves.push_back( carve_t() );
        carves.back().node = node;

        const char *why = "unknown";
        brush_t *const tools[1] = { toolDef };
        const carveResult_t r = CarveTarget( node->def, tools, 1,
                                             &carves.back().pieces, &why );
        if ( r == KBOOL_CARVED )
            continue;

        carves.pop_back();
        if ( r == KBOOL_MISS )
        {
            ++missed;
            // KIWI-UX (ROUND AQ, ITEM 7): name the gate here too — same report,
            // same reason, and the auto-carve path is where a mapper DRAGGING a
            // shape into a solid actually lands.
            Sys_Printf( "Carve: one brush left untouched — %s.\n",
                        ( why && *why ) ? why : "the tool did not reach it" );
        }
        else
        {
            // KBOOL_REFUSED includes "the tool swallows this brush whole", which for
            // a cavity means the prism is bigger than the thing it is being cut into.
            // That is a real refusal and it is reported, not silently obeyed —
            // deleting a brush the user was trying to dent is the worst possible
            // reading of the gesture.
            ++refused;
            Sys_Printf( "Carve: one brush left untouched — %s.\n",
                        why ? why : "invalid geometry" );
        }
    }

    if ( carves.empty() )
    {
        if ( outMissed )  *outMissed  = missed;
        if ( outRefused ) *outRefused = refused;
        return 0;
    }

    if ( s_carveSliverDrops > 0 )                 // ROUND AO, ITEM 3
        Sys_Printf( "Carve: %i fragment(s) came out below the validity gate and "
                    "were dropped rather than refusing the whole carve.\n",
                    s_carveSliverDrops );
    if ( s_carveRefusals > 0 )                    // KIWI-UX (ROUND AQ, ITEM 7)
        Sys_Printf( "Carve: %i individual cut(s) were refused and skipped — the "
                    "cavity may be incomplete where those planes fell.\n",
                    s_carveRefusals );
    if ( s_carveCarried > 0 )                     // KIWI-UX (ROUND AR, ITEM 2)
        Sys_Printf( "Carve: %i running intersection(s) were outside the validity "
                    "gate and were carried on anyway (intermediates, never "
                    "landed).\n", s_carveCarried );

    // ── COVER THE TARGETS FOR UNDO ──────────────────────────────────────────
    // The bracket head is Undo_AddBrushList( &selected_brushes ) (KiwiCmd_UndoBegin)
    // and a brush the prism happens to penetrate is NOT on that list in general, so
    // nothing cloned it.  Freeing it without this would make Ctrl+Z bring the carved
    // pieces back and leave the original gone forever.  Same pair, same order, as
    // kiwi_boolean.cpp's own tool cover (entity first).
    //
    // ALL of the covers run BEFORE any landing or freeing, so the record's brush
    // section is contiguous — undo.cpp:539 warns when brushes are added after an
    // entity, and interleaving cover/land/free per target would produce exactly that.
    for ( size_t c = 0; c < carves.size(); ++c )
    {
        entity_s *owner = carves[c].node->def->owner;
        if ( owner && owner->eclass && owner->eclass->fixedsize )
            Undo_AddEntity( (int)(intptr_t)owner );
        Undo_AddBrush( (entity_brush_s *)carves[c].node->def );
    }

    int landed = 0;
    for ( size_t c = 0; c < carves.size(); ++c )
    {
        carve_t &cv = carves[c];
        // ORDER (kiwi_split.h UNDO): land every piece FIRST, then free the source,
        // so the owner entity never transiently drops to zero brushes.
        for ( size_t p = 0; p < cv.pieces.size(); ++p )
        {
            selbrush_t *inst = Brush_AddToList( cv.pieces[p], cv.node->owner );
            if ( inst->next || inst->prev )
                Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
            Brush_AddToList2( inst );
            ++landed;
        }
        Brush_Free( cv.node );
    }

    if ( outPieces )  *outPieces  = landed;
    if ( outMissed )  *outMissed  = missed;
    if ( outRefused ) *outRefused = refused;
    return (int)carves.size();
}
