#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Caulk (End): selected solids get the ported Brush_AutoCaulk (faces hidden by other
// selected solids; CSG_FaceVisible 0x47DE60) and selected brush faces get caulk, in ONE
// undo record (KiwiCmd_UndoBegin).  Material only - caulk is never fitted.  Curves and
// fixed-size entities alone go through the browser apply funnel.

// Uncaulk (Shift+End): each caulk face of the selected solids, or each selected caulk
// face, copies the material + texdef of a flush same-plane face on a touching brush, else
// of its own brush's largest visible face.  One undo record.

// The literal matches Brush_AutoCaulkFace (0x47E080) and Brush_AutoCaulk (0x47E0F0).
// Leaf-name matching accepts "caulk" and "wc/caulk" but not related material names.
// Texture_GetHandle registers a first reference before the browser-index lookup.

// The key funnel must let these verbs escape an idle face gesture; they end and restart
// that gesture around their material writes.

// Registers unbound; the modern keymap uses End / Shift+End, the classic profile is unchanged.
void KiwiCaulk_RegisterCommands();

// Dispatches KIWI_CMD_CAULK_FACES and KIWI_CMD_UNCAULK_FACES.
bool KiwiCaulk_DispatchInstant( unsigned int cmdId );

// Shares the UV cores' whole-brush-or-face availability predicate.
bool KiwiCaulk_CanExecute();

// Case-insensitive leaf-name test; null and empty names are false.
bool KiwiCaulk_IsCaulkName( const char *name );
