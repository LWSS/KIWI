#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
//  kiwi_surfcache.h - the camera's entity + prefab pass is pose-invariant, so its
//  PRODUCT (surf records + immediate command bytes) is captured once and replayed.
//  Not frame caching: every pixel is still rasterised fresh from the cached inputs.
//
//  ── KIWI: THE CACHE IS PER OBJECT ────────────────────
//  USER RULING, verbatim: *"It only should invalidate the cache for the objects
//  that are being changed."*
//
//  Before this round the cache was one indivisible block: any edit anywhere ran the
//  whole entity + prefab pass live, every frame, for as long as the edit kept
//  happening — so dragging one crate cost the same as rebuilding the map, 15-25 ms
//  a frame in Debug.  Now the capture is SEGMENTED by the object that produced each
//  record, and a frame in which some objects changed:
//    * replays every CLEAN object's records straight out of the block, in the
//      comparator order they were captured in (a sorted PREFIX of the window);
//    * runs DrawBrush live for the CHANGED objects only, appending an unsorted tail;
//    * sorts that small tail and MERGES it into the prefix, instead of re-sorting
//      the whole map's worth of surfs;
//    * puts the clean objects' cached LINE SEGMENTS back into the line bucket
//      UNGROUPED, so the grouping still happens once, globally, at the flush.
//  Manipulating N objects therefore costs O(N) live draws plus one linear replay,
//  not O(map) of DrawBrush.
//
//  "Has this object changed?" is not a ledger of edit funnels — a missed funnel
//  would render stale geometry — it is a SIGNATURE recomputed every frame from the
//  object's own state (kiwi_walkcache.h KiwiWalk_SubtreeSignature).  Anything the
//  signature cannot see is a wholesale invalidation: a structural change (instance
//  alloc/free, display-list link/unlink, entity relink, classname), a filter or
//  layer or technique change, a VB POOL event, or simply not having a recording.
//
//  When the changed objects stop changing (their signatures repeat), the next frame
//  runs the pass live once to fold them into a fresh block, and the fully-cached
//  path — presorted window, resident index runs — comes back.

struct selbrush_t;

// The epoch.  Cheap (one counter bump); safe to call from anywhere, including from
// inside a draw walk.  `why` is a literal used only by the "KiwiSurfCacheInfo" print.
// This is the PER-BRUSH vertex-buffer churn signal: it retires the whole-block
// replay, and the segmented capture survives it (see KiwiSurfCache_PassMode).
void     KiwiSurfCache_Invalidate( const char *why );
// The VB POOL itself went away (device reset, editorVB_freeBuffers): every cached
// record names a buffer that may no longer exist, so nothing survives.
void     KiwiSurfCache_InvalidateAll( const char *why );
unsigned KiwiSurfCache_Epoch();
bool     KiwiSurfCache_Enabled();

// The driver (camwnd.cpp's entity + prefab pass brackets it).  TryReplay true = the
// pass's whole product is back and the caller must SKIP the pass.  `passKey` folds in,
// by value, every per-frame input to the pass that no epoch covers.
bool KiwiSurfCache_TryReplay( unsigned passKey );
void KiwiSurfCache_BeginRecord( unsigned passKey );
void KiwiSurfCache_EndRecord();
// Called after each dispatched object's DrawBrush while recording: closes that
// object's segment (its surf-record range and its line-bucket range) and stores the
// signature the next frame will compare against.
void KiwiSurfCache_RecordNode( const selbrush_t *b, unsigned long long sig );

// ── the per-object path ──────────────────────────────────────────────────────
enum KiwiSurfPassMode
{
    KIWI_SURFPASS_LIVE   = 0,   // run the pass live and record it
    KIWI_SURFPASS_REPLAY = 1,   // nothing changed: the whole block is already back
    KIWI_SURFPASS_PATCH  = 2    // replay the clean objects, redraw the changed ones
};

// Cheap pre-test: is there a segmented capture whose global gates still hold?  The
// caller only pays for the per-object signatures when this says yes.
bool KiwiSurfCache_PatchWanted( unsigned passKey );
// Decide the frame.  `brushes` / `sigs` are the pass's dispatch list IN PASS ORDER
// and their signatures (0 = unknown, which reads as changed).  A REPLAY answer means
// the whole block has ALREADY been replayed by this call.
int  KiwiSurfCache_PassMode( unsigned passKey, selbrush_t *const *brushes,
                             const unsigned long long *sigs, int count );
// PATCH only, valid between PassMode and PatchEnd: does dispatch entry `index` have
// to be drawn live?
bool KiwiSurfCache_ObjectDirty( int index );
// PATCH only: replay the clean objects' records and their line segments.  Call with
// the line bucket already open.  Reports the window's already-sorted prefix.
bool KiwiSurfCache_PatchReplay( int *sortedFirstOut, int *sortedCountOut );
void KiwiSurfCache_PatchEnd();

// A serial that changes on every REBUILD and only on a rebuild, so the backend can tell
// its own resident index runs from somebody else's window (0).
int  KiwiSurfCache_BuildSerial();

// The record store (r_ed_scene.cpp owns the surf arrays): the four bump cursors that
// together delimit one pass's surf output.
struct KiwiEdSurfMark
{
    int mesh;      // edSceneGlobals.sceneMeshCount
    int sub;       // radiant_surfCount
    int skin;      // radiant_modelSurfPos
    int surf;      // edSceneGlobals.sceneSurfCount
};

void KiwiEdScene_Mark( KiwiEdSurfMark *out );
// Snapshot [from, now), with per-record ownership: `segSurfEnd[s]` is the absolute surf
// cursor one past object s's output, so each stored entry remembers which object produced
// it (pass segCount 0 for an unsegmented block).  REFUSED unless skinnedVert ==
// xsurf->verts0 for every surf in the block: the other case is a tempSkinBuf copy whose
// cursor is rewound every frame.
bool KiwiEdScene_CaptureSegmented( const KiwiEdSurfMark *from, const int *segSurfEnd,
                                   int segCount );
// Append the snapshot at the CURRENT cursors, rebasing every internal pointer.
// false = it does not fit / nothing captured; the caller runs the pass live.
bool KiwiEdScene_Replay();
// The same, but only the entries whose owning object is marked clean.  Returns how many
// entries it wrote (they are in comparator order), or -1.
int  KiwiEdScene_ReplayFiltered( const unsigned char *segClean, int segCount );
void KiwiEdScene_DropCapture();
bool KiwiEdScene_HaveCapture();
int  KiwiEdScene_CaptureKB();

// LEG B: the resident mesh index runs.  The camera stamps its main flush with
// (buildSerial, presorted); every other flush leaves the default and keeps the tess path.
void KiwiEdScene_StampMainFlush( int runsKey, bool presorted );
// ...and this one says the window OPENS with `count` entries already in comparator
// order (the clean objects), so the flush can merge the live tail into them instead of
// sorting the lot.  Absolute, and only honoured when it names the window's own head.
void KiwiEdScene_StampSortedPrefix( int first, int count );
// Drop the run TABLE (the vertex buffers it names may be gone); the resident index
// buffer itself is MANAGED and survives.
void KiwiEdScene_DropMeshRuns();
// Shutdown / editor VB pool teardown: release the resident index buffer as well.
void KiwiEdScene_ReleaseMeshRunIB();
