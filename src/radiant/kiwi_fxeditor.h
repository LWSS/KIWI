#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-11-02, user: "an FX editor/creator ... Look at xoxor's iw3xoradiant for
// inspiration"): the "FX Editor" dock window.  Opens a raw/fx/*.efx (from the Particles
// list, the selected FX entity, or New), edits its elements - spawning, motion, size /
// colour / alpha curves, visuals - and saves it back in the stock iwfx format
// (kiwi_efx.h).  Every edit is pushed to the camera preview (kiwi_fxpreview.h), so the
// selected FX entities and the editor's test instance play the unsaved effect at once.
// The layout (element list beside tabbed property pages, curve editors, play / pause /
// repeat) follows iw3xo-radiant's effects editor (src/ggui/effects_editor_gui.cpp); the
// code is KIWI's own.

#include <string>
#include <vector>

void KiwiFxEditor_Draw();                       // KIWI_WIN_FXEDITOR
void KiwiFxEditor_RegisterCommands();
bool KiwiFxEditor_DispatchInstant( unsigned int cmdId );

// Opens raw/fx/<fxName>.efx and fronts the window.  False when it does not parse (the
// console says why); a dirty document is kept and the user asked first.
bool KiwiFxEditor_Open( const char *fxName );
// A new unsaved effect with one looping sprite element.
void KiwiFxEditor_New();

// Test DSL: `fxedit <verb> ...` (kiwi_test.cpp).  False + `err` on failure; `out` is the
// verb's report (counts, names).
bool KiwiFxEditor_TestVerb( const std::vector<std::string> &w, std::string &out, std::string &err );
