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
// Arrays (KIWI 2026-09-27 redo, Plasticity's Rectangular/RadialArrayCommand flow): select
// anything - it arrays AS ITSELF and the selection is never converted (objects clone;
// construction, regions, brush edges and faces copy as construction, kiwi_conselect.h) -
// start the tool, PICK a line (construction segment or brush edge; Radial: a snapped
// centre point), then OPTIONS in the command panel - count, distance or fixed spacing,
// flip (Radial: count, angle, axis Z / X / Y / construction-plane normal) - typed values
// (count, Tab, length / angle), [ ] count, a lollipop on the last copy of a linear array,
// P to pick again; Enter / RMB / Confirm writes.  Distance = the original to the last copy
// (the picked line's length at first).  A full 360 spaces the radial copies around the
// circle; a partial angle lands the last copy on it.
//
// Objects: one mutation-style undo record; each iteration clones the last copy and moves /
// turns it one step, and every original and copy is selected again before commit so
// Undo_EndBrushList records a consistent after-state; faces / edges / points picked beside
// the objects are picked again.  Construction copies go through KiwiConSel_ArrayAdd (its own
// construction undo snapshot, made first; the copies join the construction selection).
//
// Preview and snap marker share a bounded KiwiLines batch: per-object boxes and
// construction segments per copy, one box per copy when that does not fit.
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
#define KARR_MAX_SOURCE     512   // preview cap (boxes, construction segments)

void KiwiDupe_RegisterCommands();
KiwiEditorCommand *KiwiDupe_CommandForId( int commandId );

bool KiwiDupe_CanMirror();   // attaches to the CLASSIC ids 32956 / 32957 / 32958
bool KiwiDupe_CanArray();    // anything with a shape: objects, construction, regions, edges, faces
bool KiwiDupe_CanDuplicate();// …the same precondition; a separate name so the two
                             // palette rows can diverge later without a rename

// Returns false when cmdId is not the instant Duplicate command.
bool KiwiDupe_DispatchInstant( unsigned int cmdId );

// The "Duplicate" block in the shell panel (mirror trio + the two array commands).
void KiwiDupe_MenuItems();
