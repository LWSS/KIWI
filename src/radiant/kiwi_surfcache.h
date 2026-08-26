#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Captures the pose-invariant camera entity + prefab pass as surf records and
// immediate command bytes; pixels are still rasterized each frame.
// Clean object segments replay as a sorted prefix, dirty objects append a sorted
// tail, and cached line segments replay ungrouped for one global grouping pass.
// Dirtiness uses KiwiWalk_SubtreeSignature recomputed each frame. Structural,
// filter/layer/technique, and VB-pool changes invalidate the whole capture; once
// dirty signatures stabilize, one live pass rebuilds it.

struct selbrush_t;

// Safe inside a draw walk. Per-brush VB churn retires whole-block replay while
// preserving segmented capture; `why` records the invalidation source.
void     KiwiSurfCache_Invalidate( const char *why );
// Pool teardown or device reset invalidates every cached buffer handle.
void     KiwiSurfCache_InvalidateAll( const char *why );
unsigned KiwiSurfCache_Epoch();
bool     KiwiSurfCache_Enabled();

// Brackets the entity + prefab pass. A true replay means the caller skips live
// drawing; `passKey` covers per-frame inputs not represented by an epoch.
bool KiwiSurfCache_TryReplay( unsigned passKey );
void KiwiSurfCache_BeginRecord( unsigned passKey );
void KiwiSurfCache_EndRecord();
// Close one dispatched object's surf/line segment and retain its signature.
void KiwiSurfCache_RecordNode( const selbrush_t *b, unsigned long long sig );

enum KiwiSurfPassMode
{
    KIWI_SURFPASS_LIVE   = 0,   // run the pass live and record it
    KIWI_SURFPASS_REPLAY = 1,   // nothing changed: the whole block is already back
    KIWI_SURFPASS_PATCH  = 2    // replay the clean objects, redraw the changed ones
};

// Cheap global gate before the caller computes per-object signatures.
bool KiwiSurfCache_PatchWanted( unsigned passKey );
// `brushes` and `sigs` follow dispatch order; signature 0 means changed. REPLAY has
// already appended the cached block before returning.
int  KiwiSurfCache_PassMode( unsigned passKey, selbrush_t *const *brushes,
                             const unsigned long long *sigs, int count );
// PATCH only, between PassMode and PatchEnd.
bool KiwiSurfCache_ObjectDirty( int index );
// PATCH only: append clean surf records and ungrouped line segments with the line
// bucket open. Reports the already-sorted prefix in absolute window coordinates.
bool KiwiSurfCache_PatchReplay( int *sortedFirstOut, int *sortedCountOut );
void KiwiSurfCache_PatchEnd();

// Nonzero capture generation associating resident mesh-index runs with this window.
int  KiwiSurfCache_BuildSerial();

// Four bump cursors delimiting one pass's surf output in r_ed_scene.cpp.
struct KiwiEdSurfMark
{
    int mesh;      // edSceneGlobals.sceneMeshCount
    int sub;       // radiant_surfCount
    int skin;      // radiant_modelSurfPos
    int surf;      // edSceneGlobals.sceneSurfCount
};

void KiwiEdScene_Mark( KiwiEdSurfMark *out );
// Snapshot [from, now); `segSurfEnd[s]` is the absolute surf cursor after object s,
// while segCount 0 creates an unsegmented block. Refuses unless every skinnedVert is
// xsurf->verts0 because tempSkinBuf-backed pointers are rewound every frame.
bool KiwiEdScene_CaptureSegmented( const KiwiEdSurfMark *from, const int *segSurfEnd,
                                   int segCount );
// Append at current cursors and rebase pointers; false means absent or out of room.
bool KiwiEdScene_Replay();
// Append records owned by clean segments in comparator order; returns count or -1.
int  KiwiEdScene_ReplayFiltered( const unsigned char *segClean, int segCount );
void KiwiEdScene_DropCapture();
bool KiwiEdScene_HaveCapture();
int  KiwiEdScene_CaptureKB();

// Resident mesh-index runs apply only to a stamped main flush; other flushes tessellate.
void KiwiEdScene_StampMainFlush( int runsKey, bool presorted );
// Declare an absolute comparator-sorted prefix; honored only at the window head.
void KiwiEdScene_StampSortedPrefix( int first, int count );
// Drop the run table when its vertex buffers may be gone; the managed index buffer survives.
void KiwiEdScene_DropMeshRuns();
// Shutdown or VB-pool teardown also releases the resident index buffer.
void KiwiEdScene_ReleaseMeshRunIB();
