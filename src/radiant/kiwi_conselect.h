#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_conselect.h — SHAKEOUT F: SELECTING construction geometry, and the three
// things a selection is for (Join, Delete, Move).
//
// USER REPORT, verbatim: "Using 2(edge) you can't select lines.  You should be
// able to select lines like this and join them.  Lines aren't selectable with any
// mode."
//
// ── WHY THIS IS A SECOND, PARALLEL SELECTION ────────────────────────────────
// kiwi_construct.h SCOPE RULING 1 still stands and is NOT being reversed here: a
// construction object owns no brush, and `sel_item_t` is (selbrush_t*, index) by
// construction (kiwi_selection.h DESIGN NOTE 1).  Widening the typed selection to
// carry a second addressing mode would touch every consumer of it — the legacy
// sync, the transform units, the box-select collector, the hover accents, the
// palette's kind masks — for a thing none of them can act on.
//
// So construction selection is a SEPARATE, KIWI-OWNED list that NEVER enters
// selection_t and never reaches Sel_SyncToLegacy.  What ruling 1 gives up in
// exchange is that mixing is ALLOWED but not unified: both selections can be
// non-empty at once and both draw, and each command reads whichever list its own
// logic is written against.  The three commands this file adds are all gated on a
// PURE construction selection (nothing brush-side selected) precisely so that
// rule never has to be adjudicated at runtime — see OwnsDelete/CanMove below.
//
// ── WHAT AN ITEM IS ─────────────────────────────────────────────────────────
// An object INDEX into the construction store plus a granularity:
//   KCONSEL_OBJECT   the whole object            (Object / Face / All modes)
//   KCONSEL_POINT    one defining point/anchor   (Point mode, 1)
//   KCONSEL_SEGMENT  one tessellated segment     (Edge mode,  2)
// Store objects have NO IDENTITY — they are addressed by position, and RemoveAt
// shifts everything below it.  The store therefore tells this file whenever it
// has been rearranged (KiwiConSel_NoteStoreReplaced, called from
// KiwiCon_RemoveAt / ClearAll / UndoPop) and the selection is dropped rather than
// remapped.  APPENDING does not renumber anything, so drawing a new line while
// two are selected keeps them selected — which is the case that matters, because
// it is how a user gets to Join.
//
// ── THE CLICK ARBITRATION RULE (the one real design decision here) ──────────
// kiwi_boxselect.cpp's ClickSelect runs the ordinary Pick() first and then asks
// this file for the best construction candidate under the same pixel.  Both
// answers are in SCREEN PIXELS, and construction wins when:
//
//   1. the brush pick MISSED entirely, or
//   2. the brush pick is an AREA hit (SEL_FACE / SEL_OBJECT from Test_Ray, whose
//      `screenDist` is 0 by definition — kiwi_pick.cpp:458), or
//   3. both are POINT/LINE hits and the construction one is CLOSER in pixels.
//
// Rule 2 is the one worth arguing about, and it is deliberate: a construction
// line drawn ON a wall is ALWAYS in front of that wall for picking purposes.
// Without it, scaffolding drawn on geometry — which is the entire use case —
// would be unclickable, because an area hit reports distance 0 and would win
// every comparison.  The cost is that a 6-pixel ribbon along each construction
// segment stops selecting the wall behind it; that ribbon is exactly as wide as
// the one a brush EDGE already steals, so the behaviour is not new, only extended
// to a second kind of line.
//
// ── UNDO ────────────────────────────────────────────────────────────────────
// Every mutation here goes through the STORE'S OWN undo stack (kiwi_construct.h
// ruling 2), never the ported brush-snapshot undo — these objects are neither
// brushes nor entities and undo.cpp has no room for them.  The shape is the same
// one the drawing tools use: KiwiCon_UndoPush() BEFORE mutating, and the
// construction Ctrl+Z / "Construction: Undo Edit" pops it.
//
// A MOVE gesture uses that same snapshot as its cancel: KiwiConSel_MoveBegin
// pushes one, MoveCancel pops it (which restores the store exactly), MoveCommit
// leaves it in place as the undo record.  No second baseline format, and no way
// for the two to disagree.
//
// ── KEYS ────────────────────────────────────────────────────────────────────
// Ctrl+J = Join (modern profile; the full audit is in kiwi_keymap.h).  Delete /
// Backspace on a pure construction selection is arbitrated in KiwiUX_KeyFunnel —
// see KiwiConSel_OwnsDelete for why it CANNOT swallow a brush delete.  G reaches
// the construction arm inside the existing Move command (kiwi_transform.cpp).
// ─────────────────────────────────────────────────────────────────────────────

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

// ── the list ─────────────────────────────────────────────────────────────────
int                  KiwiConSel_Count();
const kconSelItem_t *KiwiConSel_At( int i );
void                 KiwiConSel_Clear();
inline bool          KiwiConSel_Empty() { return KiwiConSel_Count() == 0; }

// Draw-side queries (kiwi_construct.cpp's DrawWorld brightens what these report).
bool KiwiConSel_ObjectSelected ( int object );                 // ANY item names it
bool KiwiConSel_PointSelected  ( int object, int pointIndex );
bool KiwiConSel_SegmentSelected( int object, int segIndex );

// The store rearranged under us; drop the selection (see the note above).
void KiwiConSel_NoteStoreReplaced();

// ── KIWI-UX (ROUND AR, ITEM 1): the paste's selection step ──────────────────
// REPLACE the selection with exactly these object indices, at KCONSEL_OBJECT
// granularity.  Out-of-range indices are dropped and duplicates collapse.  Used by
// the construction clipboard (kiwi_conclip.h) so a paste selects the pasted set
// and nothing else — the same contract the brush paste has.
void KiwiConSel_SelectObjects( const int *objects, int count );

// ── picking (kiwi_boxselect.cpp) ─────────────────────────────────────────────
// The best construction candidate under a camera-image pixel, at the granularity
// the CURRENT selection mode asks for.  `outPixels` is the screen distance the
// arbitration rule compares.  False = nothing within the radius.
//
// THE RADIUS (round R): anchors at PICK_VERT_PIXELS, segments at
// KCON_CLICK_PIXELS (14) — which is DELIBERATELY WIDER than the KCON_LINE_PIXELS
// (10) every CONTINUOUS test uses.  kiwi_construct.h carries the reasoning; the
// short version is that a too-small click box is a failure and a too-big hover box
// is only a nuisance, so the one discrete act gets the generous number.
bool KiwiConSel_PickAt( int imgX, int imgY, kconSelItem_t *out, float *outPixels );

// Apply a click result with the usual shift/ctrl grammar (shift adds, ctrl
// toggles, neither replaces).  Passing an item with object < 0 means "the click
// hit no construction geometry", which CLEARS unless shift/ctrl is held.
void KiwiConSel_ApplyClick( const kconSelItem_t &it, bool shift, bool ctrl );

// Marquee.  `crossing` false = containment.  Appends into the same list under the
// same shift/ctrl grammar; `replace` false leaves the existing items alone (the
// caller has already decided that, because it must decide it for brushes too).
void KiwiConSel_ApplyRect( float x0, float y0, float x1, float y1,
                           bool crossing, bool shift, bool ctrl );

// ── JOIN ─────────────────────────────────────────────────────────────────────
bool KiwiConSel_CanJoin();
// Merge the selected OPEN objects into one polyline by chain-walking their
// endpoints (KiwiRegion_ChainWalk — the same walker a region uses).  A chain that
// comes back to its start is marked CLOSED, which is what makes the region form
// through the existing hook.  Prints and changes nothing when the selection does
// not chain.
bool KiwiConSel_Join();

// ── DELETE ───────────────────────────────────────────────────────────────────
// True when the Delete key belongs to construction geometry.
//
// ROUND U WIDENED IT, and the whole argument is on the definition
// (kiwi_conselect.cpp THE MIXED-SELECTION DELETE).  Short form: the test is now
// "something is selected here AND `selected_brushes` is empty".  A face / edge /
// vertex selection does NOT put its owner on that sentinel (kiwi_selection.cpp
// pass 1, since shakeout D), so a mixed marquee — construction lines plus brush
// edges, which is exactly what a mode-2 box over scaffolding produces — is OURS,
// and the brush-side items are skipped and counted rather than blocking the key.
// A WHOLE-OBJECT selection does put brushes on the sentinel, so that case still
// falls straight through to the ordinary 33003 Delete Selection, untouched.
bool KiwiConSel_OwnsDelete();
// The PALETTE's predicate, deliberately weaker than OwnsDelete: running "Delete
// Selected" from the palette is an explicit instruction about construction
// geometry, so it stays available even with brushes selected.  Only the KEY needs
// the stricter test, because the key is shared.
bool KiwiConSel_CanDelete();
bool KiwiConSel_DeleteSelected();

// ── ROUND U: HIDE ────────────────────────────────────────────────────────────
// USER DIRECTIVE: "We dont have a hide mechanic right now, you should add that(H)
// and add (unhide all) in the search menu".
//
// OwnsHide is DELIBERATELY WEAKER than OwnsDelete: it asks only "is anything
// selected here", with no brush-side condition at all, because H does not have to
// choose.  Delete has to (deleting a brush and deleting a line are different
// enough acts that doing both on one key is a trap), but hiding is additive and a
// mixed selection simply hides both halves — the funnel runs this AND lets 32923
// run.  See the definition for the whole dispatch table.
//
// HIDDEN MEANS INERT: skipped by draw, pick, marquee, snap AND region derivation.
// The flag lives on the store object and persists in the sidecar as `hidden 1`
// (kiwi_construct.h).  Hiding CLEARS the selection of what it hid.
bool KiwiConSel_CanHide();
bool KiwiConSel_OwnsHide();
bool KiwiConSel_HideSelected();

// ── ROUND K: DUPLICATE BRUSH EDGES AS CONSTRUCTION LINES (Shift+D) ──────────
// USER DIRECTIVE, verbatim: "New action (Shift-D) Duplicate.  When pressing
// Shift-D, while having edges of a solid(brush) selected, it should create new
// lines in place of those edges.  Those edges should copy exactly the edges of the
// solid and possibly create a new lineface that can be used for extrusion (light
// blue).  Plasticity has this."
//
// PLASTICITY (read, not assumed).  Its `command:duplicate` runs
// DuplicateFactory, whose `calculate` dispatches on the item kind
// (DuplicateFactory.ts) — a duplicated EDGE comes back as a CURVE, not as another
// edge, because an edge does not exist outside its solid.  KIWI's construction
// store IS its curve store (kiwi_construct.h scope ruling 1), so the same move
// lands construction lines.
//
// "AND POSSIBLY CREATE A NEW LINEFACE" IS FREE, and that is the point of doing it
// this way: the lines go into the ordinary store, the ordinary §8 region pass runs
// over them on the next generation bump, and a closed ring of duplicated edges
// becomes a light-blue region the ordinary extrude consumes.  Nothing new is
// needed for the second half of the directive — and with ROUND K's arrangement
// pass (kiwi_arrange.h) even a set of edges that do NOT quite ring up will bound a
// region as long as they enclose something.
//
// WHAT IT DOES, exactly:
//   * every SEL_EDGE item in the typed selection resolves to its winding edge's
//     two world endpoints;
//   * COINCIDENT edges are dropped.  A brush edge is shared by two faces, so
//     selecting "the edges of a face" and then its neighbour selects the shared
//     one twice; two identical construction lines on top of each other would
//     double every snap candidate and confuse the region walk.  "Coincident" is
//     KREG_JOIN_DIST on both ends, in either order — §8's one weld tolerance;
//   * contiguous RUNS are joined into POLYLINES.  Four edges round a face become
//     ONE closed polyline, which PASS 1 of the region walk turns into a region
//     immediately — rather than four separate lines that only PASS 2 could chain.
//     The chaining rule is the store's own: endpoints within KREG_JOIN_DIST.
// ONE construction undo push covers the whole thing (kiwi_undo.h), so one Ctrl+Z
// removes every line the press created.
//
// False (with a message) when nothing edge-shaped is selected.
bool KiwiConSel_CanDuplicateEdges();
bool KiwiConSel_DuplicateEdgesAsLines();

// ── MOVE (the arm kiwi_transform.cpp's G grows) ──────────────────────────────
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

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AP, ITEM 2) — THE MOVED OBJECTS ARE MUTED AS SNAP TARGETS
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "when dragging a line set using the move gizmo, it
// teleports back and forth from the pivot point to the arrow.  Fix this so the
// pivot point takes control in all cases and doesn't try to fight and teleport."
//
// THE MECHANISM, exactly.  A transform must never snap the geometry it is dragging
// to itself — kiwi_transform.cpp says so in one line
// (`PickFlags() -> PICKF_EXCLUDE_SELECTED`).  But that flag reaches only the arms
// of kiwi_snap.cpp that go through Pick(), i.e. the BRUSH arms; construction
// candidates are walked directly out of the store (kiwi_construct.h scope ruling 1
// put them outside Pick on purpose), and nothing filtered them.  So a CONSTRUCTION
// move was the one gesture in the editor that could snap to itself, and the
// geometry-snap arm is ABSOLUTE — `total = snapPos - m_ref`.  Feed it a point of
// the object it is moving and it solves
//     total_new = ( base + total_old ) - m_ref  =  total_old + ( base - m_ref )
// i.e. it adds the anchor's offset from the reference point EVERY FRAME.  The set
// jumps that far, leaves the 8 px query radius, the arm misses on the next frame,
// the cursor mapping puts it back under the arrow, the anchor re-enters the radius
// — and that two-state limit cycle at frame rate IS the reported teleport, between
// "the reference point is at the cursor" and "some anchor is at the cursor".
//
// THE FIX IS AN EXCLUSION, not a tolerance band.  kiwi_extrude.cpp solves the same
// class of self-snap with KEXT_SELF_SNAP_BAND (refuse an answer near scalar zero),
// which works there because the SOURCE cannot move.  Here the whole object set is
// travelling and its offsets are unbounded, so the only correct rule is the one the
// brush arms already have: while the move owns them, these objects are not
// candidates at all.
//
// True while a move gesture is open AND `objectIndex` is one of the objects it
// latched.  False at every other time, including between gestures — the mute is
// strictly per-gesture, so construction geometry is a first-class snap target the
// instant the drag ends.  Cheap: the moved set is the selection, i.e. a handful.
bool KiwiConSel_SnapMuted( int objectIndex );

// ── commands ─────────────────────────────────────────────────────────────────
void KiwiConSel_RegisterCommands();
bool KiwiConSel_DispatchInstant( unsigned int cmdId );
