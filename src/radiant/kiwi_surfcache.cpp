#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Replays the whole entity/prefab pass, only its clean object segments, or nothing;
// every uncertain validity check refuses.

#include "stdafx.h"
#include <vector>
#include <stdlib.h>               // getenv
#include <string.h>
#include "qe3.h"                  // g_qeglobals (g_filtersUpdated), selbrush_t
#include <gfx_d3d/r_gfx.h>        // GfxPointVertex (the cached line segments)
#include "kiwi_walkcache.h"       // walk and structure epochs
#include <universal/profile.h>
#include "kiwi_surfcache.h"

// The render-command record/replay primitives; all KISAK_RADIANT-fenced there.
extern bool __cdecl R_Ed_CmdCursor( int *cursorOut );                              // r_rendercmds.cpp:435
extern bool __cdecl R_Ed_CmdRange( int from, const void **baseOut, int *bytesOut ); // r_rendercmds.cpp:444
extern int  __cdecl R_Ed_CmdBlobCriticalBytes( const void *blob, int bytes );      // r_rendercmds.cpp:457
extern bool __cdecl R_Ed_CmdAppendBlob( const void *blob, int bytes, int criticalBytes ); // r_rendercmds.cpp:482
extern void __cdecl R_Ed_GetLastMaterialColor( float out[4] );                     // r_rendercmds.cpp:2595
extern void __cdecl R_Ed_SetLastMaterialColor( const float in[4] );                // r_rendercmds.cpp:2601
// The line bucket's ungrouped read/put-back pair (r_rendercmds.h).
extern int  __cdecl R_Ed_LineBucketMark();
extern int  __cdecl R_Ed_LineBucketFlushCount();
extern bool __cdecl R_Ed_LineBucketRead( int from, int count, GfxPointVertex *verts,
                                         unsigned char *widths, unsigned char *dimensions );
extern bool __cdecl R_Ed_LineBucketAddSegs( const GfxPointVertex *verts, const unsigned char *widths,
                                            const unsigned char *dimensions, int count );

namespace
{
    // Above 8 MiB, recording is refused to cap persistent command storage.
    const int KIWI_SURFCACHE_MAX_BLOB = 8 * 1024 * 1024;
    // Above this count the block still caches, but without per-object bookkeeping.
    const int KIWI_SURFCACHE_MAX_SEGS = 65536;
    // Above 256 dirty objects, rebuilding is cheaper than per-object patching.
    const int KIWI_SURFCACHE_MAX_DIRTY = 256;

    unsigned s_epoch      = 1;         // bumped by every invalidation funnel
    const char *s_lastWhy = "(never)";

    bool     s_have       = false;     // a whole-block capture exists and is believed valid
    unsigned s_capEpoch   = 0;         // KiwiSurfCache_Epoch() when it was taken
    unsigned s_capWalk    = 0;         // KiwiWalkCache_Epoch()
    unsigned s_capStruct  = 0;         // KiwiWalkCache_StructEpoch()
    int      s_capFilters = 0;         // g_qeglobals.g_filtersUpdated
    unsigned s_capPassKey = 0;

    std::vector< uint8_t > s_blob;     // the pass's immediate command bytes
    int      s_blobCritical = 0;       // of those, the CRITICAL ones
    float    s_blobEndColor[4] = { -2.0f, -2.0f, -2.0f, -2.0f };  // Ed_EmitLineBatch's dedup state

    struct SurfSeg
    {
        const selbrush_t   *brush;
        unsigned long long  sig;        // KiwiWalk_SubtreeSignature at capture
        int                 lineFirst;  // into s_capLineVerts / s_capLineW / s_capLineD
        int                 lineCount;
    };
    std::vector< SurfSeg >       s_segs;
    std::vector< int >           s_segSurfEnd;    // absolute surf cursor after each object
    std::vector< GfxPointVertex >s_capLineVerts;  // 2 per cached line segment, UNGROUPED
    std::vector< unsigned char > s_capLineW;
    std::vector< unsigned char > s_capLineD;
    // The whole-block capture also carries per-record ownership; false = it does not, so
    // the block can be replayed whole but not patched.
    bool     s_segmented   = false;

    // Recording state (BeginRecord .. EndRecord)
    bool           s_recording   = false;
    int            s_recCmdStart = 0;
    unsigned       s_recPassKey  = 0;
    unsigned       s_recEpoch    = 0;  // s_epoch when the bracket opened
    unsigned       s_recWalk     = 0;  // KiwiWalkCache_Epoch() when it opened
    KiwiEdSurfMark s_recMark     = { 0, 0, 0, 0 };
    bool           s_recSegOk    = false;   // the segments are still trustworthy
    int            s_recLineMark = 0;       // R_Ed_LineBucketMark at the last node close
    int            s_recLineFlush = 0;      // R_Ed_LineBucketFlushCount when recording opened
    int            s_recCmdAfterNodes = 0;  // cmd cursor after the last node closed

    // Patch state (PassMode .. PatchEnd)
    bool                              s_patching = false;
    std::vector< unsigned char >      s_segClean;    // per segment: 1 = replay it
    std::vector< unsigned long long > s_prevSigs;    // last frame's computed signatures
    unsigned                          s_prevStruct = 0;
    // A mid-patch line failure defers teardown until appended records finish the frame.
    bool                              s_patchDropAtEnd = false;

    int      s_serial   = 0;           // 0 = no resident-run key

    bool Enabled()
    {
        static const bool s_off = []{ const char *e = getenv( "RADIANT_SURFCACHE_OFF" );
                                      return e && *e && *e != '0'; }();
        return !s_off;
    }

    void DropSegments()
    {
        s_segmented = false;
        s_segs.clear();
        s_segSurfEnd.clear();
        s_capLineVerts.clear();
        s_capLineW.clear();
        s_capLineD.clear();
        s_segClean.clear();
        s_patching       = false;
        s_patchDropAtEnd = false;
    }
}

bool KiwiSurfCache_Enabled()
{
    return Enabled();
}

unsigned KiwiSurfCache_Epoch()
{
    return s_epoch;
}

// Per-brush VB churn retires whole replay but preserves segments. Uploads only consume
// free space; frees affect a signature-dirty owner or a structural removal. Pool-wide
// events use KiwiSurfCache_InvalidateAll.
void KiwiSurfCache_Invalidate( const char *why )
{
    ++s_epoch;
    s_lastWhy = why ? why : "(unnamed)";
    // May run inside a draw walk, so retain storage; resident runs name D3D9 VBs and drop now.
    s_have   = false;
    s_serial = 0;
    KiwiEdScene_DropMeshRuns();
}

void KiwiSurfCache_InvalidateAll( const char *why )
{
    KiwiSurfCache_Invalidate( why );
    DropSegments();
    KiwiEdScene_DropCapture();
}

int KiwiSurfCache_BuildSerial()
{
    return ( Enabled() && s_have ) ? s_serial : 0;
}

bool KiwiSurfCache_TryReplay( unsigned passKey )
{
    if ( !Enabled() || !s_have || s_recording )
        return false;
    if ( !KiwiEdScene_HaveCapture() )
        return false;
    if ( s_capEpoch   != s_epoch
      || s_capWalk    != KiwiWalkCache_Epoch()
      || s_capFilters != g_qeglobals.g_filtersUpdated
      || s_capPassKey != passKey )
        return false;

    PROF_SCOPED( "cam surf cache replay" );

    // Preserve pass order: surf records and command bytes are independent streams.
    if ( !KiwiEdScene_Replay() )
        return false;
    if ( !s_blob.empty()
      && !R_Ed_CmdAppendBlob( &s_blob[0], (int)s_blob.size(), s_blobCritical ) )
    {
        // Command replay failed after surf replay; discard the stored capture before fallback.
        KiwiEdScene_DropCapture();
        DropSegments();
        s_have = false;
        return false;
    }
    R_Ed_SetLastMaterialColor( s_blobEndColor );
    return true;
}

void KiwiSurfCache_BeginRecord( unsigned passKey )
{
    s_recording = false;
    if ( !Enabled() )
        return;
    if ( !R_Ed_CmdCursor( &s_recCmdStart ) )
        return;
    KiwiEdScene_Mark( &s_recMark );
    s_recPassKey = passKey;
    s_recEpoch   = s_epoch;
    s_recWalk    = KiwiWalkCache_Epoch();
    s_recording  = true;

    DropSegments();
    s_recSegOk         = true;
    s_recLineMark      = R_Ed_LineBucketMark();
    s_recLineFlush     = R_Ed_LineBucketFlushCount();
    s_recCmdAfterNodes = s_recCmdStart;
}

void KiwiSurfCache_RecordNode( const selbrush_t *b, unsigned long long sig )
{
    if ( !s_recording || !s_recSegOk )
        return;
    if ( !b || sig == 0 || (int)s_segs.size() >= KIWI_SURFCACHE_MAX_SEGS )
    {
        s_recSegOk = false;                // no signature = nothing to compare next frame
        return;
    }

    // Any line-bucket flush resets numbering and invalidates all recorded ranges.
    const int lineNow = R_Ed_LineBucketMark();
    if ( lineNow < s_recLineMark || R_Ed_LineBucketFlushCount() != s_recLineFlush )
    {
        s_recSegOk = false;
        return;
    }
    const int lineCount = lineNow - s_recLineMark;

    // Object-local command bytes cannot be separated from the grouped pass flush, so any
    // such emission makes the capture unpatchable. DrawBrush normally emits lines/surfs.
    int cmdNow = 0;
    if ( !R_Ed_CmdCursor( &cmdNow ) || cmdNow != s_recCmdAfterNodes )
    {
        s_recSegOk = false;
        return;
    }

    SurfSeg seg;
    seg.brush     = b;
    seg.sig       = sig;
    seg.lineFirst = (int)s_capLineW.size();
    seg.lineCount = lineCount;
    if ( lineCount > 0 )
    {
        const size_t vFirst = s_capLineVerts.size();
        s_capLineVerts.resize( vFirst + (size_t)lineCount * 2 );
        s_capLineW.resize( (size_t)seg.lineFirst + (size_t)lineCount );
        s_capLineD.resize( (size_t)seg.lineFirst + (size_t)lineCount );
        if ( !R_Ed_LineBucketRead( s_recLineMark, lineCount, &s_capLineVerts[vFirst],
                                   &s_capLineW[seg.lineFirst], &s_capLineD[seg.lineFirst] ) )
        {
            s_recSegOk = false;
            return;
        }
    }
    s_segs.push_back( seg );

    KiwiEdSurfMark mark;
    KiwiEdScene_Mark( &mark );
    s_segSurfEnd.push_back( mark.surf );

    s_recLineMark = lineNow;
}

void KiwiSurfCache_EndRecord()
{
    if ( !s_recording )
        return;
    s_recording = false;

    // An in-pass VB upload/free can invalidate handles captured earlier in this pass.
    if ( s_epoch != s_recEpoch || KiwiWalkCache_Epoch() != s_recWalk )
    {
        KiwiEdScene_DropCapture();
        DropSegments();
        s_have = false;
        return;
    }

    PROF_SCOPED( "cam surf cache build" );

    const void *base  = nullptr;
    int         bytes = 0;
    if ( !R_Ed_CmdRange( s_recCmdStart, &base, &bytes ) || bytes < 0
      || bytes > KIWI_SURFCACHE_MAX_BLOB )
    {
        KiwiEdScene_DropCapture();
        DropSegments();
        s_have = false;
        return;
    }
    // A malformed blob is one the executor would walk off the end of; refuse it.
    const int critical = ( bytes > 0 ) ? R_Ed_CmdBlobCriticalBytes( base, bytes ) : 0;
    const bool segOk   = s_recSegOk && !s_segs.empty();
    if ( critical < 0
      || !KiwiEdScene_CaptureSegmented( &s_recMark,
                                        segOk ? &s_segSurfEnd[0] : nullptr,
                                        segOk ? (int)s_segSurfEnd.size() : 0 ) )
    {
        KiwiEdScene_DropCapture();
        DropSegments();
        s_have = false;
        return;
    }

    s_blob.assign( (const uint8_t *)base, (const uint8_t *)base + bytes );
    s_blobCritical = critical;
    R_Ed_GetLastMaterialColor( s_blobEndColor );

    s_capEpoch   = s_epoch;
    s_capWalk    = KiwiWalkCache_Epoch();
    s_capStruct  = KiwiWalkCache_StructEpoch();
    s_capFilters = g_qeglobals.g_filtersUpdated;
    s_capPassKey = s_recPassKey;
    s_have       = true;
    s_segmented  = segOk;
    if ( !segOk )
        DropSegments();
    // Issue a new resident-run key for this capture generation.
    if ( ++s_serial == 0 )
        s_serial = 1;
    // Require one observed patch frame before declaring changed signatures quiescent.
    s_prevSigs.clear();
    s_prevStruct = s_capStruct;
}

bool KiwiSurfCache_PatchWanted( unsigned passKey )
{
    return Enabled()
        && s_segmented
        && !s_recording
        && !s_segs.empty()
        && KiwiEdScene_HaveCapture()
        && s_capStruct  == KiwiWalkCache_StructEpoch()
        && s_capFilters == g_qeglobals.g_filtersUpdated
        && s_capPassKey == passKey;
}

// Called after whole-block replay refuses, avoiding signature work on idle frames.
int KiwiSurfCache_PassMode( unsigned passKey, selbrush_t *const *brushes,
                            const unsigned long long *sigs, int count )
{
    s_patching       = false;
    s_patchDropAtEnd = false;

    if ( !KiwiSurfCache_PatchWanted( passKey ) || count <= 0 || !brushes || !sigs )
        return KIWI_SURFPASS_LIVE;
    // The structural epoch promises the same dispatch sequence; reject any mismatch.
    if ( (int)s_segs.size() != count )
        return KIWI_SURFPASS_LIVE;

    const bool sameFrameSet = ( s_prevStruct == s_capStruct
                             && (int)s_prevSigs.size() == count );

    s_segClean.assign( (size_t)count, 1 );
    int  dirty     = 0;
    bool quiescent = true;
    for ( int i = 0; i < count; ++i )
    {
        if ( s_segs[i].brush != brushes[i] )
            return KIWI_SURFPASS_LIVE;                 // not the sequence we captured
        if ( sigs[i] != 0 && sigs[i] == s_segs[i].sig )
            continue;                                  // unchanged: its records stand
        s_segClean[i] = 0;
        ++dirty;
        // Rebuild once every dirty signature repeats, restoring the presorted resident-run
        // path; an active drag keeps changing.
        if ( !sameFrameSet || sigs[i] == 0 || sigs[i] != s_prevSigs[i] )
            quiescent = false;
    }

    // Remember what we saw before any early return, so the NEXT frame can compare.
    s_prevSigs.assign( sigs, sigs + count );
    s_prevStruct = KiwiWalkCache_StructEpoch();

    if ( dirty == 0 )
    {
        // Signatures prove the block valid despite the refused whole-block gate.
        s_capEpoch = s_epoch;
        s_capWalk  = KiwiWalkCache_Epoch();
        s_have     = true;
        if ( ++s_serial == 0 )
            s_serial = 1;
        if ( KiwiSurfCache_TryReplay( passKey ) )
            return KIWI_SURFPASS_REPLAY;
        return KIWI_SURFPASS_LIVE;
    }
    if ( dirty > KIWI_SURFCACHE_MAX_DIRTY || quiescent )
        return KIWI_SURFPASS_LIVE;

    s_patching = true;
    return KIWI_SURFPASS_PATCH;
}

bool KiwiSurfCache_ObjectDirty( int index )
{
    if ( !s_patching || index < 0 || index >= (int)s_segClean.size() )
        return true;
    return s_segClean[index] == 0;
}

bool KiwiSurfCache_PatchReplay( int *sortedFirstOut, int *sortedCountOut )
{
    if ( sortedFirstOut ) *sortedFirstOut = 0;
    if ( sortedCountOut ) *sortedCountOut = 0;
    if ( !s_patching )
        return false;

    PROF_SCOPED( "cam surf cache patch replay" );

    // Absolute start lets the flush reject the prefix claim when world surfs precede it.
    int windowFirst = 0;
    {
        KiwiEdSurfMark mark;
        KiwiEdScene_Mark( &mark );
        windowFirst = mark.surf;
    }
    const int replayed = KiwiEdScene_ReplayFiltered( &s_segClean[0], (int)s_segClean.size() );
    if ( replayed < 0 )
    {
        s_patching = false;
        return false;
    }

    // Replay clean line segments ungrouped so one pass groups them with live lines.
    for ( size_t i = 0; i < s_segs.size(); ++i )
    {
        if ( !s_segClean[i] || s_segs[i].lineCount <= 0 )
            continue;
        const SurfSeg &seg = s_segs[i];
        if ( !R_Ed_LineBucketAddSegs( &s_capLineVerts[(size_t)seg.lineFirst * 2],
                                      &s_capLineW[seg.lineFirst],
                                      &s_capLineD[seg.lineFirst],
                                      seg.lineCount ) )
        {
            // Clean records are already appended, so defer teardown to PatchEnd rather than
            // make ObjectDirty double-draw them. This frame may lose unqueued clean lines.
            s_patchDropAtEnd = true;
            break;
        }
    }

    if ( sortedFirstOut ) *sortedFirstOut = windowFirst;
    if ( sortedCountOut ) *sortedCountOut = replayed;
    return true;
}

void KiwiSurfCache_PatchEnd()
{
    s_patching = false;
    if ( s_patchDropAtEnd )
    {
        s_patchDropAtEnd = false;
        KiwiEdScene_DropCapture();
        DropSegments();
        s_have = false;
    }
}
