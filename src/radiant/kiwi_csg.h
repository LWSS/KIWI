#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// CSG workflow wrappers over the ported geometry cores; no CSG math lives here.
//
// Ported csg.cpp entry points:
//   CSG_MakeHollow      0x47D3C0   command 32982
//   Brush_MergeList     0x47D600   CSG_Merge helper
//   CSG_Merge           0x47DA40   command 32927
//   CSG_FaceVisible     0x47DE60   Auto Caulk visibility test
//   Brush_AutoCaulkFace 0x47E080
//   Brush_AutoCaulk     0x47E0F0   command 33220
//
// csg.cpp has no subtract core. Boolean Difference in kiwi_boolean.cpp composes
// Brush_SplitBrushByFace (brush.cpp 0x471960), outside the classic Hollow, Merge,
// and Auto Caulk command set.
//
// Hollow and Merge mutate or free live selections. A faithful preview requires
// running the cores on clones and drawing unlinked defs, so none is provided here.
//
// Command gates:
//   Hollow (mainfrm.cpp 0x425570): exactly one non-patch, non-fixed brush; the
//     core additionally skips xx5 instances, and so does KiwiCsg_CanHollow.
//   Merge: at least two non-patch, non-fixed brushes under one owner.
//   Auto Caulk: at least one selected instance.

struct selbrush_t;   // qe3.h:429; pointer-only below

// Baseline eligibility: valid def, non-patch instance, and valid non-fixed owner.
// The Hollow core separately skips xx5 instances.
bool KiwiCsg_BrushUsable( const selbrush_t *b );

// At least two baseline-eligible brushes under one owner.
bool KiwiCsg_SelectionMergeable();

bool KiwiCsg_CanHollow();     // 32982
bool KiwiCsg_CanMerge();      // 32927
bool KiwiCsg_CanAutoCaulk();  // 33220

// mainfrm.cpp hooks: NoteBefore snapshots selection count; NoteAfter reports and
// consumes it. NoteAfter without a pending snapshot is a no-op.
void KiwiCsg_NoteBefore();
void KiwiCsg_NoteAfter( const char *label );

// Draw buttons inside the caller's existing ImGui window.
void KiwiCsg_MenuItems();

// KIWI (2026-10-22, Plasticity 2026.2 "failed operations highlight the problem area in
// red"): when Merge or Boolean Difference fails, the camera shows WHERE in translucent red,
// through walls, for KCSG_HINT_SECONDS (fading at the end).  The next operation clears it.
//   Merge (csg.cpp CSG_Merge): the gaps between the brushes that the merged brush would
//     have to fill (the convex hull minus every input), or the inputs when there is no hull.
//   Boolean difference (kiwi_boolean.cpp): where a refused cut's tool meets the piece.
struct brush_t;
void KiwiCsg_FailHintClear();
// A convex solid given by `count` outward planes, 4 doubles each (n.x, n.y, n.z, d), inside
// where n.p - d <= 0.  Faces that come out degenerate or unbounded are skipped; false when
// nothing drawable came out.
bool KiwiCsg_FailHintAddSolid( const double *planes, int count );
// A brush's face windings as they stand.
void KiwiCsg_FailHintAddBrush( const brush_t *def );
// Both brushes' planes together: their overlap.  False when it is empty.
bool KiwiCsg_FailHintAddOverlap( const brush_t *a, const brush_t *b );
// Camera overlay pass (KiwiVP_DrawCameraOverlay); no-op while there is no hint.
void KiwiCsg_DrawFailHint( float imgMinX, float imgMinY, float imgW, float imgH );
