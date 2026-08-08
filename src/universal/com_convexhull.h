#pragma once
#include <cstdint>

uint __cdecl Com_ConvexHull(float (*points)[2], uint pointCount, float (*hull)[2]);

#ifdef KISAK_RADIANT
// CoD4Radiant editor (common/polylib.cpp): like Com_ConvexHull but returns the hull
// as point INDICES (into hullOrder) instead of coords. Translates points in place.
uint __cdecl Com_ConvexHullIndices(float (*points)[2], uint pointCount, uint *hullOrder);
#endif

