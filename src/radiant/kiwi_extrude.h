#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Region and face extrusion produce ordinary brush data from plane-space profiles.
// Analytic construction circles choose brush sides or curved-patch density during
// extrusion (P toggles); their stored centre/radius are never polygonized in place.
// Creation mirrors Ed_NewBrushDrag (xywnd.cpp 0x467fa0): allocate, size the face
// array, write planepts, rebuild, link, and select.
//
// Prisms require n + 2 faces; Ed_BrushSetFaceCount exposes the in-place array swap
// used by Brush_MakeSided (brush.cpp 0x4731E0). Rebuild uses bFull 0 so the legacy
// power-of-two grid cannot quantize a profile the user did not draw.
//
// Convex profiles produce one brush; concave region profiles are decomposed into
// convex pieces. Face_MakePlane and map serialization consume planepts, so every
// face uses a well-spread triple wound outward.
//
// Every def is rebuilt and validated while unlinked, so geometry rejection lands
// no brush and opens no undo bracket. Commit ordering is
// Select_Deselect -> KiwiCmd_UndoBegin -> create; Undo_EndBrushList
// (undo.cpp 0x45E870) then stamps only the newly selected brushes.

class  KiwiEditorCommand;
struct kconPlane_t;               // kiwi_construct.h
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)

#define KEXT_MIN_DIST      0.5f   // world units; below this the gesture is a no-op

// Source geometry lies at depth zero and would otherwise pin the drag there.
// Refuse geometry snaps within this world-unit band around the source plane.
#define KEXT_SELF_SNAP_BAND 2.0f  // world units either side of the start plane

// Ctrl maps one-axis depth absolutely from the source plane; releasing it rebases
// relative mapping at the current depth.
bool KiwiExt_AbsoluteHeld();

// Absolute snapping is a ladder: aimed named geometry, face-plane magnet, then
// hard lattice. rawAbs and snap depths are world units along axis from ref;
// outMajor may be null.
struct snap_result_t;              // kiwi_snap.h:323
float KiwiExt_LadderDepth( const snap_result_t &snap, const float *ref,
                           const float *axis, float rawAbs, bool *outMajor );

#include "kiwi_region.h"

// The profile cap must track the region cap so every filled region remains extrudable.
// Hertel-Mehlhorn starts with n - 2 triangles, so 128 also covers the worst case.
#define KEXT_MAX_PROFILE   KREG_MAX_LOOP   // profile vertex cap (== the region cap)
#define KEXT_MAX_PIECES    128    // convex pieces one concave region may produce

void KiwiExtrude_RegisterCommands();
KiwiEditorCommand *KiwiExtrude_CommandForId( int commandId );
bool KiwiExtrude_CanExecute();          // at least one region is available

// Face extrusion grows a separate prism and leaves the source brush untouched.
// Negative face extrusion delegates to KiwiXform_PushFaceOnce for carve/destroy;
// region extrusion never auto-carves overlapping solids.
// Pick priority is active selected face, first selected face, then hovered face.
bool KiwiExtrudeFace_Pick( selbrush_t **outNode, int *outFace );
bool KiwiExtrudeFace_CanExecute();      // a face is available

// Shared convex-prism writer. loopUV contains n CCW plane-space points; lo/hi are
// world-unit offsets along plane.normal with hi > lo. The result is unlinked and
// unbuilt so callers can rebuild, validate, then free or land it without rollback.
brush_t *KiwiExtrude_BuildPrismDef( const kconPlane_t &plane, const float *loopUV, int n,
                                    float lo, float hi, const char **why );

// Lands a validated def in Ed_NewBrushDrag tail order (xywnd.cpp 0x467fa0):
// Entity_LinkBrush -> Brush_AddToList -> Brush_AddToList2 (selected).
selbrush_t *KiwiExtrude_LandDef( brush_t *def );
