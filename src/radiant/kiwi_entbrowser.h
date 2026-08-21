#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_entbrowser.h — the ENTITY BROWSER: a grouped tile view over `g_eclass`
// (walked through EclassList_Gather), each tile an ImGui drag source that drops a
// new entity into the camera viewport.  Thumbnails live in kiwi_entthumb.h.
//
// Groups are keyed on the classname up to the FIRST underscore; no underscore ->
// "(other)".  Creation is DEFERRED off the drop: CreateEntityFromName can raise a
// MessageBoxA and its misc_model arm posts a window message, and a nested Win32
// pump inside the frame's D3D scene bracket is a crash.  The drop records
// (classname + drop pixel) and PostMessage's KIWI_CMD_ENT_DROP.  Select_Deselect
// runs BEFORE the undo bracket opens — Entity_Create MERGES into a selected
// non-world entity instead of creating.

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

// The would-be placement box, drawn in the 3D view while an entity-class payload
// hovers the camera image.  Called from CamWnd_Draw's overlay tail; self-gating.
// The box is resolved in the ImGui frame by the drop target above, so this side
// never touches the picker or the camera basis.
void KiwiEntBrowser_DrawGhost();

// §3 registration + the instant-command tail (kiwi_command.cpp calls both).
void KiwiEntBrowser_RegisterCommands();
bool KiwiEntBrowser_DispatchInstant( unsigned int cmdId );
