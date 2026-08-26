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
