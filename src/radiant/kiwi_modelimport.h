#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// The Model Import window (KIWI_WIN_MODELIMPORT): FBX/OBJ sources -> raw\xmodel +
// collmaps\ through the importer core in src\radiant\modelimport, editing the same
// model_source\<asset>.kimport recipes the KIWI-modelimport CLI / MCP server edit.
// The preview shows the SOURCE mesh in model space, coloured by surface type, with
// the collision hulls over it.

void KiwiModelImport_Draw();              // the window; imgui_shell.cpp, beside the other panels
void KiwiModelImport_RenderPreview();     // its 3D view RT; end of ImGuiShell_RenderViewportsToRT
void KiwiModelImport_ReleaseForReset();   // device loss (RTT_ReleaseForReset): re-render the view
void KiwiModelImport_RegisterCommands();
