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

// ── User keybinds (Help > Command List) ──────────────────────────────────────────
// KIWI (2026-10-27, user: "allow assignment of new keybinds ... the keybinds will save to disk
// and be loaded upon start ... Keybind cfg is saved as you go").  The layers are now
// compiled defaults -> radiant.ini -> profile patch -> USER layer.  The user layer is a diff:
// kiwi_radiant.ini [KiwiKeybindsModern] / [KiwiKeybindsClassic] holds one line per command
// whose bindings differ from that profile's defaults ("Name=Ctrl+R, F5" or "Name=none"),
// keyed by the command's FIRST table name.  Every edit rewrites the section at once.
// A command's bindings are all rows with its id; a binding beyond its own rows takes a
// "KiwiUserBinding" alias row (registered unbound, so Reset leaves it free for reuse).
struct kiwiChord_t
{
    byte vk;
    byte mods;                    // Shift=1, Alt=2, Ctrl=4, Win=8
};

#define KIWI_KEYBIND_MAX 4        // bindings one command may hold

// Key name for a VK the ported key-name table never named (/ ; = ' Home F13..F24 ...);
// NULL when unnamed.  CommandList_KeyName falls back to it.
const char *KiwiKeymap_ExtraKeyName( unsigned vk );

// "Ctrl+Shift+Alt+Win+Key" (the hint strip's order); "" for vk 0.
void KiwiKeybind_Format( kiwiChord_t c, char *out, int outSize );

// Parses "Ctrl+Shift+R", "ctrl r", "ctrl-r", "R", "f5", "ctrl" (modifiers only: *vk = 0).
// False on an unknown token or two key tokens.
bool KiwiKeybind_Parse( const char *text, kiwiChord_t *out );

// Live / profile-default bindings of one command id, table order; returns the count.
int  KiwiKeybind_Get( int commandId, kiwiChord_t *out, int maxOut );
int  KiwiKeybind_GetDefault( int commandId, kiwiChord_t *out, int maxOut );
bool KiwiKeybind_IsCustom( int commandId );

// First command the hotkey lookup would run for a chord (0 = none), and every id holding it.
int  KiwiKeybind_Winner( kiwiChord_t c );
int  KiwiKeybind_Owners( kiwiChord_t c, int *ids, int maxIds );

// Why a chord cannot fire as a table binding for `commandId` (system chord, menu key, a fixed
// accelerator or pump key that runs first, unnamed key); NULL when it can.
const char *KiwiKeybind_Reserved( kiwiChord_t c, int commandId );

// Fixed chords that run this command OUTSIDE the table (IDR_MAIN_ACCEL accelerators, the
// pump's Ctrl+Z / Ctrl+Y), minus any the live table already lists for it.
int  KiwiKeybind_Fixed( int commandId, kiwiChord_t *out, int maxOut );

// Edits.  Each applies, strips the chord from every other command (a chord has ONE owner),
// saves, and refreshes the menu accelerator text.  slot -1 = add a binding.
bool KiwiKeybind_Assign( int commandId, int slot, kiwiChord_t c );
void KiwiKeybind_Remove( int commandId, int slot );
void KiwiKeybind_ResetCommand( int commandId );
// Reset to Default: Modern profile, both user sections dropped, every binding compiled default.
void KiwiKeybind_ResetAll();
// Re-read everything from disk (the test probe's simulated restart).
void KiwiKeybind_Reload();
// Command id for a table name (first row), 0 if absent.
int  KiwiKeybind_FindCommand( const char *name );
const char *KiwiKeybind_PrimaryName( int commandId );

// Chord capture for the Command List: the pump offers every key message first
// (radiant_main.cpp Radiant_PreTranslateMessage).  Esc cancels; modifiers pass through.
void KiwiKeybind_BeginCapture();
void KiwiKeybind_CancelCapture();
bool KiwiKeybind_Capturing();
bool KiwiKeybind_TakeCaptured( kiwiChord_t *out );
