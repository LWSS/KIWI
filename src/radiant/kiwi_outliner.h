#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_outliner.h — ROUND W: the Plasticity-style OUTLINER.
//
// USER DIRECTIVE, verbatim:
//   "Please create a collapsible giant list of all brushes on the left like it's
//    Plasticity.  Support groups in this list.  Show hidden ones with a closed
//    eyeball like in plasticity.  Split them by type (curve vs solid) just like
//    in plasticity).  allow selection by clicking on them in the list and allow
//    multiple by shift clicking, shift dragging.  When a group is created, it has
//    a group 'folder' in the list that you can drag to."
//
// ── WHAT PLASTICITY'S OUTLINER ACTUALLY IS (read, not guessed) ──────────────
// plasticity/src/components/outliner/ is three files and the whole design is in
// them:
//
//   FlattenOutline.ts:13   `export function flatten( group, scene, info,
//                           expandedGroups, indent = 0 )` — the tree is turned
//                           into a FLAT ARRAY once per render.  A collapsed group
//                           contributes exactly ONE row (`:54-57`); an expanded one
//                           contributes its own row then recurses (`:15-23`).
//   FlattenOutline.ts:18   inside a group the children are bucketed into
//                           `solids`, `curves`, `empties` while walking, and each
//                           non-empty bucket is emitted behind its own SECTION row
//                           — 'SolidSection' `:42-45`, 'CurveSection' `:46-49`.
//                           THAT is the "split them by type" the directive means:
//                           the split is per-group, not global, and a section with
//                           no members is not drawn at all.
//   Outliner.tsx:113       `const flattened = flatten( root, scene, ... )` then
//                           `:142` `flattened.map(...)` — one row element per entry.
//   Outliner.tsx:115-140   FindContiguousBlocksOfSelectedItems — selection is read
//                           from `selected.has(object)` PER ROW, PER RENDER.  The
//                           outliner caches no selection of its own; it re-asks.
//   OutlinerItems.tsx:87   `name={ !hidden ? 'eye' : 'eye-off' }` — the closed
//                           eyeball the directive asks for, and `:84` toggles it
//                           through a command rather than writing the model.
//   Outliner.tsx:158       `const name = scene.getName(object) ?? `${klass} ${id}``
//                           — an unnamed object is "Solid 12" / "Curve 3", i.e. its
//                           CLASS plus a number.  Brushes have no names either, so
//                           KIWI's rows read "Brush 12" for exactly this reason.
//   OutlinerItems.tsx:77   `onDblClick={ e => this.editName(e) }` -> `:146` renders
//                           the row's <input> editable -> `:166` SetNameCommand.
//                           Double-click renames; blur/Enter commits.
//   Outliner.tsx:196-205   the panel HEADER is a title plus ONE button, and that
//                           button is `command:group-selected` ("Create group (of
//                           selected items)").  KIWI's header mirrors it.
//   Outliner.tsx:56        `expandedGroups = new Set<GroupId>([root.id])` — the
//                           collapse state is a SET OF IDS held by the panel, never
//                           a flag on the model.  Same here (s_collapsed below).
//   Outliner.tsx:89-100    onSelectionDelta AUTO-EXPANDS the ancestors of anything
//                           that becomes selected, so a viewport selection is never
//                           invisible in the list.  KIWI does the same (see
//                           SELECTION SYNC below).
//
// NOT ported, deliberately: Plasticity's per-row "disable in viewport"
// (light-bulb) and "lock selection" (padlock) toggles (OutlinerItems.tsx:89-102).
// Radiant has ONE visibility concept — the hide bit the H family writes — and
// inventing two more editor-only per-brush states that nothing else in the editor
// respects (not the pick, not the filters, not the .map) would be three rules
// where the directive asked for one.  The eye is the whole of what shipped.
//
// ── GROUPS ARE func_group ENTITIES (the solid half) ─────────────────────────
// A "group of brushes" in a Radiant map is not an editor concept that needs
// inventing — it is a BRUSH ENTITY, and the classname classic Radiant uses for a
// pure organisational one is `func_group`.  Choosing it means the group:
//   * survives the .map save with no new format (map.cpp:693-706 writes every
//     entity with a non-empty def-list),
//   * is understood by the compiler and by stock Radiant (cm_load_obj.cpp:574 and
//     cod4map/map.cpp:1437 both name func_group),
//   * already has creation, reparenting, freeing and UNDO code in this tree.
// The exact sequences used are written down in kiwi_outliner.cpp; the short form:
//   CREATE    Undo_ClearRedo -> Undo_GeneralStart -> Undo_AddBrushList(&selected_
//             brushes) -> Entity_Create( Eclass_ForName(0,"func_group") ) ->
//             Undo_SetIdForEntity(newDef) -> Undo_End.
//   REPARENT  per brush: Undo_AddBrush(def) BEFORE the move, then the ported
//             triple Entity_UnlinkBrush(def) / Entity_LinkBrush(def,targetDef) /
//             Entity_LinkBrush_0_extern(targetInst,inst).
//   UNGROUP   Undo_AddEntity_W(groupDef) then the same triple back to worldspawn,
//             then Entity_Free — Select_Ungroup's own body (select.cpp:5079).
//
// ── CONSTRUCTION GROUPS ARE A SIDECAR FIELD (the curve half) ────────────────
// Construction geometry is editor-only and never reaches the .map, so its groups
// cannot be entities.  kconObject_t gains ONE int (`group`, -1 = ungrouped) and
// the sidecar gains two keywords — `group N` inside an object block and
// `congroup N "name"` at the top level.  Both are back-compatible in BOTH
// directions by construction, for the same reason ROUND U's `hidden` was:
// KiwiCon_LoadSidecar skips every keyword it does not know
// (kiwi_construct.cpp:3707) and defaults the field when the line is absent.
//
// ── SELECTION SYNC IS ONE-WAY-EACH-WAY, EVERY FRAME ────────────────────────
// The panel NEVER caches what is selected.  Every row asks the live state as it is
// drawn — `Sel_Contains( KiwiSel(), Sel_MakeObject(b) )` for brushes,
// `KiwiConSel_ObjectSelected(i)` for construction — which is Outliner.tsx:150-152
// verbatim in C++.  So a viewport selection lights the row up on the next frame
// with no notification path at all, and there is nothing to invalidate.
// Clicking a row goes the other way through the SAME funnels the viewport uses
// (Sel_* + Sel_SyncToLegacy, KiwiConSel_ApplyClick), so the outliner is not a
// second selection owner.
//
// LIVENESS.  A `selbrush_t *` is only ever held for the length of ONE frame's
// flatten, except in two places: the shift-range anchor and the drag payload.
// Both are guarded with Sel_BrushLive (kiwi_selection.h:117) before any deref,
// which is the discipline the whole UX layer runs on.
//
// ── SCALE ───────────────────────────────────────────────────────────────────
// The flatten is O(visible rows) — it walks the entity instance ring and each
// entity's own brush chain, and touches no windings, no faces and no epairs
// except a group's `targetname`.  Drawing is clipped with ImGuiListClipper, so a
// map with 20k brushes submits ~40 rows a frame.
// ─────────────────────────────────────────────────────────────────────────────

// The panel.  Called once per ImGui frame from imgui_shell.cpp, next to the other
// dock windows; it early-outs when its kiwi_windows flag (KIWI_WIN_OUTLINER) is
// clear, exactly like the shell panel does.
void KiwiOutliner_Draw();

// ── the two group verbs, also reachable from the §15 palette ────────────────
// Both act on the CURRENT selection and decide for themselves which half of the
// editor they are in: brushes selected -> the func_group path, construction
// objects selected -> the sidecar-group path, both -> both.
bool KiwiOutliner_CanGroup();
bool KiwiOutliner_GroupSelection();     // "New Group from Selection"
bool KiwiOutliner_CanUngroup();
bool KiwiOutliner_UngroupSelection();   // dissolve the selection's groups

// KIWI: discard collapse keys and transient row identities with the old map.
void KiwiOutliner_ResetForNewMap();

// §3 registration + the instant-command tail (kiwi_command.cpp calls both).
void KiwiOutliner_RegisterCommands();
bool KiwiOutliner_DispatchInstant( unsigned int cmdId );
