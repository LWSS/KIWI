#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Keymap profiles rewrite vk/mods in Radiant's shared mutable command table.
// Switching always layers compiled defaults -> radiant.ini -> modern patch; classic
// stops after radiant.ini. This order prevents drift and gives patched rows final values.
// Modifier bits match Radiant: Shift=1, Alt=2, Ctrl=4, LWin=8.
//
// Modern bindings:
//   1..5              selection modes: Point, Edge, Face, Object, All
//   Ctrl+1..4         convert selection to Point, Edge, Face, Object
//   F                 Command Palette
//   [ / ]             halve / double grid spacing
//   PageDown / PageUp halve / double grid spacing (aliases; PageUp is coarser)
//   G / R / S         Move / Rotate / Scale
//   bare arrows       modern polled fly; only the bare stock rows are unbound
//   Delete            Delete Selection; the base Backspace row remains bound
//   Shift+A / S / Q   Line / Spline / corner Rectangle
//   Shift+C / W       centre Circle / corner Box
//   Shift+V / Alt+V   centre Box / centre Rectangle
//   Shift+X / Z       Cylinder / Sphere
//   Ctrl+J            Join Lines
//   C / Z / J / E     Cut / Match Face / context Join / context Extrude
//   Ctrl+R            Split Face
//   T / O / B         Trim / Offset Curve / Fillet Corners
//   Q / L             Boolean / Loft
//   Shift+D           Duplicate and enter Move
//   /                 Focus On Selection
//   Shift+H / Alt+H   Hide Unselected / Show Hidden
//   Ctrl+H            Invert Hidden
//   Shift+R           Repeat Last Command
//   End               Caulk Selection; Center View moves to Shift+End
//   Space             View Face Head-on; Clone moves to Shift+Space
//   Ctrl+X            intentionally unbound; File->Exit accelerator removed upstream
//   Add Menu          intentionally unbound; Shift+A starts Line instead
//
// Binding invariants and gotchas:
// - Displaced stock rows move before modern claims; table lookup is first-match-wins.
// - Delete and PageUp/PageDown use name-bound duplicate-id aliases so Backspace and
//   the bracket grid bindings remain intact.
// - V (movable pivot) and drawing-tool Z are command-local; modal dispatch precedes
//   global hotkey lookup, so no global rows are needed.
// - Modal numeric input consumes Space before the global View Face binding.
// - Resource accelerators run before the command table; default claims were checked
//   against both stores. Modern forces KIWI_CMD_CLIP_CUT unbound; classic may remap it.
// - The ported key-name table lacks 0xBF, so radiant.ini cannot spell "/" for rebinding.
// Every displaced command remains available from its menu or the command palette; the
// exact destination is documented beside its Bind() call in kiwi_keymap.cpp.
enum kiwiKeymap_t
{
    KEYMAP_CLASSIC = 0,
    KEYMAP_MODERN  = 1,      // the overhaul's DEFAULT
};

kiwiKeymap_t KiwiKeymap_Get();

// Persist the profile, rebuild the live table, and refresh menu accelerator text.
void KiwiKeymap_Set( kiwiKeymap_t profile );

// Called after boot loaded radiant.ini; applies only the selected profile patch.
void KiwiKeymap_ApplyBoot();

// Draw the profile switcher in KiwiUX settings.
void KiwiKeymap_DrawSettings();
