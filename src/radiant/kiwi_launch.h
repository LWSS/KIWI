#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_launch.h — KIWI-UX (ROUND BF): THE BUILD & RUN LAUNCH DIALOG.
// ═════════════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: *"Hook the cod4map and cod4rad projects natively into the KIWI
// radiant.  Keep them as separate .exe, but add a launch pop-up dialog that allows building
// of the map (BSP), building of the rad (lighting), and a button to launch kisakcod and load
// into the map easily."*
//
// This file owns the dialog, the child-process runner and the three-stage pipeline.  It
// invokes the compilers; it never links them.  `src/cod4map/` and `src/cod4rad/` are
// READ-ONLY reference for this round — every contract below was derived by READING them,
// and each claim carries the file:line it came from.
//
// ── D-BF-A: WHY A LAUNCHER AND NOT A LINKED COMPILER ───────────────────────────────
// cod4map is a 32-bit console program with its own file system, its own globals and an
// `exit(-1)` on a usage error (bsp.cpp:1177); cod4rad is a SEPARATE x64 binary
// (scripts/cod4rad/CMakeLists.txt:10 refuses any other architecture, :103 `/machine:x64`)
// which a 32-bit editor could not load even in
// principle.  Both were written to own the process: they call `Com_Error`, they `remove()`
// output files at startup, and cod4rad parks a `radtrans.bin` cache in the CWD
// (compile.c:329).  "Separate .exe, driven by a dialog" is therefore not a compromise, it
// is the only shape that does not require rewriting either tool.
//
// ── D-BF-B: THE TARGET IS ALWAYS `raw\maps\mp\<name>.map`, AND IT IS NOT NEGOTIABLE ──
// Both compilers derive their BASE PATH by walking the map argument backwards for a path
// component named exactly `maps` and taking everything above the directory that CONTAINS it
// (cod4map universal/com_files.cpp:116-131; cod4rad cmdlib.c:181-246).  A map path with no
// `maps` component is a FATAL error ("No 'maps' in '%s'"), and a `maps` only one level down
// is the second fatal ("There should be two folders below 'maps' in a proper install").
// The .map files in this tree live in `bin\<CONFIG>\map_source\`, which has NO `maps`
// component at all — so a naive "compile the file the editor has open" would fatal on every
// map in the repo.
//
// The other half of the constraint is the GAME: `Com_GetBspFilename` builds
// `maps/mp/<name>.d3dbsp` for MP (universal/com_files.cpp:2098-2105) and the search path is
// `<fs_basepath>/{devraw_shared,devraw,raw_shared,raw,players}` then `main`
// (com_files.cpp:1934-1990).  So the ONE directory that satisfies both the compiler's
// two-levels-below-`maps` rule and the game's lookup is `<root>\raw\maps\mp\`.  Every build
// this dialog runs targets that directory, and the dialog creates it if it is missing.
//
// ── D-BF-C: `-loadFrom` IS THE BRIDGE, AND THE RULE FOR USING IT IS EXACT ──────────
// `-loadFrom <path>` makes cod4map READ that map while every output still derives from the
// LAST argument (bsp.cpp:29 option row, :1267-1281 Opt_loadFrom, :1240-1241 the
// `LoadMapFile(g_loadFromPath)` branch; outputs come from
// `g_outputBasePath = ExpandArg(argv[argc-1])`, :1210-1211).  So the editor's saved map, in
// `map_source\` or anywhere else, is compiled INTO `raw\maps\mp\`.
//
// THE RULE THIS FILE APPLIES is stricter than "is the saved map already two levels below a
// `maps` folder": `-loadFrom` is omitted ONLY when the saved path IS the target path
// (`GetFullPathNameA`-normalised, case-insensitive compare).  A map saved at, say,
// `<root>\usermaps\maps\mp\x.map` satisfies the compiler's rule and could be passed
// directly — and its `.d3dbsp` would then land in `usermaps\`, which is NOT on the game's
// search path, so `+devmap` would not find it.  Bridging that case through `-loadFrom`
// costs nothing and can never produce off-search-path output.
//
// ── D-BF-D: NO THREADS.  THE POLL IS THE FRAME. ────────────────────────────────────
// The radiant is a single-threaded WM_PAINT pump (radiant_main.cpp:945-1012 — PeekMessage
// drain, then one forced paint per 1/60 s).  A worker thread reading a pipe would have to
// hand its bytes to ImGui state that the paint owns, and the editor has no lock discipline
// for that.  So the runner is a STATE MACHINE polled from `KiwiLaunch_Draw`:
// `PeekNamedPipe` for the byte count, one bounded `ReadFile` per available chunk, then
// `GetExitCodeProcess`.  A 60 Hz poll with a 64 KB pipe is far more headroom than either
// compiler's output rate needs.
//
// `KiwiLaunch_Draw` is called UNCONDITIONALLY from the shell's per-frame draw
// (imgui_shell.cpp, beside `KiwiImport_Draw`) and polls BEFORE it looks at the window flag,
// so closing the window does not stall a running build or freeze the chain between stages.
// The state is file-scope, not per-open: re-opening the window shows the live build.
//
// ── D-BF-E: THE `.errlog` / `.lin` HANDOFF, AND THE ONE PLACE IT CAN DISAGREE ──────
// The compiler↔radiant contract already exists and predates this round:
//   * cod4map writes `<base>.errlog` (errors.cpp:11-15 `Error_Init` truncates it,
//     :112-114 appends one line per error) and `<base>.lin` on a leak (leakfile.cpp:39,
//     :154), where `<base>` comes from `BuildOutputPathFromLoadSource` (bsp.cpp:151-160) —
//     i.e. **the `-loadFrom` path when one is set, else the map source**;
//   * the editor reads `<currentmap minus ext>.errlog` (`Pointfile_Errorfile`,
//     errorfile.cpp:338-345) and `<currentmap minus ext>.lin` (`Pointfile_Check`,
//     points.cpp:56-69).
// Both editor readers key off `currentmap` (map.cpp:44) while this dialog compiles
// `s_currentMapPath` (mainfrm.cpp:488).  Those two are the SAME string after File->Open
// (mainfrm.cpp:628 writes one, map.cpp:425 the other) and after a plain Save (neither moves)
// — but they DIVERGE after Save-As (mainfrm.cpp:4447 moves only `s_currentMapPath`) and
// while prefab editing has stomped `currentmap` (map.cpp:1797-1813).  The dialog therefore
// compares them before calling either reader, and prints the real path instead of letting
// `Pointfile_Errorfile` pop its "Error log file was not found" box at a stale path.
//
// STALE `.lin` TRAP, found by reading rather than by testing: cod4map deletes the stale
// `.lin` at `g_outputBasePath` — the TARGET base (bsp.cpp:1216-1217) — but WRITES the new
// one at the LOAD-SOURCE base.  With `-loadFrom` in play those are different files, so a
// `.lin` left next to the .map by an earlier leak is never cleaned up and would make every
// later build look like it leaked.  This dialog deletes both candidates itself before each
// BSP run, so "a `.lin` exists afterwards" means "THIS run leaked".
//
// ── D-BF-F: ONE BUILD AT A TIME, AND WHAT KILL COSTS ──────────────────────────────
// Only one capture stage (BSP or Light) may run at a time — they write the same `.d3dbsp`
// (cod4rad reads and REWRITES it in place, cod2rad.c:76/:81) and racing them would corrupt
// it.  Run Map is exempt: it is a detached GUI process with no pipe, and pressing it twice
// is a user's business.  Kill is `TerminateProcess` behind a confirm, because a killed
// cod4rad leaves a HALF-LIT `.d3dbsp` on disk — the file is rewritten in place, so there is
// no "unchanged" state to fall back to.
//
// ── D-BF-G: WHAT THE GAME NEEDS ───────────────────────────────────────────────────
// `+devmap <name>` is parsed out of the command line by `Com_ParseCommandLine` (splits on
// '+', qcommon/common.cpp:774-790) and executed by `Com_AddStartupCommands`
// (common.cpp:1232-1248).  `devmap` is registered at server_mp/sv_ccmds_mp.cpp:332-334 and
// SETS `sv_cheats` itself from the command name (:421-422) — so the cheats checkbox below is
// only meaningful for plain `map`, and even there SV_Map_f overwrites it at spawn.  `+map`
// and `+devmap` both require a player profile (`Com_Error(ERR_DROP,
// "PLATFORM_NOTSIGNEDINTOPROFILE")`, :388-390).  `useFastFile` defaults 0 in this tree
// (common.cpp:1525-1529), so a loose `.d3dbsp` is enough and no zone/.ff build is needed.
// ═════════════════════════════════════════════════════════════════════════════════════

// Registers KIWI_CMD_BUILD_RUN ("Build & Run...").  Called by
// KiwiCmd_RegisterCommands beside the other feature registrars.
// ── KIWI-UX (ROUND BH, ITEM 5): NO LONGER UNBOUND — it registers on F9, which is
// the row's COMPILED-IN DEFAULT and is therefore live in BOTH keymap profiles.
// The full free-key audit (every F-key row in the stock table, the modern
// profile's 81 Bind rows, the KIWI/alias rows and IDR_MAIN_ACCEL) is on the
// definition; the short answer is that vk 0x78 appears nowhere in the tree except
// the radiant.ini key-NAME spelling table, so no Shift+F9 fallback was needed.
void KiwiLaunch_RegisterCommands();

// ── KIWI-UX (ROUND BH, ITEM 5): the top-bar entry ───────────────────────────
// USER DIRECTIVE, verbatim: *"Build and run needs to be in the win32 toolbar
// somewhere."*  There is no Win32 toolbar in this shell (the CToolBar and
// IDR_TOOLBAR152's resources went out with the MFC rip — see the definition),
// so this appends a TOP-LEVEL, POPUP-LESS menu-bar item carrying
// KIWI_CMD_BUILD_RUN.  Called once from radiant_main.cpp's boot sequence beside
// KiwiWindows_BuildMenu / KiwiWindows_BuildViewMenu, and AFTER them so it sits at
// the right end of the bar.  `frameMenu` is the frame's HMENU (void* so this
// header stays windows.h-free, exactly as kiwi_windows.h's twin does).
// The caption carries no key text: Radiant_ShowMenuItemKeyBindings owns that and
// will annotate this row itself on the next profile switch (see the definition).
void KiwiLaunch_BuildMenu( void *frameMenu );

// The instant-command arm: KIWI_CMD_BUILD_RUN toggles the window.
bool KiwiLaunch_DispatchInstant( unsigned int cmdId );

// Called once per ImGui frame at top-level window scope (imgui_shell.cpp).  POLLS THE
// RUNNING CHILD FIRST and only then draws — a closed window must not stall a build.
void KiwiLaunch_Draw();
