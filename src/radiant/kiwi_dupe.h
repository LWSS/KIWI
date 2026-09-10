#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Duplication operations: classic mirror commands plus KIWI linear/radial arrays.
//
// Mirror reuses Select_FlipAxis (select.cpp 0x48FD50) through DoFlip
// (select.cpp 0x424F30). bSwap=1 preserves outward windings; DoFlip also
// owns undo and fixed-size entity angle updates.
//
// Clone_Selection (select.cpp 0x48F0D0) clones through the map text (Copy ->
// Map_ImportBuffer, KIWI 2026-09-09): brushes, patches and entities all duplicate,
// originals are deselected and the copies selected.  gridSize is unused in this port;
// the import owns its own "import buffer" undo bracket (nested inside ours).
//
// Arrays use one mutation-style undo record. Each iteration clones the current
// selection, then transforms it; before commit, every original saved at begin and
// every copy must be selected so Undo_EndBrushList records a consistent after-state.
//
// Linear drag is a world-space offset on a camera-facing plane latched at Begin.
// Typed input is count; Units_ToDisplay reverses the numeric layer's length conversion.
// A picked line supplies an exact span, so count derives spacing without grid rounding.
//
// Radial arrays rotate around Z through the latched session pivot, falling back to
// selection mid. Latching once avoids drift from recomputed rotated bbox midpoints.
//
// Preview and snap marker share a bounded KiwiLines batch. Previews query remaining
// segments and degrade boxes to 3-axis crosses rather than spill.
//
// Shift+D clones, refreshes the classic special-material hints, then enters paused
// Move via KiwiCmd_AfterPaste. Its creation-style undo bracket omits Undo_AddBrushList
// and stamps only copies; Move opens a separate undo record.

class KiwiEditorCommand;

#define KARR_MIN_COUNT      2
#define KARR_MAX_COUNT      64
#define KARR_DEF_LINEAR     3     // default linear count
#define KARR_DEF_RADIAL     6
#define KARR_MIN_OFFSET     0.5f  // world units — below this a linear array is a no-op
#define KARR_MAX_SOURCE     512   // preview/original-tracking cap

// MouseMove has no grab gate, so a picked line needs a pixel deadzone or the next
// cursor twitch would release it. The generous radius favors avoiding a costly re-pick.
#define KARR_LINE_BREAK_PIXELS 48.0f

void KiwiDupe_RegisterCommands();
KiwiEditorCommand *KiwiDupe_CommandForId( int commandId );

bool KiwiDupe_CanMirror();   // attaches to the CLASSIC ids 32956 / 32957 / 32958
bool KiwiDupe_CanArray();    // at least one cloneable brush is selected
bool KiwiDupe_CanDuplicate();// …the same precondition; a separate name so the two
                             // palette rows can diverge later without a rename

// Returns false when cmdId is not the instant Duplicate command.
bool KiwiDupe_DispatchInstant( unsigned int cmdId );

// The "Duplicate" block in the shell panel (mirror trio + the two array commands).
void KiwiDupe_MenuItems();
