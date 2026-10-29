#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI: D3D9 timestamp queries on Tracy's GPU timeline (r_gpuprof.h).
//
// A zone is two TIMESTAMP queries, issued where the backend reaches its begin and end
// markers.  Tracy is told about the zone at once (with a query id per end); the GPU times
// follow when the frame's queries are ready, a few frames later.  Each editor frame is one
// TIMESTAMPDISJOINT bracket with its own TIMESTAMPFREQ, so a clock change mid-frame cannot
// mix two rates.

#include <universal/q_shared.h>
#include "r_gpuprof.h"

#include "r_init.h"            // dx, d3d9.h
#include "r_rendercmds.h"
#include "rb_backend.h"        // tess
#include "rb_shade.h"          // RB_EndTessSurface

#ifdef TRACY_ENABLE
#include <atomic>
#include <tracy/TracyC.h>
namespace tracy { TRACY_API std::atomic<uint8_t> &GetGpuCtxCounter(); }   // client/TracyProfiler.hpp
#endif

#ifdef TRACY_ENABLE
namespace
{
    const unsigned GP_MAX_QUERIES = 4096;   // timestamps in flight (a ring; Tracy ids are 16-bit)
    const unsigned GP_MAX_FRAMES  = 8;      // GPU frames in flight
    const int      GP_MAX_DEPTH   = 32;
    const int      GP_MAX_NAMES   = 128;

    struct gpFrame_t
    {
        IDirect3DQuery9 *disjoint;
        IDirect3DQuery9 *freq;
        unsigned         first;             // the frame's queries: [first, end) of s_next
        unsigned         end;
    };

    struct gpOpen_t
    {
        const char *name;
        bool        timed;                  // its begin query was issued
    };

    struct gpName_t
    {
        const char                          *key;
        struct ___tracy_source_location_data loc;
    };

    IDirect3DQuery9 *s_stamps[GP_MAX_QUERIES];
    gpFrame_t        s_frames[GP_MAX_FRAMES];
    IDirect3DQuery9 *s_syncStamp = nullptr; // calibration only
    IDirect3DQuery9 *s_syncFreq  = nullptr;
    unsigned         s_frameHead = 0;       // frames issued
    unsigned         s_frameTail = 0;       // frames handed to Tracy
    unsigned         s_next      = 0;       // timestamps issued
    unsigned         s_retired   = 0;       // timestamps handed to Tracy
    bool             s_created   = false;
    bool             s_active    = false;   // this frame is timed
    bool             s_needSync  = true;    // re-align the GPU clock before the next timed frame
    uint8_t          s_context   = 255;
    bool             s_hasContext = false;
    UINT64           s_lastFreq  = 0;
    gpOpen_t         s_open[GP_MAX_DEPTH];
    int              s_depth     = 0;
    gpName_t         s_names[GP_MAX_NAMES];
    int              s_nameCount = 0;

    int64_t ToNs( UINT64 ticks, UINT64 freq )
    {
        return (int64_t)( ( ticks / freq ) * 1000000000ull + ( ( ticks % freq ) * 1000000000ull ) / freq );
    }

    // A blocking read, for the calibration only.  Bounded: a lost device never answers.
    bool WaitFor( IDirect3DQuery9 *q, void *out, DWORD size )
    {
        for ( int spin = 0; spin < 200000; ++spin )
        {
            const HRESULT hr = q->GetData( out, size, D3DGETDATA_FLUSH );
            if ( hr == S_OK )
                return true;
            if ( hr != S_FALSE )
                return false;
            ::Sleep( 0 );
        }
        return false;
    }

    // The GPU clock now, in ns: one timestamp waited for.  Stalls the pipeline, so only at
    // device creation and when a profiler connects.
    bool GpuNow( int64_t *ns )
    {
        if ( !s_syncStamp || !s_syncFreq )
            return false;
        s_syncFreq->Issue( D3DISSUE_END );
        s_syncStamp->Issue( D3DISSUE_END );
        UINT64 freq = 0, ticks = 0;
        if ( !WaitFor( s_syncFreq, &freq, sizeof( freq ) ) || !WaitFor( s_syncStamp, &ticks, sizeof( ticks ) )
          || !freq )
            return false;
        s_lastFreq = freq;
        *ns = ToNs( ticks, freq );
        return true;
    }

    uint64_t SourceLocation( const char *name )
    {
        for ( int i = 0; i < s_nameCount; ++i )
            if ( s_names[i].key == name )
                return (uint64_t)(uintptr_t)&s_names[i].loc;
        if ( s_nameCount == GP_MAX_NAMES )
            return 0;
        gpName_t &n = s_names[s_nameCount++];
        n.key           = name;
        n.loc.name      = name;
        n.loc.function  = name;
        n.loc.file      = __FILE__;
        n.loc.line      = 0;
        n.loc.color     = 0;
        return (uint64_t)(uintptr_t)&n.loc;
    }

    // The next timestamp, or -1 when the ring cannot also hold the end of every open zone.
    int TakeQuery( int reserveEnds )
    {
        if ( s_next - s_retired + (unsigned)reserveEnds >= GP_MAX_QUERIES )
            return -1;
        const int k = (int)( s_next % GP_MAX_QUERIES );
        ++s_next;
        s_stamps[k]->Issue( D3DISSUE_END );
        return k;
    }

    // Hand Tracy every issued frame whose queries are all ready, oldest first.
    void Collect()
    {
        static UINT64 s_ticks[GP_MAX_QUERIES];
        while ( s_frameTail != s_frameHead )
        {
            const gpFrame_t &f = s_frames[s_frameTail % GP_MAX_FRAMES];
            BOOL   disjoint = FALSE;
            UINT64 freq     = 0;
            if ( f.disjoint->GetData( &disjoint, sizeof( disjoint ), 0 ) != S_OK
              || f.freq->GetData( &freq, sizeof( freq ), 0 ) != S_OK )
                return;
            const unsigned count = f.end - f.first;
            for ( unsigned i = 0; i < count; ++i )
                if ( s_stamps[( f.first + i ) % GP_MAX_QUERIES]->GetData( &s_ticks[i], sizeof( UINT64 ), 0 ) != S_OK )
                    return;
            // A disjoint frame's clock changed rate part-way; its zones still need their times
            // (Tracy would hold them open forever), so they keep the last good rate.
            if ( !disjoint && freq )
                s_lastFreq = freq;
            if ( s_lastFreq )
                for ( unsigned i = 0; i < count; ++i )
                {
                    struct ___tracy_gpu_time_data t;
                    t.gpuTime = ToNs( s_ticks[i], s_lastFreq );
                    t.queryId = (uint16_t)( ( f.first + i ) % GP_MAX_QUERIES );
                    t.context = s_context;
                    ___tracy_emit_gpu_time_serial( t );
                }
            s_retired = f.end;
            ++s_frameTail;
        }
    }

    void ReleaseQuery( IDirect3DQuery9 *&q )
    {
        if ( q )
            q->Release();
        q = nullptr;
    }
}

void R_GpuProf_CreateQueries()
{
    if ( !dx.device || s_created )
        return;
    // Drivers without D3D9 timestamps answer the probe (a null query) with an error.
    if ( FAILED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, nullptr ) )
      || FAILED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMPDISJOINT, nullptr ) )
      || FAILED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMPFREQ, nullptr ) ) )
        return;
    bool ok = SUCCEEDED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &s_syncStamp ) )
           && SUCCEEDED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMPFREQ, &s_syncFreq ) );
    for ( unsigned i = 0; ok && i < GP_MAX_QUERIES; ++i )
        ok = SUCCEEDED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &s_stamps[i] ) );
    for ( unsigned i = 0; ok && i < GP_MAX_FRAMES; ++i )
        ok = SUCCEEDED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMPDISJOINT, &s_frames[i].disjoint ) )
          && SUCCEEDED( dx.device->CreateQuery( D3DQUERYTYPE_TIMESTAMPFREQ, &s_frames[i].freq ) );
    s_created = true;                     // so the release below runs on a partial set
    if ( !ok )
    {
        R_GpuProf_ReleaseQueries();
        return;
    }
    s_frameHead = s_frameTail = 0;
    s_next = s_retired = 0;
    s_depth    = 0;
    s_active   = false;
    s_needSync = true;
    if ( !s_hasContext )
    {
        // Announced once; on-demand Tracy replays it to a profiler that connects later, and
        // the first timed frame re-aligns the clocks then (s_needSync).
        int64_t now = 0;
        if ( !GpuNow( &now ) )
            return;
        s_context = tracy::GetGpuCtxCounter().fetch_add( 1 );
        struct ___tracy_gpu_new_context_data c;
        c.gpuTime = now;
        c.period  = 1.0f;                 // times are handed over in ns
        c.context = s_context;
        c.flags   = 0;
        c.type    = 7;                    // tracy::GpuContextType::Custom
        ___tracy_emit_gpu_new_context_serial( c );
        static const char s_name[] = "D3D9";
        struct ___tracy_gpu_context_name_data n;
        n.context = s_context;
        n.name    = s_name;
        n.len     = (uint16_t)( sizeof( s_name ) - 1 );
        ___tracy_emit_gpu_context_name_serial( n );
        s_hasContext = true;
    }
}

void R_GpuProf_ReleaseQueries()
{
    if ( !s_created )
        return;
    // Zones still waiting for times stay open in this capture; a device reset is rare.
    for ( unsigned i = 0; i < GP_MAX_QUERIES; ++i )
        ReleaseQuery( s_stamps[i] );
    for ( unsigned i = 0; i < GP_MAX_FRAMES; ++i )
    {
        ReleaseQuery( s_frames[i].disjoint );
        ReleaseQuery( s_frames[i].freq );
    }
    ReleaseQuery( s_syncStamp );
    ReleaseQuery( s_syncFreq );
    s_created = false;
    s_active  = false;
    s_depth   = 0;
}

void R_GpuProf_FrameBegin()
{
    s_active = false;
    if ( !s_created || !s_hasContext )
        return;
    if ( !TracyCIsConnected )
    {
        s_needSync = true;
        return;
    }
    if ( s_needSync )
    {
        int64_t now = 0;
        if ( !GpuNow( &now ) )
            return;
        struct ___tracy_gpu_time_sync_data t;
        t.gpuTime = now;
        t.context = s_context;
        ___tracy_emit_gpu_time_sync_serial( t );
        s_needSync = false;
    }
    if ( s_frameHead - s_frameTail == GP_MAX_FRAMES )
    {
        Collect();
        if ( s_frameHead - s_frameTail == GP_MAX_FRAMES )
            return;                       // the GPU is that far behind: this frame goes untimed
    }
    gpFrame_t &f = s_frames[s_frameHead % GP_MAX_FRAMES];
    f.first = f.end = s_next;
    f.disjoint->Issue( D3DISSUE_BEGIN );
    s_depth  = 0;
    s_active = true;
}

void R_GpuProf_FrameEnd()
{
    if ( s_active )
    {
        RB_GpuProf_ListEnd();
        gpFrame_t &f = s_frames[s_frameHead % GP_MAX_FRAMES];
        f.end = s_next;
        f.freq->Issue( D3DISSUE_END );
        f.disjoint->Issue( D3DISSUE_END );
        ++s_frameHead;
        s_active = false;
    }
    if ( s_created )
        Collect();
}

void R_AddCmdGpuZone( const char *name, bool begin )
{
    if ( !s_active || !name )
        return;
    GfxCmdGpuZone *cmd = (GfxCmdGpuZone *)R_GetCommandBuffer( RC_GPU_ZONE, sizeof( GfxCmdGpuZone ) );
    if ( !cmd )
        return;                           // a full buffer: the backend matches by name
    cmd->name  = name;
    cmd->begin = begin ? 1 : 0;
}

void RB_GpuProf_ZoneBegin( const char *name )
{
    if ( !s_active || s_depth == GP_MAX_DEPTH )
        return;
    // The draws ahead of the marker must reach the GPU before its timestamp does.
    if ( tess.indexCount )
        RB_EndTessSurface();
    gpOpen_t &o = s_open[s_depth++];
    o.name  = name;
    o.timed = false;
    const uint64_t loc = SourceLocation( name );
    if ( !loc )
        return;
    const int k = TakeQuery( s_depth + 1 );
    if ( k < 0 )
        return;
    o.timed = true;
    struct ___tracy_gpu_zone_begin_data z;
    z.srcloc  = loc;
    z.queryId = (uint16_t)k;
    z.context = s_context;
    ___tracy_emit_gpu_zone_begin_serial( z );
}

void RB_GpuProf_ZoneEnd( const char *name )
{
    // An end whose begin was refused (or never timed) matches nothing open and is dropped.
    if ( !s_active || s_depth == 0 || s_open[s_depth - 1].name != name )
        return;
    const bool timed = s_open[--s_depth].timed;
    if ( !timed )
        return;
    if ( tess.indexCount )
        RB_EndTessSurface();
    const int k = TakeQuery( 0 );         // reserved when the zone began
    if ( k < 0 )
        return;
    struct ___tracy_gpu_zone_end_data z;
    z.queryId = (uint16_t)k;
    z.context = s_context;
    ___tracy_emit_gpu_zone_end_serial( z );
}

void RB_GpuProf_ListEnd()
{
    while ( s_active && s_depth > 0 )
        RB_GpuProf_ZoneEnd( s_open[s_depth - 1].name );
}

#else   // !TRACY_ENABLE

void R_GpuProf_CreateQueries() {}
void R_GpuProf_ReleaseQueries() {}
void R_GpuProf_FrameBegin() {}
void R_GpuProf_FrameEnd() {}
void R_AddCmdGpuZone( const char *, bool ) {}
void RB_GpuProf_ZoneBegin( const char * ) {}
void RB_GpuProf_ZoneEnd( const char * ) {}
void RB_GpuProf_ListEnd() {}

#endif

void RB_GpuZoneCmd( GfxRenderCommandExecState *execState )
{
    const GfxCmdGpuZone *cmd = (const GfxCmdGpuZone *)execState->cmd;
    if ( cmd->begin )
        RB_GpuProf_ZoneBegin( cmd->name );
    else
        RB_GpuProf_ZoneEnd( cmd->name );
    execState->cmd = (const char *)execState->cmd + cmd->header.byteCount;
}
