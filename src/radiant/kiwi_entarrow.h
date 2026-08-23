#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

struct entity_s;
typedef entity_s entity_s_def;

// KIWI: Resolve target/angles/angle facing and its display geometry.
bool KiwiEntArrow_GetFacing( const entity_s_def *ent, float *outOrigin,
                             float *outDir, float *outLen );

// KIWI: Camera-tail and orthographic-view overlay entry points.
void KiwiEntArrow_DrawWorld();
void KiwiEntArrow_DrawXY( int viewType, float scale );
