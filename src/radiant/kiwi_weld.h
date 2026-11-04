#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Keep seams welded - KIWI (2026-11-01, user: "make it so the terrain chunks can never form
// seams in the first place").
//
// A SEAM is where the borders of two terrain patches meet in XY:
//   * coincident border points (within 1 unit in XY, heights within 32 units) are one point
//     of the terrain - welded to their mean;
//   * a border point part-way along another patch's border segment (a fine edge beside a
//     coarse one, a T-junction) belongs ON that segment.  Edge densities that do not nest
//     (48- beside 64-unit cells) are solved together until both edges are one line.
// Heights further apart are a wall or a second layer, never a seam.  Bezier patches and
// hidden / filtered patches are never welded.
//
// The terrain ops (strokes, Split, Tessellate - kiwi_terrain.cpp) bracket themselves with
// these.  GuardBegin notes the seams that are ALREADY open (a crack that was there before
// stays as it was: a stroke never moves a neighbour outside its ring for it); GuardEnd closes
// every other seam of the `touched` patches inside the op's open undo record.  `mark` saves a
// patch into that record before its first change (nullptr = Undo_AddBrush, which skips
// patches created in the record).  Returns the points moved; 0 when the setting is off.

struct patchMesh_t;

void KiwiWeld_GuardBegin();
int  KiwiWeld_GuardEnd( patchMesh_t *const *touched, int count, void ( *mark )( patchMesh_t * ) );

// Terrain Sculpt panel row (kiwi_terrain.cpp "Chunks"): the "Keep seams welded" toggle.
void KiwiWeld_DrawPanel();

// -kiwitest: the widest open seam over every visible terrain patch (0 = watertight).
float KiwiWeld_TestMaxGap();
