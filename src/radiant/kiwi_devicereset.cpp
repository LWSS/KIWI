#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// D3D9 reset cleanup and recovery diagnostics for Radiant.
#include "stdafx.h"
#include "qe3.h"
#include <stdlib.h>                 // free() matches brush.cpp's j__free_0 thunk
#include <stdarg.h>                 // varargs reporter
#include <d3d9.h>                   // D3DERR_* diagnostics
#include <gfx_d3d/r_init.h>         // dx, g_disableRendering
#include "kiwi_devicereset.h"

extern int      Sys_Printf( const char *fmt, ... );                 // win_qe3.cpp
// Brush_InvalidateVis (0x478340) drops face/patch visuals and arms their rebuild.
extern brush_t *Brush_InvalidateVis( selbrush_t *b );               // brush.cpp

namespace
{
    // Do not return cached handles through R_Ed_FreeVertices while its VB pool is
    // being destroyed; that would populate the new pool's free lists with stale runs.
    // Brush_InvalidateVis (0x478340) does not free per-face visArray blocks, so do it first.
    void DropInstanceSurfCache( selbrush_t *b )
    {
        if ( !b || !b->def )
            return;

        if ( b->faces )
        {
            for ( int i = 0; i < b->faceCount; ++i )
            {
                faceVis_s *fv = &b->faces[i];
                if ( fv->visArray )
                    free( fv->visArray );     // Visuals_VisArray (0x46f590), minus the VB-handle returns.
                fv->visArray = nullptr;
                fv->visCount = 0;
            }
        }

        Brush_InvalidateVis( b );
    }

    int DropList( selbrush_t &sentinel )
    {
        int n = 0;
        for ( selbrush_t *b = sentinel.next; b && b != &sentinel; b = b->next )
        {
            DropInstanceSurfCache( b );
            ++n;
        }
        return n;
    }
}

void KiwiDevice_InvalidateEditorSurfCache()
{
    // Current brush-list heads may be null before map initialization or after teardown.
    int n = 0;
    n += DropList( active_brushes );
    n += DropList( selected_brushes );
    n += DropList( filtered_brushes );

    if ( n )
        Sys_Printf( "Device reset: dropped the editor surface cache for %i brush(es).\n", n );
}

// Image_Rebuild has no header declaration.
extern void __cdecl Image_Rebuild( GfxImage *image );        // r_image.cpp
extern bool Kiwi_RescueSave(char *outPath, int outPathSize);   // radiant/map.cpp

namespace
{
    // Always mirror diagnostics to OutputDebugStringA; callers throttle repeats.
    void KiwiDevice_Report( const char *fmt, ... )
    {
        char msg[512];
        va_list args;
        va_start( args, fmt );
        _vsnprintf( msg, sizeof( msg ), fmt, args );
        va_end( args );
        msg[sizeof( msg ) - 1] = '\0';

        ::OutputDebugStringA( "[KIWI device] " );
        ::OutputDebugStringA( msg );
        ::OutputDebugStringA( "\n" );
        Sys_Printf( "Device: %s\n", msg );
    }

    // Latest recovery state for the frame health watch.
    long s_lastCoopHr    = 0;     // TestCooperativeLevel, from R_TestDevice
    long s_lastResetHr   = 0;     // IDirect3DDevice9::Reset, from R_ResetDevice
    int  s_lastResetPass = -1;    // which release pass ran for that attempt
    int  s_resetAttempts = 0;
    bool        s_lossNoted   = false;
    long        s_lossHr      = 0;
    const char *s_lossWhere   = nullptr;   // a string literal from the call site; never freed
    bool        s_auditDone   = false;     // once per loss episode

    const char *HrName( long hr )
    {
        switch ( hr )
        {
        case D3D_OK:                 return "D3D_OK / not attempted";   // D3D_OK is 0
        case D3DERR_DEVICELOST:      return "D3DERR_DEVICELOST";
        case D3DERR_DEVICENOTRESET:  return "D3DERR_DEVICENOTRESET";
        case D3DERR_INVALIDCALL:     return "D3DERR_INVALIDCALL";
        case D3DERR_DRIVERINTERNALERROR: return "D3DERR_DRIVERINTERNALERROR";
        case D3DERR_OUTOFVIDEOMEMORY:    return "D3DERR_OUTOFVIDEOMEMORY";
        case E_OUTOFMEMORY:          return "E_OUTOFMEMORY";
        default:                     return "?";
        }
    }

    const char *ReleasePassName( int pass )
    {
        switch ( pass )
        {
        case 0:  return "none (plain retry)";
        case 1:  return "full R_ReleaseForShutdownOrReset";
        case 2:  return "second-chance default-pool re-release";
        default: return "(no Reset attempted)";
        }
    }

    // Mirror each false gate in R_SetupRendertarget_CheckDevice after WM_PAINT's checks.
    const char *BlackFrameReason( HWND__ *frame )
    {
        if ( !dx.device )
            return "no D3D device";
        if ( dx.targetWindowIndex >= 0 )
            return "a render target is still ACTIVE (targetWindowIndex >= 0) — a "
                   "previous frame never ran R_CheckTargetWindow/RTT_End";
        if ( g_disableRendering )
            return "g_disableRendering is set (a D3D call failed fatally; the counter is never cleared)";
        if ( dx.deviceLost )
            return "device LOST — waiting on TestCooperativeLevel/Reset";
        for ( int i = 0; i < dx.windowCount; ++i )
        {
            if ( dx.windows[i].hwnd != frame )
                continue;
            if ( dx.windows[i].width <= 0 || dx.windows[i].height <= 0 )
                return "the frame window has a degenerate size (minimised?)";
            if ( !dx.windows[i].swapChain )
                return "the frame window has NO swap chain and R_Hwnd_Resize could not make one";
            return "the device tests healthy — the paint was skipped upstream "
                   "(ImGuiShell_FrameAuthorized / ImGuiShell_PrimaryActive)";
        }
        return "the frame window is not a registered render window";
    }
}

// R_ReleaseLostImages cannot enumerate editor images because DB_EnumXAssets is a stub.
// Mirror R_FreeLostImage over imageGlobals.imageHashTable; the basemap gate makes retries safe.
void KiwiDevice_ReleaseUnmanagedImages()
{
    int released = 0;
    for ( int i = 0; i < IMAGE_HASH_TABLE_SIZE; ++i )
    {
        GfxImage *img = imageGlobals.imageHashTable[i];
        if ( !img || img->category < IMG_CATEGORY_FIRST_UNMANAGED )
            continue;
        if ( !img->texture.basemap )
            continue;                       // basemap already released
        Image_Release( img );
        ++released;
    }
    if ( released )
        Sys_Printf( "Device reset: released %i unmanaged (default-pool) image(s).\n", released );
}

// Mirror R_RebuildLostImage's unmanaged arm over the editor hash table.
// Managed textures survive Reset, so do not invoke its fatal missing-asset arm.
// Prog images are render targets rebuilt by R_InitRenderTargets.
void KiwiDevice_RebuildUnmanagedImages()
{
    int rebuilt = 0;
    for ( int i = 0; i < IMAGE_HASH_TABLE_SIZE; ++i )
    {
        GfxImage *img = imageGlobals.imageHashTable[i];
        if ( !img || img->texture.basemap )
            continue;
        if ( img->category < IMG_CATEGORY_FIRST_UNMANAGED || Image_IsProg( img ) )
            continue;
        Image_Rebuild( img );
        ++rebuilt;
    }
    if ( rebuilt )
        Sys_Printf( "Device reset: rebuilt %i unmanaged (default-pool) image(s).\n", rebuilt );
}

void KiwiDevice_NoteCoopLevel( long hr )
{
    s_lastCoopHr = hr;
}

void KiwiDevice_NoteLoss( const char *where, long hr )
{
    if ( s_lossNoted )
        return;                    // one line per episode — see kiwi_devicereset.h
    s_lossNoted = true;
    s_lossWhere = where;
    s_lossHr    = hr;
    KiwiDevice_Report( "DEVICE LOST at %s - TestCooperativeLevel=0x%08x %s. "
                       "Recovery starts now; watch for the Reset result.",
                       where ? where : "(unknown)", (unsigned)hr, HrName( hr ) );
}

void KiwiDevice_NoteResetResult( long hr, int releasePass )
{
    s_lastResetHr   = hr;
    s_lastResetPass = releasePass;
    ++s_resetAttempts;

    // A successful Reset ends the episode and rearms first-loss reporting and auditing.
    if ( hr >= 0 )
    {
        s_lossNoted = false;
        s_lossWhere = nullptr;
        s_lossHr    = 0;
        s_auditDone = false;
    }

    // INVALIDCALL means a default-pool object is still live or bound. Audit the first
    // failure after release in each episode to name uncovered RTT/image resources.
    if ( hr == D3DERR_INVALIDCALL && !s_auditDone )
    {
        s_auditDone = true;
        extern int RTT_DescribeLiveSlots( char *out, int outSize );   // radiant/radiant_rtt.cpp
        char slots[128];
        const int live = RTT_DescribeLiveSlots( slots, (int)sizeof( slots ) );

        int unmanaged = 0;
        for ( int i = 0; i < IMAGE_HASH_TABLE_SIZE; ++i )
        {
            GfxImage *img = imageGlobals.imageHashTable[i];
            if ( img && img->category >= IMG_CATEGORY_FIRST_UNMANAGED && img->texture.basemap )
                ++unmanaged;
        }

        KiwiDevice_Report(
            "Reset returned D3DERR_INVALIDCALL after the %s. STILL ALIVE: RTT slots [%s] (%i), "
            "unmanaged default-pool images %i. Anything non-empty here is a D3DPOOL_DEFAULT "
            "object the release pass did not cover.",
            ReleasePassName( releasePass ), slots, live, unmanaged );
    }
}

// Called after EndPaint, with no scene/target active and a live message pump.
void KiwiDevice_FrameHealthWatch( HWND__ *frame, bool authorized, bool painted )
{
    static unsigned s_unhealthySince = 0;   // GetTickCount of the first black tick (0 = healthy)
    static unsigned s_lastLog        = 0;
    static bool     s_escaped        = false;

    // OS and nested-loop paints intentionally render nothing; they are not health evidence.
    if ( !authorized )
        return;

    const unsigned now = ::GetTickCount();

    if ( painted )
    {
        if ( s_unhealthySince )
        {
            KiwiDevice_Report( "recovered - frames are rendering again after %u ms "
                               "(%i Reset attempt(s), last hr 0x%08x %s)",
                               now - s_unhealthySince, s_resetAttempts,
                               (unsigned)s_lastResetHr, HrName( s_lastResetHr ) );
            s_unhealthySince = 0;
            s_resetAttempts  = 0;
            // A rendered frame also ends episodes that recovered without Reset.
            s_lossNoted = false;
            s_lossWhere = nullptr;
            s_lossHr    = 0;
            s_auditDone = false;
        }
        return;
    }

    if ( !s_unhealthySince )
    {
        s_unhealthySince = now ? now : 1u;
        s_lastLog        = 0;
    }

    // Log at most every ~2 s; Sys_Printf can stall on undrained stdout.
    if ( !s_lastLog || ( now - s_lastLog ) >= 2000 )
    {
        s_lastLog = now ? now : 1u;
        KiwiDevice_Report(
            "NOTHING RENDERED for %u ms. Reason: %s. Lost at: %s (0x%08x %s). "
            "TestCooperativeLevel=0x%08x %s; "
            "Reset=0x%08x %s after %i attempt(s), release pass: %s",
            now - s_unhealthySince,
            BlackFrameReason( frame ),
            // Preserve the first loss site/HRESULT across every report.
            s_lossWhere ? s_lossWhere : "(not recorded this session)",
            (unsigned)s_lossHr, HrName( s_lossHr ),
            (unsigned)s_lastCoopHr,  HrName( s_lastCoopHr ),
            (unsigned)s_lastResetHr, HrName( s_lastResetHr ),
            s_resetAttempts,
            ReleasePassName( s_lastResetPass ) );
    }

    // After 15 s, rescue once and alert without stopping Reset retries.
    // Kiwi_RescueSave is one-shot, SEH-wrapped, and avoids window/editor mutation.
    if ( !s_escaped && ( now - s_unhealthySince ) >= 15000 )
    {
        s_escaped = true;

        char rescuePath[MAX_PATH] = "";
        const bool saved = Kiwi_RescueSave( rescuePath, sizeof( rescuePath ) ) && rescuePath[0] != '\0';

        char box[1536];
        _snprintf( box, sizeof( box ),
                   "The graphics device was LOST and could not be recovered.\n\n"
                   "The editor is still running, but nothing can be drawn.\n\n"
                   "First lost at: %s\n"
                   "  (TestCooperativeLevel 0x%08x %s)\n"
                   "Last TestCooperativeLevel: 0x%08x (%s)\n"
                   "Last Reset: 0x%08x (%s) after %i attempt(s)\n\n"
                   "%s%s\n\n"
                   "Please RESTART the editor.  Your original .map file was NOT touched.",
                    // Include the first loss in the screenshotable dialog.
                   s_lossWhere ? s_lossWhere : "(not recorded)",
                   (unsigned)s_lossHr, HrName( s_lossHr ),
                   (unsigned)s_lastCoopHr,  HrName( s_lastCoopHr ),
                   (unsigned)s_lastResetHr, HrName( s_lastResetHr ), s_resetAttempts,
                   saved ? "YOUR MAP WAS RESCUED TO:\n" : "",
                   saved ? rescuePath
                         : "No rescue file was written (nothing to save, or a rescue "
                           "had already been written this session)." );
        box[sizeof( box ) - 1] = '\0';

        KiwiDevice_Report( "PERMANENT LOSS after 15 s - rescue save: %s",
                           saved ? rescuePath : "(none)" );

        ::MessageBoxA( (HWND)frame, box, "CoD4Radiant - graphics device lost",
                       MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST );
    }
}
