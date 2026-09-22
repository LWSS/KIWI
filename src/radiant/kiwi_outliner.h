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

// Brushes are held by func_group entities so stock .map persistence, compiler
// semantics, and ported creation/reparent/free undo paths remain authoritative.
// Reparent undo must snapshot each brush before changing its owner; dissolving a
// func_group must snapshot the entity before relinking its brushes and freeing it.

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
// UNIVERSAL GROUPS (2026-09-22; the design note is in kiwi_outliner.cpp).  A group is a
// path ("village/house_1/door") kept as the `kiwi_group` epair on each member entity, so
// it holds ANYTHING - brushes, patches, models, lights, triggers, other groups - and is
// saved in the .map.  Loose brushes join through a func_group wrapper the outliner never
// shows; every func_group is a group.
//
// GroupSelection always makes a NEW group, inside the deepest group the whole selection
// already shares (parts of a house -> a sub-group; two whole houses -> a group of both).
// AddSelectionToGroup is the join: members of ONE group plus ungrouped objects selected
// together.  SelectWholeGroup grows the selection one group level per use.
bool KiwiOutliner_GroupSelection();
bool KiwiOutliner_CanAddToGroup();
bool KiwiOutliner_AddSelectionToGroup();
bool KiwiOutliner_CanSelectGroup();
bool KiwiOutliner_SelectWholeGroup();
bool KiwiOutliner_CanUngroup();
bool KiwiOutliner_UngroupSelection();   // dissolve the selection's groups

// Discard document-local collapse keys and transient row identities.
void KiwiOutliner_ResetForNewMap();

// Test DSL (kiwi_test.cpp): `group [add|select|ungroup]`, `expect groups|grouped <n>`.
int  KiwiOutliner_TestGroupCount();         // group nodes, nested ones and implied parents included
int  KiwiOutliner_TestGroupedBrushCount();  // brush instances (model boxes too) of grouped entities

// Registration and instant-command dispatch (kiwi_command.cpp).
void KiwiOutliner_RegisterCommands();
bool KiwiOutliner_DispatchInstant( unsigned int cmdId );
