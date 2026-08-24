#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_hints.cpp — the two contextual hint strips.  See kiwi_hints.h.
//
// NEW code; it reads the command table and the selection and draws text.  It
// mutates nothing.
//
// ── SHAKEOUT I: THE PANEL BECAME TWO STRIPS ─────────────────────────────────
// USER REPORT, verbatim: "this popup box is not very helpful, also it should be
// in the bottom left and bottom right like modern plasticity."
//
// Both halves of that are addressed, and they are different complaints.
//
// (1) POSITION.  Plasticity puts exactly two things at the bottom of its
// viewport, one per corner — `src/index.html:31-32` mounts
// `<plasticity-keybindings>` and `<plasticity-dialog>` inside the viewport, and
// their own class strings pin them:
//     components/viewport/Keybindings.tsx:52   "flex absolute right-3 bottom-3 …"
//     components/dialog/Dialog.tsx:32          "absolute bottom-2 left-2 w-96 …"
// i.e. in the 0.6 checkout in this tree the KEYBOARD CHIPS are bottom-RIGHT and
// the COMMAND OPTIONS are bottom-LEFT.  This file ships them the other way round
// (prompts left, verbs right) because that is what the round's brief specified;
// swapping them is two constants below (KHINT_PROMPTS_LEFT).
//
// (2) "NOT VERY HELPFUL", which is the part that matters.  The old panel was a
// single right-edge box that showed ONE list — either the command's keys or the
// selection's verbs, never both — as a column of "<key>  <label>" rows in a
// fixed-width font layout.  Three concrete failures:
//     * it never showed a command's OWN keys.  A drawing tool's Z (the vertical
//       constraint, shakeout H) and its Esc-clears-the-chain rung existed and
//       were advertised nowhere.  That is what KiwiEditorCommand::HudPrompts
//       (kiwi_command.h) is for.
//     * with a selection AND a live command you got the command list only, so
//       the verbs you were one Esc away from vanished mid-gesture.
//     * a column down the right edge sat exactly where the geometry being edited
//       usually is, and it was the widest thing on screen at 45% of the image.
// So: KEYS on the left, VERBS on the right, both as compact CHIPS on one or two
// rows, both hugging the bottom edge where nothing is ever being modelled.
//
// ── THE CONTENT MODEL, in one paragraph ────────────────────────────────────
// BOTTOM-LEFT is "what do the keys do RIGHT NOW".  With a command live that is
// the framework's own grammar (confirm / cancel / Tab / digits / axis) — derived
// here, because it is identical for every modal command — plus whatever the
// command adds through HudPrompts().  With nothing running it is the keys that
// act on the current selection (Delete, the Ctrl+digit conversions, Esc), or,
// with nothing selected, the CREATE chords.
// BOTTOM-RIGHT is "what can I DO with what is selected" — the verb menu for the
// dominant selection kind, or for a construction selection.  It is EMPTY while a
// command runs (a verb list about a selection is noise in the middle of an edit
// to it) and empty with nothing selected.
//
// EVERY VERB CHIP STILL READS ITS KEY LIVE from g_radiantCommands, exactly as the
// old panel did — remap G, switch to the classic profile, edit radiant.ini, and
// the strip says the truth on the next frame.  The PROMPT chips are literals,
// because the keys they name (Esc, Tab, X/Y/Z, a command's own Z) are handled
// inside KiwiCmd_KeyDown and the command's KeyDown and are not table rows at all.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_hints.h"
#include "radiant_frame.h"          // KIWI-UX (CLEANUP, C-6): struct RadiantCommand + the table API
#include "kiwi_command.h"
#include "kiwi_construct.h"       // ROUND AT, ITEM 2 — the working-plane readout chip
#include "kiwi_grid.h"            // ROUND AJ, ITEM 5 — the grid-snap master switch
#include "kiwi_lollipop.h"        // ROUND K — the prompt strip names the ball
#include "kiwi_region.h"          // ROUND K — the region-selected strip
#include "kiwi_conselect.h"          // shakeout F — the construction-selection rows
#include "kiwi_selection.h"
#include "kiwi_sun.h"             // the sun helper's own strip (pitch / yaw readout)
#include "kiwi_transform.h"
#include "kiwi_units.h"           // ROUND AT, ITEM 2 — the working height, in §17 units
#include "radiant_registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// KIWI-UX (CLEANUP, C-6): the struct AND these declarations now live in
// radiant_frame.h, included above.  This file used to carry a verbatim copy of
// `struct RadiantCommand` plus its own externs, as six other TUs did.

// ── mainfrm.cpp bindings (verified against their definitions) ───────────────
// ROUND AI, ITEM 6 (link fix): declared at FILE scope, not inside the anonymous
// namespace below — MSVC mangles a block-scope extern declared inside an anon
// namespace WITH that namespace (?A0x...@), which can never match the real
// external definition (kiwi_patchverts.cpp:289) and dies at link.
extern bool        KiwiPatchVerts_Active();                                 // kiwi_patchverts.cpp
// KIWI-UX (ROUND AO, ITEM Y): "is the advanced-terrain paint tool ARMED" — a mode
// other than Disabled AND outer radius > inner radius.  Signature copied verbatim
// from its definition, `int sub_401D50()` at patchdialog.cpp:229 (IDB 0x401D50);
// it is the same gate the cursor ring (camwnd.cpp:3189) and the Alt+LMB routing
// (kiwi_viewport.cpp's LMB arm) use, so the chip can only appear when the stroke
// really would fire.  FILE scope for the reason the note above gives.
extern int         sub_401D50();                                            // patchdialog.cpp:229

namespace
{
    // Chips are far cheaper per item than the old full-width rows, so the budget
    // goes up: 12 per strip, laid out over at most KHINT_MAX_ROWS rows.
    enum { KHINT_MAX_CHIPS = 12, KHINT_MAX_ROWS = 2 };

    // Which corner the KEY PROMPTS take.  The verbs take the other one.  See the
    // Plasticity note at the top: 0.6 does it the other way round, and this is the
    // one constant that decides it.
    // KIWI-UX (CLEANUP, C-39): a COMPILE-TIME KNOB, not state.  Nothing writes it
    // and nothing can; it exists so the two corners are named once rather than
    // twice, and `!KHINT_PROMPTS_LEFT` reads as "the other corner".  The false arm
    // is unreachable by construction and is meant to be.
    constexpr bool KHINT_PROMPTS_LEFT = true;

    int s_show = -1;                    // -1 = not read from the profile yet

    // KIWI-UX (CLEANUP, C-35): ONE cap for the keycap text.  LiveKey formatted into
    // 32 bytes and AddChip then truncated into a 24-byte field, silently — one
    // constant, so the two cannot disagree.  ("Shift+Ctrl+PageDown" is 19 chars and
    // the palette fallback wraps it in "(...)", so the margin is thin.)
    enum { KHINT_KEYCAP_MAX = 32 };

    struct chip_t
    {
        char  key[KHINT_KEYCAP_MAX];
        char  label[40];
        // KIWI-UX (CLEANUP, C-34): measured ONCE, by MeasureChips, right after the
        // build.  DrawStrip measures the strip and then lays it out, and DrawChip
        // needs the keycap width again, so an un-cached ChipWidth ran CalcTextSize
        // three times per chip per frame.
        float kw;                       // keycap box width (text + 2 * KHINT_PAD_X)
        float w;                        // whole chip: keycap + gap + label
    };

    // The live binding for a command id, formatted compactly ("Shift+G", "Del").
    // CommandList_Mods is deliberately NOT used: it spells the modifiers out in
    // full ("Control + ") for the command-list panel's wide column, which is
    // three times too wide for a chip.  The KEY NAME still comes from mainfrm's own
    // table, so the two never disagree about what a vk is.
    // Returns false when the command has no binding in the current profile.
    // ── ROUND J: OEM PUNCTUATION KEY NAMES ──────────────────────────────────
    // CommandList_KeyName (mainfrm.cpp:3921) is a PORT of the binary's sub_40BBC0
    // and falls back to `keybuf[0] = (char)c.vk`, which for a VK_OEM_* code is a
    // meaningless glyph — Focus On Selection is on 0xBF ("/") and would print as
    // one.  The ported key-NAME table (g_radiantKeys, mainfrm.cpp:1323-1336) was
    // lifted verbatim from the exe and is not edited to fix a display detail, so
    // the override lives HERE, in the KIWI file that actually draws the chip.
    // The command-list panel still shows the raw glyph; that is logged in
    // RADIANT_KNOWN_ISSUES rather than papered over in two places.
    const char *OemKeyName( unsigned vk )
    {
        switch ( vk )
        {
        case 0xBA: return ";";
        case 0xBB: return "=";
        case 0xBF: return "/";
        case 0xC0: return "~";
        case 0xDE: return "'";
        default:   return 0;
        }
    }

    bool LiveKey( int commandId, char *out, size_t outSize )
    {
        const RadiantCommand *table = 0;
        const int count = Radiant_GetCommandTable( &table );
        if ( !table )
            return false;
        for ( int i = 0; i < count; ++i )
        {
            if ( table[i].commandId != commandId || !table[i].vk )
                continue;
            char keybuf[8];
            const char *key = OemKeyName( table[i].vk );
            if ( !key )
                key = CommandList_KeyName( table[i], keybuf );
            char mods[24];
            mods[0] = '\0';
            if ( table[i].mods & 1 ) strcat( mods, "Shift+" );
            if ( table[i].mods & 2 ) strcat( mods, "Alt+" );
            if ( table[i].mods & 4 ) strcat( mods, "Ctrl+" );
            if ( table[i].mods & 8 ) strcat( mods, "Win+" );
            _snprintf( out, outSize, "%s%s", mods, key ? key : "?" );
            out[outSize - 1] = '\0';
            return true;
        }
        return false;
    }

    void AddChip( chip_t *chips, int *n, const char *key, const char *label )
    {
        if ( *n >= KHINT_MAX_CHIPS )
            return;
        _snprintf( chips[*n].key,   sizeof( chips[*n].key ),   "%s", key );
        _snprintf( chips[*n].label, sizeof( chips[*n].label ), "%s", label );
        chips[*n].key  [sizeof( chips[*n].key   ) - 1] = '\0';
        chips[*n].label[sizeof( chips[*n].label ) - 1] = '\0';
        chips[*n].kw = 0.0f;            // MeasureChips fills both, once, after the build
        chips[*n].w  = 0.0f;
        ++( *n );
    }

    // ── KIWI-UX (ROUND AT, ITEM 2b): THE WORKING-PLANE READOUT ──────────────
    // Offered only while a plane-placing tool is live (KiwiCon_PlanePlacement —
    // the same question kiwi_snap.cpp's CPLANE arm asks), and only when the plane
    // is somewhere a user would want told: OFF the world's major plane, or tilted.
    // Both of those are states the round-AT report was unable to see.
    // KIWI-UX (ROUND BS): no code change here, and that is the point — round BS
    // narrowed KiwiCon_PlanePlacement to the PLANAR placers, so this chip stopped
    // appearing for the line/polyline/spline tools by itself.  A plane readout
    // during line work would name a surface the line does not use.
    void AddWorkingPlaneChip( chip_t *chips, int *n )
    {
        if ( !KiwiCon_PlanePlacement() )
            return;
        const kconPlane_t &p = KiwiCon_ActivePlane();
        int axis = -1;
        for ( int k = 0; k < 3; ++k )
            if ( fabsf( p.normal[k] ) > KCON_PLANE_PARALLEL )
                axis = k;
        if ( axis < 0 )
        {
            AddChip( chips, n, "Plane", "tilted" );
            return;
        }
        const float h = p.origin[axis];
        if ( fabsf( h ) < 0.01f )
            return;                    // the world's own major plane: nothing to say
        char hs[24];
        KiwiUnits_Format( hs, (int)sizeof( hs ), h );
        char label[40];
        _snprintf( label, sizeof( label ), "%s %s",
                   ( axis == 2 ) ? "XY" : ( axis == 1 ) ? "XZ" : "YZ", hs );
        label[sizeof( label ) - 1] = '\0';
        AddChip( chips, n, "Plane", label );
    }

    // ── ROUND AI, ITEM 6: the PATCH VERTEX MODE chip ────────────────────────
    // A MODE with no command running has nowhere else to announce itself — the
    // status line (KiwiNum_DrawHud) reads a live command's HudStatus and there is
    // none here — and a mode the user cannot see they are in is how you get "V
    // opens something and then nothing works".  It leads both idle strips, and it
    // names the way OUT, which is the one thing a toggle must always advertise.
    void AddPatchVertsChip( chip_t *chips, int *n )
    {
        if ( !KiwiPatchVerts_Active() )               // declared at file scope above
            return;
        // ── KIWI-UX (ROUND AJ, ITEM 1): THE MULTI-POINT GRAMMAR, ON THE STRIP ──
        // USER REPORT, verbatim: "it's also impossible to edit more than 1."  Both
        // gestures already existed (Shift is Sel_Add in kiwi_boxselect.cpp's click
        // grammar; the rect's SEL_VERTEX arm collects control points) — what did
        // not exist was any statement that they did.  These lead both idle strips
        // while the mode is live, and the LEAVE chip stays last so the way out is
        // always the thing the eye lands on after the verbs.
        AddChip( chips, n, "Shift+LMB", "Add point" );
        AddChip( chips, n, "Ctrl+LMB",  "Remove point" );
        AddChip( chips, n, "Drag box",  "Take several" );
        AddChip( chips, n, "V / Esc",   "Leave patch vertex mode" );
    }

    // A VERB chip: the key comes from the live table.  An unbound command falls
    // back to the PALETTE's own live key, because that is how it is actually
    // reached — the same fallback the old panel used.
    void AddVerb( chip_t *chips, int *n, int commandId, const char *label )
    {
        // KIWI-UX (CLEANUP, C-35): no cap test here — AddChip owns it, and testing
        // it in two places is how the two drift apart.
        char key[KHINT_KEYCAP_MAX];
        if ( !LiveKey( commandId, key, sizeof( key ) ) )
        {
            char pal[KHINT_KEYCAP_MAX];
            if ( LiveKey( KIWI_CMD_PALETTE, pal, sizeof( pal ) ) )
                _snprintf( key, sizeof( key ), "(%s)", pal );
            else
                _snprintf( key, sizeof( key ), "%s", "(palette)" );
            key[sizeof( key ) - 1] = '\0';
        }
        AddChip( chips, n, key, label );
    }

    // ── BOTTOM-LEFT, command live ───────────────────────────────────────────
    // The framework's own grammar, exactly as kiwi_command.cpp's KiwiCmd_KeyDown
    // ladder implements it (numeric + Tab first, then the command's axis keys,
    // then Esc, then Enter) plus the mouse grammar KiwiCmd_MouseButton and
    // kiwi_viewport.cpp's RMB arm provide — then the command's OWN keys.
    //
    // Shakeout E's flow is what this describes: an LMB release no longer COMMITS,
    // it PAUSES, and RMB-click or Enter is what confirms (kiwi_command.h HOT vs
    // PAUSED).  A MULTI-CLICK tool is called out separately: there a click still
    // PLACES A POINT.
    int BuildCommandPrompts( chip_t *chips, KiwiEditorCommand *cmd )
    {
        int n = 0;
        if ( KiwiDrop_Active() )
        {
            // Exact drop-mode hint: Drop to ground · G Move gizmo · Ctrl Snap · Esc Cancel
            AddChip( chips, &n, "Drop", "to ground" );
            AddChip( chips, &n, "G",    "Move gizmo" );
            AddChip( chips, &n, "Ctrl", "Snap" );
            AddChip( chips, &n, "Esc",  "Cancel" );
            return n;
        }
        if ( cmd && cmd->PreemptIdle() )
        {
            AddChip( chips, &n, "LMB",       "Pick" );
            AddChip( chips, &n, "Shift+LMB", "Add" );
            AddChip( chips, &n, "Ctrl+LMB",  "Remove" );
        }
        const bool clicks = ( cmd && cmd->WantsClicks() );
        AddChip( chips, &n, "RMB/Enter", "Confirm" );
        AddChip( chips, &n, "Esc",       "Cancel" );
        // KIWI-UX (ROUND K): a command wearing the LOLLIPOP has ONE thing to drag
        // and it is the ball, so say so rather than the generic "Drag / Adjust" —
        // the whole complaint the handle answers is that a paused push/pull looked
        // like nothing was happening (kiwi_lollipop.h).
        const bool lolli = KiwiLollipop_Active();
        AddChip( chips, &n, clicks ? "LMB" : "Drag",
                            clicks ? "Place point" : ( lolli ? "Drag the ball" : "Adjust" ) );
        AddChip( chips, &n, "X/Y/Z",     "Axis" );
        // Only the MOVE command accepts a plane lock (kiwi_transform.cpp
        // HandleAxisKey's `allowPlane`; R and S pass false).
        if ( cmd && cmd->Name() && strcmp( cmd->Name(), "Move" ) == 0 )
            AddChip( chips, &n, "Shift+XYZ", "Plane" );
        AddChip( chips, &n, "Tab",       "Field" );
        AddChip( chips, &n, "0-9",       "Exact" );
        // ── KIWI-UX (ROUND BO, ITEM 3): THE ONE SNAP CHIP, ALWAYS SHOWN ──────
        // USER REPORT, verbatim: *"some operations it's the opposite, it's
        // confusing"*.  Round Z showed a "Ctrl Snap" chip only for the three
        // opted-in commands and said nothing at all for the rest, so the strip's
        // SILENCE was carrying the other half of the grammar — which is a hint that
        // lies by omission, and this file's rule forbids that.
        //
        // Now every gesture says which way Ctrl goes for IT, in its own words, read
        // off the same SnapContext the snap layer reads (kiwi_command.h), so the
        // chip and the behaviour cannot drift:
        //     TRANSFORM   "Ctrl  Snap"        (raw until held)
        //     CONSTRUCT   "Ctrl  Free"        (snaps until held)
        //     ALWAYS      "Snap  On"          (the pivot placement, no modifier)
        if ( cmd )
        {
            switch ( cmd->SnapContext() )
            {
            case KiwiEditorCommand::KSNAPCTX_CONSTRUCT:
                AddChip( chips, &n, "Ctrl", "Free" );
                break;
            case KiwiEditorCommand::KSNAPCTX_ALWAYS:
                AddChip( chips, &n, "Snap", "On" );
                break;
            default:
                AddChip( chips, &n, "Ctrl", "Snap" );
                break;
            }
        }
        // ── KIWI-UX (ROUND AJ, ITEM 5): SAY WHEN THE GRID IS OFF ────────────
        // The new checkbox by the grid pill is persistent and lives at the far
        // top-right of the viewport; a user mid-gesture is looking at the cursor
        // and the chip strip.  Every numbered chip above ("0-9 Exact", "Ctrl
        // Snap") is about how a value gets decided, so the strip is exactly where
        // "and the grid is not one of the deciders right now" belongs.  Shown ONLY
        // in the non-default state, so the strip does not grow for everyone.
        if ( !KiwiGrid_SnapEnabled() )
            AddChip( chips, &n, "Grid",  "Snap OFF" );
        // ── KIWI-UX (ROUND AT, ITEM 2b): SAY WHERE THE WORKING PLANE IS ──────
        // USER REPORT: "it doesn't draw them on the major plane […] IT draws them
        // slightly above it??"  The working plane's offset was invisible state —
        // announced once in the console, by a line that scrolls away, and never
        // shown again.  Item 2b makes it a grid-quantised working height; this
        // makes it a thing you can SEE, which is what turns "slightly above" from
        // a mystery into a number.
        //
        // Shown only in the NON-DEFAULT state, exactly as the grid chip above is:
        // on the world's own major plane (offset 0) there is nothing to warn
        // about, and the strip does not grow for everyone.  A TILTED plane always
        // says so — that is the other half of the same report.
        AddWorkingPlaneChip( chips, &n );
        // The MOVABLE PIVOT is command-local (it has no table row — kiwi_keymap.h),
        // so it is a literal, offered on exactly the two commands that implement it.
        if ( cmd && cmd->Name()
          && ( strcmp( cmd->Name(), "Move" ) == 0 || strcmp( cmd->Name(), "Rotate" ) == 0 ) )
            AddChip( chips, &n, "V", "Pivot" );

        // SHAKEOUT I: the command's own invented keys (kiwi_command.h HudPrompts).
        // LAST, because the framework's grammar is the part a user needs to find
        // first and a chip strip reads left to right.
        if ( cmd )
        {
            const kiwiPrompt_t *extra = 0;
            const int extraCount = cmd->HudPrompts( &extra );
            for ( int i = 0; i < extraCount && extra; ++i )
                AddChip( chips, &n, extra[i].key ? extra[i].key : "?",
                                    extra[i].label ? extra[i].label : "" );
        }
        return n;
    }

    // ── BOTTOM-LEFT, idle ───────────────────────────────────────────────────
    // With NOTHING selected this is the one moment the answer to "how do I make
    // something" is guaranteed useful (shakeout C's finding, and the reason the
    // old panel grew a "Create" list at all).  The creators carry REAL chords
    // since shakeout F, so these are verb chips and print whatever the live
    // profile gives them.
    int BuildCreatePrompts( chip_t *chips )
    {
        int n = 0;
        AddPatchVertsChip( chips, &n );              // ROUND AI, ITEM 6 — leads the strip
        AddChip( chips, &n, "LMB",   "Pick" );
        AddChip( chips, &n, "Shift", "Add" );
        AddChip( chips, &n, "Ctrl",  "Remove" );
        AddVerb( chips, &n, KIWI_CMD_DRAW_LINE,     "Line" );
        AddVerb( chips, &n, KIWI_CMD_DRAW_RECT,     "Rect" );
        AddVerb( chips, &n, KIWI_CMD_DRAW_CIRCLE,   "Circle" );
        AddVerb( chips, &n, KIWI_CMD_PRIM_BOX,      "Box" );
        AddVerb( chips, &n, KIWI_CMD_PRIM_CYLINDER, "Cylinder" );
        AddVerb( chips, &n, KIWI_CMD_ADD_MENU,      "Add menu" );
        // ROUND J: with nothing selected, Focus frames the whole visible world
        // (kiwi_focus.h) — which is the answer to "where is everything", the other
        // question an empty selection asks.
        AddVerb( chips, &n, KIWI_CMD_FOCUS_SELECTION, "Frame all" );
        // ROUND P: the one navigation chord with no table row and no widget of its
        // own — Alt+MMB steps the view cube's six face views (kiwi_viewcube.h).
        // A LITERAL chip for the same reason the prompt chips are: it is arbitrated
        // in kiwi_viewport.cpp's MMB arm, not in g_radiantCommands.
        // ROUND S: it is a SWIPE now — a flick left/right/up/down steps 90 degrees
        // that way, a short press still steps the ring (kiwi_viewcube.h).
        AddChip( chips, &n, "Alt+MMB",              "Swipe views" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE,       "Commands" );
        return n;
    }

    // With a selection but no command: the keys that act on it regardless of kind.
    // Deliberately SHORT — the kind-specific verbs are the other strip's job, and
    // repeating them here is what made the old single panel feel like noise.
    int BuildSelectionPrompts( chip_t *chips, bool construction )
    {
        int n = 0;
        AddPatchVertsChip( chips, &n );              // ROUND AI, ITEM 6 — leads the strip
        // ── KIWI-UX (ROUND AO, ITEM Y): the terrain-paint chord ─────────────
        // A LITERAL chip, for the same reason "Alt+MMB / Swipe views" is one: the
        // chord is arbitrated in kiwi_viewport.cpp's LMB arm, not in
        // g_radiantCommands, so a table lookup would print no key at all.  Shown
        // ONLY while the tool is armed (the Y panel has a mode picked and
        // outer > inner), which is exactly when the cyan brush ring is on screen —
        // so the strip explains the ring the user is already looking at, and says
        // nothing the other 99% of the time.  FIRST after the patch-verts chip
        // because while it is armed it is what the left button now does.
        if ( sub_401D50() )
            AddChip( chips, &n, "Alt+LMB", "Paint terrain" );
        AddChip( chips, &n, "LMB",   "Pick" );
        AddChip( chips, &n, "Shift", "Add" );
        AddChip( chips, &n, "Ctrl",  "Remove" );
        AddChip( chips, &n, "Del",   "Delete" );
        AddChip( chips, &n, "Esc",   "Deselect" );
        if ( !construction )
        {
            AddChip( chips, &n, "1-5",      "Mode" );
            AddChip( chips, &n, "Ctrl+1-4", "Convert" );
        }
        // ROUND J: Focus is kind-agnostic and is the single most useful key with
        // something selected, so it belongs on the PROMPT strip rather than in one
        // kind's verb list.
        AddVerb( chips, &n, KIWI_CMD_FOCUS_SELECTION, "Focus" );
        return n;
    }

    // ── BOTTOM-RIGHT: the VERB menu for what is selected ────────────────────
    // Shakeout F: construction geometry lives outside the typed selection
    // (kiwi_conselect.h), so KiwiXform_DominantKind cannot see it and the strip
    // would otherwise advertise everything EXCEPT the things that selection is for.
    // Delete has NO command row of its own (the key is arbitrated in the funnel),
    // so it is a LITERAL chip rather than a table lookup.
    int BuildConstructionVerbs( chip_t *chips )
    {
        int n = 0;
        AddVerb( chips, &n, KIWI_CMD_CONSTRUCT_JOIN,   "Join" );
        AddVerb( chips, &n, KIWI_CMD_TRIM,             "Trim" );
        // ROUND J: the two curve editors are construction-selection verbs and this
        // is the only strip that ever describes one.
        AddVerb( chips, &n, KIWI_CMD_OFFSET_CURVE,     "Offset" );
        AddVerb( chips, &n, KIWI_CMD_FILLET_CURVE,     "Fillet" );
        AddVerb( chips, &n, KIWI_CMD_MOVE,             "Move" );
        AddVerb( chips, &n, KIWI_CMD_EXTRUDE_REGION,   "Extrude region" );
        AddChip( chips, &n, "Del",                     "Delete" );
        // ROUND U: H hides the selected scaffolding.  A LITERAL chip for the same
        // reason Delete is one — the key is arbitrated in the funnel
        // (KiwiConSel_OwnsHide) and KIWI_CMD_CONSTRUCT_HIDE carries no binding, so
        // a table lookup would print no key at all.
        AddChip( chips, &n, "H",                       "Hide" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE,          "More" );
        return n;
    }

    // ── ROUND K: the strip for a SELECTED REGION ────────────────────────────
    // A region is a KIWI-owned selection that never enters selection_t
    // (kiwi_region.h), so KiwiXform_DominantKind cannot see it and the strips would
    // otherwise describe an EMPTY selection while a light-blue face sits lit up in
    // the viewport — the same gap shakeout F had to close for construction
    // geometry.
    int BuildRegionPrompts( chip_t *chips )
    {
        int n = 0;
        AddChip( chips, &n, "LMB",   "Pick" );
        AddChip( chips, &n, "Shift", "Add" );
        AddChip( chips, &n, "Ctrl",  "Remove" );
        AddChip( chips, &n, "Drag", "the ball" );
        AddChip( chips, &n, "Esc",  "Deselect" );
        AddVerb( chips, &n, KIWI_CMD_FOCUS_SELECTION, "Focus" );
        return n;
    }

    // ── the strip for a SELECTED SUN HELPER ─────────────────────────────────
    // A fourth KIWI-owned selection that KiwiXform_DominantKind cannot see
    // (kiwi_sun.h), so without this the strips would describe an EMPTY selection
    // while a lit glyph and an amber frustum sit on screen — the same gap
    // shakeout F closed for construction geometry and round K for regions.
    //
    // The pitch/yaw chip is a READOUT, in the shape AddWorkingPlaneChip uses: it
    // is the number the drag is producing, and it is the only place it is shown.
    int BuildSunPrompts( chip_t *chips )
    {
        int n = 0;
        float pitch = 0.0f, yaw = 0.0f;
        if ( KiwiSun_Angles( &pitch, &yaw ) )
        {
            char label[40];
            _snprintf( label, sizeof( label ), "%.1f / %.1f deg", pitch, yaw );
            label[sizeof( label ) - 1] = '\0';
            AddChip( chips, &n, "Sun", label );
        }
        // LITERAL chips: the drag and its snap modifier are arbitrated in
        // kiwi_viewport.cpp's LMB arm and in KiwiCmd_SnapEngaged, not in
        // g_radiantCommands, so a table lookup would print no key at all.
        AddChip( chips, &n, "Shift+LMB", "Add" );
        AddChip( chips, &n, "Ctrl+LMB",  "Remove" );
        AddChip( chips, &n, "Drag",      "Orbit the sun" );
        AddChip( chips, &n, "Ctrl+drag", "Snap 5 deg" );
        AddChip( chips, &n, "Esc",  "Deselect" );
        return n;
    }

    int BuildSunVerbs( chip_t *chips )
    {
        int n = 0;
        // The one thing that has to be said and has no key: the change does not
        // reach the baked lighting until the map is compiled again (kiwi_sun.h
        // "WHAT A CHANGE COSTS THE USER").
        AddChip( chips, &n, "!", "Needs BSP + light recompile" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE, "More" );
        return n;
    }

    int BuildRegionVerbs( chip_t *chips )
    {
        int n = 0;
        AddVerb( chips, &n, KIWI_CMD_EXTRUDE_REGION, "Extrude" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE,        "More" );
        return n;
    }

    int BuildSelectionVerbs( chip_t *chips, sel_kind_t kind )
    {
        int n = 0;
        switch ( kind )
        {
        case SEL_OBJECT:
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Move" );
            AddVerb( chips, &n, KIWI_CMD_ROTATE,        "Rotate" );
            AddVerb( chips, &n, KIWI_CMD_SCALE,         "Scale" );
            // ROUND J: Duplicate is the object verb a mapper presses most often
            // after the three transforms, so it goes fourth.  Isolate sits LATER,
            // next to Delete — DrawStrip fills greedily and drops whatever does not
            // fit in KHINT_MAX_ROWS rows, i.e. the TAIL, and this list is now 10 of
            // KHINT_MAX_CHIPS.  Ordering by how often a verb is wanted is what makes
            // that degradation harmless.
            AddVerb( chips, &n, KIWI_CMD_DUPLICATE,     "Duplicate" );
            // Cut is an OBJECT verb (it needs a selected solid).  ROUND L dropped
            // "(w/ line)" with the precondition it described: the line is CLICKED
            // inside the gesture now, so the chip no longer has to warn about a
            // second selection the user has to arrange first.
            AddVerb( chips, &n, KIWI_CMD_CUT,           "Cut" );
            // ROUND L: the Q boolean.  Placed straight after Cut — they are the two
            // "one solid against another thing" verbs and a mapper reaches for them
            // in the same breath.
            AddVerb( chips, &n, KIWI_CMD_BOOLEAN,       "Boolean" );
            AddVerb( chips, &n, KIWI_CMD_ARRAY_LINEAR,  "Array" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_FACE,  "To faces" );
            AddVerb( chips, &n, 32934,                  "Isolate" );  // classic HideUnSelected
            AddVerb( chips, &n, 33003,                  "Delete" );   // classic Delete Selection
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        case SEL_FACE:
            // Shakeout G: the face row is the one the modelling verbs landed on.
            // "Push/Pull (auto)" says out loud that clicking a face in mode 3
            // ALREADY started this gesture (kiwi_boxselect.cpp) — otherwise it
            // reads as "press G to do the thing that is already happening".
            AddVerb( chips, &n, KIWI_CMD_EXTRUDE_FACE,  "Extrude" );
            AddVerb( chips, &n, KIWI_CMD_MATCH_FACE,    "Match" );
            AddVerb( chips, &n, KIWI_CMD_JOIN,          "Join" );
            // ROUND S: "Split brush" — it always split the BRUSH along the face's
            // line (a convex plane-brush cannot carry a divided face), and the
            // label said otherwise.  kiwi_split.h has the whole argument.
            AddVerb( chips, &n, KIWI_CMD_SPLIT_FACE,    "Split brush" );
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Push/Pull (auto)" );
            AddVerb( chips, &n, KIWI_CMD_INSET_FACE,    "Inset" );
            // ROUND T: the DEL restore.  Advertised HERE because the rule is
            // "Delete with exactly one face selected" and a key that quietly
            // means something different in one mode has to say so where that mode
            // is (kiwi_bevel.h REMOVE FACE).  AddVerb prints the real binding, and
            // the row is unbound, so the chip reads by NAME — which is correct:
            // the key is Delete, arbitrated in the funnel, not on this id.
            AddVerb( chips, &n, KIWI_CMD_REMOVE_FACE,   "Remove face (Del)" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_EDGE,  "To edges" );
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        case SEL_EDGE:
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Move edge" );
            // ROUND Q / ROUND T: ONE chip for ONE tool.  The chip is keyed on
            // FILLET_CURVE and labelled for the EDGE command, deliberately: chips
            // are pure display (key + label, AddChip), the key the user presses
            // with edges selected IS bare B, and B is the row that carries the
            // binding — the edge command is unbound because
            // KiwiPatchFillet_ContextB redirects into it (kiwi_patchfillet.h THE B
            // KEY).  Keying it on the edge id instead would print "(F)" — the
            // palette fallback — which is simply false.
            //
            // ROUND T dropped the separate "Bevel" chip next to it: the two verbs
            // merged into one command whose DEFAULT is the chamfer, so a second
            // chip would offer the same gesture twice under two names.  The label
            // names both halves and the D key that swaps them.
            AddVerb( chips, &n, KIWI_CMD_FILLET_CURVE,  "Bevel / Fillet (D)" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_FACE,  "To faces" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_OBJECT,"To object" );
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        default:
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Move point" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_EDGE,  "To edges" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_OBJECT,"To object" );
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        }
        return n;
    }

    // ── the chip renderer ───────────────────────────────────────────────────
    // Chips are laid out on ONE row where they fit and wrap UPWARD into a second
    // when they do not; anything that still does not fit is dropped rather than
    // allowed to run under the opposite strip.  `alignLeft` decides which edge the
    // row hugs and therefore which end gets dropped — the LAST chips on the left
    // strip, the FIRST on the right one, which in both cases is the "More…" tail.
    const float KHINT_PAD_X   = 5.0f;    // inside a keycap
    const float KHINT_GAP     = 4.0f;    // keycap -> label
    const float KHINT_CHIPGAP = 10.0f;   // chip -> chip
    const float KHINT_ROWGAP  = 3.0f;
    const float KHINT_EDGE    = 10.0f;   // inset from the image edge

    // KIWI-UX (CLEANUP, C-34): ONE CalcTextSize pair per chip per frame.  Called
    // once on each strip after it is built; DrawStrip (measure pass AND layout
    // pass) and DrawChip then read the cached numbers.
    void MeasureChips( chip_t *chips, int n )
    {
        for ( int i = 0; i < n; ++i )
        {
            chips[i].kw = ImGui::CalcTextSize( chips[i].key ).x + KHINT_PAD_X * 2.0f;
            chips[i].w  = chips[i].kw
                        + ( ( chips[i].label[0] != '\0' )
                            ? ( KHINT_GAP + ImGui::CalcTextSize( chips[i].label ).x )
                            : 0.0f );
        }
    }

    float ChipWidth( const chip_t &c ) { return c.w; }

    // ROUND Z, ITEM 5: one definition of a chip row's height.  DrawStrip lays rows
    // out with it and KiwiHints_Draw reserves the band slot with it, and a strip
    // reserved at one height and drawn at another would re-create the overlap this
    // round removed.
    float StripLineH() { return ImGui::GetTextLineHeight() + 3.0f; }

    void DrawChip( ImDrawList *dl, const chip_t &c, float x, float y, float lineH )
    {
        const float kw = c.kw;          // KIWI-UX (CLEANUP, C-34): measured once
        dl->AddRectFilled( ImVec2( x, y ), ImVec2( x + kw, y + lineH ),
                           IM_COL32( 44, 48, 58, 225 ), 3.0f );
        dl->AddRect      ( ImVec2( x, y ), ImVec2( x + kw, y + lineH ),
                           IM_COL32( 96, 102, 118, 170 ), 3.0f, 0, 1.0f );
        dl->AddText( ImVec2( x + KHINT_PAD_X, y ), IM_COL32( 236, 240, 248, 245 ), c.key );
        if ( c.label[0] != '\0' )
            dl->AddText( ImVec2( x + kw + KHINT_GAP, y ),
                         IM_COL32( 186, 194, 208, 230 ), c.label );
    }

    // Lay `n` chips into at most KHINT_MAX_ROWS rows of `maxW`, hugging the given
    // corner.  `bottomY` is the BASELINE of the lowest row's box.
    //
    // ROUND Z, ITEM 5: returns the ROW COUNT, and `measure` runs the identical
    // greedy fill without emitting anything.  The strip's height is not known until
    // the fill has run, and the shared bottom band has to be told that height
    // BEFORE the rows can be placed — so it is asked twice.  Deterministic input,
    // deterministic layout: the measuring pass and the drawing pass cannot disagree
    // because they are the same code with one branch.
    int DrawStrip( const chip_t *chips, int n, bool alignLeft,
                   float leftX, float rightX, float bottomY, float maxW,
                   bool measure )
    {
        if ( n <= 0 )
            return 0;

        ImDrawList *dl    = ImGui::GetWindowDrawList();
        const float lineH = StripLineH();

        // Greedy fill, then draw bottom row first so the strip grows upward.
        int   rowStart[KHINT_MAX_ROWS + 1];
        float rowW     [KHINT_MAX_ROWS];
        int   rows = 0;
        int   i    = 0;
        rowStart[0] = 0;
        while ( i < n && rows < KHINT_MAX_ROWS )
        {
            float w = 0.0f;
            const int start = i;
            while ( i < n )
            {
                const float cw = ChipWidth( chips[i] ) + ( i > start ? KHINT_CHIPGAP : 0.0f );
                if ( w + cw > maxW && i > start )
                    break;
                w += cw;
                ++i;
            }
            if ( i == start )                 // one chip wider than the whole strip
                break;
            rowW[rows]        = w;
            rowStart[rows + 1] = i;
            ++rows;
        }
        if ( rows <= 0 || measure )
            return rows;

        // Row `r` (0 = the first-filled row) sits ABOVE the later ones, so the
        // reading order is top-to-bottom and the last row hugs the bottom edge.
        for ( int r = 0; r < rows; ++r )
        {
            const float y = bottomY - (float)( rows - r ) * ( lineH + KHINT_ROWGAP ) + KHINT_ROWGAP;
            float x = alignLeft ? leftX : ( rightX - rowW[r] );
            for ( int k = rowStart[r]; k < rowStart[r + 1]; ++k )
            {
                DrawChip( dl, chips[k], x, y, lineH );
                x += ChipWidth( chips[k] ) + KHINT_CHIPGAP;
            }
        }
        return rows;
    }

    // ── ROUND Z, ITEM 5: the shared bottom band (kiwi_hints.h) ──────────────
    // FRAME-LOCAL: opened and drained inside ONE KiwiVP_DrawCameraOverlay call.
    //
    // The "is it open" test is an ImGui FRAME STAMP rather than a bool, because
    // there is no end-of-frame hook to clear a bool with — a latched flag would
    // stay true forever and hand a stale band to any caller reached by another
    // route on a later frame.  Comparing the frame the band was opened on against
    // the frame it is being taken on is self-clearing and needs nothing to
    // maintain.  -1 is "never opened".
    int   s_bandFrame  = -1;
    float s_bandBottom = 0.0f;    // the bottom edge the NEXT box gets
    float s_bandFloor  = 0.0f;    // never climb above the image
    const float KHUD_BAND_EDGE = 12.0f;   // the inset kiwi_numeric/kiwi_uv both used
    const float KHUD_BAND_GAP  = 6.0f;    // between stacked rows
}

void KiwiHud_BandBegin( float imgMinY, float imgH )
{
    s_bandFrame  = ImGui::GetFrameCount();
    s_bandBottom = imgMinY + imgH - KHUD_BAND_EDGE;
    s_bandFloor  = imgMinY;
}

float KiwiHud_BandTake( float boxH, float fallbackTop )
{
    if ( s_bandFrame != ImGui::GetFrameCount() )
        return fallbackTop;
    float top = s_bandBottom - boxH;
    // Out of room: pile at the top rather than off the image.  The band is then
    // exhausted and every later taker gets the same y — visibly wrong, which is the
    // honest failure for a viewport too short to hold its own status rows.
    if ( top < s_bandFloor )
        top = s_bandFloor;
    s_bandBottom = top - KHUD_BAND_GAP;
    return top;
}

// ─── toggle ──────────────────────────────────────────────────────────────────
bool KiwiHints_Show()
{
    if ( s_show < 0 )
        s_show = Radiant_ProfileGetInt( "KiwiUX", "ShowHints", 1 ) ? 1 : 0;
    return s_show != 0;
}

void KiwiHints_SetShow( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_show == v )
        return;
    s_show = v;
    Radiant_ProfileSetInt( "KiwiUX", "ShowHints", v );
}

// ─── draw ────────────────────────────────────────────────────────────────────
void KiwiHints_Draw( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( !KiwiHints_Show() )
        return;

    // ── KIWI-UX (CLEANUP, C-34): THE GEOMETRY GUARD RUNS FIRST ──────────────
    // It used to sit below the build, so up to 24 chips were assembled — each
    // AddVerb a linear scan of the ~200-row command table plus an _snprintf, each
    // chip three CalcTextSize calls — and then thrown away, every frame, whenever
    // the viewport was small.
    //
    // VERIFIED SAFE TO HOIST: nothing between the top of this function and the
    // guard has a side effect anything after it depends on.  KiwiHints_Show()
    // (which lazily loads the profile pref — the one latch here) stays above it and
    // is unchanged; the Build* helpers only READ selection/command state and write
    // into the two local arrays; and the one genuinely side-effecting call in this
    // function, KiwiHud_BandTake, was already below the guard and still is.
    //
    // Each strip gets a little under half the width, so the two can never collide
    // however wide a chip's label is.
    const float maxW   = imgW * 0.46f;
    const float leftX  = imgMinX + KHINT_EDGE;
    const float rightX = imgMinX + imgW - KHINT_EDGE;
    if ( maxW < 80.0f || imgH < 120.0f )
        return;                                  // no room — draw nothing, never overlap

    chip_t prompts[KHINT_MAX_CHIPS];
    chip_t verbs  [KHINT_MAX_CHIPS];
    int    nP = 0;
    int    nV = 0;

    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( cmd )
    {
        // A live gesture owns the keyboard, and a verb list about the selection is
        // noise in the middle of an edit to it — so the right strip stays EMPTY,
        // which is also where Plasticity puts its command dialog and nothing else.
        nP = BuildCommandPrompts( prompts, cmd );
    }
    else
    {
        sel_kind_t kind;
        const bool haveBrushSel = KiwiXform_DominantKind( &kind );
        const bool haveConSel   = !KiwiConSel_Empty();
        // ROUND K: a selected REGION is a third selection this strip can describe.
        // Ranked BELOW the brush one for the same reason the construction one is
        // (a brush selection is what every classic command would act on) and ABOVE
        // the construction one, because selecting a region deliberately clears the
        // construction selection (kiwi_boxselect.cpp), so the two can only both be
        // non-empty when the construction one is stale.
        const bool haveRegion   = KiwiRegion_HasSelection();
        // The SUN HELPER is tested FIRST, ahead even of the brush selection, and
        // the reason is a property of its click grammar rather than a preference:
        // any plain click that is NOT on the glyph clears it (kiwi_boxselect.cpp's
        // sun arm), so a live sun selection means the glyph is the most recent
        // thing the user deliberately took hold of.  Nothing else in this ladder
        // can say that about itself.
        if ( KiwiSun_Selected() )
        {
            nP = BuildSunPrompts( prompts );
            nV = BuildSunVerbs  ( verbs );
        }
        else if ( haveBrushSel )
        {
            // First of the three MAP selections, on purpose: with more than one of
            // them non-empty the strips describe the BRUSH one, because that is
            // what G, Delete and every classic command would act on
            // (kiwi_conselect.h gates its verbs on a PURE construction selection).
            // The sun arm above is not one of the three — it is worldspawn state
            // and no classic command can see it at all.
            nP = BuildSelectionPrompts( prompts, false );
            nV = BuildSelectionVerbs  ( verbs, kind );
        }
        else if ( haveRegion )
        {
            nP = BuildRegionPrompts( prompts );
            nV = BuildRegionVerbs  ( verbs );
        }
        else if ( haveConSel )
        {
            nP = BuildSelectionPrompts( prompts, true );
            nV = BuildConstructionVerbs( verbs );
        }
        else
        {
            nP = BuildCreatePrompts( prompts );
        }
    }

    if ( nP <= 0 && nV <= 0 )
        return;

    MeasureChips( prompts, nP );        // KIWI-UX (CLEANUP, C-34) — once per chip
    MeasureChips( verbs,   nV );

    // ── KIWI-UX (ROUND Z, ITEM 5): ONE SLOT FOR BOTH STRIPS ─────────────────
    // The prompts and the verbs sit at OPPOSITE ends of the same row band and can
    // never overlap each other (the 0.46 split is what guarantees that), so they
    // share ONE band slot sized to the TALLER of the two.  Taking a slot each would
    // stack the right-hand strip above the left-hand one for no reason.
    const float lineH = StripLineH();
    const int   rowsP = DrawStrip( prompts, nP,  KHINT_PROMPTS_LEFT,
                                   leftX, rightX, 0.0f, maxW, true );
    const int   rowsV = DrawStrip( verbs,   nV, !KHINT_PROMPTS_LEFT,
                                   leftX, rightX, 0.0f, maxW, true );
    const int   rows  = ( rowsP > rowsV ) ? rowsP : rowsV;
    if ( rows <= 0 )
        return;

    const float stripH  = (float)rows * ( lineH + KHINT_ROWGAP ) - KHINT_ROWGAP;
    const float bottomY = KiwiHud_BandTake( stripH, imgMinY + imgH - KHINT_EDGE )
                        + stripH;

    DrawStrip( prompts, nP,  KHINT_PROMPTS_LEFT, leftX, rightX, bottomY, maxW, false );
    DrawStrip( verbs,   nV, !KHINT_PROMPTS_LEFT, leftX, rightX, bottomY, maxW, false );
}
