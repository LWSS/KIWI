#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Selected brush reference export to STEP plus patch/model meshes to OBJ.
bool KiwiPlastBridge_CanPush();
void KiwiPlastBridge_RegisterCommands();
bool KiwiPlastBridge_DispatchInstant( unsigned int cmdId );
void KiwiPlastBridge_BuildMenu( void *frameMenu );

// KIWI: Export details and the default-on auto-import preference in the settings panel.
void KiwiPlastBridge_DrawSettings();
