#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Construction copy/paste shares Ctrl+C/Ctrl+V with the brush path but keeps a
// separate process-local payload so mixed copies retain both halves.
// A mixed paste lands both halves but skips auto-Move because construction Move
// requires an empty brush selection; modern single-kind output enters paused Move.
// See RADIANT_UX_DESIGN.md §70.

// Copy each selected construction object once; point/segment items name whole objects.
// Brush-only Copy clears older construction data. With no legacy brush selection,
// a zero result preserves the existing construction payload.
int KiwiConClip_Copy();

// Paste values in place with hidden=false and group=-1; KiwiCon_Add normalises/refits.
// One pre-add undo snapshot covers the batch, and selection becomes the landed set.
// Returns the landed count; an empty construction payload is a silent zero.
int KiwiConClip_Paste();

// Return and clear the last construction paste count. KiwiCmd_AfterPaste also tails
// clone/create paths, so it must not infer paste output from the live selection.
int KiwiConClip_TakeJustPasted();
