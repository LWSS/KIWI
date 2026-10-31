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
// Edits are staged in the panel.  Save byte-patches ONLY three header fields
// (MaterialRaw::info.sortKey +0x09, info.surfaceFlags +0x20, info.contents +0x24) in EVERY
// loose copy of materials/<name> - raw/ and main/ under fs_homepath and fs_basepath - since the
// editor reads the first one its searchpaths reach (main/ for stock materials) and the
// compilers read raw/.  A packed-only material gets a raw/materials copy.  It then re-reads the
// file through the engine filesystem to prove the editor sees the fields, and reloads the live
// material and the browser entry the way kiwi_matconvert does.  Before the first overwrite the
// file as the editor read it is copied to raw/materials/_kiwi_backup/<name>.matprops (never
// overwritten, so it stays the pre-KIWI original); Revert puts its three fields back.
// Compiled .d3dbsp maps carry their own copy of surface flags: recompile to see a change
// in game.

void KiwiMatProps_Draw();      // imgui_shell.cpp, top-level window scope, every frame

// Show `material` in the Material window (opening and focusing it) - the Textures browser's
// right-click "Material properties".  It stays shown until the browser's current material
// changes; the browser selection itself is left alone.
struct qtexture_s;
void KiwiMatProps_Open( qtexture_s *material );
