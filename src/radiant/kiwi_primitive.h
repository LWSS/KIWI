#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Solid primitives are staged on the active construction plane:
// corner/centre -> base extent -> height, except a sphere ends at its radius.
// Typed values describe the active stage's half-size, radius, or height.
//
// Boxes use KiwiExtrude_BuildPrismDef. The other shapes reshape an AABB through
// Brush_MakeSided (0x4731E0), Brush_MakeSidedCone (0x47BC10), or
// Brush_MakeSidedSphere (0x47BE90); their ported cores require rebuilt bounds.
// Cylinder validates before linking because Brush_MakeSided accepts a def.
// Cone/sphere require a live selection, so they link first and undo on rejection.
//
// Ported constraints:
// - All three rebuild with bFull=1 and can snap to the legacy power-of-two grid.
// - Cone fixes its apex at world +Z and therefore requires an XY plane with +Z normal.
// - Cylinder takes a world axis and therefore requires an axis-aligned plane.
// - Sphere fills a cube and is effectively orientation-independent.
// Cylinder/cone default to the ported core's 16 sides; sphere defaults to 8
// because Brush_MakeSidedSphere creates sides*sides faces.

class KiwiEditorCommand;

#define KPRIM_MIN_EXTENT     1.0f   // world units — below this the gesture is a no-op

// A view aligned with the height axis cannot map cursor motion reliably. In that
// case, create 5 ft toward the camera and arm that cap's face-push handle.
// Radiant world units are inches, so keep the conversion explicit.
#define KPRIM_AUTO_HEIGHT   ( 5.0f * 12.0f )   // 5 ft, in world units (inches)
#define KPRIM_CYL_SIDES_DEF  16     // brush.cpp:3598 (Brush_MakePhysCylinder)
#define KPRIM_CYL_SIDES_MIN  3      // brush.cpp:3383 rejects < 3
// Match the 64-segment construction/extrusion profile ceiling.
#define KPRIM_CYL_SIDES_MAX  64
// Patch cylinders use stock Radiant's fixed 9x3 quadratic control net. Smoothness
// comes from tessellation, not `sides`; patches have neither collision nor caps.
#define KPRIM_PATCH_SPANS    4      // quarter arcs -> width 9
#define KPRIM_PATCH_ROWS     3      // bottom / middle / top, the stock layout

// "RoundToolPatch" is loaded lazily because the profile is unavailable at init.
bool KiwiPrim_PatchMode();
void KiwiPrim_SetPatchMode( bool on );

#define KPRIM_SPH_SIDES_DEF  8      // 64 faces; see the note above
#define KPRIM_SPH_SIDES_MIN  4      // brush.cpp:3706 rejects < 4
// Sphere cost is quadratic: 16 sides already produces 256 faces (0x47BE90).
#define KPRIM_SPH_SIDES_MAX  16     // 256 faces - already a lot for one brush

void KiwiPrim_RegisterCommands();
KiwiEditorCommand *KiwiPrim_CommandForId( int commandId );

// The "Solids" block inside the shell's Construct panel (same shape as
// KiwiCon_MenuItems — buttons, because that panel is a window, not a menu bar).
void KiwiPrim_MenuItems();
