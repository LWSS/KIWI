#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_uv.h — RADIANT_UX_DESIGN §26 (Textures/UV), Phase 6 v1.
//
// §26 is deliberately SMALL: "preserve per-face material + texdef through every
// new op … TrenchBroom-style UV manipulation is Phase 6, after modeling ships."
// What ships here is the FIRST half of that — geometry-safe texture manipulation
// FROM THE VIEWPORT, wrapped around the ported texture cores — plus a read-only
// texdef readout.  The drag-handles-on-the-texture-plane editor is NOT attempted
// (see the "WHAT v1 IS NOT" note below and RADIANT_KNOWN_ISSUES).
//
// Three modal commands + one instant command:
//     KIWI_CMD_TEX_SHIFT     34044   MODAL   Brush_ShiftTexture   (select.cpp 0x491F20)
//     KIWI_CMD_TEX_ROTATE    34045   MODAL   Brush_RotateTexture  (select.cpp 0x4929F0)
//     KIWI_CMD_TEX_SCALE     34046   MODAL   Brush_ScaleTexture   (select.cpp 0x492650)
//     KIWI_CMD_PICK_TEXTURE  34006   INSTANT Texture_SetTexture   (texwnd.cpp 0x45BE50)
//
// ── ID NOTE (a deliberate deviation from the brief's "34047") ────────────────
// kiwi_command.h reserves 34030..34069 as the MODAL band and KiwiCmd_Dispatch
// tests `KiwiCmd_IsModalId` FIRST (kiwi_command.cpp:381) — an INSTANT command at
// 34047 would be routed to KiwiCmd_Start, find no command object and silently do
// nothing.  Pick Texture therefore takes 34006, the next free INSTANT id the
// header itself names.  The three modal ids are the requested 34044..34046.
//
// ── WHAT THE CORES ACTUALLY OPERATE ON (read, not assumed) ──────────────────
// All three cores run TWO passes and need NO selection argument:
//   pass 1  every brush on `selected_brushes` — EVERY FACE of it (and, for a
//           patch brush, Patch_{Shift,Scale,Rotate}Texture on the patch def);
//   pass 2  every entry in `g_SelectedFaces` — that one face only.
// So "whole brushes get all faces, face selections get the picked faces", and a
// selection holding both does both.  Each core early-returns when both are empty,
// which is exactly what KiwiUv_CanEdit() tests.
//
// Units, granularity and side effects, per core:
//   Brush_ShiftTexture( float ds, float dt )        select.cpp:3058
//       texdef shift[0] += ds, shift[1] += dt, then TexMatToFakeTexCoords.
//       BRUSH pass TRUNCATES: `(float)(int)ds` — a sub-unit delta is DROPPED.
//       FACE pass uses the raw float.  PATCH brushes scale by 0.001 (a1*0.001f).
//       Calls Brush_BuildWindings + MarkMapModified + ++def->version;
//       g_nUpdateBits |= 1.  IT OPENS NO UNDO BRACKET — the caller owns undo.
//   Brush_ScaleTexture( int ds, int dt )            select.cpp:3231
//       texdef size[0] += (float)ds, size[1] += (float)dt.  ADDITIVE ON THE
//       TEXTURE SIZE — it is NOT a multiplicative factor (the classic texture
//       bar's HScale/VScale spins step it by ±d_savedinfo.d_gridsize,
//       texturebar.cpp:220).  OPENS ITS OWN BRACKET ("scale texture").
//   Brush_RotateTexture( int deg )                  select.cpp:3313
//       texdef rotate = (int)(rotate + deg + 2^-30) % 360 — WHOLE DEGREES only,
//       wrapped.  OPENS ITS OWN BRACKET ("rotate texture").
// None of the three reads `g_PrefsDlg->m_bTextureLock` (select.cpp has no
// reference to it at all): texture lock governs texdef preservation across
// GEOMETRY moves, and these commands ARE texdef edits, so the pref cannot fight
// them in either direction.
//
// ── THE UNDO RULING (the one thing that shapes this whole file) ─────────────
// `Undo_GeneralStart` ALWAYS allocates a fresh record (undo.cpp:347) and
// `g_undoMaxSize` is 64 (undo.cpp:82), so a self-bracketing core driven once per
// frame would (a) push 64 junk records and EVICT THE USER'S REAL HISTORY inside
// two seconds of dragging, and (b) leave `g_lastundo` pointing at the core's
// record while our own bracket was open, so KiwiCmd_UndoCommit's
// Undo_EndBrushList/Undo_End would close the WRONG one.  Therefore:
//
//   Texture Shift   the core brackets nothing → LIVE.  One KiwiCmd_UndoBegin
//                   bracket per gesture, opened at the first real mutation, and
//                   per-frame residual application (target − applied).
//   Texture Rotate  } the core brackets itself → DEFERRED.  The gesture mutates
//   Texture Scale   } NOTHING until Commit, which makes exactly one core call and
//                   inherits exactly one undo record (the core's own).  The HUD
//                   carries the live value; the geometry does not preview.
//
// That deferral is a real, honest limitation and is listed in
// RADIANT_KNOWN_ISSUES.  It also makes those two commands' Cancel PERFECT: with
// nothing applied there is nothing to restore.
//
// ── CANCEL SEMANTICS, PER COMMAND ───────────────────────────────────────────
//   Shift   Cancel applies the inverse of the applied total AND the framework's
//           KiwiCmd_UndoCancel rolls the bracket back (Undo_Undo restores the
//           cloned brush defs, texdefs included — Undo_AddBrush clones the whole
//           def, undo.cpp:474).  The bracket is the authority; the inverse call
//           is the belt to its braces, exactly as kiwi_transform's R does.
//           PRECISION CAVEAT: the inverse leg is pure-delta float arithmetic, so
//           on its own it is not bit-exact (a+n−n need not be a).  The bracket
//           restore that follows IS exact, so the caveat only bites for a brush
//           the bracket failed to cover.
//   Rotate  nothing was applied → exact by construction.
//   Scale   nothing was applied → exact by construction.
//
// ── UNDO COVERAGE FOR FACE SELECTIONS ───────────────────────────────────────
// Face-selected brushes are NOT on `selected_brushes` (kiwi_selection.h DESIGN
// NOTE 2), so KiwiCmd_UndoBegin's Undo_AddBrushList misses them.  Shift adds each
// one with Undo_AddBrush (entity first for a fixed-size owner) BEFORE the first
// mutation — the same UndoCoverBrush body kiwi_transform.cpp:254 uses.
//
// ── CURSOR MAPPING (S = X = horizontal, T = Y = vertical) ───────────────────
//   Shift    1 px = 1 texture unit, quantised to the CLASSIC grid step
//            (grid_sizes[d_gridsize], the same quantum CamWnd_MouseMoved's
//            RMB+Alt texture drag uses, camwnd.cpp:2750) — or to 1 unit while
//            CTRL suppresses snapping.  Right/down are positive, matching the
//            classic path's screen-space accumulation sign.
//   Rotate   0.5°/px horizontal, the same rate as kiwi_transform's R
//            (KX_DEG_PER_PIXEL), snapped to 5° while snapping is live.
//   Scale    1 size step per 8 px; right = +S, down = +T.
// X constrains to S only, Y constrains to T only, a second press releases (the
// v1 rule kiwi_transform.h documents for its own axis locks).
//
// ── NUMERIC ENTRY ───────────────────────────────────────────────────────────
// The numeric layer hands commands RAW WORLD UNITS; texture units, degrees and
// size steps are NOT lengths, so each command runs the value back through
// Units_ToDisplay to recover exactly what was typed (kiwi_transform.h says the
// same about R and S).  One scalar, so it targets the S axis unless T is locked;
// the HUD says which.  A typed value is applied as an EXACT CORRECTION:
//   Shift   the final call carries (typedTotal − appliedTotal);
//   Rotate  the single commit call carries the typed degrees, rounded to int;
//   Scale   the single commit call carries the typed step count, rounded to int.
//
// ── WHAT v1 IS NOT ──────────────────────────────────────────────────────────
// No drag handles on the texture plane, no UV-space viewport, no per-vertex UV
// editing, no fit/centre/best-axis operators, no texture lock toggles.  The
// Surface Inspector remains THE numeric editor; the readout added here is
// strictly read-only.  §26's forward direction (TrenchBroom-style plane drag) is
// documented and deliberately not started.
// ─────────────────────────────────────────────────────────────────────────────

// ── KEYS: none taken this phase ─────────────────────────────────────────────
// All four rows register UNBOUND, and the modern profile (kiwi_keymap.cpp) adds
// nothing — the "Textures (UV v1)" panel block and the command palette ARE the
// route in both profiles, plus the classic middle-button pick over the 3D view.
// CANDIDATES, verified free in BOTH the classic default table (mainfrm.cpp:981-
// 1169) and the modern profile (kiwi_keymap.cpp:62-99), logged for whoever wants
// them:  Texture Shift Alt+T (0x54/2) · Texture Rotate Alt+R (0x52/2) ·
//        Texture Scale Alt+E (0x45/2) · Pick Texture Alt+P (0x50/2).
// (Shift+T / Shift+R / Shift+E are NOT free — they are ToggleTexMoveLock,
// ToggleTexRotateLock and RedisperseRows respectively.)

class KiwiEditorCommand;

// ── tuning (all in one place, like kiwi_transform.cpp's block) ──────────────
#define KUV_DEG_PER_PIXEL     0.5f   // Rotate drag rate (== KX_DEG_PER_PIXEL)
#define KUV_ANGLE_STEP        5.0f   // Rotate snap increment, whole degrees
#define KUV_PX_PER_SIZE_STEP  8.0f   // Scale: cursor pixels per integer size step
#define KUV_MAX_SIZE_STEP     512    // sanity clamp on one scale gesture
#define KUV_MAX_SHIFT         16384.0f  // sanity clamp on one shift gesture

// Registers the four rows into the shared `g_radiantCommands` table.  Called by
// KiwiCmd_RegisterCommands (kiwi_command.cpp) so there is one registration point.
void KiwiUv_RegisterCommands();

// The command object for a KIWI_CMD_TEX_* id, or NULL.  Called by
// kiwi_command.cpp's CommandForId.
KiwiEditorCommand *KiwiUv_CommandForId( int commandId );

// The instant half (Pick Texture).  Called from KiwiCmd_Dispatch's default arm.
bool KiwiUv_DispatchInstant( unsigned int commandId );

// §3 canExecute predicate shared by the three modal rows: the exact condition all
// three cores test before doing anything — at least one whole brush selected or
// at least one face in `g_SelectedFaces`.
bool KiwiUv_CanEdit();

// Pick Texture is available whenever the camera has a cursor to pick under.
bool KiwiUv_CanPick();

// The read-only texdef readout for the ACTIVE face, drawn from
// KiwiVP_DrawCameraOverlay's block.  Pure ImDrawList output — it can never take
// the camera image's hover away from a marquee.  No-op unless KiwiSel().active is
// a live SEL_FACE item.
void KiwiUv_DrawReadout( float imgMinX, float imgMinY, float imgW, float imgH );

// The "Textures" block in the shell's panel window.
void KiwiUv_MenuItems();

// ── KIWI-UX (ROUND BD): THE PER-FACE TEXDEF ACCESSOR, EXPORTED ───────────────
// Round BD's UV editor window (kiwi_uveditor.h) reads and writes exactly the slot
// this file's readout reads, and a second spelling of the two-level layer walk —
// `mtldef[current_edit_layer]` selects the CHANNEL, then
// `+ LayerMat::GetCurrentLayer(md)` selects the SUB-LAYER, deliberately walking
// into the following MaterialDefs of the mtldef[4] block — is exactly the kind of
// duplicate the cleanup pass spent a wave removing.  So it moved out of this
// file's anonymous namespace to file scope; see the definition for the layout
// note.  Returns NULL when the face has no valid MaterialDef on the current
// layer (it TESTS the MtlDef_IsValid invariant rather than tripping its L0
// assert).  `outMtl` (optional) receives the owning MaterialDef, which is what
// TexMatToFakeTexCoords and Materialdef_GetName both want.
struct brush_t;
struct MaterialDef;
struct texdef_sub_t;
texdef_sub_t *KiwiUv_FaceTexdef( brush_t *def, int faceIndex, MaterialDef **outMtl );
// ── KIWI-UX end ─────────────────────────────────────────────────────────────

// ── KIWI-UX (ROUND BH, ITEM 3): END A LIVE GESTURE BEFORE WRITING A MATERIAL ─
// USER REPORT, verbatim: *"Make it so texture applications dont require a
// right-click/enter to confirm, they are just an operation that goes through
// when you click the new texture (same with UVs).  Leave them undoable."*
//
// The apply was never gated — it was being REVERTED by the face gesture that a
// face click auto-enters, whose baseline snapshot covers the whole MaterialDef
// block and is written back by both Cancel and every push frame.  The full
// mechanism chain (with the file:line for every link) is on the definition in
// kiwi_uv.cpp; the short form is: KiwiMoveCommand::BeginFaces memcpys
// `u.baseMtl` from the face at kiwi_transform.cpp:1663, and RestoreAll (:1573)
// and ApplyFaces (:2638) memcpy it back.
//
// CALL THIS BEFORE ANY MATERIAL / TEXDEF WRITE THAT COMES FROM OUTSIDE THE
// MODAL FRAMEWORK — never after (Cancel would undo the write it was meant to
// protect).  Returns FALSE when a live gesture refused to yield, in which case
// the caller must NOT apply; a line naming the command has already been
// printed.  `what` names the caller for that line ("Texture", "UV editor").
// Callers today: TexWnd_ApplyMaterialAtIndex (texwnd.cpp — the browser click,
// the Sky tab's apply and the End-key caulk all funnel through it) and the UV
// editor's UndoOpen (kiwi_uveditor.cpp).
bool KiwiUv_EndGestureBeforeApply( const char *what );

// The other half, for callers that want the FACE push/pull gizmo back afterwards.
// Re-enters KIWI_CMD_MOVE paused under exactly kiwi_boxselect.cpp's own auto-enter
// gate, and ONLY when the call above actually ended an auto-entered face push — so
// it can never start a gesture the user did not have.  The restart also takes a
// FRESH BeginFaces baseline, which is what makes the material that was just applied
// survive the next Cancel.  Call it at the END of the apply, after every mutation.
// A caller that does not want it simply never calls it (the UV editor does not: its
// gestures are its own, and it would be restarting a command over a window that is
// still being dragged in).
void KiwiUv_RestoreGestureAfterApply();
