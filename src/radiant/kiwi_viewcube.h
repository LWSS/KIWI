#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Top-right camera overlay: a projected cube with six face and eight corner snap
// targets. Face and corner snaps preserve the orbit pivot and reference distance.
// Edge targets are deliberately omitted because a third hit region is too crowded
// on the 104-pixel widget.
//
// Controls drawn in the camera window use ImDrawList and resolve their own hover
// and clicks; they must not extend that window's content after its image. The grid
// InputText lives in a separate top-level window so its cursor cannot violate that
// invariant. Returned hover claims prevent clicks reaching the image underneath.
bool KiwiViewCube_Draw( float imgMinX, float imgMinY, float imgW, float imgH );

// Persisted cube-only switch, default on; the adjacent controls remain visible.
bool KiwiViewCube_Show();
void KiwiViewCube_SetShow( bool on );

// Detects a face view within cos(3 degrees). outAxis is its world-axis normal
// (0=X, 1=Y, 2=Z); outSign is the look direction. Either output may be null.
bool KiwiViewCube_ViewAxis( int *outAxis, float *outSign );
