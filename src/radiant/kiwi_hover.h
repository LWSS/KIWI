#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_hover.h — RADIANT_UX_DESIGN §18 (the hover half) + Phase-1 item 8.
//
// One pick per frame while the cursor is over the camera image and no mouse
// button is down; the result is drawn as an ADDITIVE accent in the Cam_Draw tail.
//
// Deliberately additive: the ported passes already tint selected geometry (the
// tech-29 white outline, Cam_DrawSelectedFaceFill, Ed_DrawVertexHandles).  This
// pass NEVER re-renders selected brushes — it draws the hovered item's outline
// and, when it is finer than a whole object, the ACTIVE item's accent.  Cost is
// bounded to one item each.
//
// Colours (§18's language): hover = light cyan, distinct from the selection red /
// white outline; active = brighter warm accent.
// ─────────────────────────────────────────────────────────────────────────────

#include "kiwi_pick.h"

// ROUND AG, ITEM 3: named by the outliner-hover prototypes below.  Forward
// declarations rather than a qe3.h include, so this header stays cheap and stops
// depending on every includer having pulled qe3.h in first.
struct selbrush_t;
struct entity_s;

// Re-pick under the cursor (camera-image coords, TOP-LEFT origin).  Uses the
// current KiwiSel_GetModeMask() so hover granularity always matches what a click
// would select.  No-op when the hover toggle is off.
void KiwiHover_Update( int imgX, int imgY );

// Cursor left the camera image / a drag started — drop the highlight.
void KiwiHover_Clear();

// The last hover result (invalid when nothing is hovered).
const pick_result_t &KiwiHover_Get();

// Cam_Draw tail hook (// KIWI-UX in camwnd.cpp): hover outline + active accent.
void KiwiHover_DrawWorld();

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AG, ITEM 3 — THE OUTLINER'S HOVER IS A VIEWPORT HOVER
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "While mousing over the brushes in the outliner, it
// should highlight them in 3D so I can find them easier."
//
// PLASTICITY DOES EXACTLY THIS, and it is one line of its own outliner:
// plasticity/src/components/outliner/Outliner.tsx binds `onPointerEnter` /
// `onPointerLeave` on every row to `this.editor.selection.hovered.add(item)` /
// `.remove(item)` — i.e. the row hover writes into the SAME hover collection the
// viewport's raycast writes into, so the highlight the 3D view already draws for
// a mouse-over is what the list gets, free.  (The same file's `select` handler
// goes through `editor.selection.selected`, which is the reason hover and
// selection read as one language there and not two.)
//
// KIWI reproduces the SHAPE and not the plumbing: pick_result_t is a single
// result built by a raycast and it is not a set, so instead of pushing rows into
// it the outliner publishes a small, separate HOVER TARGET that
// KiwiHover_DrawWorld consumes in the same pass, with the same fill colour, right
// after the raycast hover it already draws.
//
// THE CONTRACT, and it is what keeps this from leaking stale pointers:
//   * KiwiHover_OutlinerClear() runs at the TOP of every outliner draw, so a
//     target survives exactly one frame unless the row re-publishes it;
//   * every brush is Sel_BrushLive-guarded at DRAW time, not at publish time —
//     a row can be hovered and the brush deleted by a hotkey in the same frame;
//   * the brush list is BOUNDED (KHOVER_OUT_MAX): an entity row publishes all its
//     members, and a 500-brush func_group would otherwise be 500 fill commands.
//     Past the cap the highlight is partial and says so once.
// If the outliner draws AFTER the camera in a frame the highlight lags by one
// frame; that is invisible at any frame rate and is why the target is a plain
// latch rather than anything synchronised.
void KiwiHover_OutlinerClear();
void KiwiHover_OutlinerBrush( selbrush_t *b );          // one row
void KiwiHover_OutlinerEntity( entity_s *e );           // a group / entity row
void KiwiHover_OutlinerCon( int conIndex );             // one construction object
void KiwiHover_OutlinerConGroup( int conGroup );        // a construction folder

// Read side, for kiwi_construct.cpp's line pass (a curve is drawn there, not
// here).  -1 / 0 mean "nothing".
int  KiwiHover_OutlinerConIndex();
int  KiwiHover_OutlinerConGroupId();
