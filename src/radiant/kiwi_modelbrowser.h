#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// KIWI: The Models dock window enumerates static xmodels through the editor
// filesystem, previews them through kiwi_entthumb, and defers camera placement
// until after the ImGui frame.  A placement creates one selected misc_model.

#define KMODEL_PAYLOAD "KIWI_XMODEL"

void KiwiModelBrowser_Draw();
void KiwiModelBrowser_RegisterCommands();
bool KiwiModelBrowser_DispatchInstant( unsigned int cmdId );
bool KiwiModelBrowser_CameraDropTarget( float imgMinX, float imgMinY );
void KiwiModelBrowser_DrawGhost();
void KiwiModelBrowser_ResetForNewMap();
