#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Hide/isolate command family.
//
// Modern: H hides selected, Shift+H isolates, Alt+H shows hidden, and Ctrl+H
// inverts hidden. Classic swaps the Shift+H and Alt+H commands.
//
// Select_Hide, Select_HideUnselected, and ShowHidden are ported cores. KIWI adds
// invert hidden without introducing a separate isolate mode.
//
// Invert walks selected then active and changes only brushFlags bit 2 and xx5:
// hidden becomes clear/depth 0; visible becomes set/depth 1. This deliberately
// flattens hide depth, matching Select_Hide's unconditional `xx5 = 1`.
//
// Undo model.
// Visibility uses its own snapshot domain; legacy undo restores geometry and
// cannot safely carry instance-side brushFlags.
//
// Visibility undo.
// KUNDO_VISIBILITY stores `{ def, hidden, depth }` for each live instance. The
// definition survives instance recreation, but duplicate instances sharing one
// definition cannot be distinguished during restore.
//
// Restore only brushFlags bit 2 and xx5; the other bits cache filter, layer, and
// selection state. Missing definitions are skipped and newly created ones retain
// their current visibility. One snapshot covers each gesture, and unchanged
// pre/post states create no undo record.

// Capture before the first write and commit after the last. `label` must have
// static lifetime because the journal retains its pointer.
void KiwiVis_UndoPush( const char *label );
// Idempotent; the next push or stack read flushes a caller's missed commit.
void KiwiVis_UndoCommit();
bool KiwiVis_UndoPop();
bool KiwiVis_RedoPop();
void KiwiVis_ClearRedo();
void KiwiVis_UndoReset();

// Apply hidden state to one instance through the shared bit/depth writer.
void KiwiVis_SetHidden( selbrush_t *b, bool hidden );

// KIWI-owned fourth member of the family (Ctrl+H).
void KiwiVis_InvertHidden();

// Hide and isolate require a selected brush.
bool KiwiVis_CanHide();
// Scan both display lists for hidden brushes.
bool KiwiVis_HasHidden();
// Invert is available whenever either display list contains a brush.
bool KiwiVis_CanInvert();

// Hidden-solid sidecar persistence.
// MapFile_WriteEntity writes definitions, not instance brushFlags, so hidden state
// lives in optional top-level KIWI2 fields in `<mapname>.kiwi`; older readers skip them.
//
// Positional identity.
// `hiddenbrush N` is positional: N indexes Map_SaveFile's entity/definition walk,
// which BuildSaveOrder must mirror. `brushes.prev`/`onext` is insertion/file order,
// and ParseEntity recreates the same order on load. Patches share this ordinal space.
//
// Deliberate divergence: fixed-size bbox brushes are counted although not written;
// ParseEntity recreates one per fixed-size entity in entity order.
//
// Limits.
// `hiddenbrushtotal` drops the set on cardinality mismatch; invalid ordinals and
// definitions without live instances are skipped. Equal-count external edits or
// reordering can still shift positional identity undetected. Fixed-size entities
// with extra definitions also lose those extras on reload. Parsing remains non-fatal,
// and Show Hidden clears any wrong hide.
//
// Sidecar API.
// Build once, then read its total and hidden ordinals.
int  KiwiVis_SidecarBuild();
int  KiwiVis_SidecarTotal();
// Return -1 when i is outside the hidden-ordinal list.
int  KiwiVis_SidecarOrdinal( int i );
// Begin before parsing every sidecar; apply only after map instances are live.
void KiwiVis_SidecarLoadBegin();
void KiwiVis_SidecarLoadTotal( int total );
void KiwiVis_SidecarLoadNote( int ordinal );
void KiwiVis_SidecarLoadApply();

// Patch-selection diagnostics.
// Report the four silent gates: "Don't select curves", hidden curve and terrain
// filters, and Face selection mode. Filter conditions have no accessor, so filter
// rows are inferred by display name and the diagnostic is advisory.
//
// `force == false` reports once per session and only when a patch exists. Forced
// mode always reports, including the all-clear and missing-curveDef fallback.
void KiwiVis_ReportPatchGates( bool force );

void KiwiVis_RegisterCommands();
bool KiwiVis_DispatchInstant( unsigned int cmdId );
