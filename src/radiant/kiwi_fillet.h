#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI_CMD_FILLET_CURVE (34061) is the modal construction-chain fillet.
// Mirrors ContourFilletFactory.ts:44-72 / ModifyContourGizmo.ts:14: closed chains
// use every vertex, open chains use interiors, point selection narrows the set,
// and one radius serves all chosen corners. Modern B is deliberately KIWI-specific.
//
// Radius r consumes t = r / tan(θ/2) along both adjacent edges. Clamping to half
// the shorter edge prevents neighboring fillets from overlapping. Oversized radii
// saturate; near-straight, reversing, or sub-minimum corners remain sharp.
//
// Fillet replaces its source; offset deliberately keeps its source. Generated arcs
// form one tessellated KCON_POLYLINE because the store has no mixed line/arc contour.
// Non-planar, cornerless, fully skipped, and over-budget results are refused.
//
// The gesture only previews. Commit pushes one construction snapshot immediately
// before RemoveAt + Add, so cancellation is mutation-free and Ctrl+Z restores it.

class KiwiEditorCommand;

#define KFIL_MIN_RADIUS      0.5f      // below this a corner is left sharp
// Per-corner tessellation bounds; FilletChain2D separately enforces KCON_MAX_POINTS.
#define KFIL_SEGS_MIN        2
#define KFIL_SEGS_MAX        16
// Reject nearly straight continuations and reversals outside this cosine band.
#define KFIL_FLAT_DOT        0.9995f

// Palette predicate for a selected planar construction chain with a corner.
bool KiwiFillet_CanFillet();

void KiwiFillet_RegisterCommands();
KiwiEditorCommand *KiwiFillet_CommandForId( int commandId );
