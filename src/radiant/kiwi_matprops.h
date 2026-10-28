#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-10-26, user: "material properties panel") - the "Material" dock window
// (KIWI_WIN_MATPROPS).  It shows the Textures browser's current material (the active edit
// layer's random_texture_stuff[].mtl.radMtl): colour-map swatch, techset, surface type (which
// drives footsteps, impact effects and bullet penetration), the per-weapon-size penetration
// depths for that type from info/bullet_penetration_mp, the named surfaceFlags and contents
// bits, the sort key and the texture table.
//
// Edits are staged in the panel.  Save byte-patches ONLY three header fields of
// raw/materials/<name> (MaterialRaw::info.sortKey +0x09, info.surfaceFlags +0x20,
// info.contents +0x24) through KiwiFS_WriteRawAtomic, re-reads the file through the engine
// filesystem to prove the bytes landed (and that no other searchpath shadows them), then
// reloads the live material and the browser entry the way kiwi_matconvert does.  Before the
// first overwrite the file as it was is copied to raw/materials/_kiwi_backup/<name>.matprops
// (never overwritten, so it stays the pre-KIWI original); Revert writes that copy back.
// Compiled .d3dbsp maps carry their own copy of surface flags: recompile to see a change
// in game.

void KiwiMatProps_Draw();      // imgui_shell.cpp, top-level window scope, every frame
