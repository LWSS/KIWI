#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Explicit faces use the browser apply funnel; whole solids use the ported AutoCaulk.
// This keeps material validation, patch handling, gesture handoff, and undo ownership
// in their existing entry points.

// The literal matches Brush_AutoCaulkFace (0x47E080) and Brush_AutoCaulk (0x47E0F0).
// Leaf-name matching accepts "caulk" and "wc/caulk" but not related material names.
// Texture_GetHandle registers a first reference before the browser-index lookup.

// Brush_FitTexture( 1, 1, 0 ) mirrors Surface Inspector Fit (0x4939E0): one repeat
// per face. Apply and fit self-bracket, so explicit-face caulk costs two undo records.
// Only caulk triggers the fit because fitting ordinary materials would destroy aligned UVs.

// Command 33220 supplies AutoCaulk's undo bracket. CSG_FaceVisible (0x47DE60) limits
// it to faces hidden by other selected solids; patches and fixed-size entities are skipped.
// Auto-caulked faces are not fitted because the port returns no changed-face subset.

// Mixed face/object input is split because Brush_SetTexture walks both legacy selections.
// Its three operations create three undo records, and its selection swaps currently leave
// an auto-entered face gizmo stopped rather than re-arming it against a transient subset.

// The key funnel must let this verb escape an idle face gesture; the browser funnel owns
// ending and restarting that gesture around an explicit-face material write.

// Registers unbound; the modern keymap uses End while the classic profile is unchanged.
void KiwiCaulk_RegisterCommands();

// Dispatches KIWI_CMD_CAULK_FACES.
bool KiwiCaulk_DispatchInstant( unsigned int cmdId );

// Shares the UV cores' whole-brush-or-face availability predicate.
bool KiwiCaulk_CanExecute();

// Case-insensitive leaf-name test; null and empty names are false.
bool KiwiCaulk_IsCaulkName( const char *name );

// Browser-funnel tail hook: fit only caulk over the selection just applied.
void KiwiCaulk_AutoFitApplied( const char *appliedName );
