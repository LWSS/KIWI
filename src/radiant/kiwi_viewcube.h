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

// Alt+MMB click snaps to the nearest face view, or advances an aligned view through
// front -> right -> back -> left -> top -> bottom. Uses the cube's snap path.
void KiwiViewCube_StepAxisView();

// Detects a face view within cos(3 degrees). outAxis is its world-axis normal
// (0=X, 1=Y, 2=Z); outSign is the look direction. Either output may be null.
bool KiwiViewCube_ViewAxis( int *outAxis, float *outSign );

// Returns the aligned view's name, or null when the camera is off-axis.
const char *KiwiViewCube_ViewAxisName();

// Alt+MMB swipe quantizes the nearest face view by 90 degrees. Horizontal swipes
// walk the side ring; vertical swipes wrap a great circle through both poles.
// Phase is stored because this pitch/yaw camera has no roll, then validated against
// the live camera on every swipe. A horizontal pole swipe steps yaw in place.
// dir: 0=left, 1=right, 2=up, 3=down.
void KiwiViewCube_SwipeAxisView( int dir );
