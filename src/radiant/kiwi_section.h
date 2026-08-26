#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Horizontal-Z section analysis. Command 34133 is instant and unbound.
// The cut uses an oblique projection near plane because the tested D3D9 user clip
// state was accepted but ignored under the programmable vertex path.
//
// World z <= level survives. Clicking sets level from the hit Z, and the lollipop
// slides it; the internal general-plane math cannot produce a nonhorizontal cut.
// State transitions are OFF -> PICKING -> ON, with Escape cancelling PICKING.
//
// Picking is gated on KiwiSection_Cutting(), the current frame's render verdict,
// so an armed but refused cut cannot clamp rays or hide pick candidates.
// Perspective ordering requires the eye above the cut; ortho ordering requires a
// downward view. Refused frames draw uncut and leave picking unclamped.

struct ray_t;          // kiwi_pick.h

// Command.
void KiwiSection_RegisterCommands();
bool KiwiSection_DispatchInstant( unsigned int cmdId );

// Shared by the view-cube button and command palette.
void KiwiSection_Toggle();

// State.
bool  KiwiSection_Active();     // ON: a level is set and the camera wants to cut
bool  KiwiSection_Picking();    // PICKING: waiting for the click that sets the level
float KiwiSection_Level();      // the Z level (meaningless while OFF)

// True only when the current frame installed the oblique projection.
bool KiwiSection_Cutting();

// Reset all view state; a section must not survive a document change.
void KiwiSection_Reset();

// Consumes Escape only while PICKING.
bool KiwiSection_HandleEscape();

// Input from kiwi_viewport.cpp.
// While PICKING, LMB sets the level and is consumed.
bool KiwiSection_ClickPick( int imgX, int imgY );

// HandleDown takes the gesture and rebases at the press pixel to prevent a jump.
bool KiwiSection_HandleDown( int imgX, int imgY );
void KiwiSection_HandleDrag( int imgX, int imgY );
void KiwiSection_HandleUp();
void KiwiSection_HandleAbort();          // lost-capture: restore the slide at the grab

// `over` false clears the ball's hover highlight.
void KiwiSection_Hover( int imgX, int imgY, bool over );

// Render.
// Compatibility no-ops preserve camwnd.cpp's matched call sites; they arm nothing.
void KiwiSection_EmitClipBegin();
void KiwiSection_EmitClipEnd();

// Writes projection depth column m[0..3][2]; false leaves the base projection.
// The caller must pass the result to NoteFrameCut so picking matches rendering.
// guardC is the far guard band; tanX/tanY/zNear are perspective inputs, while
// halfW/halfH/depthHalf are ortho world-space half-extents.
bool KiwiSection_ObliqueDepthColumn( const float origin[3], const float vpn[3],
                                     const float vright[3], const float vup[3],
                                     bool ortho, float guardC,
                                     float tanX, float tanY, float zNear,
                                     float halfW, float halfH, float depthHalf,
                                     float outCol[4] );

// CamWnd_SetupScene records whether the fold went in.
void KiwiSection_NoteFrameCut( bool cutting );

// Plane outline and lollipop for the camera overlay tail.
void KiwiSection_DrawWorld();

// Picking from kiwi_pick.cpp. The area pass clamps rays to the section plane; the
// screen-space pass rejects a hidden vertex/edge winner. Both require Cutting().
void KiwiSection_ClampRayStart( float *start, const float *dir );
bool KiwiSection_PointVisible( const float *p );
