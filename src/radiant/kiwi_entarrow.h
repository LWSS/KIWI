#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

struct entity_s;
typedef entity_s entity_s_def;

bool KiwiEntArrow_GetFacing( const entity_s_def *ent, float *outOrigin,
                             float *outDir, float *outLen );

void KiwiEntArrow_DrawWorld();
void KiwiEntArrow_DrawXY( int viewType, float scale );
