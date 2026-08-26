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
//     core additionally skips xx5 instances.
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
