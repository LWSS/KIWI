#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// "Paste Group In Place" (Ctrl+Shift+V) - replace each selected model by the clipboard GROUP,
// posed so the group's matching model lands exactly where the selected one was.
//
// KIWI (2026-09-18, user): "I have concrete fences with 3 barbwire arms on top of them ... I
// deleted the arms on a large chunk of them.  I want to be able to copy a wall + the 3 arms
// and then select a wall without arms and exact copy in place the wall where the current one
// is (replace it!) and get the 3 arms in their corresponding translated positions."
//
//   1. Copy (Ctrl+C) the group: the wall and its three arms.
//   2. Select any number of bare walls.
//   3. Ctrl+Shift+V: for every selected model the clipboard is pasted once; the pasted model
//      with the SAME xmodel as the target (else the first pasted model) is the ANCHOR; the
//      whole pasted group is rotated and moved by the rigid transform that carries the
//      anchor's pose onto the target's (origin + angles; the anchor then takes the target's
//      exact angle string), and the target is deleted.  Brushes / patches / other entities
//      in the clipboard ride along.
//
// One undo record for the whole thing (Map_ImportBuffer runs inside it through
// g_kiwiImportInCallerUndo).  Model scale is NOT matched: a target whose modelscale differs
// from the anchor's is replaced at the clipboard's scale and reported.
// Hooked into: palette (KIWI_CMD_PASTE_IN_PLACE), Edit menu, keymap, test DSL
// (`cmd KiwiPasteInPlace`).

void KiwiPastePlace_RegisterCommands();
bool KiwiPastePlace_CanExecute();
bool KiwiPastePlace_DispatchInstant( unsigned int cmdId );
void KiwiPastePlace_BuildMenu( void *frameMenu );
