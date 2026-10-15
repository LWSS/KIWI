#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-10-14, user: "involve the existing physics engine to allow xmodels to be dropped
// upon the map and upon resting become static props (or props with physics as well)"):
// Physics Drop (KIWI_CMD_PHYS_DROP, palette "Physics Drop").  The selected models fall under
// gravity in the in-tree ODE (kiwi_physdrop_sim.h), tumble against visible brushes, patches,
// other models and each other, live in the camera, and come to rest.  RMB / Enter keeps the
// result as ONE undo record (still falling = settled first), Esc / Ctrl+Z puts them back.
// Options: Scatter (seeded spin + sideways push), Seed (N = a new one, re-dropped from the
// start), Result (Static prop keeps the class; Physics prop = dyn_model, the game's clutter).

class KiwiEditorCommand;

void KiwiPhysDrop_RegisterCommands();
KiwiEditorCommand *KiwiPhysDrop_CommandForId( int commandId );

// A selected model entity (not a prefab).
bool KiwiPhysDrop_CanExecute();

// The command is live / and everything in it is at rest (hint-strip wording).
bool KiwiPhysDrop_Active();
bool KiwiPhysDrop_Resting();

// Once per frame before the camera renders: steps the simulation and poses the models.
void KiwiPhysDrop_Tick();
