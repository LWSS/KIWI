#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Enumerates and previews static xmodels; camera drops defer creation until after
// the ImGui frame and leave one selected misc_model.

#define KMODEL_PAYLOAD "KIWI_XMODEL"

void KiwiModelBrowser_Draw();
void KiwiModelBrowser_RegisterCommands();
bool KiwiModelBrowser_DispatchInstant( unsigned int cmdId );
bool KiwiModelBrowser_CameraDropTarget( float imgMinX, float imgMinY );
void KiwiModelBrowser_DrawGhost();
void KiwiModelBrowser_ResetForNewMap();
