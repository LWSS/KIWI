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

// KIWI: the Ctrl+Shift+P options dialog (construction-line prisms, auto-import).
// A MODAL POPUP drawn at top-level window scope every frame, like KiwiImport_Draw.
void KiwiPlastBridge_Draw();

// KIWI: export without the dialog (test verb `plasticity_export`).  `handOff`
// false only writes the files and prints their paths.  `includeTerrain` adds every
// visible unselected terrain sheet to the OBJ as a mesh (2026-09-13).  `atOrigin`
// re-frames the whole export onto the origin (first model's frame, or the brushes'
// dominant yaw + base centre); -1 = the dialog's preference (2026-09-16).
bool KiwiPlastBridge_ExportNow( bool includeConstruction, bool includeTerrain, bool handOff,
                                int atOrigin = -1 );
