#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_command.cpp — RADIANT_UX_DESIGN §3 + §4 + §5 implementation.
// See kiwi_command.h for the registry rule, the reserved id range and the undo
// bracket protocol this file wraps.
//
// NEW code over the ported cores.  It ADDS rows to `g_radiantCommands` and a
// metadata side-table; it never duplicates dispatch and never edits ported logic.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_command.h"
#include "radiant_frame.h"          // KIWI-UX (CLEANUP, C-6): struct RadiantCommand + the table API
#include "kiwi_addmenu.h"                // §16b the Shift+A add menu
#include "kiwi_bevel.h"
#include "kiwi_boolean.h"                // ROUND L — Q (difference / union)
#include "kiwi_camera.h"                 // KiwiCam_FlySwallowKey (the fly-key funnel rung)
#include "kiwi_construct.h"
#include "kiwi_csg.h"
#include "kiwi_dupe.h"
#include "kiwi_extrude.h"
#include "kiwi_fillet.h"                 // round J — B  (fillet construction corners)
#include "kiwi_patchfillet.h"            // ROUND Q — B on brush EDGES (fillet -> patch)
#include "kiwi_focus.h"                  // round J — /  (frame the selection)
#include "kiwi_lines.h"
#include "kiwi_loft.h"                   // ROUND AF — L  (bridge two brush faces)
#include "kiwi_offset.h"                 // round J — O  (offset a construction chain)
#include "kiwi_visibility.h"             // round J — Ctrl+H (invert hidden)
#include "kiwi_conclip.h"                // ROUND AR, ITEM 1 — the construction clipboard
#include "kiwi_conselect.h"              // shakeout F — the construction selection
#include "kiwi_join.h"                   // shakeout G — J, the context verb
#include "kiwi_autobool.h"               // ROUND AB — Auto Bool (consolidate brushes)
#include "kiwi_matchface.h"              // shakeout G — Z
#include "kiwi_trim.h"                   // shakeout H — T
#include "kiwi_split.h"                  // shakeout G — C (cut) + Ctrl+R (face split)
#include "kiwi_selext.h"
#include "kiwi_numeric.h"
#include "kiwi_palette.h"
#include "kiwi_patchverts.h"             // ROUND AJ, ITEM 1 — Esc leaves the mode
#include "kiwi_pick.h"
#include "kiwi_primitive.h"              // §16b Box / Cylinder / Sphere / Cone
#include "kiwi_region.h"                 // ROUND K — the region selection Esc drops
#include "kiwi_selconv.h"                // shakeout D — Ctrl+1..4 selection conversion
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_transform.h"
#include "kiwi_units.h"
#include "kiwi_ux.h"                     // KiwiUX_ModernInput (the paste/clone hook's gate)
#include "kiwi_undo.h"                   // shakeout I — the unified journal (cancel suppression)
#include "kiwi_uv.h"
#include "kiwi_entbrowser.h"            // ROUND AU — the entity browser + its deferred drop
#include "kiwi_skybox.h"                // ROUND AZ — the Sky tab + its two verbs
#include "kiwi_uveditor.h"              // KIWI-UX (ROUND BD) — the UV editor window row
#include "kiwi_import.h"                // KIWI-UX (ROUND BE) — texture import (drop + wizard)
#include "kiwi_launch.h"                // KIWI-UX (ROUND BF) — the Build & Run launch dialog
#include "kiwi_caulk.h"                 // KIWI-UX (ROUND BH) — Caulk Selection (End)
#include "kiwi_sun.h"                   // the sun helper — Place Sun + the Esc deselect
#include "kiwi_section.h"               // KIWI-UX (ROUND BM) — Section Analysis (the view-cube button)
#include "kiwi_outliner.h"              // ROUND W — the outliner's two group verbs
#include "kiwi_windows.h"

#include <string.h>

// ── ported entry points (verified against their definitions) ────────────────
extern int  Sys_Printf( const char *fmt, ... );                  // win_qe3.cpp:118 (undo.cpp:57 declares it the same way)
extern void Undo_ClearRedo();                                    // undo.cpp 0x45e2b0
// KIWI-UX (CLEANUP, UndoCoverBrush): the two the per-brush cover needs.
extern void Undo_AddBrush( entity_brush_s *pBrushInst );         // undo.cpp:494  (0x45E680)
extern void Undo_AddEntity( int a1 );                            // undo.cpp:601  (0x45E8B0)
extern void Undo_GeneralStart( const char *operation );          // undo.cpp 0x45e3f0
extern void Undo_AddBrushList( selbrush_t *sb );                 // undo.cpp 0x45e7c0
extern void Undo_EndBrushList( selbrush_t *brushlist );          // undo.cpp 0x45e870
extern void Undo_End();                                          // undo.cpp 0x45ea20
extern void Undo_Undo();                                         // undo.cpp 0x45ea90
extern int  g_nUpdateBits;                                       // 0x25D5A74 (mainfrm.cpp)
extern bool ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );   // imgui_shell.cpp

// mainfrm.cpp — the shared command table's append hook (// KIWI-UX there).
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

// ── ROUND N: reading the BINDING TABLE, not executing it ────────────────────
// The preempt rung has to answer "what would this chord run?" WITHOUT running it,
// because the decision (cancel the parked gesture, or swallow the key) has to be
// made before the chord is dispatched.  Radiant_TryHotkey (mainfrm.cpp:1497) is
// lookup-and-execute in one, so this reads the same table it walks.
//
// KIWI-UX (CLEANUP, C-6): the struct AND these declarations now live in
// radiant_frame.h, included above.  This file used to carry a verbatim copy of
// `struct RadiantCommand` plus its own externs, as six other TUs did.

namespace
{
    // ── §4 the single active-command owner ──────────────────────────────────
    KiwiEditorCommand *g_activeCommand = nullptr;
    snap_result_t      s_lastSnap;
    bool               s_undoOpen = false;

    // ── shakeout E: the HOT / PAUSED gesture state (kiwi_command.h) ─────────
    // A command starts HOT.  An LMB release parks it; any LMB press resumes it;
    // RMB-click or Enter confirms.  MULTI-CLICK tools never pause.
    bool s_hot = true;

    // ── shakeout G: the DEFERRED START (kiwi_command.h KiwiCmd_StartDeferred) ─
    // A command that wants to hand off to another one at the end of its own
    // gesture cannot simply call KiwiCmd_Start from inside Commit(): KiwiCmd_Commit
    // still has work to do afterwards — KiwiCmd_UndoCommit (which would close the
    // NEW command's bracket if it had opened one) and KiwiNum_Reset (which would
    // throw away the field table KiwiCmd_Start just installed for it).  So the
    // request is parked here and drained after all of that.
    int  s_deferId    = 0;
    bool s_deferPause = false;

    void DrainDeferred()
    {
        const int  id     = s_deferId;
        const bool paused = s_deferPause;
        s_deferId = 0;                        // cleared FIRST: Start may re-enter
        if ( !id || g_activeCommand )
            return;
        if ( KiwiCmd_Start( id ) && paused )
            KiwiCmd_Pause();
    }

    // ── KIWI: the DEFERRED DESELECT ──────────────────
    // Same shape and the same reason as the deferred start above: a command that
    // wants the selection dropped when its gesture ends cannot do it from inside
    // Commit(), because KiwiCmd_Commit still has to close the undo bracket and
    // KiwiCmd_UndoCommit's tail is `Undo_EndBrushList( &selected_brushes )` — it
    // reads the list to record the AFTER state of everything the head cloned.
    // Emptying that list first would hand the record an empty tail, and the undo
    // of a move that had just been confirmed would have nothing to restore.  So
    // the request is parked here and served after the bracket is closed.
    bool s_deferDeselect = false;

    void DrainDeferredDeselect()
    {
        if ( !s_deferDeselect )
            return;
        s_deferDeselect = false;
        // The ROUND K spelling (kiwi_transform.cpp deselectAfter), reused verbatim
        // so the selection epoch and the legacy globals move exactly as they do
        // after a confirmed face push.
        Sel_Clear( KiwiSel() );
        Sel_SyncToLegacy();
        g_nUpdateBits = -1;
    }

    // The one cursor source for the active gesture (see KiwiCmd_LastCursor).
    int  s_cursorX    = 0;
    int  s_cursorY    = 0;
    bool s_cursorHave = false;
    // ── KIWI-UX (ROUND AA, ITEM 9): the modifier that came WITH the press ────
    // Latched beside the pixel, in the same statement, because it is the same
    // fact about the same event (kiwi_command.h KiwiCmd_LastShift says why it is
    // a latch and not a Click() parameter).  Only the LMB path writes it —
    // KiwiCmd_MouseButton refuses every other button before this is reached.
    bool s_lastShift  = false;

    // ── §17 grid-step bounds for the modern [ ] keys ────────────────────────
    const float KGRID_MIN_INCHES = 0.125f;
    // ── KIWI-UX (ROUND X, ITEM 3): NO UPPER CAP ─────────────────────────────
    // USER DIRECTIVE, verbatim: "why does the inch snapping only go to 32? Weird!
    // You should have this all the way imo, fine control can be done with ctrl."
    //
    // The ceiling was 1024 and the ladder below stopped there with it.  Both move
    // to 65536 — half the engine's own ±131072 world bound (brush.cpp:1459), which
    // is the only number in this editor that is genuinely a limit.  The LOWER end
    // is untouched: the directive names only the top, and "fine control can be done
    // with ctrl" is a statement that the bottom is already covered (Ctrl suppresses
    // snapping outright — §6, kiwi_snap.cpp arm 0).
    const float KGRID_MAX_INCHES = 65536.0f;

    void SetModeMask( sel_mask_t m )
    {
        KiwiSel_SetModeMask( m );
        g_nUpdateBits |= 1;
    }

    // ── KIWI-UX (ROUND R): THE NICE-NUMBER LADDER ───────────────────────────
    // USER DIRECTIVE: "it should be whole numbers by default with the arrows."
    // Ascending, and the ONE table every stepper in the editor walks — the [ ] keys,
    // PageUp/PageDown and the top-right pill's [-] / [+] all reach it through
    // StepGrid.  Whole numbers from 1 up, in the sizes a level actually gets built
    // on: the powers of two a compiler-facing editor needs (8/16/32/64/128/256/512),
    // the decimal sizes a human thinks in (5/10/20/25/50/100/200/500/1000) and 1/2/4
    // for detail work.
    //
    // The three SUB-INCH rungs are NOT in the directive's list and are here on
    // purpose: KGRID_MIN_INCHES has been 0.125 since §17 shipped, and a ladder that
    // stopped at 1 would quietly delete the whole sub-inch range from the only
    // controls that reach it.  They are round numbers too, and a user who never
    // works below an inch never sees them.
    //
    // ── KIWI-UX (ROUND X, ITEM 3): THE LADDER KEEPS CLIMBING ────────────────
    // USER DIRECTIVE, verbatim: "why does the inch snapping only go to 32? Weird!
    // You should have this all the way imo, fine control can be done with ctrl."
    //
    // HONEST NOTE ON THE "32": as shipped in round R this table already ran to
    // 1024, and no cap at 32 could be found anywhere in the grid chain — not in
    // StepGrid, not in KiwiUnits_SetGridSpacingInches, not in KiwiGrid_Snap, not in
    // the pill (kiwi_viewcube.cpp) and not in the settings field (kiwi_ux.cpp).  So
    // rather than guess at a mechanism, ROUND X removes every upper bound there is:
    // the rungs continue past 1024 in the same mixed style (the powers of two a
    // compiler-facing editor needs, the decimals a human thinks in) and
    // KGRID_MAX_INCHES moves to the half-world-bound above.  Whatever the observed
    // stop was, there is now nothing above 32 for it to be.
    const float KCMD_GRID_LADDER[] =
    {
        0.125f, 0.25f, 0.5f,
        1.0f, 2.0f, 4.0f, 5.0f, 8.0f, 10.0f, 16.0f, 20.0f, 25.0f, 32.0f, 50.0f,
        64.0f, 100.0f, 128.0f, 200.0f, 256.0f, 500.0f, 512.0f, 1000.0f, 1024.0f,
        2000.0f, 2048.0f, 4096.0f, 5000.0f, 8192.0f, 10000.0f, 16384.0f,
        32768.0f, 65536.0f,
    };
    const int KCMD_GRID_LADDER_COUNT =
        (int)( sizeof( KCMD_GRID_LADDER ) / sizeof( KCMD_GRID_LADDER[0] ) );

    void StepGrid( bool doubleIt )
    {
        const float cur = KiwiUnits_GridSpacingInches();
        // A RELATIVE epsilon, so "the same rung" means the same thing at 0.125 and
        // at 1024 — an absolute one would make every small rung compare equal.
        const float eps = cur * 1.0e-3f;

        // STRICTLY past the current value, in the direction asked for.  Written as a
        // search rather than an index lookup because the current spacing may be OFF
        // the ladder entirely (it can be typed, and older sessions restore whatever
        // halving left in the registry) — and from an off-ladder value the honest
        // answer is the nearest rung on the far side, not a doubling.
        float s = cur;
        if ( doubleIt )
        {
            s = KCMD_GRID_LADDER[KCMD_GRID_LADDER_COUNT - 1];
            for ( int i = 0; i < KCMD_GRID_LADDER_COUNT; ++i )
                if ( KCMD_GRID_LADDER[i] > cur + eps ) { s = KCMD_GRID_LADDER[i]; break; }
        }
        else
        {
            s = KCMD_GRID_LADDER[0];
            for ( int i = KCMD_GRID_LADDER_COUNT - 1; i >= 0; --i )
                if ( KCMD_GRID_LADDER[i] < cur - eps ) { s = KCMD_GRID_LADDER[i]; break; }
        }

        if ( s < KGRID_MIN_INCHES ) s = KGRID_MIN_INCHES;
        if ( s > KGRID_MAX_INCHES ) s = KGRID_MAX_INCHES;
        KiwiUnits_SetGridSpacingInches( s );
        g_nUpdateBits = -1;
        Sys_Printf( "grid spacing %g in\n", (double)KiwiUnits_GridSpacingInches() );
    }

    // ── ROUND N: which VERBS may preempt a parked auto-entered gesture ───────
    // USER REPORT: Ctrl+R "still isn't in the editor".  See KiwiEditorCommand::
    // PreemptIdle (kiwi_command.h) for the full swallow chain; this is the
    // ALLOW-LIST half of it.
    //
    // It is an explicit list, NOT "every modal id", and that is the whole safety
    // argument: preempting means CANCELLING the gesture the user is in, so only a
    // verb that is ABOUT the thing the parked gesture is holding may do it.  All
    // six below are exactly that — they act on the FACE / SOLID selection that the
    // auto-enter produced and that is still intact after the cancel:
    //     Ctrl+R  Split Face      (the reported one)
    //     Z       Match Face
    //     E       Extrude Face    (the face -> new body arm)
    //     J       Join            (faces -> CSG_Merge; INSTANT, not modal)
    //     C       Cut             (needs the selected solid)
    //     Q       Boolean         (ditto)
    // Everything else — G/R/S, the creation chords, the view toggles, the palette —
    // is left to the existing swallow, because none of them is a statement about
    // the parked gesture's own subject and cancelling for them would be a surprise.
    // ── KIWI-UX (ROUND U): …AND THE CONVERSIONS AND THE CREATION CHORDS ──────
    // USER REPORTS, verbatim: "Still can't press Ctrl-2 on a face to get the edges
    // for that face." and "I should be able to press Shift-A (or similar) while a
    // face is selected and in the extrusion mode."
    //
    // Both are the SAME swallow round N fixed for six verbs and no more: in Face
    // mode a face selection IS a live PAUSED push/pull, so every chord that is not
    // on this list is eaten by KiwiCmd_KeyDown's catch-all rung.
    //
    // WHY THE FOUR CONVERSIONS QUALIFY under round N's own safety argument ("only a
    // verb that is ABOUT the thing the parked gesture is holding may cancel it"):
    // Ctrl+1..4 are statements about the FACE SELECTION itself — they replace it
    // with its points / edges / faces / owning objects (kiwi_selconv.cpp).  Nothing
    // is more about the parked gesture's subject than "convert that subject".
    // AND THE CONVERSION CANNOT RE-TRIGGER THE AUTO-ENTER: the face->edges arm
    // DESELECTS the face (Sel_Clear then Sel_Add of the edge items — kiwi_selconv.cpp
    // Convert), so afterwards there is no face context at all; the auto-enter lives
    // in kiwi_boxselect.cpp's ClickSelect and needs a CLICK, which a chord is not.
    //
    // WHY THE CREATION CHORDS QUALIFY, which is a wider claim and is made
    // deliberately: "draw a rectangle on the face I have selected" is the flow the
    // directive describes end to end (face -> Shift+A -> draw -> RMB -> region ->
    // E -> Q).  A creation chord while a face is parked can only mean "start
    // drawing", never "keep pushing this face", so cancelling the parked push is
    // the ONLY reading of it.  The cancel is the same provably record-free one
    // (PreemptIdle's gate), and the face STAYS SELECTED across it — which is what
    // makes KiwiCon_AutoPlaneForTool's new selected-face rung fire and put the
    // sketch ON the face.
    // Everything else — G/R/S, the view toggles, the palette — is still left to the
    // swallow, because none of them is a statement about the parked gesture.
    bool PreemptVerb( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_SPLIT_FACE:
        case KIWI_CMD_MATCH_FACE:
        case KIWI_CMD_EXTRUDE_FACE:
        case KIWI_CMD_JOIN:
        case KIWI_CMD_CUT:
        case KIWI_CMD_BOOLEAN:
        case KIWI_CMD_LOFT:              // ROUND AF — a verb ABOUT the parked face
        // ROUND U — the Ctrl+1..4 selection conversions (34100..34103).
        case KIWI_CMD_SELCONV_POINT:
        case KIWI_CMD_SELCONV_EDGE:
        case KIWI_CMD_SELCONV_FACE:
        case KIWI_CMD_SELCONV_OBJECT:
        // ROUND U — the WHOLE Shift-chord creation set (kiwi_keymap.cpp step 2),
        // plus the two curve tools that have no chord of their own and the add
        // menu, so the palette and radiant.ini routes behave identically.
        case KIWI_CMD_DRAW_LINE:
        case KIWI_CMD_DRAW_POLYLINE:
        case KIWI_CMD_DRAW_RECT:
        case KIWI_CMD_DRAW_RECT_CENTER:
        case KIWI_CMD_DRAW_CIRCLE:
        case KIWI_CMD_DRAW_CIRCLE_2PT:
        case KIWI_CMD_DRAW_ARC:
        case KIWI_CMD_DRAW_POLYGON:
        case KIWI_CMD_DRAW_SPLINE:
        case KIWI_CMD_PRIM_BOX:
        case KIWI_CMD_PRIM_BOX_CENTER:   // ROUND AF, ITEM 8
        case KIWI_CMD_PRIM_CYLINDER:
        case KIWI_CMD_PRIM_SPHERE:
        case KIWI_CMD_PRIM_CONE:
        case KIWI_CMD_ADD_MENU:
            return true;
        default:
            return false;
        }
    }

    // ── KIWI-UX (ROUND BR): WHICH OF THOSE ARE THE *CREATION* HALF ──────────
    // USER RULING: *"construction planes should only be made with [space],
    // usually"* — so the selected-face working plane (round U's exception) may no
    // longer be a standing rung of KiwiCon_AutoPlaneForTool.  It fires for THIS
    // gesture and no other: a CREATION chord pressed with a face parked, which is
    // the flow round U wrote the exception for ("face -> Shift+A -> draw"), and
    // which the rung above already recognises.  The face-context VERBS
    // (Ctrl+R / Z / E / J / C / Q / Loft) and the Ctrl+1..4 CONVERSIONS are not
    // creation and must not arm anything — none of them starts a drawing tool, and
    // the conversions deselect the face outright (kiwi_selconv.cpp Convert).
    //
    // Deliberately a SECOND switch rather than a flag threaded through the first:
    // the allow-list above is a safety argument about CANCELLING a parked gesture
    // and has nothing to do with planes.  Fusing them would make one edit to either
    // question silently answer the other.
    bool CreationVerb( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_DRAW_LINE:
        case KIWI_CMD_DRAW_POLYLINE:
        case KIWI_CMD_DRAW_RECT:
        case KIWI_CMD_DRAW_RECT_CENTER:
        case KIWI_CMD_DRAW_CIRCLE:
        case KIWI_CMD_DRAW_CIRCLE_2PT:
        case KIWI_CMD_DRAW_ARC:
        case KIWI_CMD_DRAW_POLYGON:
        case KIWI_CMD_DRAW_SPLINE:
        case KIWI_CMD_PRIM_BOX:
        case KIWI_CMD_PRIM_BOX_CENTER:
        case KIWI_CMD_PRIM_CYLINDER:
        case KIWI_CMD_PRIM_SPHERE:
        case KIWI_CMD_PRIM_CONE:
        case KIWI_CMD_ADD_MENU:
            return true;
        default:
            return false;
        }
    }

    // ── ROUND Z, ITEM 1: WHICH VERBS MAY SWAP INTO A LIVE GESTURE ───────────
    // USER REPORT, verbatim: "when pasting a brush, it goes into move mode
    // automatically, however if I want to paste and rotate(or similar) a brush, it
    // requires a de-selection first.  This is unacceptable, allow tool swaps."
    //
    // The THREE TRANSFORMS and nothing else, in any order (G->R, R->S, S->G, and
    // G->G to restart).  The safety argument is PreemptVerb's, sharpened: a
    // transform is a statement about the SELECTION, the selection is exactly what
    // survives both a Commit and a Cancel of another transform, and the incoming
    // verb's own canExecute is still asked before anything is torn down.
    //
    // The creation chords, the view toggles and the face-context verbs are NOT
    // here: the face-context set already has its own, narrower rung (PreemptVerb +
    // PreemptIdle) that only ever CANCELS, and widening this list to a verb that
    // does not act on the surviving selection would commit an edit to run something
    // unrelated to it.
    bool SwapVerb( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_MOVE:
        case KIWI_CMD_ROTATE:
        case KIWI_CMD_SCALE:
            return true;
        default:
            return false;
        }
    }

    // ROUND S: the §3 canExecute for the clipboard Cut.  Its own predicate rather
    // than a borrowed one: both ported halves (Copy's RadiantClipboard_Copy and
    // Cmd_OnSelectionDelete) walk `selected_brushes` and do nothing when it is
    // empty, so "is anything selected" IS the condition, and naming it here keeps
    // the palette row honest if either half's precondition ever changes.
    bool CanClipCut()
    {
        return selected_brushes.next != &selected_brushes;
    }

    // What (vk, mods) resolves to in the LIVE binding table, or 0.  First-match-wins
    // over row order, exactly as Radiant_TryHotkey (mainfrm.cpp:1508-1516) does —
    // deliberately the same walk, so this can never disagree with what the key will
    // actually run one rung later.
    int LookupBinding( unsigned int vk, unsigned int mods )
    {
        RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTableMutable( &table );
        if ( !table || count <= 0 )
            return 0;
        for ( int i = 0; i < count; ++i )
            if ( table[i].vk == vk && table[i].mods == mods )
                return table[i].commandId;
        return 0;
    }

    // ── §3 metadata, keyed by command id ────────────────────────────────────
    // A PARALLEL table: it never carries a binding or a handler, only the palette
    // facts.  Ids with no row fall back to the g_radiantCommands name + "Classic".
    bool Can_HaveSelection()
    {
        return !KiwiSel().items.empty();
    }

    const struct { int id; kiwiCommandInfo_t info; } KCMD_META[] =
    {
        // ── the KIWI range ──────────────────────────────────────────────────
        { KIWI_CMD_SELMODE_POINT,  { "Select Mode: Point",        "Selection", SEL_MASK_VERTEX,     nullptr } },
        { KIWI_CMD_SELMODE_EDGE,   { "Select Mode: Edge",         "Selection", SEL_MASK_EDGE,       nullptr } },
        { KIWI_CMD_SELMODE_FACE,   { "Select Mode: Face",         "Selection", SEL_MASK_FACE,       nullptr } },
        { KIWI_CMD_SELMODE_OBJECT, { "Select Mode: Object",       "Selection", SEL_MASK_OBJECT,     nullptr } },
        { KIWI_CMD_SELMODE_ALL,    { "Select Mode: Everything",   "Selection", SEL_MASK_EVERYTHING, nullptr } },
        { KIWI_CMD_PALETTE,        { "Command Palette",           "KIWI",      0,                   nullptr } },
        { KIWI_CMD_GRID_HALVE,     { "Grid Spacing: Halve",       "Grid",      0,                   nullptr } },
        { KIWI_CMD_GRID_DOUBLE,    { "Grid Spacing: Double",      "Grid",      0,                   nullptr } },
        { KIWI_CMD_SNAP_TOGGLE,    { "Snap Markers: Toggle",      "Grid",      0,                   nullptr } },
        { KIWI_CMD_SELFTEST,       { "UX: Modal Self-Test",       "KIWI",      0,                   nullptr } },

        // ── §13 / §20 / §21 / §22 direct manipulation (kiwi_transform.cpp) ──
        // Move's selKindMask is EVERYTHING because G is context-aware: it moves
        // whichever kind dominates the selection.  Rotate/Scale are whole-object
        // only in v1, which is exactly what spec §13's table says.
        { KIWI_CMD_MOVE,   { "Move (G)",   "Transform", SEL_MASK_EVERYTHING, KiwiXform_CanMove   } },
        { KIWI_CMD_ROTATE, { "Rotate (R)", "Transform", SEL_MASK_OBJECT,     KiwiXform_CanRotate } },
        { KIWI_CMD_SCALE,  { "Scale (S)",  "Transform", SEL_MASK_OBJECT,     KiwiXform_CanScale  } },

        // ── §7 / §16 / §23 construction geometry (kiwi_construct / kiwi_extrude).
        // selKindMask 0 throughout: construction geometry lives OUTSIDE the typed
        // selection in v1 (kiwi_construct.h scope ruling 1), so no selection kind
        // makes these more or less meaningful.
        // ROUND P: both ids run the ONE chained curve tool (kiwi_construct.cpp).
        // KIWI-UX (CLEANUP, A-69): canExecute 0 rather than KiwiCon_CanDraw, which
        // was a predicate that could only answer true.  KiwiCmd_CanExecute treats a
        // null pointer as "always executable" (:815), so this is the same answer
        // with nothing to maintain.
        { KIWI_CMD_DRAW_LINE,       { "Construct: Line (chained curve)", "Construct", 0, 0                 } },
        { KIWI_CMD_DRAW_POLYLINE,   { "Construct: Polyline (= Line)",    "Construct", 0, 0                 } },
        { KIWI_CMD_DRAW_RECT,       { "Construct: Rectangle",        "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_CIRCLE,     { "Construct: Circle",           "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_ARC,        { "Construct: Arc",              "Construct", 0, 0                     } },
        // §16b (shakeout C): the rest of the Plasticity curve inventory, plus the
        // four SOLID primitives and the add menu that lists all of them.
        { KIWI_CMD_DRAW_RECT_CENTER,{ "Construct: Rectangle (center)","Construct",0, 0                     } },
        { KIWI_CMD_DRAW_CIRCLE_2PT, { "Construct: Circle (2-point)", "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_POLYGON,    { "Construct: Polygon (n-gon)",  "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_SPLINE,     { "Construct: Spline",           "Construct", 0, 0                     } },
        { KIWI_CMD_PRIM_BOX,        { "Box (corner)",                "Solids",    0, nullptr               } },
        // ROUND AF, ITEM 8 — Plasticity ships corner-box and centre-box as two
        // commands (BoxCommand.ts:60 / :155), so the palette lists two rows.
        { KIWI_CMD_PRIM_BOX_CENTER, { "Box (centre)",                "Solids",    0, nullptr               } },
        { KIWI_CMD_PRIM_CYLINDER,   { "Cylinder",                    "Solids",    0, nullptr               } },
        { KIWI_CMD_PRIM_SPHERE,     { "Sphere",                      "Solids",    0, nullptr               } },
        { KIWI_CMD_PRIM_CONE,       { "Cone",                        "Solids",    0, nullptr               } },
        { KIWI_CMD_ADD_MENU,        { "Add Menu (create)",           "Construct", 0, nullptr               } },
        { KIWI_CMD_VIEW_SHOW_GRID,  { "Show Grid",                   "View",      0, nullptr               } },
        { KIWI_CMD_VIEW_SHOW_AXES,  { "Show Axes",                   "View",      0, nullptr               } },
        // ROUND M: the projection toggle.  Named for the ACTION, not the state —
        // the pill under the view cube is where the state is read (kiwi_viewcube.h).
        { KIWI_CMD_VIEW_ORTHO,      { "Toggle Orthographic Camera",  "View",      0, nullptr               } },
        { KIWI_CMD_EXTRUDE_REGION,  { "Extrude Region",              "Construct", 0, KiwiExtrude_CanExecute } },
        // ── shakeout G: the Plasticity modelling verbs ──────────────────────
        // Masks are declarative (the palette greys on the PREDICATE — kiwi_command.h
        // says so at KIWI_CMD_BEVEL_EDGE): Cut wants a solid AND a construction
        // line, so its mask is OBJECT and its predicate carries the rest; the other
        // three are face verbs outright.  E's row is the FACE arm's, because that
        // is the one the key resolves to whenever a face exists — the region arm
        // keeps its own row above.
        { KIWI_CMD_CUT,             { "Cut (along a line)",          "Modeling", SEL_MASK_OBJECT, KiwiSplit_CanCut } },
        // ROUND L: the Q boolean.  Mask OBJECT and the predicate carries the rest
        // (>= 1 CSG-usable solid); the TOOL is picked inside the gesture, so it is
        // not a precondition and must not grey the row.
        { KIWI_CMD_BOOLEAN,         { "Boolean (difference / union)","Modeling", SEL_MASK_OBJECT, KiwiBool_CanBoolean } },
        // ROUND AF, ITEM 6: LOFT.  Mask FACE and the predicate asks for ONE usable
        // face, not two — the second is picked inside the gesture, exactly as the
        // boolean's tool is, so requiring it here would grey the row in the state
        // the command is designed to be started from (kiwi_loft.h THE GRAMMAR).
        { KIWI_CMD_LOFT,            { "Loft (bridge two faces)",     "Modeling", SEL_MASK_FACE,   KiwiLoft_CanExecute } },
        { KIWI_CMD_MATCH_FACE,      { "Match Face",                  "Modeling", SEL_MASK_FACE,   KiwiMatch_CanMatch } },
        // ROUND S: renamed to say what it DOES.  A convex plane-brush cannot carry
        // a divided face, so "split face" always meant "split the brush along that
        // face's line" — kiwi_split.h spells the limitation out.
        { KIWI_CMD_SPLIT_FACE,      { "Split Brush at Face (Ctrl+R)", "Modeling", SEL_MASK_FACE,  KiwiSplit_CanSplitFace } },
        { KIWI_CMD_EXTRUDE_FACE,    { "Extrude Face (new body)",     "Modeling", SEL_MASK_FACE,   KiwiExtrudeFace_CanExecute } },
        { KIWI_CMD_JOIN,            { "Join (faces / lines)",        "Modeling", 0,               KiwiJoin_CanJoin } },
        // ROUND AB: Auto Bool.  Mask OBJECT; the predicate carries the rest (>= 2
        // same-entity, non-patch brushes) so the row greys for exactly the selections
        // the ported CSG_Merge would refuse outright.
        { KIWI_CMD_AUTO_BOOL,       { "Auto Bool (consolidate brushes)", "Modeling", SEL_MASK_OBJECT, KiwiAutoBool_CanExecute } },
        // ── shakeout H: TRIM (kiwi_trim.h).  selKindMask 0 — it reads the
        // CONSTRUCTION store and no sel_kind_t makes the row more meaningful.
        { KIWI_CMD_TRIM,            { "Trim (lines)",                "Construct", 0,               KiwiTrim_CanTrim } },
        // ── ROUND J: the two construction-curve editors.  selKindMask 0 on both
        // — they read the CONSTRUCTION selection (kiwi_conselect.h), which no
        // sel_kind_t describes, so the predicate carries the whole precondition.
        { KIWI_CMD_OFFSET_CURVE,    { "Offset Curve",                "Construct", 0,               KiwiOffset_CanOffset } },
        { KIWI_CMD_FILLET_CURVE,    { "Fillet Corners",              "Construct", 0,               KiwiFillet_CanFillet } },
        // ── ROUND Q: the SOLID fillet — a chamfer plus a q3 bezier patch in the
        // notch (kiwi_patchfillet.h).  "Modeling", not "Construct": it edits a
        // brush and creates a patch.  Bare B redirects here when brush edges are
        // selected; this row is how the palette reaches it by name.
        // ROUND T: ONE tool now — the row below starts it in FILLET intent (the
        // command still opens as a chamfer; D is the toggle), and KIWI_CMD_BEVEL_EDGE
        // is the same command reached by its chamfer name.  Both rows survive so a
        // user searching the palette for either word finds it.
        { KIWI_CMD_FILLET_EDGE,     { "Bevel / Fillet Edge (D toggles)", "Modeling", SEL_MASK_EDGE, KiwiPatchFillet_CanFillet } },
        { KIWI_CMD_CPLANE_XY,       { "Construction Plane: XY",      "Construct", 0, nullptr               } },
        { KIWI_CMD_CPLANE_XZ,       { "Construction Plane: XZ",      "Construct", 0, nullptr               } },
        { KIWI_CMD_CPLANE_YZ,       { "Construction Plane: YZ",      "Construct", 0, nullptr               } },
        { KIWI_CMD_CPLANE_FACE,     { "Construction Plane: From Face","Construct",0, nullptr               } },
        { KIWI_CMD_CPLANE_VIEW,     { "Construction Plane: From View","Construct",0, nullptr               } },
        { KIWI_CMD_CONSTRUCT_CLEAR, { "Construction: Clear All",     "Construct", 0, KiwiCon_HasObjects    } },
        // SHAKEOUT I: renamed, because the command IS the unified undo now
        // (kiwi_construct.cpp's dispatch arm forwards to KiwiUndo_Undo) and a
        // palette row that still said "Construction:" would send a user looking for
        // a construction-only undo that no longer exists.
        { KIWI_CMD_CONSTRUCT_UNDO,  { "Undo (unified timeline)",     "Construct", 0, nullptr               } },

        // ── §25 Phase 5: modeling power ─────────────────────────────────────
        // Bevel wants EDGES and Inset wants FACES, so their masks say so.  The
        // palette does not filter on the mask today (it only greys on
        // canExecute — kiwi_palette.cpp:114), so the mask is declarative: it is
        // the machine-readable form of "this needs an edge selection", ready for
        // the mode-aware row filter and for the gizmo's context menu.  The
        // GREYING that users actually see comes from the predicates.
        { KIWI_CMD_BEVEL_EDGE,      { "Bevel Edge (chamfer)",        "Modeling", SEL_MASK_EDGE,   KiwiBevel_CanBevel } },
        { KIWI_CMD_INSET_FACE,      { "Inset Face (clone)",          "Modeling", SEL_MASK_FACE,   KiwiBevel_CanInset } },
        { KIWI_CMD_ARRAY_LINEAR,    { "Array (Linear)",              "Duplicate", SEL_MASK_OBJECT, KiwiDupe_CanArray } },
        { KIWI_CMD_ARRAY_RADIAL,    { "Array (Radial)",              "Duplicate", SEL_MASK_OBJECT, KiwiDupe_CanArray } },
        // ── ROUND J ────────────────────────────────────────────────────────
        { KIWI_CMD_DUPLICATE,       { "Duplicate (and move)",        "Duplicate", SEL_MASK_OBJECT, KiwiDupe_CanDuplicate } },

        // ── §25 selection expansion (kiwi_selext.cpp) ───────────────────────
        { KIWI_CMD_SELECT_COPLANAR, { "Select Coplanar Faces",       "Selection", SEL_MASK_FACE,   KiwiSelExt_CanCoplanar } },
        { KIWI_CMD_SELECT_TOUCHING, { "Select Touching",             "Selection", 0,               KiwiSelExt_CanTouching } },
        { KIWI_CMD_SELECT_MATERIAL, { "Select Same Material",        "Selection", 0,               KiwiSelExt_CanMaterial } },
        { KIWI_CMD_SELECT_CONNECTED,{ "Select Connected (touching)", "Selection", 0,               KiwiSelExt_CanConnected } },

        // ── shakeout D: selection conversion (kiwi_selconv.cpp) ─────────────
        // selKindMask is EVERYTHING on all four: each converts FROM any kind, so
        // no single source kind makes a row more or less meaningful.  The one
        // real precondition — something is selected — is the predicate.
        { KIWI_CMD_SELCONV_POINT,   { "Convert Selection to Points",  "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },
        { KIWI_CMD_SELCONV_EDGE,    { "Convert Selection to Edges",   "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },
        { KIWI_CMD_SELCONV_FACE,    { "Convert Selection to Faces",   "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },
        { KIWI_CMD_SELCONV_OBJECT,  { "Convert Selection to Objects", "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },

        // ── shakeout F: the construction selection's verbs (kiwi_conselect.h) ──
        // selKindMask 0: these read the PARALLEL construction list, not the typed
        // selection, so no sel_kind_t makes either row more or less meaningful.
        { KIWI_CMD_CONSTRUCT_JOIN,  { "Join Lines",                   "Construct", 0, KiwiConSel_CanJoin } },
        { KIWI_CMD_CONSTRUCT_DELETE,{ "Construction: Delete Selected","Construct", 0, KiwiConSel_CanDelete } },
        // ROUND U — the construction hide family.  "(unhide all) in the search
        // menu" is the directive's own words for the second row; the first is
        // listed too so H is discoverable without knowing the key.
        { KIWI_CMD_CONSTRUCT_HIDE,  { "Hide Selected Lines (H)",      "Construct", 0, KiwiConSel_CanHide } },
        { KIWI_CMD_CONSTRUCT_UNHIDE,{ "Unhide All (construction)",    "Construct", 0, KiwiCon_HasHidden  } },

        // ── ROUND W: the OUTLINER's two group verbs (kiwi_outliner.h) ───────
        // selKindMask SEL_MASK_OBJECT on both: a group is made of WHOLE objects,
        // and a face or an edge selection has nothing a func_group could hold.
        // The predicates are the real preconditions — "is anything groupable
        // selected" and "does the selection belong to a group" — so the rows grey
        // out rather than printing a refusal after the fact.
        { KIWI_CMD_GROUP_CREATE,    { "Group Selection",              "Selection", SEL_MASK_OBJECT, KiwiOutliner_CanGroup   } },
        { KIWI_CMD_GROUP_UNGROUP,   { "Ungroup Selection",            "Selection", SEL_MASK_OBJECT, KiwiOutliner_CanUngroup } },

        // ── ROUND Y: the material-state readout (kiwi_material.h) ───────────
        // No selection mask and no predicate: it reads the face under the CURSOR
        // and the current template, both of which exist at all times.
        { KIWI_CMD_MATINFO,         { "Material info (under cursor)", "Textures",  0, 0 } },

        // ── ROUND AW: the xmodel-surface readout (kiwi_command.h:KIWI_CMD_MODELINFO)
        // No mask and no predicate for the same reason: it reads whatever the NEXT
        // frame draws, which needs neither a selection nor a cursor ray.
        { KIWI_CMD_MODELINFO,       { "Model info (next frame's models)", "Textures", 0, 0 } },

        // ── the instance-batching kill switch (kiwi_command.h:KIWI_CMD_INSTBATCH)
        // No mask and no predicate, for the same reason as the two diagnostics above:
        // it flips a draw-path flag that the next frame reads.
        { KIWI_CMD_INSTBATCH,       { "Instance batching (toggle)", "View", 0, 0 } },

        // ── ROUND BE: the texture-import entry point (kiwi_import.h) ────────
        // No mask and no predicate: importing art needs neither a selection nor a
        // cursor.  Only the BROWSE id is listed — KIWI_CMD_IMPORT_DROPPED is an
        // internal continuation and must not be reachable by name.
        { KIWI_CMD_IMPORT_BROWSE,   { "Import Textures...",           "Textures",  0, 0 } },

        // ── ROUND BF: the Build & Run dialog (kiwi_launch.h) ────────────────
        // No mask and no predicate: opening the dialog needs neither a selection
        // nor a saved map — the dialog itself greys the build buttons and says
        // why (an untitled map, a missing tool exe, a build already running).
        { KIWI_CMD_BUILD_RUN,       { "Build & Run...",               "Build",     0, 0 } },

        // ── ROUND BH: Caulk Selection (kiwi_caulk.h) ───────────────────────
        // The mask is SEL_MASK_FACE | SEL_MASK_OBJECT for the same reason the UV
        // rows below carry it: the apply funnel bottoms out on Brush_SetTexture,
        // which does BOTH passes (every face of a whole-selected brush, and each
        // entry of g_SelectedFaces).  The predicate is KiwiUv_CanEdit itself.
        { KIWI_CMD_CAULK_FACES,     { "Caulk Selection",              "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiCaulk_CanExecute } },

        // ── §26 Phase 6: the UV workflow v1 (kiwi_uv.cpp) ───────────────────
        // The mask is SEL_MASK_FACE | SEL_MASK_OBJECT because the cores take BOTH
        // (whole brushes get every face, g_SelectedFaces entries get one face
        // each — kiwi_uv.h "WHAT THE CORES ACTUALLY OPERATE ON").  Pick Texture
        // needs no selection at all: it reads the face under the 3D cursor.
        { KIWI_CMD_TEX_SHIFT,       { "Texture Shift",               "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiUv_CanEdit } },
        { KIWI_CMD_TEX_ROTATE,      { "Texture Rotate",              "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiUv_CanEdit } },
        { KIWI_CMD_TEX_SCALE,       { "Texture Scale",               "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiUv_CanEdit } },
        { KIWI_CMD_PICK_TEXTURE,    { "Pick Texture",                "Textures",  0,                               KiwiUv_CanPick } },

        // ── a small curated set over the CLASSIC ids, so the palette's most-used
        //    rows read like commands rather than like table keys.  Everything not
        //    listed still appears, named from g_radiantCommands.
        { 33002, { "Deselect All",              "Selection", 0, nullptr           } },
        { 33003, { "Delete Selection",          "Edit",      0, Can_HaveSelection } },
        { 33001, { "Clone Selection",           "Edit",      0, Can_HaveSelection } },
        { 33101, { "Invert Selection",          "Selection", 0, nullptr           } },
        // ── ROUND J: the HIDE / ISOLATE family (kiwi_visibility.h).  Three of the
        // four are ported ids and gain only a name and a predicate here; the
        // fourth is the KIWI id below.  "Isolate" is spelled out on 32934 because
        // that is what Plasticity's Shift+H means and what a mapper looks for —
        // "Hide Unselected" is the same act described from the wrong end.
        { 32923, { "Hide Selected",             "View",      0, KiwiVis_CanHide   } },
        { 32934, { "Isolate (hide unselected)", "View",      0, KiwiVis_CanHide   } },
        { 32924, { "Show Hidden (unhide all)",  "View",      0, KiwiVis_HasHidden } },
        { 33246, { "Show Last Hidden",          "View",      0, KiwiVis_HasHidden } },
        { KIWI_CMD_HIDE_INVERT,     { "Invert Hidden",       "View",      0, KiwiVis_CanInvert } },
        { KIWI_CMD_FOCUS_SELECTION, { "Focus On Selection",  "View",      0, KiwiFocus_CanFocus } },
        // KIWI-UX (ROUND BK, ITEM 7): the FACE twin of the row above.  SEL_MASK_FACE
        // because it is meaningless without one, and its own predicate because a
        // hovered face counts (kiwi_focus.h).
        { KIWI_CMD_VIEW_FACE,       { "View Face Head-on",   "View",      SEL_MASK_FACE, KiwiFocus_CanViewFace } },
        // KIWI-UX (ROUND BM, ITEM 1b): the section toggle.  NO selection mask and no
        // predicate — it is always available, because arming it is what starts the
        // pick that decides what it cuts (kiwi_section.h).
        { KIWI_CMD_SECTION_TOGGLE,  { "Section Analysis",    "View",      0, nullptr } },
        // The sun helper.  No selection mask — the sun is worldspawn state and has
        // nothing to do with what is selected — and the predicate is only "is there
        // a worldspawn to write onto" (kiwi_sun.h).  Its own category rather than a
        // wrong one: it is neither a view toggle nor a solid, and "Editor" is where
        // Preferences and Map Info live.
        { KIWI_CMD_PLACE_SUN,       { "Place Sun",           "Lighting",  0, KiwiSun_CanPlace } },
        { KIWI_CMD_REPEAT_LAST,     { "Repeat Last Command", "Edit",      0, KiwiCmd_HasRepeatable } },
        // ROUND S: the clipboard Cut.  SEL_MASK_OBJECT + the same "is anything
        // selected" predicate the other whole-object edits use.
        // ROUND U: RENAMED and UNBOUND (kiwi_keymap.cpp).  "Cut to Clipboard"
        // rather than "Cut (Ctrl+X)" — the chord is gone per the directive ("Cut
        // should only be on (C), not ctrl-X"), and the longer name is also what
        // keeps it from colliding in the palette with "Cut (along a line)", the
        // C-key modelling verb the user actually means by "Cut".
        { KIWI_CMD_CLIP_CUT,        { "Cut to Clipboard",    "Edit",      SEL_MASK_OBJECT, CanClipCut } },
        // ROUND T: the DEL restore, listed by name so it is discoverable without
        // knowing the key.  SEL_MASK_FACE — it is a face verb and nothing else.
        { KIWI_CMD_REMOVE_FACE,     { "Remove Face (restore edge)", "Modeling",
                                      SEL_MASK_FACE, KiwiBevel_CanRemoveFace } },
        // §24: the CSG rows keep the CLASSIC ids — no id is minted for them, so
        // there is still exactly one palette row and one handler per operation.
        // Phase 5 only upgrades their predicates from "anything is selected" to
        // the cores' REAL preconditions (kiwi_csg.h lists them).
        { 32982, { "CSG: Hollow",               "Modeling",  0, KiwiCsg_CanHollow    } },
        { 32927, { "CSG: Merge",                "Modeling",  0, KiwiCsg_CanMerge     } },
        { 33220, { "CSG: Auto Caulk",           "Modeling",  0, KiwiCsg_CanAutoCaulk } },
        // §25 mirror: metadata over the ALREADY-WIRED classic flip ids.
        { 32956, { "Mirror X (Flip)",           "Duplicate", 0, KiwiDupe_CanMirror   } },
        { 32957, { "Mirror Y (Flip)",           "Duplicate", 0, KiwiDupe_CanMirror   } },
        { 32958, { "Mirror Z (Flip)",           "Duplicate", 0, KiwiDupe_CanMirror   } },
        { 33041, { "Surface Inspector",         "Textures",  0, nullptr           } },
        { 33092, { "Patch Inspector",           "Patch",     0, nullptr           } },
        { 33104, { "View Filters",              "View",      0, nullptr           } },
        { 32784, { "Preferences",               "Editor",    0, nullptr           } },
        { 32786, { "Map Info",                  "Editor",    0, nullptr           } },
        { 32793, { "Toggle Snap To Grid",       "Grid",      0, nullptr           } },
        { 33083, { "Grid Size: Next (classic)", "Grid",      0, nullptr           } },
        { 33084, { "Grid Size: Prev (classic)", "Grid",      0, nullptr           } },
        { 33183, { "Drop To Floor",             "Edit",      0, Can_HaveSelection } },
        { 32810, { "Mouse Rotate Mode",         "Transform", 0, nullptr           } },
        { 32783, { "Toggle Clipper",            "Modeling",  0, nullptr           } },
        { 33005, { "Drag Vertices",             "Transform", 0, nullptr           } },
        { 33006, { "Drag Edges",                "Transform", 0, nullptr           } },
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  The ONE built-in modal command — proves the whole lifecycle without ever
    //  touching geometry (spec Phase-2 item 11/12/13/14 in a single gesture).
    //
    //  It MUTATES NOTHING, so per kiwi_command.h it opens NO undo bracket: an
    //  empty record would make the next Ctrl+Z a no-op, which is exactly the
    //  "one gesture = one record" rule read the other way round.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiSelfTestCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "UX: Modal Self-Test"; }

        bool Begin() override
        {
            m_haveStart = false;
            m_haveNum   = false;
            m_numWorld  = 0.0f;

            // Latch the snap under the cursor right now, so a command invoked from
            // the palette (no mouse move yet) already has a start point.
            ray_t ray;
            if ( Pick_RayFromCursor( &ray ) )
            {
                snap_result_t s;
                int cx = 0, cy = 0;
                ImGuiShell_CameraPaintCursor( &cx, &cy, nullptr, nullptr );
                if ( KiwiSnap_Query( ray, cx, cy, &s ) && s.valid )
                {
                    m_start[0] = s.position[0];
                    m_start[1] = s.position[1];
                    m_start[2] = s.position[2];
                    m_current  = s;
                    m_haveStart = true;
                }
            }
            Sys_Printf( "Modal self-test: begin (Esc cancels, Enter/LMB commits).\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_current = snap;
            if ( !m_haveStart && snap.valid )
            {
                m_start[0] = snap.position[0];
                m_start[1] = snap.position[1];
                m_start[2] = snap.position[2];
                m_haveStart = true;
            }
            g_nUpdateBits |= 1;              // repaint the camera so the marker tracks
        }

        void NumericChanged( bool has, float world ) override
        {
            m_haveNum  = has;
            m_numWorld = world;
        }

        void Commit() override
        {
            char bx[32], by[32], bz[32];
            if ( m_current.valid )
            {
                KiwiUnits_Format( bx, sizeof( bx ), m_current.position[0] );
                KiwiUnits_Format( by, sizeof( by ), m_current.position[1] );
                KiwiUnits_Format( bz, sizeof( bz ), m_current.position[2] );
                Sys_Printf( "Modal self-test: commit at %s, %s, %s (snap = %s)\n",
                            bx, by, bz, KiwiSnap_TypeName( m_current.type ) );
            }
            else
            {
                Sys_Printf( "Modal self-test: commit with no snap point.\n" );
            }
            if ( m_haveNum )
            {
                char bv[32];
                KiwiUnits_Format( bv, sizeof( bv ), m_numWorld );
                Sys_Printf( "Modal self-test: typed value %s (= %g world units)\n",
                            bv, (double)m_numWorld );
            }
            g_nUpdateBits |= 1;
        }

        void Cancel() override
        {
            Sys_Printf( "Modal self-test: cancelled.\n" );
            m_haveStart = false;
            m_haveNum   = false;
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( !m_haveStart || !m_current.valid )
                return;
            // 1 segment: the start-to-cursor rubber band.  The snap marker itself is
            // emitted by KiwiCmd_DrawWorld, in the same batch.
            KiwiLines_Color( 1.00f, 0.80f, 0.25f );
            KiwiLines_Add( m_start, m_current.position );
        }

    private:
        float         m_start[3] = { 0.0f, 0.0f, 0.0f };
        bool          m_haveStart = false;
        bool          m_haveNum   = false;
        float         m_numWorld  = 0.0f;
        snap_result_t m_current;
    };

    KiwiSelfTestCommand s_selfTest;

    KiwiEditorCommand *CommandForId( int id )
    {
        if ( id == KIWI_CMD_SELFTEST )
            return &s_selfTest;
        if ( KiwiEditorCommand *c = KiwiXform_CommandForId( id ) )      // §13/§20/§21/§22
            return c;
        if ( KiwiEditorCommand *c = KiwiCon_CommandForId( id ) )        // §7  drawing tools
            return c;
        if ( KiwiEditorCommand *c = KiwiPrim_CommandForId( id ) )       // §16b solid primitives
            return c;
        if ( KiwiEditorCommand *c = KiwiExtrude_CommandForId( id ) )    // §23 extrude
            return c;
        if ( KiwiEditorCommand *c = KiwiBevel_CommandForId( id ) )      // §25 bevel / inset
            return c;
        if ( KiwiEditorCommand *c = KiwiDupe_CommandForId( id ) )       // §25 arrays
            return c;
        // ── shakeout G: the Plasticity modelling verbs ──────────────────────
        if ( KiwiEditorCommand *c = KiwiSplit_CommandForId( id ) )      // C cut, Ctrl+R split
            return c;
        if ( KiwiEditorCommand *c = KiwiMatch_CommandForId( id ) )      // Z match face
            return c;
        if ( KiwiEditorCommand *c = KiwiTrim_CommandForId( id ) )       // T trim lines
            return c;
        // ── round J: the two construction-curve editors ────────────────────
        if ( KiwiEditorCommand *c = KiwiOffset_CommandForId( id ) )     // O offset curve
            return c;
        if ( KiwiEditorCommand *c = KiwiFillet_CommandForId( id ) )     // B fillet corners
            return c;
        if ( KiwiEditorCommand *c = KiwiPatchFillet_CommandForId( id ) ) // B on brush edges
            return c;
        // ── ROUND L: Q, difference / union ─────────────────────────────────
        if ( KiwiEditorCommand *c = KiwiBool_CommandForId( id ) )       // Q boolean
            return c;
        // ── ROUND AF: L, the face-to-face bridge ───────────────────────────
        if ( KiwiEditorCommand *c = KiwiLoft_CommandForId( id ) )       // L loft
            return c;
        return KiwiUv_CommandForId( id );                               // §26 texture shift/rotate/scale
    }

    unsigned int CurrentMods()
    {
        // The same mask Radiant_TryHotkey builds (Shift=1, Alt=2, Ctrl=4, Win=8).
        unsigned int mods = 0;
        if ( ::GetKeyState( VK_MENU )    < 0 ) mods |= 2;
        if ( ::GetKeyState( VK_CONTROL ) < 0 ) mods |= 4;
        if ( ::GetKeyState( VK_SHIFT )   < 0 ) mods |= 1;
        if ( ::GetKeyState( VK_LWIN )    < 0 ) mods |= 8;
        return mods;
    }
}

// ─── ROUND N: the grid stepper, exported ─────────────────────────────────────
// USER DIRECTIVE: "the gridsize needs to be changeable.  Put that in the top right
// somewhere and maybe put it on pageup/pagedown."  The top-right READOUT
// (kiwi_viewcube.cpp) steps the spacing by clicking, and it must use the SAME
// clamp, the same doubling and the same console line the [ ] / PageUp / PageDown
// keys use — so it calls this instead of touching KiwiUnits_SetGridSpacingInches,
// which has no clamp of its own.  Two entry points, one rule.
void KiwiCmd_StepGrid( bool doubleIt )
{
    StepGrid( doubleIt );
}

// KIWI-UX (ROUND R): the typed spacing — see kiwi_command.h.  NOT ladder-snapped:
// §17 says any positive spacing is legal, and a control whose whole point is
// "let me type in the grid spacing manually" must not overrule what was typed.
bool KiwiCmd_SetGridSpacing( float inches )
{
    if ( !( inches > 0.0f ) )
        return false;
    if ( inches < KGRID_MIN_INCHES ) inches = KGRID_MIN_INCHES;
    if ( inches > KGRID_MAX_INCHES ) inches = KGRID_MAX_INCHES;
    KiwiUnits_SetGridSpacingInches( inches );
    g_nUpdateBits = -1;
    Sys_Printf( "grid spacing %g in\n", (double)KiwiUnits_GridSpacingInches() );
    return true;
}

// ─── §3 metadata lookup ──────────────────────────────────────────────────────
const kiwiCommandInfo_t *KiwiCmd_Info( int commandId )
{
    for ( const auto &m : KCMD_META )
        if ( m.id == commandId )
            return &m.info;
    return nullptr;
}

bool KiwiCmd_CanExecute( int commandId )
{
    const kiwiCommandInfo_t *info = KiwiCmd_Info( commandId );
    if ( !info || !info->canExecute )
        return true;
    return info->canExecute();
}

// ─── §3 registration into the SHARED table ───────────────────────────────────
// Called by Radiant_SeedCommandTable (mainfrm.cpp) the first time anything reads
// the table, so ordering never matters.  These are the CLASSIC-profile bindings:
// only the self-test's palette-only entry and the three grid/snap helpers exist
// unbound; kiwi_keymap.cpp lays the modern keys on top.
void KiwiCmd_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiSelectModePoint",  0, 0, KIWI_CMD_SELMODE_POINT );
    Radiant_RegisterCommand( "KiwiSelectModeEdge",   0, 0, KIWI_CMD_SELMODE_EDGE );
    Radiant_RegisterCommand( "KiwiSelectModeFace",   0, 0, KIWI_CMD_SELMODE_FACE );
    Radiant_RegisterCommand( "KiwiSelectModeObject", 0, 0, KIWI_CMD_SELMODE_OBJECT );
    Radiant_RegisterCommand( "KiwiSelectModeAll",    0, 0, KIWI_CMD_SELMODE_ALL );
    Radiant_RegisterCommand( "KiwiCommandPalette",   0, 0, KIWI_CMD_PALETTE );
    Radiant_RegisterCommand( "KiwiGridHalve",        0, 0, KIWI_CMD_GRID_HALVE );
    Radiant_RegisterCommand( "KiwiGridDouble",       0, 0, KIWI_CMD_GRID_DOUBLE );
    Radiant_RegisterCommand( "KiwiSnapMarkers",      0, 0, KIWI_CMD_SNAP_TOGGLE );
    Radiant_RegisterCommand( "KiwiModalSelfTest",    0, 0, KIWI_CMD_SELFTEST );
    KiwiXform_RegisterCommands();       // §13 G / R / S — one registration point
    KiwiCon_RegisterCommands();         // §7 / §16 drawing tools + construction planes
    KiwiPrim_RegisterCommands();        // §16b Box / Cylinder / Sphere / Cone
    KiwiAdd_RegisterCommands();         // §16b the Shift+A add menu
    KiwiExtrude_RegisterCommands();     // §23 Extrude Region
    KiwiBevel_RegisterCommands();       // §25 Bevel Edge + Inset Face
    KiwiDupe_RegisterCommands();        // §25 Array (Linear) + Array (Radial)
    KiwiSelExt_RegisterCommands();      // §25 selection expansion
    KiwiSelConv_RegisterCommands();     // shakeout D — Ctrl+1..4 selection conversion
    KiwiConSel_RegisterCommands();      // shakeout F — construction Join / Delete
    KiwiSplit_RegisterCommands();       // shakeout G — Cut (C) + Split Face (Ctrl+R)
    KiwiBool_RegisterCommands();        // ROUND L    — Boolean (Q)
    KiwiLoft_RegisterCommands();        // ROUND AF   — Loft (L)
    KiwiAutoBool_RegisterCommands();    // ROUND AB   — Auto Bool (unbound, palette only)
    KiwiMatch_RegisterCommands();       // shakeout G — Match Face (Z)
    KiwiTrim_RegisterCommands();        // shakeout H — Trim (T)
    KiwiJoin_RegisterCommands();        // shakeout G — Join (J)
    KiwiUv_RegisterCommands();          // §26 texture shift/rotate/scale + Pick Texture
    KiwiWindows_RegisterCommands();     // §9 shakeout B — the window toggles (+ ROUND W's)
    KiwiOutliner_RegisterCommands();    // ROUND W    — Group / Ungroup Selection
    KiwiEntBrowser_RegisterCommands();  // ROUND AU   — the Entities window toggle
    KiwiSky_RegisterCommands();         // ROUND AZ   — the Sky window toggle + two verbs
    KiwiUvEd_RegisterCommands();        // ROUND BD   — the UV editor window toggle
    KiwiImport_RegisterCommands();      // ROUND BE   — "Import Textures..." (the drop's twin)
    KiwiLaunch_RegisterCommands();      // ROUND BF   — "Build & Run..." (cod4map / cod4rad / game)
    KiwiCaulk_RegisterCommands();       // ROUND BH   — "Caulk Selection" (End, modern profile)
    KiwiSection_RegisterCommands();     // ROUND BM   — "Section Analysis" (the view-cube button)
    KiwiSun_RegisterCommands();         //            — "Place Sun" (the worldspawn sun keys)
    // ── ROUND J — the last Plasticity verbs (kiwi_offset/fillet/focus/visibility)
    KiwiOffset_RegisterCommands();      // O       offset a construction chain
    KiwiFillet_RegisterCommands();      // B       round its corners
    KiwiPatchFillet_RegisterCommands(); // (B)     ROUND Q — fillet a brush edge into a patch
    KiwiFocus_RegisterCommands();       // /       frame the selection
    KiwiVis_RegisterCommands();         // Ctrl+H  invert hidden
    Radiant_RegisterCommand( "KiwiRepeatLastCommand", 0, 0, KIWI_CMD_REPEAT_LAST );
    // ROUND Y: the material-state readout (kiwi_material.h).  Unbound in both
    // profiles — it is a diagnostic reached by name from the §15 palette, and a
    // key spent on it would be a key not spent on a modelling verb.
    Radiant_RegisterCommand( "KiwiMatInfo", 0, 0, KIWI_CMD_MATINFO );
    // ROUND AW: the model-surface twin of KiwiMatInfo.  Unbound for the same reason.
    Radiant_RegisterCommand( "KiwiModelInfo", 0, 0, KIWI_CMD_MODELINFO );
    // the instance-batching kill switch.  Unbound for the same reason —
    // it exists so a suspected rendering difference can be A/B'd in session.
    Radiant_RegisterCommand( "KiwiInstBatch", 0, 0, KIWI_CMD_INSTBATCH );
    // ── KIWI-UX (ROUND BB, ITEM 4): CSG HOLLOW REACHES THE PALETTE ──────────────
    // USER DIRECTIVE, verbatim: *"Add the hollow operation from legacy radiant to
    // the search (F) menu as a command."*
    //
    // THE HANDLER IS ALREADY THERE AND IS NOT A STUB.  Selection->CSG->Hollow is
    // classic id 32982: Cmd_OnSelectionMakehollow (mainfrm.cpp:2412, the ported
    // CMainFrame::OnSelectionMakehollow 0x425570) opens its own "hollow" undo
    // record and calls the ported CSG_MakeHollow (csg.cpp:318, 0x47D3C0), which
    // insets every face of the one selected brush by a grid step into a new thin
    // brush and deletes the original.  It is dispatched at mainfrm.cpp:5478 and it
    // already has a §15 label and predicate in the table above ("CSG: Hollow",
    // KiwiCsg_CanHollow) and a button in the CSG panel (kiwi_csg.cpp:150).
    //
    // WHAT WAS MISSING IS ONLY THE REGISTRY ROW.  The palette lists the RADIANT
    // COMMAND TABLE (kiwi_palette.cpp:87-95) and the shipped table binds CSGMerge
    // (32927) and AutoCaulk (33220) but NOT hollow — faithfully, because the binary
    // gave hollow a menu item and no key.  So the row existed everywhere except in
    // the one place the F menu reads.  UNBOUND, exactly like the binary: this adds
    // a way to FIND the command, not a key that was never there.  The name follows
    // the two neighbours' spelling (mainfrm.cpp:1076-1077, "CSGMerge"/"AutoCaulk"),
    // and the palette's search matches the label, the category and this name.
    Radiant_RegisterCommand( "CSGHollow", 0, 0, 32982 );
    // ROUND S: unbound in the CLASSIC profile — Ctrl+X's accelerator was removed
    // from res/radiant.rc (it ran File->Exit), and the classic profile's rule is
    // "leave the key alone" rather than "invent a binding".  kiwi_keymap.cpp's
    // modern profile puts it on Ctrl+X.
    Radiant_RegisterCommand( "KiwiClipCut", 0, 0, KIWI_CMD_CLIP_CUT );

    // KIWI-UX (shakeout B, user directive "Change backspace to delete"): a SECOND
    // binding row for the ported Delete Selection (33003), so the modern profile can
    // put it on VK_DELETE while the classic Backspace row stays exactly as the binary
    // shipped it.  A second row for one id is a shape the table already has — the
    // binary itself carries "Patch TAB" (33089) twice — and every consumer copes:
    // Radiant_TryHotkey is first-match-wins over rows (this row is unbound in classic,
    // so it can never shadow anything), the §15 palette dedups by id and shows only
    // the first (kiwi_palette.cpp:104-107), and the command-list panel shows both the
    // same way it already shows 33089 twice.  It needs the ALIAS registrar because
    // Radiant_RegisterCommand refuses a duplicate id by design (mainfrm.cpp:1241-1243).
    {
        extern bool Radiant_RegisterCommandAlias( const char *name, byte vk, byte mods,
                                                  int commandId );   // mainfrm.cpp
        Radiant_RegisterCommandAlias( "KiwiDeleteSelection", 0, 0, 33003 );

        // ROUND N, same shape and the same registrar: the modern profile puts the
        // grid steppers on PageUp / PageDown AS WELL AS on [ ] (user directive:
        // "maybe put it on pageup/pagedown"), and one id can only carry one binding
        // per row.  Two alias rows, unbound in the classic profile so they can never
        // shadow anything, bound by NAME in kiwi_keymap.cpp so the base [ ] rows
        // stay exactly where they are.
        Radiant_RegisterCommandAlias( "KiwiGridDoublePage", 0, 0, KIWI_CMD_GRID_DOUBLE );
        Radiant_RegisterCommandAlias( "KiwiGridHalvePage",  0, 0, KIWI_CMD_GRID_HALVE );
    }
}

// ─── ROUND J: REPEAT LAST COMMAND (kiwi_command.h) ───────────────────────────
namespace
{
    int s_lastCommand = 0;               // 0 = nothing recorded yet

    // The ids that are NOT "commands" in Plasticity's sense, i.e. the ones its own
    // executor never writes into `lastCommand` because they are `viewport:` /
    // `selection:` / `edit:` actions on the editor rather than Command subclasses.
    // Keeping this as an explicit list rather than a range test is deliberate: a
    // new id defaults to REPEATABLE, which is right for a modelling verb and is
    // the class almost every future id will be in.
    bool Repeatable( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_SELMODE_POINT:                    // selection:mode:set:*
        case KIWI_CMD_SELMODE_EDGE:
        case KIWI_CMD_SELMODE_FACE:
        case KIWI_CMD_SELMODE_OBJECT:
        case KIWI_CMD_SELMODE_ALL:
        case KIWI_CMD_SELCONV_POINT:                    // selection:convert:*
        case KIWI_CMD_SELCONV_EDGE:
        case KIWI_CMD_SELCONV_FACE:
        case KIWI_CMD_SELCONV_OBJECT:
        case KIWI_CMD_PALETTE:                          // (KIWI's own; not a verb)
        case KIWI_CMD_ADD_MENU:
        case KIWI_CMD_GRID_HALVE:                       // viewport:grid:*
        case KIWI_CMD_GRID_DOUBLE:
        case KIWI_CMD_SNAP_TOGGLE:
        case KIWI_CMD_CPLANE_XY:                        // viewport:grid:selection kin
        case KIWI_CMD_CPLANE_XZ:
        case KIWI_CMD_CPLANE_YZ:
        case KIWI_CMD_CPLANE_FACE:
        case KIWI_CMD_CPLANE_VIEW:
        case KIWI_CMD_WINDOW_XY:                        // window / view toggles
        case KIWI_CMD_WINDOW_Z:
        case KIWI_CMD_WINDOW_TEXTURE:
        case KIWI_CMD_WINDOW_CONSOLE:
        case KIWI_CMD_WINDOW_SHELL:
        case KIWI_CMD_WINDOW_OUTLINER:                  // ROUND W — a window toggle
        case KIWI_CMD_WINDOW_ENTITIES:                  // ROUND AU — likewise
        case KIWI_CMD_WINDOW_SUN:                       // the Sun tab — likewise
        // ROUND AU: the entity DROP is the tail of a drag that has already
        // happened and carries its argument in kiwi_entbrowser.cpp's one-shot
        // pending slot.  Repeating it would re-run a gesture with no payload —
        // and, worse, "Repeat Last" would silently become the drop instead of the
        // modelling verb the user actually last ran.
        case KIWI_CMD_ENT_DROP:
        case KIWI_CMD_VIEW_SHOW_GRID:
        case KIWI_CMD_VIEW_SHOW_AXES:
        case KIWI_CMD_VIEW_ORTHO:                       // ROUND M — a view toggle, not a verb
        case KIWI_CMD_CONSTRUCT_UNDO:                   // edit:undo
        case KIWI_CMD_FOCUS_SELECTION:                  // viewport:focus
        case KIWI_CMD_VIEW_FACE:                        // ROUND BK — a view verb too
        case KIWI_CMD_SECTION_TOGGLE:                   // ROUND BM — view state, and its own inverse
        // Invert Hidden is a VIEW verb by kiwi_visibility.h's own argument (it
        // takes no undo bracket for exactly that reason), and it is its own
        // inverse — repeating it would undo the thing the user just did.  Its
        // three ported siblings are classic ids and are never recorded anyway.
        case KIWI_CMD_HIDE_INVERT:
        // ROUND U: the construction hide family is VIEW state, exactly like its
        // three ported siblings above — those are classic ids and are never
        // recorded, so recording these would make Shift+R behave differently
        // depending on which half of one family the user last used.
        case KIWI_CMD_CONSTRUCT_HIDE:
        case KIWI_CMD_CONSTRUCT_UNHIDE:
        case KIWI_CMD_REPEAT_LAST:                      // edit:repeat-last-command
        // ROUND S: Cut is an edit: action on the clipboard, not a modelling verb —
        // the same class as Copy/Paste, which are classic ids and never recorded.
        case KIWI_CMD_CLIP_CUT:
        // ROUND BF: "Build & Run..." TOGGLES A WINDOW — same class as the window
        // toggles at the top of this list, and repeating it would close the dialog
        // the user just opened rather than repeat the last modelling verb.
        case KIWI_CMD_BUILD_RUN:
        case KIWI_CMD_SELFTEST:                         // a diagnostic, not a verb
            return false;
        default:
            return true;
        }
    }
}

bool KiwiCmd_HasRepeatable()
{
    return s_lastCommand != 0;
}

const char *KiwiCmd_RepeatLabel()
{
    if ( !s_lastCommand )
        return 0;
    const kiwiCommandInfo_t *info = KiwiCmd_Info( s_lastCommand );
    return info ? info->displayName : 0;
}

bool KiwiCmd_RepeatLast()
{
    const int id = s_lastCommand;
    if ( !id )
    {
        Sys_Printf( "Repeat: no command has been run yet.\n" );
        return false;
    }
    if ( !KiwiCmd_CanExecute( id ) )
    {
        const char *name = KiwiCmd_RepeatLabel();
        Sys_Printf( "Repeat: \"%s\" cannot run right now.\n", name ? name : "the last command" );
        return false;
    }
    // Straight back through the ordinary tail, so a repeated MODAL id restarts the
    // gesture and a repeated INSTANT id acts — which is exactly what Plasticity's
    // `enqueue(new this.lastCommand(editor))` does (CommandExecutor.ts:58-61).
    return KiwiCmd_Dispatch( (unsigned int)id );
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND S — CUT (the CLIPBOARD one), i.e. what Ctrl+X now does.
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "Ctrl-X should not quit the app!!!"
//
// WHAT IT DID.  res/radiant.rc's IDR_MAIN_ACCEL bound Ctrl+X to command 32951 =
// ID_FILE_EXIT_RAD (res/resource.h:34), dispatched at mainfrm.cpp:5646 as
// `case ID_FILE_EXIT_RAD: Radiant_FileExit();`.  The accelerator entry is REMOVED
// (the .rc carries the // KIWI-UX fence and the reasoning); the File->Exit menu
// item keeps the id, so quitting from the menu is untouched.
//
// WHAT IT DOES NOW.  There is NO ported clipboard-cut core to call — the binary
// ships Copy (33039 -> Cmd_OnEditCopybrush -> XYWnd_CopyClip, mainfrm.cpp:2990)
// and Paste (33040) and nothing between them.  So Cut is those two ported cores in
// order: Copy, then Delete Selection (33003 -> Cmd_OnSelectionDelete,
// mainfrm.cpp:2670).
//
// UNDO: NO bracket is opened here, on purpose.  Cmd_OnSelectionDelete is already a
// COMPLETE record of its own (Undo_ClearRedo + Undo_GeneralStart("delete") +
// Undo_AddBrushList + per-brush Undo_AddEntity_W + Undo_EndBrushList + Undo_End),
// and Copy mutates nothing, so one Ctrl+Z restores exactly the brushes the cut
// removed.  Wrapping it in a second bracket would nest two records for one act.
// ACCEPTED COST, stated: the undo entry is therefore LABELLED "delete", not "cut".
// Relabelling it would mean editing the ported handler for a cosmetic string.
//
// A cut with nothing selected must not clear the clipboard: Copy is skipped and the
// console says so, which is also what makes Ctrl+X harmless on an empty selection.
void KiwiCmd_ClipCut()
{
    extern void Radiant_ExecCommand( unsigned int cmdId );   // mainfrm.cpp:4054

    if ( selected_brushes.next == &selected_brushes )
    {
        Sys_Printf( "Cut: nothing is selected - the clipboard is unchanged.\n" );
        return;
    }
    Radiant_ExecCommand( 33039u );      // Edit->Copy   (serialise to the clipboard)
    Radiant_ExecCommand( 33003u );      // Edit->Delete (its own complete undo record)
    Sys_Printf( "Cut: selection copied to the clipboard and deleted.\n" );
    g_nUpdateBits = -1;
}

// ─── §3 dispatch tail for the reserved range ─────────────────────────────────
static bool KiwiCmd_DispatchInner( unsigned int cmdId );

// KIWI-UX (CLEANUP, C-4): the range test mainfrm.cpp's gate asks before calling
// KiwiCmd_Dispatch below.  ONE spelling of "is this id ours" — the two #defines
// in kiwi_command.h are the only place the numbers appear.
bool KiwiCmd_IsKiwiId( int id )
{
    return id >= KIWI_CMD_FIRST && id <= KIWI_CMD_LAST;
}

bool KiwiCmd_Dispatch( unsigned int cmdId )
{
    const bool handled = KiwiCmd_DispatchInner( cmdId );
    // Recorded on SUCCESS only, and after the fact, so a command that refused to
    // start (CanExecute said no) never becomes the thing Shift+R repeats.
    if ( handled && Repeatable( (int)cmdId ) )
        s_lastCommand = (int)cmdId;
    return handled;
}

static bool KiwiCmd_DispatchInner( unsigned int cmdId )
{
    // ── KIWI-UX (ROUND Q): B IS A CONTEXT VERB ───────────────────────────────
    // With brush EDGES selected, B means the SOLID fillet (chamfer + bezier
    // patch); otherwise it stays round J's construction-corner fillet.  The
    // redirect sits here, above every arm, so the key funnel, the palette, the
    // menus and Repeat Last all arbitrate identically — the same shape J uses
    // (kiwi_join.cpp), and the whole argument is in kiwi_patchfillet.h THE B KEY.
    // Total and side-effect free: it returns every other id unchanged.
    cmdId = (unsigned int)KiwiPatchFillet_ContextB( (int)cmdId );

    if ( KiwiCmd_IsModalId( (int)cmdId ) )
        return KiwiCmd_Start( (int)cmdId );

    switch ( cmdId )
    {
    case KIWI_CMD_SELMODE_POINT:  SetModeMask( SEL_MASK_VERTEX );     return true;
    case KIWI_CMD_SELMODE_EDGE:   SetModeMask( SEL_MASK_EDGE );       return true;
    case KIWI_CMD_SELMODE_FACE:   SetModeMask( SEL_MASK_FACE );       return true;
    case KIWI_CMD_SELMODE_OBJECT: SetModeMask( SEL_MASK_OBJECT );     return true;
    case KIWI_CMD_SELMODE_ALL:    SetModeMask( SEL_MASK_EVERYTHING ); return true;
    case KIWI_CMD_PALETTE:        KiwiPalette_Toggle();               return true;
    case KIWI_CMD_GRID_HALVE:     StepGrid( false );                  return true;
    case KIWI_CMD_GRID_DOUBLE:    StepGrid( true );                   return true;
    case KIWI_CMD_SNAP_TOGGLE:
        KiwiSnap_SetShowMarkers( !KiwiSnap_ShowMarkers() );
        g_nUpdateBits |= 1;
        return true;
    // ROUND J: Repeat is handled HERE rather than in a DispatchInstant arm,
    // because it is the one command whose implementation lives in this file and
    // because routing it through the recorder above would be circular.
    case KIWI_CMD_REPEAT_LAST:    KiwiCmd_RepeatLast();               return true;
    case KIWI_CMD_CLIP_CUT:       KiwiCmd_ClipCut();                  return true;
    // ROUND T: Remove Face (restore edge).  It brackets its own undo and prints
    // its own line either way, so the return value is only "was it mine".
    case KIWI_CMD_REMOVE_FACE:
        // ── KIWI-UX (ROUND AA, ITEM 4): the PALETTE route parks the same way ──
        // A face selection IS a live PAUSED push/pull (kiwi_boxselect.cpp
        // ClickSelect), so reaching this verb by NAME finds the same parked
        // gesture the key funnel's new rung finds — and running the removal
        // underneath it would leave that gesture holding a face index the
        // Brush_RemoveFace shift has just invalidated.  Same cancel, same
        // provably record-free gate, so the two routes behave identically.
        if ( g_activeCommand && g_activeCommand->PreemptIdle() )
            KiwiCmd_Cancel();
        if ( !KiwiBevel_RemoveFaceRestoreEdge() )
            Sys_Printf( "Remove Face: select exactly ONE brush face (mode 3).\n" );
        return true;
    default:
        // §7 / §16: the construction-plane and store commands are instant ids too.
        if ( KiwiCon_DispatchInstant( cmdId ) )
            return true;
        // §25: the selection-expansion helpers, likewise instant.
        if ( KiwiSelExt_DispatchInstant( cmdId ) )
            return true;
        // Shakeout D: the Ctrl+1..4 conversions — the whole of INSTANT BLOCK 2
        // (34100..34103).  Reached because the mainfrm gate now routes
        // 34000..34199 here; see kiwi_command.h THE SECOND INSTANT BLOCK.
        if ( KiwiSelConv_DispatchInstant( cmdId ) )
            return true;
        // Shakeout F: the construction selection's Join / Delete, instant block 2.
        if ( KiwiConSel_DispatchInstant( cmdId ) )
            return true;
        // Shakeout G: J, the CONTEXT verb (faces -> CSG_Merge, lines -> JoinLines).
        if ( KiwiJoin_DispatchInstant( cmdId ) )
            return true;
        // ROUND AB: Auto Bool — greedy pairwise CSG merge over the whole selection.
        if ( KiwiAutoBool_DispatchInstant( cmdId ) )
            return true;
        // §26: Pick Texture — the one instant command in the UV block.
        if ( KiwiUv_DispatchInstant( cmdId ) )
            return true;
        // §16b (shakeout C): the Shift+A add menu.
        if ( KiwiAdd_DispatchInstant( cmdId ) )
            return true;
        // §9 (shakeout B): the Windows-menu per-window visibility toggles, and
        // (shakeout C) the two native View-menu Show Grid / Show Axes items —
        // kiwi_windows.cpp owns both because it owns the CheckMenuItem sync.
        if ( KiwiWindows_DispatchInstant( cmdId ) )
            return true;
        // ── ROUND J: the three new instant verbs ────────────────────────────
        if ( KiwiDupe_DispatchInstant( cmdId ) )        // Shift+D duplicate + Move
            return true;
        // ROUND BK, ITEM 7: …and Space (KIWI_CMD_VIEW_FACE), which the same
        // function owns because it is the same framing math.
        if ( KiwiFocus_DispatchInstant( cmdId ) )       // /  frame  ·  Space  face
            return true;
        // ROUND BM, ITEM 1b: the SECTION ANALYSIS toggle — a view verb like the two
        // above it, and the only entry point the view-cube button uses too.
        if ( KiwiSection_DispatchInstant( cmdId ) )
            return true;
        // The sun helper's one id.  Same split as every feature above: the file
        // owns its id and its whole body, this one owns only the route to it.
        if ( KiwiSun_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiVis_DispatchInstant( cmdId ) )         // Ctrl+H  invert hidden
            return true;
        // ROUND W: Group / Ungroup Selection.  AFTER KiwiWindows_DispatchInstant,
        // which owns KIWI_CMD_WINDOW_OUTLINER — the two files split the round's
        // three ids along the same line every other round splits them: the window
        // flag belongs to the §9 table, the verbs belong to the feature.
        if ( KiwiOutliner_DispatchInstant( cmdId ) )
            return true;
        // ROUND AU: the entity browser's DEFERRED DROP (KIWI_CMD_ENT_DROP).  The
        // window TOGGLE is not here — KiwiWindows_DispatchInstant above owns every
        // §9 flag, this file owns only the verb, which is the same split round W
        // made between the Outliner's window id and its two group verbs.
        if ( KiwiEntBrowser_DispatchInstant( cmdId ) )
            return true;
        // ROUND AZ: the Sky tab's two VERBS (apply / create shell).  Its window
        // toggle is not here for the same reason the entity browser's is not —
        // KiwiWindows_DispatchInstant above owns every §9 flag.
        if ( KiwiSky_DispatchInstant( cmdId ) )
            return true;
        // ROUND BE: texture import — the internal drop continuation (34128, posted by the
        // WM_DROPFILES handler in radiant_main.cpp) and the browse verb (34129).  Same split
        // as the entity browser above: this file owns nothing, the feature owns both ids.
        if ( KiwiImport_DispatchInstant( cmdId ) )
            return true;
        // ROUND BF: the Build & Run dialog's one id (34130).  Same split again — the
        // feature owns its id; this file owns nothing but the route to it.
        if ( KiwiLaunch_DispatchInstant( cmdId ) )
            return true;
        // ROUND BH: "Caulk Selection" (34131).  Same split again — the feature owns its
        // id and its whole body; this file owns only the route to it.
        if ( KiwiCaulk_DispatchInstant( cmdId ) )
            return true;
        // ROUND Y: the material-state readout.  A pure diagnostic — no undo, no
        // mutation, no selection change; it prints and returns.
        if ( cmdId == (unsigned int)KIWI_CMD_MATINFO )
        {
            // kiwi_material.h:206 (definition kiwi_material.cpp:288)
            extern void KiwiMtl_InfoCommand();
            KiwiMtl_InfoCommand();
            return true;
        }
        // ROUND AW: the model-surface readout.  Also a pure diagnostic — it only arms a
        // one-shot flag that r_ed_scene.cpp's SkinModelInst reads and that its own
        // per-frame reset clears, so nothing is drawn, mutated or selected here.
        if ( cmdId == (unsigned int)KIWI_CMD_MODELINFO )
        {
            // r_ed_scene.cpp — definition beside Editor_ModelSurfTech.
            extern void KiwiEdScene_ArmModelInfoDump();
            KiwiEdScene_ArmModelInfoDump();
            return true;
        }
        // the instance-batching kill switch.  Also pure state: it flips one
        // int that r_ed_scene.cpp's flush reads, and the OFF path is round BY3's
        // per-instance draw, unchanged.
        if ( cmdId == (unsigned int)KIWI_CMD_INSTBATCH )
        {
            // r_ed_scene.cpp — definition beside Editor_DrawMergedInstanceRun.
            extern void KiwiEdScene_ToggleInstBatching();
            KiwiEdScene_ToggleInstBatching();
            return true;
        }
        return false;                     // an unwired id in the reserved range
    }
}

// ─── KIWI-UX (shakeout G): PASTE / CLONE ENTER MOVE ──────────────────────────
//
// USER DIRECTIVE, verbatim: "when copy and pasting solids (brushes), the new
// brush should automatically be selected and in move mode (where the gizmo is
// required)."
//
// ── WHERE THE HOOK LIVES, and why not anywhere else ─────────────────────────
// It is a POST-DISPATCH TAIL on the two classic ids, added in mainfrm.cpp's own
// switch as two // KIWI-UX one-liners:
//
//     case 33040: Cmd_OnEditPastebrush(); KiwiCmd_AfterPaste(); return true;
//     case 33001: Cmd_OnSelectionClone(); KiwiCmd_AfterPaste(); return true;
//
// The alternatives were both worse.  Wrapping the ids in the KIWI range arm
// (`cmdId >= 34000`) does not reach them at all.  Re-entering
// Radiant_DispatchCommandDirect from a KIWI wrapper would need a recursion guard
// for a call that has exactly one caller.  And changing the ported handlers
// THEMSELVES would fire the hook for the internal callers too — map.cpp:569 and
// entity.cpp:1873 both run XYWnd_PasteClip directly, to carry a selection across
// File→New, where starting a modal command would be nonsense.  A tail on the two
// COMMAND ids fires for exactly the user-initiated Ctrl+V / Space and nothing
// else.
//
// ── CLONE TOO?  YES, and deliberately ───────────────────────────────────────
// Clone (33001, the classic Space key) is the same act: it produces new brushes,
// leaves them selected, and the very next thing anyone does is put them
// somewhere.  Clone_Selection ends with the copies on selected_brushes
// (select.cpp:2513-2531 — Select_Deselect(1) then Brush_AddToList2 per copy) and
// its own comment says that IS the binary's "paste selects the new brushes" net
// effect.  Two ids, one behaviour, no third rule to remember.
//
// ── DOES THE PASTE ACTUALLY SELECT?  VERIFIED, NOT ASSUMED ──────────────────
// Cmd_OnEditPastebrush -> XYWnd_PasteClip (mainfrm.cpp:4340) ->
// RadiantClipboard_Paste (entity.cpp:1978) -> Map_ImportBuffer (map.cpp 0x487C90).
// Map_ImportBuffer opens with Select_Deselect(1) and then, per parsed entity,
// either Brush_AddToList + Brush_AddToList2 (worldspawn brushes merged into the
// world) or Select_Brush per instance brush (everything else) — i.e. the pasted
// geometry IS the selection when it returns.  So the wrapper does NOT need to
// select anything; it only has to observe it.  It still checks, because a paste
// of an empty clipboard selects nothing and must not start a command.
void KiwiCmd_AfterPaste()
{
    if ( !KiwiUX_ModernInput() )
        return;                             // classic profile: untouched behaviour
    if ( g_activeCommand )
        return;                             // never stomp a gesture already running

    // The legacy funnels the paste ran through have already marked the typed
    // selection dirty; KiwiSel() folds that in before anyone observes it
    // (kiwi_selection.h).  Reading it here is what makes CanExecute correct.
    const bool haveBrushes = !KiwiSel().items.empty()
                          || selected_brushes.next != &selected_brushes;

    // ── KIWI-UX (ROUND AR, ITEM 1): THE CONSTRUCTION HALF ───────────────────
    // USER DIRECTIVE, verbatim: "allow copy pasting of construction lines! after a
    // paste, it should automatically go into G(move) mode."
    //
    // A construction paste needs NO new entry point here: KiwiXform_CanMove already
    // answers true for a pure construction selection (kiwi_transform.cpp:3565 ->
    // KiwiConSel_CanMove) and the Move command already has its construction arm
    // (kiwi_transform.cpp:1146).  So the ONE thing this function has to learn is
    // that "nothing landed" now has a second half to ask about.
    //
    // THE MIXED CASE IS REFUSED, LOUDLY, and it is a framework fact rather than a
    // preference: KiwiConSel_CanMove() requires the brush-side selection to be
    // EMPTY, and Move reaches its construction arm only when DominantKind fails —
    // so ONE gesture structurally cannot carry both, and starting it would move the
    // solids and leave the lines behind at the paste position.  Both halves are
    // pasted and selected either way; only the auto-enter is withheld, with a line
    // saying so.  kiwi_conclip.h carries the full argument.
    // ONE-SHOT, and it has to be: this function is ALSO the tail on CLONE (33001,
    // the classic Space), which never touches the construction clipboard.  Asking
    // "is anything selected construction-side" would then make a stale line
    // selection turn every Clone into the mixed refusal.  The question that is
    // actually being asked is "did THIS paste land construction geometry", and only
    // KiwiConClip_Paste knows it.
    const int  pastedLines = KiwiConClip_TakeJustPasted();
    const bool haveLines   = ( pastedLines > 0 );
    if ( !haveBrushes && !haveLines )
        return;                             // nothing landed — an empty clipboard
    if ( haveBrushes && haveLines )
    {
        Sys_Printf( "Paste: solids AND construction geometry were pasted, and one "
                    "Move gesture carries one kind at a time — nothing was "
                    "auto-entered.  Select one kind and press G.\n" );
        return;
    }

    // PAUSED, exactly like the face-click auto-enter: the gizmo is up, nothing
    // follows the cursor until a handle is grabbed, RMB / Enter places it, Esc
    // leaves the paste where it landed (Cancel restores a move that never moved,
    // which is a no-op — it does NOT un-paste).
    if ( KiwiCmd_Start( KIWI_CMD_MOVE ) )
        KiwiCmd_Pause();
}

// ─── §4 the active-command owner ─────────────────────────────────────────────
KiwiEditorCommand *KiwiCmd_Active()
{
    return g_activeCommand;
}

bool KiwiCmd_Start( int commandId )
{
    KiwiEditorCommand *cmd = CommandForId( commandId );
    if ( !cmd )
        return false;

    // Re-invoking the SAME command restarts it; a different one cancels the old.
    if ( g_activeCommand )
        KiwiCmd_Cancel();

    if ( !cmd->CanExecute() )
        return false;

    KiwiNum_Reset();
    s_lastSnap = snap_result_t();
    s_hot      = true;                     // shakeout E: every gesture starts HOT

    // KIWI-UX (shakeout E): install the command's NAMED FIELDS between the reset
    // and Begin, so Begin may still relabel one per stage (kiwi_command.h
    // NumericFields).  A command that declares none keeps the single unnamed
    // LENGTH field KiwiNum_Reset just installed — the pre-shakeout-E grammar.
    {
        const kiwiNumField_t *fields = nullptr;
        const int nf = cmd->NumericFields( &fields );
        if ( nf > 0 && fields )
            KiwiNum_SetFields( fields, nf );
    }

    // Seed the cursor BEFORE Begin: a command invoked from the palette or a hotkey
    // has had no mouse move yet, but its Begin() has to latch a start point from
    // wherever the cursor already is (KiwiCmd_LastCursor).
    {
        int cx = 0, cy = 0;
        s_cursorHave = ImGuiShell_CameraPaintCursor( &cx, &cy, nullptr, nullptr );
        s_cursorX = cx;
        s_cursorY = cy;
        // ROUND AA, ITEM 9: no press has been fed to THIS gesture yet, so the
        // modifier latch must not still be answering for the previous one — a
        // command started by a Shift+click on a palette entry would otherwise read
        // its very first Click() as additive.
        s_lastShift = false;
    }

    g_activeCommand = cmd;                 // set BEFORE Begin: Begin may query Active()
    if ( !cmd->Begin() )
    {
        g_activeCommand = nullptr;
        KiwiCmd_UndoCancel();              // self-guards when Begin opened nothing
        return false;
    }
    g_nUpdateBits |= 1;
    return true;
}

void KiwiCmd_Commit()
{
    KiwiEditorCommand *cmd = g_activeCommand;
    if ( !cmd )
        return;
    g_activeCommand = nullptr;             // clear FIRST: Commit may re-enter the layer
    cmd->Commit();
    KiwiCmd_UndoCommit();                  // no-op when the command opened no bracket
    // KIWI: AFTER the bracket, never inside it — see
    // DrainDeferredDeselect.  Before the deferred START, so a handed-off command
    // (shakeout G) begins from the selection state it is meant to see.
    DrainDeferredDeselect();
    KiwiNum_Reset();
    s_lastSnap = snap_result_t();
    DrainDeferred();                       // shakeout G — AFTER the reset, never before
    g_nUpdateBits |= 1;
}

void KiwiCmd_Cancel()
{
    KiwiEditorCommand *cmd = g_activeCommand;
    if ( !cmd )
        return;
    g_activeCommand = nullptr;
    cmd->Cancel();
    KiwiCmd_UndoCancel();                  // no-op when the command opened no bracket
    KiwiNum_Reset();
    s_lastSnap = snap_result_t();
    s_deferId = 0;                         // shakeout G: a cancelled gesture hands off
                                           // to nothing — there is no result to place
    // KIWI: and a CANCELLED gesture never deselects — the
    // operation did not finish, so there is nothing to be done with.
    s_deferDeselect = false;
    g_nUpdateBits |= 1;
}

void KiwiCmd_StartDeferred( int commandId, bool paused )
{
    s_deferId    = commandId;
    s_deferPause = paused;
}

// KIWI — see kiwi_command.h and DrainDeferredDeselect.
void KiwiCmd_DeselectAfterCommit()
{
    s_deferDeselect = true;
}

// ─── §4 HOT / PAUSED (shakeout E) ────────────────────────────────────────────
void KiwiCmd_Pause()
{
    if ( !g_activeCommand || !s_hot )
        return;
    // A MULTI-CLICK tool has no "drag" to park — a click there places a point.
    // Pausing one would silently stop its rubber band tracking the cursor, which
    // is the only feedback it has (kiwi_command.h HOT vs PAUSED).
    if ( g_activeCommand->WantsClicks() )
        return;
    s_hot = false;
    g_nUpdateBits |= 1;
}

void KiwiCmd_Resume()
{
    if ( !g_activeCommand || s_hot )
        return;
    s_hot = true;
    // Re-latch the command's input mapping at the CURRENT cursor before anything
    // is fed through it, or the first MouseMove would apply every pixel the
    // cursor travelled while the gesture was parked.
    g_activeCommand->Rebase();
    g_nUpdateBits |= 1;
}

// ── KIWI-UX (ROUND BO, ITEM 3): THE ONE SNAP MODIFIER RULE (kiwi_command.h) ──
// Ctrl INVERTS the active context's default.  GetAsyncKeyState is the physical
// read every ported drag path uses (drag.cpp:308) and the one kiwi_snap.cpp's arm
// 0 already made — this layer runs post-present from the pump, same as those, so
// there is no message-queue latch to go stale.  Polled, never latched, which is
// Plasticity's own discipline (it clears `snaps.xor` on every command teardown —
// plasticity/src/command/CommandExecutor.ts:125).
bool KiwiCmd_SnapEngaged()
{
    const bool ctrl = ( ::GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) != 0;
    const KiwiEditorCommand::kiwiSnapCtx_t ctx =
        g_activeCommand ? g_activeCommand->SnapContext()
                        : KiwiEditorCommand::KSNAPCTX_TRANSFORM;
    if ( ctx == KiwiEditorCommand::KSNAPCTX_ALWAYS )
        return true;
    return ( ctx == KiwiEditorCommand::KSNAPCTX_CONSTRUCT ) != ctrl;
}

// ── KIWI-UX (ROUND BK, ITEM 2): the confirm-source latch (kiwi_command.h) ────
// File-scope rather than a parameter on KiwiCmd_KeyDown, because the ladder is
// re-entered from several places (the numeric layer, kiwi_cmdoptions.cpp) and
// only the ONE call below is an RMB.  Restored on every exit path, including the
// one where the dispatch re-enters this layer (a Commit can start a new command).
namespace { bool s_confirmIsRmb = false; }

bool KiwiCmd_ConfirmIsRmb()
{
    return s_confirmIsRmb;
}

void KiwiCmd_Confirm()
{
    if ( !g_activeCommand )
        return;
    // Deliberately the ENTER path, not KiwiCmd_Commit: a command that consumes
    // VK_RETURN (the polyline's "end the chain") must see an RMB confirm the same
    // way, and that veto lives in exactly one place — KiwiCmd_KeyDown's ladder.
    const bool prev = s_confirmIsRmb;      // nested confirms cannot lie to each other
    s_confirmIsRmb  = true;
    KiwiCmd_KeyDown( 0x0D, 0 );            // VK_RETURN, no modifiers
    s_confirmIsRmb  = prev;
}

// ─── §4 keys ─────────────────────────────────────────────────────────────────
bool KiwiCmd_KeyDown( int vk, unsigned int mods )
{
    if ( !g_activeCommand )
        return false;

    // 0. KIWI-UX (SHAKEOUT G): TAB, WHEN THERE IS NOTHING TO CYCLE.
    //
    //    KiwiNum_Key consumes VK_TAB UNCONDITIONALLY (kiwi_numeric.cpp:238-242 —
    //    above its own modifier gate, so Shift+Tab is taken too), which means no
    //    command could ever see it.  The Ctrl+R face split needs it for the U/V
    //    flip and has no scalar to type, so this rung offers Tab to the active
    //    command FIRST — but only when the numeric layer has AT MOST ONE FIELD,
    //    i.e. when cycling would be a no-op anyway.
    //
    //    WHY THAT CONDITION IS SAFE, checked against every command that exists:
    //    KiwiNum_Reset installs exactly one default field, and KiwiCmd_Start only
    //    replaces it when NumericFields returns > 0.  The commands that declare TWO
    //    are the construction tools with an angle field (kiwi_construct.cpp:780-789)
    //    and the ring primitives (kiwi_primitive.cpp:173-182); every other one
    //    declares one or none.  So this rung is INERT for the two-field commands
    //    (Tab still cycles, as it must) and, for the one-field ones, it only
    //    matters if they consume Tab — and none of them overrides KeyDown to do so.
    //    Behaviour is therefore unchanged everywhere except the one new command
    //    that asks for it.
    if ( vk == 0x09 && KiwiNum_FieldCount() <= 1 )       // VK_TAB
    {
        if ( g_activeCommand->KeyDown( vk, mods ) )
        {
            g_nUpdateBits |= 1;
            return true;
        }
        // Not wanted: fall through to rung 1, where the numeric layer swallows it
        // exactly as it always has.
    }

    // 1. Numeric entry outranks the command's own keys, so a command may bind a
    //    letter without shadowing a digit.  SHAKEOUT E: it also owns Tab, and the
    //    change is dispatched PER FIELD (kiwi_command.h NumericFieldChanged —
    //    field 0 still lands on the old NumericChanged, so nothing pre-shakeout-E
    //    behaves differently).
    if ( KiwiNum_Key( vk, mods ) )
    {
        const int f = KiwiNum_Focus();
        g_activeCommand->NumericFieldChanged( f, KiwiNum_HasValueField( f ),
                                              KiwiNum_ValueWorldField( f ) );
        g_nUpdateBits |= 1;
        return true;
    }

    // 2. Escape's FIRST rung is the framework's: it clears a pending numeric entry
    //    (§13).  This one is NOT offered to the command — a half-typed value must
    //    always be droppable, whatever the command thinks.  SHAKEOUT E: it clears
    //    the FOCUSED field only, so on a two-field command Esc walks back one box
    //    at a time before it reaches the cancel rung.
    if ( vk == 0x1B && KiwiNum_Has() )      // VK_ESCAPE
    {
        const int f = KiwiNum_Focus();
        KiwiNum_ClearField( f );
        g_activeCommand->NumericFieldChanged( f, false, 0.0f );
        g_nUpdateBits |= 1;
        return true;
    }

    // 3. The command's own keys (axis locks; Phase 4's construction Ctrl+Z and its
    //    Esc/Enter rungs).  CHANGED IN PHASE 4: Esc and Enter now reach this call,
    //    which is what kiwi_command.h always documented ("Esc and Enter are taken
    //    by the framework AFTER it, so a command may still veto them") and what
    //    the previous ordering did not actually do.  Behaviour is unchanged for
    //    every Phase-2/3 command: none of them consumes either key, so both fall
    //    through to rungs 4 and 5 exactly as before.
    if ( g_activeCommand->KeyDown( vk, mods ) )
    {
        g_nUpdateBits |= 1;
        return true;
    }

    // 4. Escape cancels.
    if ( vk == 0x1B )                       // VK_ESCAPE
    {
        KiwiCmd_Cancel();
        return true;
    }

    // 5. Enter commits — unless a typed value is pending and the command has a
    //    stage left to spend it on (KIWI-UX ROUND AQ, ITEM 8; the whole rule and
    //    the report behind it are on KiwiEditorCommand::AdvanceStage).
    if ( vk == 0x0D )                       // VK_RETURN
    {
        if ( KiwiNum_Has() )
        {
            if ( !KiwiNum_HasValue() )
            {
                // Typed, but not a complete expression.  Swallow rather than
                // commit: a half-finished number must never be the thing that
                // ends the gesture.
                Sys_Printf( "%s: \"%s\" is not a finished value yet — finish it, "
                            "or clear it with Esc.\n",
                            g_activeCommand->Name(), KiwiNum_Text() );
                return true;
            }
            if ( g_activeCommand->AdvanceStage() )
            {
                g_nUpdateBits |= 1;
                return true;                // the gesture stays live
            }
        }
        KiwiCmd_Commit();
        return true;
    }

    // Everything else is SWALLOWED while a modal command owns the gesture: letting
    // an arbitrary hotkey fire mid-gesture is how a half-finished edit gets an
    // unrelated undo record stapled to it.
    return true;
}

// ─── §5 mouse (from kiwi_viewport.cpp — MMB/RMB/wheel never arrive here) ─────
bool KiwiCmd_MouseMove( int imgX, int imgY )
{
    if ( !g_activeCommand )
        return false;

    // Latched even when the ray fails, so a command's own screen-space mapping
    // (R / S) keeps working while the viewport is mid-resize.  Latched even while
    // PAUSED, so the Rebase() on the next resume starts from where the cursor
    // actually is.
    s_cursorX    = imgX;
    s_cursorY    = imgY;
    s_cursorHave = true;

    // KIWI-UX (shakeout E): PAUSED means the preview is parked — no pick, no snap
    // query, no MouseMove.  Consumed all the same: the press→release cycle still
    // belongs to the command (kiwi_viewport.cpp GESTURE OWNERSHIP), and the pick
    // and snap queries are the expensive half of this function.
    if ( !s_hot )
        return true;

    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return true;                        // consumed; nothing to update

    // The command's own flags drive BOTH queries, so what it is dragging is
    // excluded from its own snap AND its own pick (kiwi_pick.h).
    const unsigned flags = g_activeCommand->PickFlags();

    // ── KIWI-UX (ROUND T): THE SNAP QUERY MAY BE ASKED SOMEWHERE ELSE ────────
    // kiwi_command.h SnapQueryAnchor says why.  Short form: with a pivot placed on
    // a corner and an axis arrow grabbed, the CURSOR is not where the thing that
    // should be snapping is.  A command that knows better names an image position
    // and the query runs THERE — with its own ray, because every snap arm ranks by
    // pixel distance from the query point AND the geometry arms intersect the ray
    // (kiwi_snap.h), so handing the anchor's pixel to the cursor's ray would rank
    // one thing while hitting another.
    //
    // The PICK is untouched: it is what the command's own hover and the mode chips
    // read, and those follow the mouse.
    int snapX = imgX, snapY = imgY;
    ray_t snapRay = ray;
    {
        int   ax = 0, ay = 0;
        ray_t anchorRay;
        if ( g_activeCommand->SnapQueryAnchor( &ax, &ay )
          && ( ax != imgX || ay != imgY )
          && Pick_RayFromImagePos( ax, ay, &anchorRay ) )
        {
            snapRay = anchorRay;             // written only on success
            snapX   = ax;
            snapY   = ay;
        }
    }
    KiwiSnap_Query( snapRay, snapX, snapY, &s_lastSnap, flags );
    const pick_result_t pick = Pick( ray, KiwiSel_GetModeMask(), flags );
    g_activeCommand->MouseMove( pick, s_lastSnap );
    return true;
}

bool KiwiCmd_LastCursor( int *imgX, int *imgY )
{
    if ( !s_cursorHave )
        return false;
    if ( imgX ) *imgX = s_cursorX;
    if ( imgY ) *imgY = s_cursorY;
    return true;
}

// ── KIWI-UX (ROUND AA, ITEM 9) ───────────────────────────────────────────────
bool KiwiCmd_LastShift()
{
    return s_lastShift;
}

bool KiwiCmd_WantsMarquee()
{
    return g_activeCommand != nullptr && g_activeCommand->WantsMarquee();
}

void KiwiCmd_Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift )
{
    if ( !g_activeCommand )
        return;
    // Re-asked rather than trusted: the viewport tested WantsMarquee at the PRESS
    // and the release is a whole drag later, in which the command may have changed
    // stage (or been replaced entirely by a deferred start).
    if ( !g_activeCommand->WantsMarquee() )
        return;
    g_activeCommand->Marquee( x0, y0, x1, y1, crossing, shift );
}

bool KiwiCmd_MouseButton( int btn, int imgX, int imgY, bool shift )
{
    if ( !g_activeCommand )
        return false;
    if ( btn != 0 )                         // only LMB; MMB/RMB stay with the camera
        return false;

    KiwiEditorCommand *cmd = g_activeCommand;

    // ── KIWI-UX (shakeout G): the command may CLAIM the press outright ───────
    // ABOVE the resume arm on purpose: the movable pivot's placement click must
    // not also be read as "resume the drag" (kiwi_command.h PressIntercept).  The
    // pixel is latched first so KiwiCmd_LastCursor is current inside the hook.
    // ROUND AA, ITEM 9: …and the modifier with it, so KiwiCmd_LastShift is current
    // for every rung below — PressIntercept, IdlePressReselect and Click() alike.
    s_cursorX    = imgX;
    s_cursorY    = imgY;
    s_cursorHave = true;
    s_lastShift  = shift;
    if ( cmd->PressIntercept( imgX, imgY ) )
        return true;

    // ── ROUND K: RECLAIM THE PLAIN CLICK WHILE PARKED AND UNMOVED ───────────
    // USER REPORT: "When clicking a face, left clicking is disabled."  The resume
    // arm below is the broadest rule in the layer and it was eating every click
    // that followed a face auto-enter.  Offered ONLY while PAUSED, and only to a
    // command that says it has applied nothing (kiwi_command.h IdlePressReselect);
    // once a real edit exists, everything below runs exactly as shakeout E left it.
    //
    // The command may CANCEL ITSELF inside this call — the face path does, because
    // "click another face" means the old gesture is over — so nothing after this
    // point may assume `cmd` is still the active command.
    // ── KIWI-UX (ROUND N): …AND AN ADDITIVE PRESS IS OFFERED IT WHEN HOT TOO ─
    // USER REPORT, verbatim: "it's not possible to select multiple solid faces at
    // once with shift-clicking."
    //
    // Round K gated this rung on `!s_hot` alone, which is one state too few.  A
    // face auto-enter parks the gesture, but ANY of the ordinary ways back to HOT —
    // the resume arm below, a lollipop-ball grab (KiwiCmd_HandleGrab step 2a), the
    // boolean's own KiwiCmd_Resume — leave the command hot with NOTHING applied,
    // and in that state a Shift+LMB fell straight past this rung to the HOT tail at
    // the bottom of this function, which only pauses.  The press was consumed, the
    // selection never grew, and the second face was never added.
    //
    // SHIFT IS THE WHOLE WIDENING.  A plain click keeps the exact round-K rule
    // (paused only), because while HOT a plain press genuinely does mean "resume
    // this drag".  Shift never means that — it is additive by definition, in the
    // marquee, in the no-command click grammar and here — so offering it in both
    // states is the same rule stated once instead of once per gesture state.  The
    // command still refuses unless it has applied nothing (kiwi_transform.cpp's own
    // gate is unchanged), so a Shift+click mid-edit is untouched.
    if ( ( !s_hot || shift ) && cmd->IdlePressReselect( imgX, imgY, shift ) )
        return true;

    // ── shakeout E: PAUSED → HOT.  Any LMB press in the viewport resumes ─────
    // (the broadest resume rule, and kiwi_command.h says why).  Latch the press
    // pixel FIRST so the command's Rebase() re-latches against it, then feed the
    // move so the preview picks up from the cursor without a jump.
    if ( !s_hot )
    {
        s_cursorX    = imgX;
        s_cursorY    = imgY;
        s_cursorHave = true;
        KiwiCmd_Resume();
        KiwiCmd_MouseMove( imgX, imgY );
        return true;
    }

    // Phase 4: a MULTI-CLICK tool takes the click as an EVENT (a placed point) and
    // only commits when it says so.  UNCHANGED by shakeout E — a click here places
    // a point, it does not pause.
    //
    // ── KIWI-UX (ROUND X, ITEM 9): the MOVE IS NOW THIS BRANCH'S ONLY ────────
    // It used to run unconditionally, above the WantsClicks test.  A multi-click
    // tool genuinely needs it — the click PLACES A POINT and the point must be the
    // one under the cursor — so it stays here, unchanged.  The PAUSE path below no
    // longer gets it; the reason is written out there.
    if ( cmd->WantsClicks() )
    {
        KiwiCmd_MouseMove( imgX, imgY );    // act at the point actually under the cursor
        if ( !g_activeCommand )
            return true;                    // the move ended the gesture under us
        if ( cmd->Click() )
            return true;                    // still running
        KiwiCmd_Commit();
        return true;
    }

    // ── shakeout E: HOT → PAUSED.  The press USED TO COMMIT here ─────────────
    // USER DIRECTIVE: "Releasing an action shouldn't commit it, it should just
    // pause the wip move.  A right click OR an enter press confirms it."  The
    // preview and the value stay exactly as this move left them; RMB-click or
    // Enter ends the gesture, another LMB press resumes it, Esc cancels it.
    //
    // ── KIWI-UX (ROUND X, ITEM 9): PARK FREEZES.  IT DOES NOT RE-EVALUATE ────
    // USER REPORT, verbatim: "Sometimes when extruding and releasing the mouse, it
    // snaps back to the starting value for no reason.  Seems related to snapping."
    //
    // It was.  This arm used to run a full KiwiCmd_MouseMove — a fresh pick AND a
    // fresh snap query — on the very event that parks the gesture, and then pause.
    // A command with no grab gate (the region extrude is the plain case: its
    // Recompute maps the cursor unconditionally) therefore took one more snap
    // answer AFTER the user had stopped aiming, at the parking pixel.  The snap
    // ranking is a "closest candidate in pixels" contest and the source geometry of
    // an extrusion is always ON SCREEN, right where the gesture started, so the
    // extra query is exactly the one most likely to land back on the source — i.e.
    // "it snaps back to the starting value".
    //
    // Parking is not an edit.  The value the user was looking at when they let go
    // IS the parked value, and the cursor stream has already delivered every move
    // up to this pixel (the shell dispatches moves continuously, kiwi_viewport.cpp),
    // so nothing is lost by not asking a second time.  ITEM 4's self-snap exclusion
    // removes the attractor as well; this removes the extra question.
    KiwiCmd_Pause();
    return true;
}

// ─── ROUND L: the ONE handle-grab arm (kiwi_command.h) ───────────────────────
// Every numbered step below is the one in the header, in that order, and every
// one of them earns its place there.  kiwi_gizmo.cpp (arrows, plane corners,
// centre square, rotate rings) and kiwi_lollipop.cpp (the ball) both call THIS
// and neither keeps a copy of it any more.
bool KiwiCmd_HandleGrab( int imgX, int imgY )
{
    KiwiEditorCommand *cmd = g_activeCommand;
    if ( !cmd )
        return false;

    KiwiCmd_MouseMove( imgX, imgY );        // 1. the press pixel IS the grab point
    if ( !s_hot )
        KiwiCmd_Resume();                   // 2a. …which Rebase()s at that pixel
    else
        cmd->Rebase();                      // 2b. already hot: re-latch anyway
    cmd->HandleGrab( true );                // 3. open the command's own gate
    cmd->Rebase();                          // 4. the gate may have moved the origin
    KiwiCmd_MouseMove( imgX, imgY );        // 5. frame one: delta exactly zero
    return true;
}

void KiwiCmd_HandleRelease()
{
    if ( g_activeCommand )
        g_activeCommand->HandleGrab( false );
}

const snap_result_t &KiwiCmd_LastSnap()
{
    return s_lastSnap;
}

// ─── §4 world overlay (Cam_Draw tail) ────────────────────────────────────────
void KiwiCmd_DrawWorld()
{
    if ( !g_activeCommand )
        return;

    // One batch for the command's own geometry AND the snap marker: they are the
    // same gesture, they share the budget, and one batch means one colour-run
    // split at the marker instead of two full command pairs (kiwi_lines.h).
    //
    // BUDGET RAISED 64 -> 192 in shakeout C.  The §16b previews are the first
    // gestures that can legitimately want more than 64 segments: a 32-side
    // polygon is 32, a 16-side cylinder is 48 (two rings + the verticals), a
    // sphere is three 32-segment great circles = 96, and a long spline is
    // 8 segments per span.  64 truncated them mid-shape, which reads as a
    // rendering bug rather than as a budget.  192 is still a fifth of the grid
    // pass's own ceiling and it is per-GESTURE, not per-frame-per-object.
    // KIWI-UX (ROUND X, ITEM 4b): 192 -> 288.  The prism previews gained their
    // BOTTOM RING, so a profile now costs THREE segments per vertex instead of two
    // — and at the KEXT_MAX_PROFILE cap of 64 that is exactly 192, i.e. the old
    // budget truncated the largest legal preview at its own last vertex and left no
    // room at all for the snap marker below.  288 is 3 x 64 + 96 of headroom and is
    // still well under the grid pass's own 440.
    //
    // ── KIWI-UX (ROUND AF, ITEM 3): …AND IT IS NOW A FLOOR, NOT A CEILING ────
    // USER REPORT, verbatim: "When adding a bunch of objects to a boolean (windows
    // on a building), After about 10-12, the red previewer stopped working on newly
    // selected diff tools."  288 / 24 segments-per-box = 12, exactly.  A fixed
    // number cannot serve an overlay whose cost is a function of a set the user is
    // still growing, so a command may now name what it actually needs
    // (KiwiEditorCommand::LineBudget) and the framework takes the larger of that
    // and its own default, clamped to KCMD_LINE_BUDGET_MAX.  Every command that
    // does not override it returns 0 and gets exactly the 288 it always had.
    int budget = KCMD_LINE_BUDGET;
    const int want = g_activeCommand->LineBudget();
    if ( want > budget )
        budget = ( want > KCMD_LINE_BUDGET_MAX ) ? KCMD_LINE_BUDGET_MAX : want;
    KiwiLines_Begin( budget, 2 );
    g_activeCommand->DrawWorld();
    KiwiSnap_EmitMarker( s_lastSnap );
    KiwiLines_Flush();
}

// ─── §5 the shell key funnel ─────────────────────────────────────────────────
bool KiwiUX_KeyFunnel( unsigned int vk )
{
    // ── KIWI-UX (ROUND U): A BARE MODIFIER IS NEVER SWALLOWED ────────────────
    // USER REPORT, verbatim: "I still cant select 2 faces at once with
    // shift-clicks."  THIS is why — and it is not in the selection layer at all.
    //
    // THE MECHANISM, end to end.  Radiant_PreTranslateMessage (radiant_main.cpp:647)
    // calls this for every WM_KEYDOWN and, when it returns true, returns true
    // itself — which means the pump calls NEITHER TranslateMessage NOR
    // DispatchMessage for that message (radiant_main.cpp's pump).  The message
    // therefore never reaches the window procedure, and ImGui's Win32 backend
    // updates io.KeyShift / io.KeyCtrl / io.KeyAlt in exactly ONE place: its
    // WM_KEYDOWN / WM_KEYUP arm (deps/imgui/backends/imgui_impl_win32.cpp:848-857,
    // ImGui_ImplWin32_UpdateKeyModifiers).  Nothing else polls them —
    // ImGui_ImplWin32_NewFrame only runs ProcessKeyEventsWorkarounds
    // (imgui_impl_win32.cpp:531), which can only take a stuck modifier DOWN->UP,
    // never UP->DOWN.
    //
    // So: with a face auto-enter parked (the state the user is in every single
    // time), the LAST arm of this funnel fed VK_SHIFT to KiwiCmd_KeyDown, whose
    // final rung swallows every key it does not recognise — and the Shift press was
    // consumed before ImGui ever heard of it.  io.KeyShift stayed FALSE, so
    // ImGuiShell_ViewportInput (imgui_shell.cpp:344) handed
    // KiwiVP_CameraButtonDown shift = false, KiwiCmd_MouseButton got shift = false,
    // and IdlePressReselect ran its PLAIN arm: cancel, click-select, REPLACE the
    // selection with the one face under the cursor.  Round K's rung and round N's
    // widening were both correct and both unreachable, which is exactly why two
    // rounds of work on them changed nothing.
    //
    // THE FIX IS THIS RUNG AND NOTHING ELSE: a key that IS a modifier is passed
    // straight through, before any other arm, in every state (command live, palette
    // open, add menu open).  It costs nothing downstream — TranslateAccelerator
    // never fires on a bare modifier, and Radiant_TryHotkey cannot either: NO row of
    // g_radiantCommandsDefault (mainfrm.cpp:986-1240) carries vk 0x10/0x11/0x12, and
    // kiwi_keymap.cpp binds no bare modifier.  The L/R variants are included because
    // Windows delivers those to a raw-input/extended keyboard path and the backend's
    // own VK_SHIFT arm reads them (imgui_impl_win32.cpp:874-877).
    //
    // IT ALSO FIXES CTRL.  Ctrl is the DESELECT modifier in the same click grammar
    // (kiwi_boxselect.cpp ClickSelect) and was losing its state the same way.
    switch ( vk )
    {
    case 0x10: case 0x11: case 0x12:            // VK_SHIFT / VK_CONTROL / VK_MENU
    case 0xA0: case 0xA1:                       // VK_LSHIFT / VK_RSHIFT
    case 0xA2: case 0xA3:                       // VK_LCONTROL / VK_RCONTROL
    case 0xA4: case 0xA5:                       // VK_LMENU / VK_RMENU
    case 0x5B: case 0x5C:                       // VK_LWIN / VK_RWIN
        return false;
    default:
        break;
    }

    // The palette owns the keyboard outright while it is open.  Its OWN keys are
    // read from ImGui inside the draw (its InputText holds focus, so
    // ImGuiShell_WantsKeyboard has already let them through upstream of this); this
    // arm is what covers the frame between "opened" and "focused", and any frame
    // where the user clicked off the field.
    // §16b (shakeout C): the add menu owns the keyboard on exactly the same terms
    // as the palette — same frames-before-focus gap, same unclosable-on-Escape
    // failure if this arm were missing.
    if ( KiwiAdd_IsOpen() )
    {
        if ( vk == 0x1B )                   // VK_ESCAPE
            KiwiAdd_Close();
        return true;
    }

    if ( KiwiPalette_IsOpen() )
    {
        // …and because those are exactly the frames where the palette's own Esc
        // handler cannot see the key, close it from here.  Otherwise a palette that
        // has lost text focus would swallow Escape and become unclosable.
        if ( vk == 0x1B )                   // VK_ESCAPE
            KiwiPalette_Close();
        return true;
    }

    // ── KIWI-UX (ROUND N): A FACE-CONTEXT VERB PREEMPTS A PARKED AUTO-ENTER ──
    // USER REPORT, verbatim: "the Ctrl-R feature I asked for still isn't in the
    // editor (splitting of a face on a solid)."
    //
    // THE EXACT SWALLOW POINT was the arm immediately below this one, feeding
    // KiwiCmd_KeyDown, whose final rung returns true for every key it did not
    // recognise.  In Face mode a face selection and a live PAUSED push/pull are the
    // same state (kiwi_boxselect.cpp auto-enters on the click that makes the
    // selection), so Ctrl+R — and Z, E, J, C, Q with it — was swallowed on EVERY
    // press, in exactly the state its own canExecute requires.  kiwi_command.h
    // PreemptIdle writes the chain out in full.
    //
    // WHAT THIS RUNG DOES, and what it deliberately does not:
    //   * it asks the ACTIVE command whether it is an unmoved, auto-entered gesture
    //     (PreemptIdle — round K's IdlePressReselect gate without the pixels);
    //   * it resolves the chord in the LIVE binding table and requires the id to be
    //     one of the six context verbs (PreemptVerb above);
    //   * it requires that verb to be RUNNABLE RIGHT NOW (KiwiCmd_CanExecute), read
    //     against the selection as it stands — which is the same selection that
    //     survives the cancel, because Cancel restores geometry and never clears
    //     the selection;
    //   * then it CANCELS (provably record-free — the same proof round K wrote out
    //     on IdlePressReselect: Begin never mutates, ApplyFaces returns before the
    //     bracket while the scalar is zero, and a PAUSED command receives no
    //     MouseMove at all) and returns FALSE, so the chord falls through to
    //     TranslateAccelerator and then Radiant_TryHotkey and starts the verb by
    //     the ordinary route.  One dispatch path, not two.
    // It changes NOTHING for a gesture that has applied something: PreemptIdle is
    // false there and the swallow below is untouched, which is what keeps an
    // unrelated hotkey from stapling a record onto a half-finished edit.
    //
    // IT IS NOT GATED ON PAUSED, unlike round K's press rung, and the difference is
    // deliberate: for a PRESS, "hot" genuinely means "resume this drag", so the
    // click has a competing meaning; for a KEY it does not — a chord is never a
    // continuation of a drag.  PreemptIdle's own "nothing applied" gate is the real
    // safety, and it holds in both states (an auto-entered gesture reaches HOT
    // through the resume arm and through a handle grab without applying anything).
    if ( g_activeCommand && g_activeCommand->PreemptIdle() )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( id && PreemptVerb( id ) && KiwiCmd_CanExecute( id ) )
        {
            // KIWI-UX (ROUND BR): THIS is the Shift+A gesture, and the only place
            // the selected-face working plane may be minted from now on.  The face
            // survives the cancel below (Cancel restores geometry and never clears
            // the selection — the rung's own argument, above), so the tool that the
            // chord is about to start still finds it; the arm is what tells
            // KiwiCon_AutoPlaneForTool that the user ASKED for that face's plane
            // rather than merely having a face selected.  One shot: it is consumed
            // by the next tool start whether or not the rung fires.
            if ( CreationVerb( id ) )
                KiwiCon_ArmSelectedFacePlane();
            KiwiCmd_Cancel();
            return false;                   // …and let the hotkey table run it
        }
    }

    // ── KIWI-UX (ROUND Z, ITEM 1): A TOOL SWAP TAKES THE GESTURE OVER ───────
    // USER REPORT, verbatim: "when pasting a brush, it goes into move mode
    // automatically, however if I want to paste and rotate(or similar) a brush, it
    // requires a de-selection first.  This is unacceptable, allow tool swaps."
    //
    // PLACED BELOW THE PREEMPT RUNG ON PURPOSE.  The face-context set keeps its own,
    // narrower behaviour (cancel-only, and only for an unmoved auto-enter) byte for
    // byte; this rung is what the keys that fall THROUGH it now reach, instead of
    // KiwiCmd_KeyDown's catch-all swallow one line below.
    //
    // THE FORK IS PLASTICITY'S (kiwi_command.h CanSwapTo has the citations): a
    // gesture that has applied something is COMMITTED, so its undo record closes on
    // its own edit; one that has not is CANCELLED record-free.  Either way the
    // SELECTION survives — Commit only clears it on the FACE push (round K), and
    // KiwiMoveCommand::CanSwapTo refuses that case rather than commit into a verb
    // with nothing left to act on.
    //
    // Then FALSE, so the chord falls through to TranslateAccelerator and
    // Radiant_TryHotkey and the new verb starts by the ordinary route.  One dispatch
    // path, exactly as the round-N rung above does it.
    if ( g_activeCommand )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( id && SwapVerb( id ) && g_activeCommand->CanSwapTo( id )
          && KiwiCmd_CanExecute( id ) )
        {
            const bool moved = g_activeCommand->GestureMoved();
            if ( moved ) KiwiCmd_Commit();
            else         KiwiCmd_Cancel();
            return false;                   // …and let the hotkey table run it
        }
    }

    // ── KIWI-UX (ROUND AA, ITEM 4): DELETE REACHES REMOVE FACE THROUGH A ────
    //    PARKED FACE AUTO-ENTER.
    //
    // USER REPORT, verbatim: "You still can't delete a chamfer that was made on a
    // brush."
    //
    // THE EXACT SWALLOW POINT is the line immediately below this rung.  Round T
    // put Remove Face on the funnel — but BELOW that line, and in Face mode a face
    // selection and a live PAUSED push/pull are the SAME STATE: clicking a face
    // auto-enters KIWI_CMD_MOVE and pauses it (kiwi_boxselect.cpp ClickSelect).
    // So `g_activeCommand` is always set in exactly the state Remove Face requires
    // — one face selected — and DELETE went to KiwiCmd_KeyDown, which is not Tab,
    // not numeric, not Esc/Enter and not one of Move's own keys, and died on its
    // catch-all "Everything else is SWALLOWED" rung.  Round T's rung could only
    // ever fire in the states that do NOT auto-enter (mode 5, an additive click,
    // or after an Esc), which is the "it only works sometimes" the report names.
    // It is the same trap round N found for Ctrl+R and round U found for Ctrl+1..4
    // and the creation chords; kiwi_command.h PreemptIdle writes the chain out.
    //
    // THE SAME SHAPE AS ROUND N'S RUNG, and for the same safety argument: only an
    // UNMOVED, AUTO-ENTERED gesture yields (PreemptIdle), the cancel is provably
    // record-free (no bracket was ever opened — the proof is on PreemptIdle), and
    // the SELECTION survives a Cancel untouched, which is what leaves Remove Face
    // the one face it needs.  "Delete this face" could not be more ABOUT the thing
    // the parked push/pull is holding.
    //
    // WHY IT IS NOT A PreemptVerb ENTRY.  That rung resolves the chord through the
    // binding table and allow-lists the resulting id; DELETE resolves to 33003
    // (Delete Selection, kiwi_keymap.cpp:297), never to KIWI_CMD_REMOVE_FACE,
    // which is registered UNBOUND on purpose so that one key never has two
    // competing rows.  So the arbitration has to be written here, as round T
    // already wrote it below.
    //
    // THE CONSTRUCTION SELECTION STILL OWNS DELETE FIRST — !KiwiConSel_OwnsDelete
    // reproduces the ladder order of the two rungs below, so adding this one
    // changes nothing for any selection that is not exactly "one brush face".
    if ( ( vk == 0x2E || vk == 0x08 )                            // VK_DELETE / VK_BACK
      && g_activeCommand && g_activeCommand->PreemptIdle()
      && !KiwiConSel_OwnsDelete()
      && KiwiBevel_CanRemoveFace() )
    {
        KiwiCmd_Cancel();
        if ( KiwiBevel_RemoveFaceRestoreEdge() )
            return true;
    }

    // ── KIWI-UX (ROUND BJ, ITEM 1): END (CAULK) REACHES A PARKED FACE AUTO-ENTER ──
    //
    // USER REPORT, verbatim: *"The End bind for caulking needs to work on faces.
    // Currently it only works for solids."*
    //
    // THE EXACT SWALLOW POINT is the line immediately below this rung, and the
    // chain is the same one round N found for Ctrl+R, round U for Ctrl+1..4 and
    // round AA for DELETE.  In Face mode a face selection and a live PAUSED
    // push/pull are THE SAME STATE — clicking a face auto-enters KIWI_CMD_MOVE and
    // pauses it (kiwi_boxselect.cpp:884-891, gated `!shift && !ctrl` and Face mode
    // only) — so `g_activeCommand` is set in exactly the state the caulk verb is
    // FOR.  End (VK_END 0x23) is not a bare modifier, not a PreemptVerb (the caulk
    // id is not in that list), not a SwapVerb (only G/R/S) and not DELETE, so it
    // fell to KiwiCmd_KeyDown, which is not Tab, not numeric, not Esc/Enter and not
    // one of Move's own keys, and died on its catch-all "Everything else is
    // SWALLOWED" rung (:1615-1619).  With WHOLE BRUSHES selected nothing
    // auto-enters, `g_activeCommand` is NULL, the key falls through to
    // Radiant_TryHotkey and the verb runs — which is precisely "it only works for
    // solids".  Nothing in kiwi_caulk.cpp was ever reached on a face selection.
    //
    // WHY THIS RUNG DOES *NOT* CANCEL, unlike round N's.  The caulk verb goes
    // through TexWnd_ApplyMaterialAtIndex, whose head already runs the round-BH
    // handshake KiwiUv_EndGestureBeforeApply / …RestoreGestureAfterApply
    // (texwnd.cpp:1289/:1330).  That pair cancels the parked push AND RE-ARMS it
    // afterwards with a fresh BeginFaces baseline, so the face gizmo survives the
    // caulk.  Cancelling here would end the gesture without the latch that brings
    // it back, and the user would lose the push/pull gizmo on every End press.  So
    // this rung only DECLINES to swallow: it returns false and the chord takes the
    // ordinary Radiant_TryHotkey route, exactly as round N's does after its cancel.
    //
    // The gate is still PreemptIdle — a gesture that has APPLIED something is not
    // yielded to a texture write here any more than anywhere else; the handshake
    // itself refuses that case and says so on the console.
    if ( g_activeCommand && g_activeCommand->PreemptIdle() )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( id == KIWI_CMD_CAULK_FACES && KiwiCmd_CanExecute( id ) )
            return false;                   // …the BH handshake owns the gesture
    }

    // ── KIWI-UX (ROUND BN, ITEM 5): A **VIEW** VERB REACHES A PARKED FACE ────
    //    AUTO-ENTER.  THE §82.3 TRAP, FIFTH OCCURRENCE.
    //
    // USER REPORT, verbatim: *"The spacebar feature I asked for isn't here.
    // Pressing spacebar does nothing.  Does the input get eaten somewhere?  Used
    // for something else?  What's going on."*
    //
    // THE ANSWER IS "EATEN, ONE LINE BELOW THIS RUNG", and it is the same chain
    // round N found for Ctrl+R, round U for Ctrl+1..4, round AA for DELETE and
    // round BJ for End.  Space is meaningful exactly when a FACE is selected — and
    // in Face mode a face selection and a live PAUSED push/pull are THE SAME STATE
    // (kiwi_boxselect.cpp ClickSelect auto-enters KIWI_CMD_MOVE on the click that
    // makes the selection).  So `g_activeCommand` is set in exactly the state
    // KiwiFocus_CanViewFace requires, the key fell to KiwiCmd_KeyDown, and Space is
    // not Tab, not Esc/Enter, not one of Move's own keys and — with an EMPTY
    // numeric buffer — not numeric either (kiwi_numeric.cpp:602-603 declines Space
    // when `s_text[f][0]` is 0).  It therefore died on the catch-all "Everything
    // else is SWALLOWED" rung, on EVERY press.  Round BK bound the key one round
    // AFTER round BJ wrote the rule down (UX_DESIGN §82.3) and did not add a rung;
    // its own audit note said "this binding is what Space means with NO command
    // running", which is precisely the state a face selection can never be in.
    //
    // A DECLINE-TO-SWALLOW RUNG, NOT A PreemptVerb ENTRY, and the choice follows
    // the verb's semantics.  PreemptVerb CANCELS the parked gesture before letting
    // the chord through — right for a verb that is about to REBUILD the geometry
    // the gesture is holding (split, extrude, boolean), wrong for a verb that only
    // moves the CAMERA: cancelling would cost the user the face gizmo on every
    // Space press, for a command that does not touch the selection, the geometry or
    // the undo journal at all.  So this rung is round BJ's shape: it returns FALSE
    // and the chord takes the ordinary TranslateAccelerator -> Radiant_TryHotkey
    // route, with the parked push left exactly as it was.
    //
    // ── AND THE NUMERIC SEPARATOR IS FENCED OFF, WHICH IS THE OTHER HALF ─────
    // `!KiwiNum_Has()` is what keeps "10ft 6in" working: a modal command with a
    // half-typed value in ANY field keeps every key, so Space inside a typed value
    // is still part of the value (kiwi_numeric.cpp:602) and Enter/Esc still resolve
    // it.  The guard is on the RUNG rather than in kiwi_numeric.cpp because the
    // numeric layer's own gate is already correct and per-FIELD; this one has to be
    // per-ENTRY, and it is what lets the same rung carry `/` (Focus On Selection)
    // safely — 0xBF is ALSO the division operator in a typed expression.
    //
    // BOTH VIEW VERBS, because they are one class and had one defect: `/` (Focus
    // On Selection, kiwi_focus.h) was swallowed by the identical mechanism in the
    // identical state and nobody had reported it yet.
    //
    // STILL GATED ON PreemptIdle, like all four rungs above it.  Not because a
    // camera move could corrupt anything — it cannot; it writes no geometry, no
    // selection and no undo record — but because a gesture that is genuinely BEING
    // DRAGGED maps the cursor into the world every MouseMove, and re-aiming the
    // camera underneath it would teleport the drag.  "Auto-entered and has applied
    // nothing" is exactly the parked state the report is about, and it is the state
    // in which there is no drag to disturb.
    if ( g_activeCommand && g_activeCommand->PreemptIdle() && !KiwiNum_Has() )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( ( id == KIWI_CMD_VIEW_FACE || id == KIWI_CMD_FOCUS_SELECTION )
          && KiwiCmd_CanExecute( id ) )
            return false;                   // …the camera verb runs, the gesture stays
    }

    if ( g_activeCommand )
        return KiwiCmd_KeyDown( (int)vk, CurrentMods() );

    // ── KIWI-UX (shakeout F): DELETE on a PURE construction selection ────────
    // WHY HERE and not by hijacking command id 33003: 33003 is the ported Delete
    // Selection and it is reached from the Edit menu, from the palette, from
    // Backspace and from the shakeout-B VK_DELETE alias row.  Putting a
    // construction test INSIDE it would make all four routes conditional on a
    // KIWI-only piece of state, which is exactly the kind of silent divergence
    // "ported-logic changes only as // KIWI-UX fences" exists to stop.  The funnel
    // is the right place because it is the ONE rung that sees a raw key BEFORE the
    // hotkey table resolves it to any command at all.
    //
    // IT CANNOT SWALLOW A BRUSH DELETE.  KiwiConSel_OwnsDelete requires that
    // construction items are selected AND that `selected_brushes` is EMPTY.
    // ROUND U widened the second half (it used to read KiwiSel() as well, which
    // made a mixed marquee decline and then do nothing at all — the whole argument
    // is on the definition, kiwi_conselect.cpp THE MIXED-SELECTION DELETE).  With
    // any WHOLE-BRUSH selection this still returns false, the funnel returns false,
    // and the key proceeds to Radiant_TryHotkey exactly as it did before.
    // Backspace is included for the same reason the modern profile made Delete a
    // second delete key: the two must never mean different things.
    // ── KIWI-UX (ROUND K): Escape drops a REGION selection ──────────────────
    // A selected region (kiwi_region.h) is a third KIWI-owned selection and nothing
    // in the ported layer knows it exists, so nothing else would ever clear it on
    // Escape — which is what the region hint strip advertises.
    //
    // IT DOES NOT CONSUME THE KEY.  Escape means "deselect", and with both a brush
    // selection and a region live it has to mean deselect BOTH; returning true here
    // would leave the brush half selected and turn one key into two presses.  So
    // this clears its own state and falls through to the ordinary hotkey table,
    // which is unchanged.
    if ( vk == 0x1B && KiwiRegion_HasSelection() )                   // VK_ESCAPE
        KiwiRegion_ClearSelection();

    // The SUN HELPER is a fourth KIWI-owned selection (kiwi_sun.h) and nothing in
    // the ported layer knows it exists.  Same rung shape and same ruling as the
    // region one immediately above: it clears its own state and does NOT consume
    // the key, because Escape means "deselect" and with a brush selection live too
    // it has to mean deselect BOTH.
    if ( vk == 0x1B && KiwiSun_Selected() )                          // VK_ESCAPE
        KiwiSun_ClearSelection();

    // ── KIWI-UX (ROUND AJ, ITEM 1): ESCAPE LEAVES PATCH VERTEX MODE ─────────
    // USER REPORT: "The curve vertex mode needs to hide the points when the curve
    // is no longer selected."  Escape is the universal back-out in this editor and
    // a mode with only one way out (V) is a mode users get stuck in.
    //
    // IT CONSUMES THE KEY, unlike the region rung immediately above.  A region and
    // a brush selection are two halves of ONE selection, so Escape has to clear
    // both; a MODE is a level, and backing out one level at a time is what makes
    // "Esc again to deselect" readable.  Leaving hands the patches back as whole
    // objects, so the second Escape reaches 33002 with something to drop.
    if ( vk == 0x1B && KiwiPatchVerts_HandleEscape() )               // VK_ESCAPE
        return true;

    // ── KIWI-UX (ROUND BM, ITEM 1b): ESCAPE CANCELS A SECTION *PICK* ────────
    // It consumes the key, on the same terms as the patch-vertex rung above: the
    // pick is a MODE the user just entered and backing out one level at a time is
    // what makes "Escape again to deselect" readable.  It does NOT turn a live
    // section off — Escape means "drop the selection" and a section is not one; the
    // button (or the palette row) is what clears it.  Self-gated: false in every
    // state but PICKING, so Escape is untouched the rest of the time.
    if ( vk == 0x1B && KiwiSection_HandleEscape() )                  // VK_ESCAPE
        return true;

    if ( ( vk == 0x2E || vk == 0x08 ) && KiwiConSel_OwnsDelete() )   // VK_DELETE / VK_BACK
    {
        KiwiConSel_DeleteSelected();
        return true;
    }

    // ── KIWI-UX (ROUND U): BARE H ON A CONSTRUCTION SELECTION ───────────────
    // USER DIRECTIVE: "We dont have a hide mechanic right now, you should add
    // that(H)".  Same shape and same reasoning as the Delete arm above — the key
    // is SHARED with a ported id (32923 Hide Selected, which round J left on bare
    // H) and the funnel is the one rung that sees the raw key before the table
    // resolves it, so this is where the two halves are arbitrated.
    //
    // THE DISPATCH, in full:
    //   construction only  -> hide them, CONSUME the key
    //   brush only         -> OwnsHide is false; untouched, 32923 runs
    //   BOTH               -> hide the construction half, then return FALSE so
    //                         32923 hides the brush half too.  One press, both.
    //
    // BARE H ONLY.  Shift+H (isolate), Alt+H (show hidden) and Ctrl+H (invert)
    // are read from CurrentMods and left entirely alone, so round J's H family is
    // unchanged in every other chord.  KiwiVis_CanHide is the same predicate the
    // palette greys 32923 on, so "would the classic half do anything" is asked in
    // exactly one place.
    // ── KIWI-UX (ROUND AF, ITEM 9): H ALSO EXPLAINS THE PATCH GATES, ONCE ────
    // USER REPORT, verbatim: "When hiding selected objects, q3 curves don't hide.
    // actually they aren't selectable at all. Fix this."
    //
    // The hide half is fixed at the defect (camwnd.cpp's patch pass never asked
    // FilterBrush).  The SELECTABILITY half has four legitimate, silent gates, and
    // this is the keystroke at which the user discovers it — so it is where the one
    // line naming them belongs.  It READS state and changes nothing, it fires at
    // most once per session, and it says nothing at all in a map with no patches in
    // it (kiwi_visibility.h).  Deliberately ABOVE the OwnsHide arm: the report is
    // about brush-side patches, which is exactly the case OwnsHide answers false
    // for and lets fall through to 32923.
    if ( vk == 0x48 && CurrentMods() == 0 )                          // H
        KiwiVis_ReportPatchGates( false );

    if ( vk == 0x48 && CurrentMods() == 0 && KiwiConSel_OwnsHide() )  // H
    {
        KiwiConSel_HideSelected();
        if ( !KiwiVis_CanHide() )
            return true;                    // nothing brush-side to hide as well
        return false;                       // …and let 32923 hide the brushes too
    }

    // ── KIWI-UX (ROUND T): DELETE ON EXACTLY ONE BRUSH FACE ─────────────────
    // USER DIRECTIVE, verbatim: "Once a chamfer/bevel is made, allow (DEL) key to
    // remove the chamfer and restore the original edge/corner."
    //
    // THE RULE, and it is deliberately the simple one:
    //     DELETE with exactly ONE FACE selected (and nothing else but patches)
    //         → remove that face; the neighbours re-extend and the edge comes
    //           back.  §19-validated; a refusal restores the brush and says so.
    //     DELETE with anything else selected
    //         → the classic Delete Selection, untouched.
    //
    // WHY NOT "detect a bevel face".  Nothing records that a face was made by a
    // bevel, and every heuristic for guessing it (face count > 6, a two-neighbour
    // obtuse winding, …) is a guess that is wrong on somebody's map.  Removing a
    // face from a convex brush IS the restore — a chamfer is one extra half-space
    // and nothing more — so the verb is the general one, named for what it does.
    //
    // IT CONSUMES THE KEY EVEN WHEN IT REFUSES, on purpose.  Falling through to
    // the classic delete after "I could not remove that face" would delete the
    // whole brush, which is the one outcome a user pressing DEL on a face never
    // means.  KiwiBevel_RemoveFaceRestoreEdge returns false ONLY when the
    // selection is not the shape this rung is for, i.e. when it never started.
    //
    // ABOVE the fly-swallow rung and BELOW the construction one, matching the
    // ownership order everywhere else in this ladder: construction selection,
    // then brush-side selection, then the camera.
    if ( ( vk == 0x2E || vk == 0x08 ) && KiwiBevel_CanRemoveFace() )  // VK_DELETE / VK_BACK
    {
        if ( KiwiBevel_RemoveFaceRestoreEdge() )
            return true;
    }

    // KIWI-UX (shakeout A): while the RMB mouselook is live, W/A/S/D/Q/E belong to
    // the fly.  The fly itself reads them with GetAsyncKeyState and never touches
    // the message queue, so this rung exists only to stop the SAME physical press
    // ALSO reaching Radiant_TryHotkey — S is Scale in the modern keymap, D is
    // "Select Complete Tall" in the classic one, and flying should not run either.
    // Placed BELOW the active-command arm on purpose: a modal command still owns
    // its own keys even while the user is flying (spec §4 — camera navigation
    // never interrupts a command, and a command never loses its keys to it).
    if ( KiwiCam_FlySwallowKey( vk ) )
        return true;

    return false;
}

// ─── §4 undo bracket helpers ─────────────────────────────────────────────────
// The protocol verified in undo.cpp and mirrored from drag.cpp:377-378 /
// drag.cpp:1705-1706 (and select.cpp:1873-1874 for the expanded head).  See the
// long note in kiwi_command.h for what a command must feed and why `operation`
// has to be a literal.
void KiwiCmd_UndoBegin( const char *operation )
{
    if ( s_undoOpen )
        return;                             // one bracket per gesture, always
    // ── KIWI-UX (ROUND BJ, ITEM 4): SNAPSHOT THE SELECTION FOR THE JOURNAL ──────
    // Taken HERE because this is the one place every KIWI gesture opens its
    // bracket, and because it has to be the selection as it stood BEFORE the first
    // mutation.  The snapshot is by VALUE (brush ordinal + bounds fingerprint +
    // indices, never a pointer — Undo_Undo re-creates the brushes), it rides the
    // ticket the closing Undo_End mints, and it is re-applied after the legacy
    // undo/redo completes.  kiwi_undo.h "THE FACE-GRANULAR RESTORE" has the
    // root-cause chain.  Ported records never call this function, so classic undo
    // behaviour is untouched.
    KiwiUndo_ArmSelectionSnapshot();
    Undo_ClearRedo();
    Undo_GeneralStart( operation );         // stores the POINTER — literals only
    Undo_AddBrushList( &selected_brushes );
    s_undoOpen = true;
}

// KIWI-UX (CLEANUP, UndoCoverBrush): the ONE per-brush cover — see the note on
// the declaration in kiwi_command.h for why it is here and what it mirrors.
void KiwiCmd_UndoCoverBrush( selbrush_t *node )
{
    if ( !node || !node->def )
        return;
    entity_s *owner = node->def->owner;
    if ( owner && owner->eclass && owner->eclass->fixedsize )
        Undo_AddEntity( (int)(intptr_t)owner );
    Undo_AddBrush( (entity_brush_s *)node->def );
}

void KiwiCmd_UndoCommit()
{
    if ( !s_undoOpen )
        return;
    s_undoOpen = false;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

void KiwiCmd_UndoCancel()
{
    if ( !s_undoOpen )
        return;
    s_undoOpen = false;
    // Close the record first — Undo_Undo warns and misbehaves on an unfinished one
    // (undo.cpp:743) — then roll it back, then drop the redo it just produced, so a
    // cancelled gesture leaves NOTHING behind in either direction.
    //
    // KIWI-UX (shakeout I): "in either direction" now has to include the unified
    // JOURNAL.  The Undo_End below is a real close and would otherwise mint a
    // LEGACY ticket for a record the very next line destroys — one phantom Ctrl+Z
    // per cancelled gesture, which is precisely the class of bug the journal
    // exists to remove.  The suppression stops the APPEND hooks only: the
    // Undo_ClearRedo hook still fires inside the window, because the legacy redo
    // list genuinely is cleared here and the journal's redo stack must follow it.
    KiwiUndo_SuppressBegin();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    Undo_Undo();
    Undo_ClearRedo();
    KiwiUndo_SuppressEnd();
    g_nUpdateBits = -1;
}
