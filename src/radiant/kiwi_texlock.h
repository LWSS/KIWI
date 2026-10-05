#pragma once
// KIWI (2026-10-05, precision): exact texture lock for whole-face transforms (brush.cpp).
// Translation: Face_TexLockTranslate (Brush_Move).  Rotate / flip: Begin before the planepts
// move (rows turned in double from the pre-move texdef and normal), End after (texturevecs_02
// against the moved face) - Select_ApplyMatrix (0x47CDE0) and KIWI R.  lockFlags are the
// binary's 3 bytes: texture lock, lightmap lock, 1.

struct face_t;
typedef unsigned char byte;

#define KTEXLOCK_LAYERS 4       // layers per channel kept (MaterialDef_04 is 0 or 1 in practice)

void Face_TexLockTranslate( face_t *f, const float *d, const byte *lockFlags );
void Face_TexLockAffineBegin( face_t *f, const float *normal, const double ( *turn )[3],
                              const double *origin, const byte *lockFlags, double ( *rows )[KTEXLOCK_LAYERS][8] );
void Face_TexLockAffineEnd( face_t *f, double ( *rows )[KTEXLOCK_LAYERS][8], const byte *lockFlags );

// texturevecs.cpp: texturevecs_02 in double, snapping only float noise onto its grids.
void texturevecs_02_exact( float *outSize, const double *texMat, const float *planeNormal, double planeDist,
                           float *outShift, float *outRotate, float *outCrossterm );
