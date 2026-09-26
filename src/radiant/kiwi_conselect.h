#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Construction geometry uses an index-based, KIWI-owned selection because
// sel_item_t addresses brush geometry. It never enters selection_t or legacy sync.
// Store replacement clears it because RemoveAt renumbers indices; append preserves it.
//
// Click arbitration uses screen pixels. Construction wins against a miss, against
// an area hit, or when its point/line hit is closer. Lines beat area hits so
// construction drawn on a wall remains clickable.
//
// Mutations use construction's whole-store undo. MoveBegin's snapshot is both the
// cancel baseline and, after MoveCommit, the undo record.

#include "kiwi_construct.h"

enum kconSelKind_t
{
    KCONSEL_OBJECT = 0,
    KCONSEL_POINT,
    KCONSEL_SEGMENT
};

struct kconSelItem_t
{
    int           object = -1;
    kconSelKind_t kind   = KCONSEL_OBJECT;
    int           index  = -1;      // point / segment ordinal; -1 for KCONSEL_OBJECT
};

// Selection list
int                  KiwiConSel_Count();
const kconSelItem_t *KiwiConSel_At( int i );
void                 KiwiConSel_Clear();
inline bool          KiwiConSel_Empty() { return KiwiConSel_Count() == 0; }

// Draw-side queries (kiwi_construct.cpp's DrawWorld brightens what these report).
bool KiwiConSel_ObjectSelected ( int object );                 // ANY item names it
bool KiwiConSel_PointSelected  ( int object, int pointIndex );
bool KiwiConSel_SegmentSelected( int object, int segIndex );

// Drop index-based selection after structural store replacement.
void KiwiConSel_NoteStoreReplaced();

// Replace selection with whole objects; drop invalid indices and duplicates.
void KiwiConSel_SelectObjects( const int *objects, int count );

// Picking (kiwi_boxselect.cpp)
// Return the best mode-granular candidate and its screen-pixel distance.
// Anchors use PICK_VERT_PIXELS; segment clicks use 14 px for access, while
// continuous hover/snap stays at 10 px to limit noise.
bool KiwiConSel_PickAt( int imgX, int imgY, kconSelItem_t *out, float *outPixels );

// Shift adds, Ctrl toggles, and no modifier replaces. A miss clears only when
// neither modifier is held.
void KiwiConSel_ApplyClick( const kconSelItem_t &it, bool shift, bool ctrl );

// Marquee uses crossing or containment; Ctrl removes, Shift adds, and neither replaces.
void KiwiConSel_ApplyRect( float x0, float y0, float x1, float y1,
                           bool crossing, bool shift, bool ctrl );

// Join
bool KiwiConSel_CanJoin();
// Chain-walk selected open objects into one polyline. A closed result becomes
// eligible for ordinary region derivation; invalid chains change nothing.
bool KiwiConSel_Join();

// Extend (KIWI_CMD_CONSTRUCT_EXTEND, modal)
// A lollipop on the selected end nearest the cursor, along its line: dragging it (or a
// typed length) moves every selected end that far along its OWN end segment - the
// objects keep direction, other points, group, name and plane.  Whole objects offer
// their FREE ends (not on another line within KiwiRegion_WeldDist; with neither free,
// the end nearer the cursor); Point mode (1) end points / end segments offer those.
// Snapping (on by default, Ctrl frees) clicks the handle onto the path's line crossings.
// "Stop at the first line" (F, default on): each end halts at the first line ahead, at
// another selected end's path (an opened corner), or halfway to a facing collinear end.
// Enter commits ONE construction undo snapshot; Esc leaves the lines untouched.
class KiwiEditorCommand;
bool               KiwiConSel_CanExtend();
KiwiEditorCommand *KiwiConSel_CommandForId( int commandId );
// Contextual E (KiwiCmd_DispatchInner, beside ContextB): KIWI_CMD_EXTRUDE_FACE becomes
// KIWI_CMD_CONSTRUCT_EXTEND when the selection is construction lines only (no brush
// selection, no region selected, something extendable); otherwise returns commandId.
int                KiwiConSel_ContextE( int commandId );

// Delete
// Key ownership requires construction selection and an empty selected_brushes list.
// Fine typed brush items remain untouched; whole-object selection falls through.
bool KiwiConSel_OwnsDelete();
// Palette availability only requires construction selection.
bool KiwiConSel_CanDelete();
bool KiwiConSel_DeleteSelected();

// Hide
// Bare H hides construction first, then falls through for the brush half of a
// mixed selection. Hidden objects are inert and are removed from selection.
bool KiwiConSel_CanHide();
bool KiwiConSel_OwnsHide();
bool KiwiConSel_HideSelected();

// Duplicate brush edges as construction lines
// Resolve selected edges to world endpoints, drop coincident shared edges, and
// combine contiguous runs into polylines. Closed runs enter ordinary region derivation.
// Copied brush vertices should coincide, so edge/run matching uses bare
// KREG_JOIN_DIST; Join uses its scaled, bounded weld.
// One construction undo snapshot covers the command.
bool KiwiConSel_CanDuplicateEdges();
bool KiwiConSel_DuplicateEdgesAsLines();

// Move (the construction arm of kiwi_transform.cpp's G)
// True when G should run the construction arm: construction items are selected
// and the typed/legacy selection is empty.
bool KiwiConSel_CanMove();
// Latch the baseline and push ONE store-undo snapshot.  `outRef` gets the
// reference point the drag maps against (the centroid of the selected objects'
// anchors).  False = nothing movable.
bool KiwiConSel_MoveBegin( float outRef[3] );
// Absolute, from the baseline — never incremental (kiwi_transform.h rule 1).
void KiwiConSel_MoveApply( const float delta[3] );
void KiwiConSel_MoveCancel();
void KiwiConSel_MoveCommit();
// R / S arms: same baseline + undo snapshot as Move (call MoveBegin first, then
// MoveCommit / MoveCancel).  Both are ABSOLUTE from the baseline.  Rotation is a
// right-hand-rule turn about the world axis through `pivot`; scale multiplies the
// pivot-relative position per world axis.  Circles/arcs keep their parametric
// form: they rotate rigidly, and a non-uniform scale becomes the mean of the two
// factors spanning their plane (no ellipses).
void KiwiConSel_RotateApply( const float pivot[3], int axis, float degrees );
// General form: `m` (world = m * baseline, column vectors) is the whole turn from
// the baseline about `pivot`; the axis form is one such matrix.  A chained ring
// gesture feeds the composition of its segments through this.
void KiwiConSel_RotateApplyMatrix( const float pivot[3], const float m[3][3] );
void KiwiConSel_ScaleApply ( const float pivot[3], const float factor[3] );

// Construction snap candidates bypass Pick's selected-object exclusion. Mute the
// latched move set explicitly so an absolute drag cannot repeatedly snap to itself.
bool KiwiConSel_SnapMuted( int objectIndex );

// Commands
void KiwiConSel_RegisterCommands();
bool KiwiConSel_DispatchInstant( unsigned int cmdId );
