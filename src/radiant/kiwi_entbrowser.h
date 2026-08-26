#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Grouped EclassList_Gather tiles that drag entities into the camera; thumbnails
// live in kiwi_entthumb.h. Groups use the prefix before the first underscore.
// Creation is deferred because CreateEntityFromName may pump Win32 inside the D3D
// frame. The drop latches its classname, image-relative pixel, and any valid box.
// Deselect before opening undo so Entity_Create cannot merge into another entity.

// ImGui payload types are capped at 32 chars; this is 11.
#define KENTB_PAYLOAD "KIWI_ECLASS"

// Ray length used when neither a surface nor the working plane can answer.
#define KENTB_FALLBACK_DIST 256.0f

// Self-contained KIWI_WIN_ENTITIES dock window.
void KiwiEntBrowser_Draw();

// Call immediately after the camera ImGui::Image so BeginDragDropTarget sees its
// last-item rect. Returns whether a payload was accepted this frame.
bool KiwiEntBrowser_CameraDropTarget( float imgMinX, float imgMinY );

// Self-gated CamWnd_Draw overlay; the ImGui frame resolves the box so this path
// never touches the picker or camera basis.
void KiwiEntBrowser_DrawGhost();

// Registration and instant-command tail used by kiwi_command.cpp.
void KiwiEntBrowser_RegisterCommands();
bool KiwiEntBrowser_DispatchInstant( unsigned int cmdId );
