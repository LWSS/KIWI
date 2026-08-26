#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Drag-and-drop texture import wizard and .iwi/material writer orchestration.

// World units per repeat == autoTexScale * the current edit layer's sampleSize.
// Defaults preserve source aspect while capping autoTexScale's long edge at 512.

// `hDropOpaque` is WM_DROPFILES's HDROP. Paths are copied before DragFinish without
// touching ImGui. True means the caller should post KIWI_CMD_IMPORT_DROPPED.
bool KiwiImport_HandleDropFiles( void *hDropOpaque );

// Handles the dropped-file continuation and the multi-select browse command.
bool KiwiImport_DispatchInstant( unsigned int cmdId );

// Registers only BROWSE; DROPPED is an internal, non-bindable continuation.
void KiwiImport_RegisterCommands();

// Draw once per top-level ImGui frame; an empty queue draws nothing.
void KiwiImport_Draw();
