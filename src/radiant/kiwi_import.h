#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_import.h — drag-and-drop texture import wizard: groups dropped files into
// colour/normal/spec slots, then drives the .iwi writer (kiwi_iwi.h) and the material
// writer (kiwi_matwriter.h) and verifies the result through the engine's own loaders.

// Constraint: world units per repeat == autoTexScale * layer-0 sampleSize (0.25), so the
// wizard's tiling spinner is denominated in world units and converts through that same
// sampleSize.  Default caps the written autoTexScale's long edge at 512, aspect preserved.

// The WM_DROPFILES handler.  `hDropOpaque` is the HDROP wParam; it is enumerated and
// DragFinished immediately (an HDROP is only valid until DragFinish returns).  Returns true
// when at least one file was queued, i.e. the caller should post KIWI_CMD_IMPORT_DROPPED.
// Safe from inside a message handler: touches no ImGui state.
bool KiwiImport_HandleDropFiles( void *hDropOpaque );

// The instant-command arm: KIWI_CMD_IMPORT_DROPPED (open the wizard on the queue) and
// KIWI_CMD_IMPORT_BROWSE (GetOpenFileName multi-select -> the same queue -> the same wizard).
bool KiwiImport_DispatchInstant( unsigned int cmdId );

// Registers KIWI_CMD_IMPORT_BROWSE.  KIWI_CMD_IMPORT_DROPPED is deliberately NOT registered:
// it is an internal continuation of a finished gesture, like KIWI_CMD_ENT_DROP.
void KiwiImport_RegisterCommands();

// The wizard.  Called once per ImGui frame at top-level window scope.  Draws nothing when
// the queue is empty.
void KiwiImport_Draw();
