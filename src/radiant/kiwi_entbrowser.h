#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_entbrowser.h — RADIANT_UX_DESIGN §72 (ROUND AU): the ENTITY BROWSER.
//
// USER DIRECTIVE, verbatim:
//   "I want to add a new panel that's in the same viewport (tabbed) with the
//    textures tab.  Make a whole new entity browser similar to the one in
//    trenchbroom that shows each entity as a 3d preview and you can add them
//    into the scene by dragging from the entity viewer to the 3d scene.  Also
//    make sure the legacy entity inspector (N) bind works so we can change their
//    properties."
//
// ── WHERE THE LIST COMES FROM ───────────────────────────────────────────────
// `g_eclass` (eclass.cpp:117), the alphabetised singly-linked list
// Eclass_InitForSourceDirectory / Load_Defs build from the .def files.  It is
// walked through the EXISTING reader, `EclassList_Gather` (win_ent.cpp:169 —
// FillClassList 0x496800's own g_eclass walk, lifted out of the listbox), so the
// browser and the inspector's class list can never disagree about what exists.
//
// THE RMB ENTITY MENU DOES NOT EXIST IN THIS SHELL, so there was no grouping to
// mirror.  `CXYWnd::ContextMenu` (IDB 0x467100) — the popup that carried the
// recursive eclass create-entity tree — is a stub: xywnd.cpp:3175 is a bare
// `(void)hwnd;   // raw shell: no context menu until the HMENU rewrite lands`.
// The only surviving eclass enumeration in the build is the FLAT one above.  So
// the grouping here is KIWI's own and it is stated rather than implied: the
// group key is the classname's text up to the FIRST underscore ("trigger_hurt"
// -> "trigger", "misc_model" -> "misc"), and a name with no underscore goes to
// "(other)".  That is the same split the .def files themselves are organised by
// and the same one classic Radiant's submenu tree produced.
//
// ── THE PREVIEW ─────────────────────────────────────────────────────────────
// KIWI-UX (CLEANUP, C-59): real 3D MODEL thumbnails ship — see kiwi_entthumb.h,
// which is the whole render path (a standalone RTT slot, its own ortho camera
// through R_Ed_SetSceneParms, its own model registration, one thumbnail per tick).
// The isometric ImDrawList box built from the eclass's own [mins,maxs] and colour
// is the FALLBACK, for the classes with no class-level model — `misc_model` /
// `script_model` / `misc_prefab` name their model per ENTITY, so there is nothing
// for a per-CLASS tile to preview.
//
// ── THE DROP ────────────────────────────────────────────────────────────────
// Each tile is an ImGui drag SOURCE carrying the classname (payload
// KENTB_PAYLOAD).  The CAMERA IMAGE is the drop TARGET: imgui_shell.cpp calls
// KiwiEntBrowser_CameraDropTarget immediately after `ImGui::Image`, which is the
// documented pattern for a drop onto an item submitted with id 0 —
// BeginDragDropTarget derives the target id from the last item's RECT when
// `g.LastItemData.ID == 0` (imgui.cpp:16032-16037), so no dummy id is needed.
//
// PLACEMENT is a pick at the DROP PIXEL, not at wherever the cursor drifts to
// afterwards: the pixel is recorded in the drop handler and the ray is cast from
// it.  `Test_Ray` (select.cpp:770) is called exactly the way Cam_ContextMenu
// calls it (camwnd.cpp:4273) with `Pick_CameraContents()`; the first hit gives a
// point and a face normal (edTrace_t.normal, qe3.h:335).  The entity is placed
// so its bbox sits ON that surface along the normal's dominant axis, centred on
// the hit in the other two.  With no hit the ray is met against the ACTIVE
// WORKING PLANE (KiwiCon_RayPlane / KiwiCon_ActivePlane — a read, never a set:
// a drop must not move the plane the user is drawing on), and if even that is
// edge-on, at KENTB_FALLBACK_DIST units along the ray.  The result is
// grid-quantised through KiwiGrid_Snap, which copies through untouched when the
// round-AJ snap master switch is off.
//
// ── AND WHY THE CREATION IS DEFERRED ────────────────────────────────────────
// The drop handler runs INSIDE the compositing ImGui frame.  Entity creation is
// not safe there: `CreateEntityFromName` can raise a MessageBoxA (xywnd.cpp:3392
// worldspawn, :3489 "Failed to create entity.") and its misc_model arm posts a
// window message; a nested Win32 message pump inside the frame's D3D scene
// bracket is the exact crash class the shell moved viewport context menus
// post-present to avoid (imgui_shell.cpp:344).  So the drop only RECORDS
// (classname + pixel) and PostMessage's KIWI_CMD_ENT_DROP to the frame — the
// same deferral kiwi_palette.cpp:151 and kiwi_addmenu.cpp:160 already use, and
// the same route every menu command takes.
//
// ── THE CREATION PATH IS THE PORTED ONE ─────────────────────────────────────
// Not a new one.  `CreateEntityFromClassname` (xywnd.cpp:3336, IDB 0x466480)
// with nothing selected does exactly three things, and the deferred handler does
// the same three in the same order:
//     Undo_ClearRedo(); Undo_GeneralStart( "create entity" );
//     <drop a placeholder world brush where the entity goes>
//     CreateEntityFromName( classname );
//     Undo_End();
// ONE record per drop.  The placeholder is what a fixedsize class binds to —
// Entity_Create derives the entity origin from it as
// `placeholderMins - eclass->mins` (entity.cpp:1758-1760) and then consumes it
// with Select_Delete — so the placeholder's MINS are the whole of the placement
// arithmetic, and a brush class simply keeps the box as its first brush (the
// same thing the XY menu's create does).
//
// THE ROUND-W TRAP IS AVOIDED EXPLICITLY: Entity_Create MERGES into a selected
// non-world entity instead of creating (entity.cpp:1633-1666), so the handler
// runs `Select_Deselect( 1 )` BEFORE opening the bracket — before, so the record
// clones nothing that is about to be dropped, which is §23's ordering rule
// (kiwi_primitive.cpp:1173 does the same thing for the same reason).
//
// After creation CreateEntityFromName has already selected the new entity
// (xywnd.cpp:3419-3420 Select_Deselect + Select_Brush), so the tail is one call:
// `KiwiCmd_AfterPaste()` — the paste/clone precedent — which starts KIWI_CMD_MOVE
// PAUSED so the thing that just landed is under the gizmo.
//
// ── THE N BIND ──────────────────────────────────────────────────────────────
// N was never unbound.  `{ "ViewEntityInfo", 0x4E, 0, 33017 }` (mainfrm.cpp:1140)
// is in the DEFAULT table and kiwi_keymap.cpp's modern profile does not touch vk
// 0x4E at all, so N -> 33017 -> Cmd_OnViewEntity (mainfrm.cpp:3055) ->
// ImGuiPanel_Entity_Toggle in both profiles.  What was broken is what the panel
// SHOWED — see kiwi_entbrowser.cpp's N BIND block and RADIANT_UX_DESIGN §72
// item 3.  This header owns the browser; the two inspector fixes live in
// imgui_panel_entity.cpp and win_ent.cpp behind // KIWI-UX fences.
// ─────────────────────────────────────────────────────────────────────────────

// The ImGui drag-drop payload type.  Payload types are capped at 32 chars
// (imgui.cpp's IM_ASSERT on ImGuiPayload::DataType); this is 11.
#define KENTB_PAYLOAD "KIWI_ECLASS"

// Ray length used when neither a surface nor the working plane can answer.
#define KENTB_FALLBACK_DIST 256.0f

// The dock window (§9 flag KIWI_WIN_ENTITIES).  Begins and Ends itself and
// early-outs when the flag is clear, exactly like KiwiOutliner_Draw.
void KiwiEntBrowser_Draw();

// Called by imgui_shell.cpp IMMEDIATELY after the CAMERA viewport's ImGui::Image
// (the last-item rect is what BeginDragDropTarget keys off).  Returns true when a
// payload was accepted this frame — the caller uses it only for the log.
bool KiwiEntBrowser_CameraDropTarget( float imgMinX, float imgMinY );

// KIWI-UX (ROUND AX, ITEM 6).  The would-be placement box, drawn in the 3D view
// while an entity-class payload hovers the camera image.  Called from CamWnd_Draw's
// overlay tail; self-gating (nothing is drawn unless a drag is live over the camera)
// and self-budgeting (one 12-segment KiwiLines batch).  The BOX is resolved in the
// ImGui frame by the drop target above — running the real ladder, read-only — so this
// side never touches the picker or the camera basis.  See kiwi_entbrowser.cpp.
void KiwiEntBrowser_DrawGhost();

// §3 registration + the instant-command tail (kiwi_command.cpp calls both).
void KiwiEntBrowser_RegisterCommands();
bool KiwiEntBrowser_DispatchInstant( unsigned int cmdId );
