#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Presentation/input handle for active one-axis modal commands; it never edits geometry.
// While wanted, the translate gizmo stands down from both drawing and hit-testing.
//
// The command supplies a live world anchor and signed unit direction each frame,
// so the ring rides a moving face and the stem stays on the travel side.
// Grabbing rebases at the press pixel before movement to prevent a jump.
//
// Screen sizes: 14 px face-plane ring, 42 px stem, and 5 px ball.  The invisible
// 12 px pick radius remains larger for usability.

// Face offsets follow travel sign once push is nonzero; at rest the camera chooses
// the visible normal side.  This changes presentation only, not cursor mapping.
// Direction-semantic handles such as bevel bisectors must not call this helper.
float KiwiLollipop_FaceSide( const float anchor[3], const float normal[3], float push );

// Returns the active command's live world anchor and signed direction, normalized here.
// Draw, hit-test, and the translate-gizmo stand-down all share this predicate.
bool KiwiLollipop_Wanted( float outAnchor[3], float outDir[3] );

// Predicate-only form used by the gizmo gate.
bool KiwiLollipop_Active();

// Coordinates are top-left camera-image pixels; `over` false clears hover.
void KiwiLollipop_Hover( int imgX, int imgY, bool over );

// True takes the ball and transfers gesture ownership through release; the command
// is resumed and rebased at this top-left camera-image pixel before movement.
bool KiwiLollipop_MouseDown( int imgX, int imgY );

void KiwiLollipop_Release();      // release edge: ungrab, then PAUSE the gesture
void KiwiLollipop_Abort();        // lost-capture edge: ungrab, then CANCEL it

// Cam_Draw tail hook (called from the same block as KiwiGizmo_DrawWorld).
void KiwiLollipop_DrawWorld();
