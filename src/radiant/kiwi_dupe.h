#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Duplication operations: the Mirror tool plus KIWI linear/radial arrays.
//
// Mirror reuses the classic flip core (Select_FlipAxisAbout, split from Select_FlipAxis,
// select.cpp 0x48FD50): bSwap=1 preserves outward windings.  The classic instant
// Mirror X/Y/Z commands (32956-32958, DoFlip) remain in mainfrm's dispatch but have no
// KIWI UI (user 2026-10-12: "the 3 flip variants are unneeded").
//
// Clone_Selection (select.cpp 0x48F0D0) clones through the map text (Copy ->
// Map_ImportBuffer, KIWI 2026-09-09): brushes, patches and entities all duplicate,
// originals are deselected and the copies selected.  gridSize is unused in this port;
// the import owns its own "import buffer" undo bracket unless g_kiwiImportInCallerUndo is
// set (arrays set it, so the whole array is one Ctrl+Z).
//
// Arrays (KIWI 2026-09-27 redo, Plasticity's Rectangular/RadialArrayCommand flow): select
// anything - it arrays AS ITSELF and the selection is never converted (objects clone;
// construction, regions, brush edges and faces copy as construction, kiwi_conselect.h) -
// start the tool, PICK a line (construction segment or brush edge; Radial: a snapped
// centre point), then OPTIONS in the command panel - count, spacing Fit to distance (the
// distance first to last / count; the picked line's length at first), Fixed, or Natural
// (KIWI 2026-10-05: end to end - the smallest step along the line after which no object of
// a copy overlaps one of the copy before, so they touch - plus a Gap, 0 at first), reverse
// direction (Radial: count, angle, axis Z / X / Y / construction-plane normal) - typed
// values (count, Tab, the mode's distance / spacing / gap, or the angle), [ ] count, a
// lollipop on the last copy of a linear array (Natural: it sets how many fit; else the
// distance), P to pick again; Enter / RMB / Confirm writes.  The mode sticks between
// arrays.  A full 360 spaces the radial copies around the circle; a partial angle lands the
// last copy on it.
//
// Objects: one mutation-style undo record (the clones' imports join it); each iteration
// clones the last copy and moves / turns it one step, and every original and copy is
// selected again before commit so Undo_EndBrushList records a consistent after-state; faces
// / edges / points picked beside the objects are picked again.  Construction copies go
// through KiwiConSel_ArrayAdd (its own construction undo snapshot, made first; the copies
// join the construction selection) - a mixed selection is therefore two Ctrl+Z steps.
//
// Preview and snap marker share a bounded KiwiLines batch: per-object outlines (brush face
// edges, a model's oriented box, else the bounding box) and construction segments per copy,
// one box per copy when that does not fit.
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
#define KARR_MAX_SOURCE     512   // preview cap (construction segments)

void KiwiDupe_RegisterCommands();
KiwiEditorCommand *KiwiDupe_CommandForId( int commandId );

bool KiwiDupe_CanMirror();   // objects selected (the Mirror tool's object arm)
// KIWI (2026-10-12): the Mirror tool (KIWI_CMD_MIRROR): engage, X / Y / Z previews the flip
// across that axis through the selection's centre (the same key again takes it back), RMB /
// Enter confirms, Esc restores.  Brushes and patches: the classic flip core; xmodels: a
// mirrored twin asset ("<name>_mir", written to raw/xmodel* once, reused after; mirroring a
// twin goes back to its source) on the mirrored origin; prefabs and models a twin cannot be
// made for (a surface off the root bone): turned to the mirror image of their frame and kept
// on their bounds centre; construction: the store.
bool KiwiDupe_CanMirrorTool();
bool KiwiDupe_CanArray();    // anything with a shape: objects, construction, regions, edges, faces
bool KiwiDupe_CanDuplicate();// …the same precondition; a separate name so the two
                             // palette rows can diverge later without a rename

// Returns false when cmdId is not the instant Duplicate command.
bool KiwiDupe_DispatchInstant( unsigned int cmdId );

// The "Duplicate" block in the shell panel (mirror trio + the two array commands).
void KiwiDupe_MenuItems();
