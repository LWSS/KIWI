#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Breakable glass (KIWI 2026-10-03, BO1 glass port P5; user: "we need a way to have brushes
// transform into glass, and a way to define which glass textures to use for that").
//
// A pane is one convex slab brush in its own `glass` brush entity.  Its "glassdef" key names
// a texture set, raw/glass/<set>: a GLASSDEF infostring with the pane's pristine material,
// the cracked material swapped onto it when shot, the shard material and the break numbers.
// The game builds every pane at map load from the compiled submodel (src/glass/
// glass_kiwi_load.cpp); the editor validates with the same slab routine
// (src/glass/glass_kiwi_slab.h), so the two cannot disagree.  cod4map is unchanged.
//
// Make Breakable Glass turns each selected brush into its own pane (faces/edges/vertices
// mean their brushes; the selection itself is left as it was).  Make Regular Brush puts pane
// brushes back.  Both are one undo record.  The Glass window lists the sets, edits them and
// applies one to the selected panes; selecting a pane fronts it.

#define KIWI_GLASS_CLASSNAME "glass"

void KiwiGlass_RegisterEclass();             // Radiant_LoadEclassDefs, after every def scan
void KiwiGlass_RegisterCommands();
bool KiwiGlass_DispatchInstant( unsigned int cmdId );

bool KiwiGlass_CanMake();
bool KiwiGlass_CanUnmake();
bool KiwiGlass_CanApply();
bool KiwiGlass_Make();                       // Make Breakable Glass (active set)
bool KiwiGlass_Unmake();                     // Make Regular Brush
bool KiwiGlass_ApplySetToSelection();        // selected panes take the active set
bool KiwiGlass_SetStartState( int state );   // selected panes' "startbroken": 0 intact, 1 cracked, 2 shattered

void KiwiGlass_Draw();                       // the "Glass" dock window (KIWI_WIN_GLASS)
void KiwiGlass_Focus();                      // open the window and front its tab (a pane got selected)
void KiwiGlass_BeforeMapSave();              // Map_SaveFile: refresh every pane's uvscale
void KiwiGlass_DrawBuildCheck();             // Build window: pane errors, non-blocking

// Test DSL
bool KiwiGlass_SetActiveSet( const char *name );   // false = no such set file
int  KiwiGlass_Validate( bool log, int *outWarnings, int *outPanes );   // returns the error count
