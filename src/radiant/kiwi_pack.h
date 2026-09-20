#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Fast unit-vector packing for the EDITOR's vertex-buffer uploads.
//
// The shipped encoder (qcommon Vec3PackUnitVec) brute-forces all 256 scale bytes and
// normalises a decoded candidate (sqrt, double math) for each one - ~2 x 256 candidates
// per vertex, per VB run.  Fine for a one-off asset conversion; in the editor every patch
// or face re-upload pays it again, once per material run, and on 2026-09-17 a stack
// profile of a terrain dig on a painted map put 350 of 355 upload samples inside it.
//
// KiwiPack_UnitVec keeps the same wire format and the same error metric, but only tries
// the handful of scales that quantise finest without wrapping a byte, and memoises the
// result (terrain and brush faces repeat their vectors heavily).  Game-side packing (BSP
// / xmodel loaders) is untouched.

#include <qcommon/com_pack.h>

PackedUnitVec KiwiPack_UnitVec( const float *v );
