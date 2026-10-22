#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Enumerates and previews static xmodels; camera drops defer creation until after
// the ImGui frame and leave one selected misc_model.

#include <stddef.h>

#define KMODEL_PAYLOAD "KIWI_XMODEL"

void KiwiModelBrowser_Draw();
void KiwiModelBrowser_RegisterCommands();
bool KiwiModelBrowser_DispatchInstant( unsigned int cmdId );
bool KiwiModelBrowser_CameraDropTarget( float imgMinX, float imgMinY );
void KiwiModelBrowser_DrawGhost();
// KIWI: the selected models' info / physPreset / collision section (Models window and
// Inspector).  Draws nothing when no model is selected.
void KiwiModelBrowser_DrawSelectedModelInfo( bool inspector );
void KiwiModelBrowser_ResetForNewMap();
// Re-list raw\xmodel on the next draw (the Model Import window just built models).
void KiwiModelBrowser_RequestRefresh();
void KiwiModelBrowser_BuildFileMenu( void *frameMenu );
// Synchronous command/test entry; path must be relative to project map_source.
bool KiwiModelBrowser_InsertPrefab( const char *relativePath );
// "Swap in place": rewrite the "model" key of every selected misc_model to `modelName`
// (origin / angles / modelscale / other keys kept; one undo record). False + `err`
// when nothing is selected, the name is empty, or every target already uses it.
bool KiwiModelBrowser_SwapSelected( const char *modelName, char *err, size_t errSz );
