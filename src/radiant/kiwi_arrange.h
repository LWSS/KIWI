#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Planar arrangement for regions formed by mid-span crossings and T-junctions.
// Segments are fragmented at crossings, then clockwise-next half-edge walks emit
// bounded faces CCW and discard unbounded faces by signed area.
// Fragments are a throwaway view; construction objects and undo state remain untouched.

#include <vector>

#include "kiwi_construct.h"

// Bound quadratic crossing tests and node welding within one coplanar group.  KIWI
// (2026-09-27): raised for window grids on a building side - 64 cells was 16 columns of
// four rows (segments 256 -> 512, edges / nodes 1024 -> 2048, cells 64 -> 512).
#define KARRG_MAX_SEGS    512
#define KARRG_MAX_EDGES   2048
#define KARRG_MAX_NODES   2048
#define KARRG_MAX_CELLS   512

// Bounded face as a CCW plane-space loop; the first point is not repeated.
struct karrCell_t
{
    std::vector<float> pts;
};

// `objects` may mix open and closed construction objects. The caller must ensure
// they lie on `plane`; projection silently flattens any out-of-plane points.
// False means invalid input or logged segment/fragment/node overflow. True may be
// empty; cell-cap overflow logs and returns the cells already collected.
bool KiwiArrange_Cells( const int *objects, int count, const kconPlane_t &plane,
                        std::vector<karrCell_t> *outCells );
