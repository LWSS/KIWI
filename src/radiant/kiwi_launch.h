#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// BSP/LIGHT build panel and captured child-process runner.
// cod4map (32-bit) and cod4rad (x64) stay out of process: they own global filesystem/error
// state and may terminate directly on bad input.
// Builds target `<fs_basepath>\raw\maps\mp\<name>.map`; `-loadFrom` reads the saved source
// without moving output and is omitted only when normalized source and target paths match.
// Polling runs even while the panel is hidden so pipe capture and the BSP-to-LIGHT handoff
// cannot stall. Diagnostics follow `-loadFrom`, but Radiant's readers follow `currentmap`;
// Save-As and prefab editing can make those paths diverge.
// LIGHT rewrites the BSP in place, so cancellation warns that termination can corrupt it.

// Preserve the legacy command identity and F9 binding.
void KiwiLaunch_RegisterCommands();

// `frameMenu` is an HMENU kept void* so this header remains windows.h-free.
void KiwiLaunch_BuildMenu( void *frameMenu );

bool KiwiLaunch_DispatchInstant( unsigned int cmdId );

// Polls the child once per ImGui frame before deciding whether to draw.
void KiwiLaunch_Draw();

// The panel's path comparison (GetFullPathName + case-insensitive), for errlog map tokens.
bool KiwiLaunch_SamePath( const char *a, const char *b );

// The live fragile check (kiwi_fragile.cpp) runs the panel's cod4map from the panel's root.
struct kiwiLaunchTool_t
{
    char exe[260];              // KIWI-cod4map.exe beside the editor
    char root[260];             // fs_basepath: the build root and the compiler's working directory
    char binDir[260];           // the editor's directory (data next to the exe)
    char name[128];             // the open map's base name, "untitled" when unsaved
};
bool KiwiLaunch_Cod4mapTool( kiwiLaunchTool_t *out );   // false without a root or the exe
bool KiwiLaunch_Busy();                                 // a build is running
