#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_launch.h — the BSP/LIGHT build panel and captured child-process runner.
// ═════════════════════════════════════════════════════════════════════════════════════
// The compilers remain separate executables. cod4map owns a 32-bit process and can exit
// directly on bad input; cod4rad is a separate x64 program. Both also own filesystem and
// error state that cannot safely be hosted inside the editor.
//
// Every build targets `<fs_basepath>\raw\maps\mp\<name>.map`. Both compilers walk the map
// argument back to a `maps` component and require two folders beneath it. The panel creates
// the target directory as needed.
//
// `-loadFrom <path>` lets cod4map read the editor's saved map while deriving output from the
// final target argument (cod4map bsp.cpp Opt_loadFrom). The flag is omitted only when the
// normalized saved path exactly matches the normalized target path. This keeps output in a
// stable location without copying the source map.
//
// The editor has no thread/lock discipline for handing pipe bytes into ImGui state, so
// `KiwiLaunch_Draw` polls a state machine once per frame: `PeekNamedPipe`, one bounded
// `ReadFile` per available chunk, then `GetExitCodeProcess`. It polls before checking the
// window-open flag, so hiding the panel cannot stall capture or the BSP-to-LIGHT handoff.
//
// cod4map diagnostics need special care. It writes `.errlog` and `.lin` beside the
// `-loadFrom` source, while Radiant's readers key off `currentmap`; those can diverge after
// Save-As or during prefab editing. The panel compares paths before invoking the readers.
// It also deletes stale diagnostics at both possible bases before BSP because cod4map can
// delete a target-side `.lin` but write the new one beside the load source.
//
// Only one compiler runs at a time because LIGHT rewrites the BSP in place. Cancellation is
// `TerminateProcess` behind confirmation: terminating LIGHT can leave a half-lit output, so
// the warning and the existing termination path must remain visible and explicit.
// ═════════════════════════════════════════════════════════════════════════════════════

// Registers the legacy KIWI_CMD_BUILD_RUN command identity on F9. The identifier is retained
// for command-table and user-keybinding compatibility; the visible panel is named "Build".
void KiwiLaunch_RegisterCommands();

// Appends the top-level "Build" menu-bar command. `frameMenu` is the frame HMENU; void* keeps
// this header windows.h-free. Keybinding annotation remains owned by the command system.
void KiwiLaunch_BuildMenu( void *frameMenu );

// The legacy command id toggles the panel.
bool KiwiLaunch_DispatchInstant( unsigned int cmdId );

// Called once per ImGui frame. Polls the running child before deciding whether to draw.
void KiwiLaunch_Draw();
