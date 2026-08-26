#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Plasticity-style outliner: live scene state is flattened into equal-height rows
// each frame, while collapse state remains a panel-local set of stable ids
// (FlattenOutline.ts:13; Outliner.tsx:56, :113).
// Rows query live selection, double-click renameable items, and use class/id
// fallbacks for unnamed items (OutlinerItems.tsx:77; Outliner.tsx:115, :158).
// The eye maps to Radiant's existing hide state. Plasticity's separate lock and
// disable concepts are omitted because Radiant picking, filters, and maps do not
// share them.

// Brush groups are func_group entities so stock .map persistence, compiler
// semantics, and ported creation/reparent/free undo paths remain authoritative.
// Reparent undo must snapshot each brush before changing its owner; ungroup must
// snapshot the entity before relinking its brushes and freeing it.

// Construction groups are editor-only sidecar records because construction
// geometry never enters the .map; absent or unknown fields remain load-compatible.
// Selection is never cached: rows query live brush/construction selection and
// clicks use the same sync funnels as the viewport.
// Brush pointers retained by anchors or drag payloads are checked with
// Sel_BrushLive before dereference. ImGuiListClipper limits drawing to visible rows.

// Called once per ImGui frame; early-outs while its window flag is clear.
void KiwiOutliner_Draw();

// Palette-visible commands dispatch brush and construction halves independently.
bool KiwiOutliner_CanGroup();
bool KiwiOutliner_GroupSelection();     // "New Group from Selection"
bool KiwiOutliner_CanUngroup();
bool KiwiOutliner_UngroupSelection();   // dissolve the selection's groups

// Discard document-local collapse keys and transient row identities.
void KiwiOutliner_ResetForNewMap();

// Registration and instant-command dispatch (kiwi_command.cpp).
void KiwiOutliner_RegisterCommands();
bool KiwiOutliner_DispatchInstant( unsigned int cmdId );
