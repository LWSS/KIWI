#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Sun keys live on worldspawn, so this helper is a virtual selection with no
// entity, brush, origin, or selection_t entry.
//
// Both compilers pass `sundirection` (pitch yaw roll) through AngleVectors
// (cod4rad/mapio.c:511-512; cod4map/tris_sunshadow.cpp:311 and
// primarylights.cpp:1448). Its forward vector points from the scene toward the
// sun (primarylights.cpp:493,674); light and the projected frustum travel along
// -sunDir, while the glyph sits at targetCentre + sunDir * radius.
//
// With roll ignored, forward = (cos p cos y, cos p sin y, -sin p), so for unit d:
// p = -asin(d.z), y = atan2(d.y, d.x). Placement and dragging share this round trip.
// Pitch stays in [-89, -1] degrees to keep d.z positive. A refused pitch delta
// rewinds the absolute-from-latch accumulator so the handle cannot stick at the
// limit (the KiwiCam_OrbitDrag invariant, kiwi_camera.cpp:781-799).
//
// `sundirection` is baked during BSP/light compilation
// (tris_sunshadow.cpp:310-311; primarylights.cpp:1447-1448), so commits warn that
// editor preview changes do not update baked lighting.
//
// The Sun window shares the Textures/Entities/Sky/UV dock node. New default
// windows require one KIWI_LAYOUT_VERSION bump because it versions both the dock
// ini and the persisted default-open state.
void KiwiSun_Draw();

// Commands and palette dispatch.
void KiwiSun_RegisterCommands();
bool KiwiSun_DispatchInstant( unsigned int cmdId );

// Write only absent worldspawn sun keys and select the helper. Existing values
// are preserved; all writes share one undo record, with none for a no-op.
void KiwiSun_Place();

// Remove all worldspawn sun keys (one undo record) so a fresh sun can be placed.
void KiwiSun_Delete();

// A worldspawn is required as the key owner.
bool KiwiSun_CanPlace();

// State.
// True only for a parseable worldspawn `sundirection`.
bool KiwiSun_Exists();

bool KiwiSun_Selected();
void KiwiSun_Select();              // the click grammar's "take it" (kiwi_boxselect.cpp)
void KiwiSun_ClearSelection();

// Live degrees: the drag pair while grabbed, otherwise the committed key.
bool KiwiSun_Angles( float *outPitch, float *outYaw );

// Drop document-derived state on the new-map path.
void KiwiSun_ResetForNewMap();

// Input from kiwi_viewport.cpp / kiwi_boxselect.cpp. Pixel coordinates are
// camera-RTT-image relative with a top-left origin (kiwi_pick.h).

// Tested before brushes because the glyph is a screen-space handle.
bool KiwiSun_GlyphHit( int imgX, int imgY );

// True transfers the LMB gesture to the caller. Already-selected gating prevents
// the selection click from also orbiting the sun.
bool KiwiSun_HandleDown( int imgX, int imgY );
void KiwiSun_HandleDrag( int imgX, int imgY );
void KiwiSun_HandleUp();              // commit: one undo record for the whole drag
// Lost capture: no drag writes occur, so dropping the grab restores key-backed angles.
void KiwiSun_HandleAbort();

// `over == false` clears the hover highlight.
void KiwiSun_Hover( int imgX, int imgY, bool over );

// Viewport gesture predicate.
bool KiwiSun_Grabbed();

// Camera-only overlay: kiwi_lines has no XY/Z emitter, and RADIANT_UX_DESIGN section 18
// forbids a parallel renderer. The frustum appears only while selected/grabbed.
void KiwiSun_DrawWorld();

// Hard line budget (kiwi_lines.h TRAP 1): glyph 20+8+1+4 = 33, frustum
// 8+4+9 = 21, worst case 54 below KSUN_MAX_SEGMENTS (64).
