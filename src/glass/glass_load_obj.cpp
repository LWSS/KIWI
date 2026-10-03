#include "glass_load_obj.h"
#include "glass.h"
#include <universal/com_memory.h>
#include <universal/com_files.h>

// KIWI (GLASS): BO1's loose-file path returned an empty table (new Glass[1000], numGlasses 0)
// and its fastfile path read a Linker-built ASSET_TYPE_GLASSES that KIWI doesn't have.
// KIWI builds the table at map load from the entity string and the panes' collision
// brushes (glass_kiwi_load.cpp), in loose-file and fastfile mode alike.
// BO1 globals glassesInited / glasses are dropped with the old bodies.

Glasses *__cdecl GetGlasses_LoadObj()
{
    return GlassKiwi_GetGlasses(); // KIWI (GLASS): was the empty-table init
}

Glasses *__cdecl GetGlasses_FastFile()
{
    return GlassKiwi_GetGlasses(); // KIWI (GLASS): was DB_GetAllXAssetOfType(ASSET_TYPE_GLASSES)
}

Glasses *__cdecl GetGlasses()
{
    // KIWI (GLASS): was the fs_game / useFastFile dispatch through truncated function-pointer casts
    return GetGlasses_LoadObj();
}

