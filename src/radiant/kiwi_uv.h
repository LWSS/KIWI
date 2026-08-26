#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Viewport UV v1 wraps the ported texture cores and adds a read-only texdef overlay.
//   KIWI_CMD_TEX_SHIFT     34044   MODAL   Brush_ShiftTexture   // 0x491F20
//   KIWI_CMD_TEX_ROTATE    34045   MODAL   Brush_RotateTexture  // 0x4929F0
//   KIWI_CMD_TEX_SCALE     34046   MODAL   Brush_ScaleTexture   // 0x492650
//   KIWI_CMD_PICK_TEXTURE  34006   INSTANT Texture_SetTexture   // 0x45BE50
// Pick uses 34006 because 34047 is in the modal band and would be routed to
// KiwiCmd_Start instead of instant dispatch.
//
// All three cores process every face of `selected_brushes`, then each entry in
// `g_SelectedFaces`; canExecute must accept either selection set.
// Shift uses texture-unit deltas: the brush pass truncates to int, the face pass
// keeps float, patches scale by 0.001, and the core opens no undo bracket.
// Scale uses additive integer texdef-size steps, not factors; its core owns undo.
// Rotate uses integer degrees modulo 360; its core owns undo.
// Texture lock does not apply because these are direct texdef edits, not geometry moves.
//
// Shift runs live in one caller-owned bracket using residual target-minus-applied
// deltas. Rotate and scale defer to one commit call because per-frame calls would
// create self-owned undo records, evict history, and interfere with the live bracket.
// Face-only brushes are absent from `selected_brushes`, so shift adds them to undo
// before its first mutation. Shift cancel's inverse is not bit-exact by itself;
// framework rollback is authoritative. Rotate and scale apply nothing before commit.
//
// Input space maps screen X to texture S and screen Y to texture T.
// Shift: 1 px = 1 texture unit, right/down positive; classic grid when snapped,
// one unit otherwise. Rotate: 0.5°/horizontal px, snapped to 5°.
// Scale: one additive size step per 8 px, right = +S and down = +T.
// X/Y constrain S/T respectively; pressing the same constraint again releases it.
// Numeric input arrives in world units, so non-length values pass through
// Units_ToDisplay; one scalar targets S unless T is constrained.
//
// UV-plane handles, per-vertex editing, fit/centre operators, and texture-lock
// controls are out of scope; Surface Inspector remains the numeric editor.
// Commands register unbound; the panel/palette and classic MMB pick remain available.

class KiwiEditorCommand;

// Gesture tuning.
#define KUV_DEG_PER_PIXEL     0.5f   // Rotate drag rate (== KX_DEG_PER_PIXEL)
#define KUV_ANGLE_STEP        5.0f   // Rotate snap increment, whole degrees
#define KUV_PX_PER_SIZE_STEP  8.0f   // Scale: cursor pixels per integer size step
#define KUV_MAX_SIZE_STEP     512    // sanity clamp on one scale gesture
#define KUV_MAX_SHIFT         16384.0f  // sanity clamp on one shift gesture

// Registers the four shared command-table rows.
void KiwiUv_RegisterCommands();

// Returns the modal command for a KIWI_CMD_TEX_* id, or NULL.
KiwiEditorCommand *KiwiUv_CommandForId( int commandId );

// Dispatches instant Pick Texture.
bool KiwiUv_DispatchInstant( unsigned int commandId );

// True when either core selection set is non-empty.
bool KiwiUv_CanEdit();

// Pick Texture is available whenever the camera has a cursor to pick under.
bool KiwiUv_CanPick();

// Draws a non-interactive texdef overlay for the active live face.
void KiwiUv_DrawReadout( float imgMinX, float imgMinY, float imgW, float imgH );

// The "Textures" block in the shell's panel window.
void KiwiUv_MenuItems();

// Returns the active sub-layer texdef, intentionally walking contiguous mtldef[]
// storage like the ported cores. Invalid materials return NULL; outMtl is optional.
struct brush_t;
struct MaterialDef;
struct texdef_sub_t;
texdef_sub_t *KiwiUv_FaceTexdef( brush_t *def, int faceIndex, MaterialDef **outMtl );

// Call before any external material/texdef write: a live face-push baseline would
// otherwise restore over it. False means the caller must not apply.
bool KiwiUv_EndGestureBeforeApply( const char *what );

// Optionally restores an auto-entered face push after the write; restart takes a
// fresh baseline containing the applied material. Call after all mutations.
void KiwiUv_RestoreGestureAfterApply();
