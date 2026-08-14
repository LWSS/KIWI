#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_devicereset.cpp — KIWI-UX ROUND AB, ITEM 1.  See kiwi_devicereset.h for the
// full crash analysis (RB_EndSurfacePrologue:202 / g_primStats == NULL).
#include "stdafx.h"
#include "qe3.h"
#include <stdlib.h>                 // free() — the port's j__free_0 thunk (brush.cpp:30)
#include <stdarg.h>                 // KIWI-UX (ROUND AD): the one-line reporter's varargs
#include <d3d9.h>                   // KIWI-UX (ROUND AD): D3DERR_* names for the diagnostics
#include <gfx_d3d/r_init.h>         // KIWI-UX (ROUND AD): dx, g_disableRendering
#include "kiwi_devicereset.h"

// ── ported entry points (each verified against its definition) ───────────────
extern int      Sys_Printf( const char *fmt, ... );                 // win_qe3.cpp
// Brush_InvalidateVis (0x478340) — frees the faceVis array, drops a patch instance's
// visuals through PMESH_22_Indices, and sets version = def->version - 1 so the next
// Brush_CheckBuildFaceVis rebuilds.  Returns b->def.
extern brush_t *Brush_InvalidateVis( selbrush_t *b );               // brush.cpp:1478
// active_brushes / selected_brushes / filtered_brushes are the three embedded
// 56-byte display-list sentinels; declared in qe3.h:1053-1055.  Iterate as
// `for (b = sel.next; b != &sel; b = b->next)` (qe3.h:374-375).

namespace
{
    // Drop ONE instance's cached surf-cache state.
    //
    // The per-face `visArray` blocks are freed here rather than by
    // `Visuals_VisArray` (brush.cpp:1793) on purpose: that function's loop calls
    // `sub_51CB70` (R_Ed_FreeVertices, r_ed_vertbuf.cpp:516) on every
    // `visuals->vertHandle`, which returns the run to the per-material pool.  On
    // this path the pool is about to be — or has just been — destroyed wholesale
    // by `Editor_VB_ReleaseForReset` (r_ed_vertbuf.cpp:125), so those frees would
    // manufacture free-slots inside pools for D3D9 buffers that no longer exist,
    // and `Editor_VB_GetHandle` (r_ed_vertbuf.cpp:169) would later hand one of
    // them back — a handle pointing at nothing.  `Brush_InvalidateVis` is the
    // binary's own drop-don't-return primitive; it just does not free the
    // per-face visArray blocks (0x478340 leaks them), so the loop below does.
    void DropInstanceSurfCache( selbrush_t *b )
    {
        if ( !b || !b->def )
            return;

        if ( b->faces )
        {
            for ( int i = 0; i < b->faceCount; ++i )
            {
                // faceVis_s is 12 bytes; b->faces is the faceCount-element array
                // Brush_MakeFaceVisuals allocated (brush.cpp:153).
                faceVis_s *fv = (faceVis_s *)( (char *)b->faces + 12 * i );
                if ( fv->visArray )
                    free( fv->visArray );     // operator new'd at brush.cpp:2659; the
                                              // port's own free is j__free_0 (brush.cpp:1805)
                fv->visArray = nullptr;
                fv->visCount = 0;
            }
        }

        // Frees b->faces, runs PMESH_22_Indices for a patch instance, and arms the
        // rebuild via version = def->version - 1.
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
    // The three display lists are the complete set of live brush INSTANCES
    // (qe3.h:360-375).  A brush that is on none of them is not drawn and holds no
    // surf-cache state.  `.next` is NULL rather than the sentinel before map.cpp
    // has ever linked them (engine_stubs.cpp:778-779 zero-initialises two of the
    // sentinels), so DropList tolerates a NULL walk — this runs on the shutdown
    // path too, where the lists may already be torn down.
    int n = 0;
    n += DropList( active_brushes );
    n += DropList( selected_brushes );
    n += DropList( filtered_brushes );

    if ( n )
        Sys_Printf( "Device reset: dropped the editor surface cache for %i brush(es).\n", n );
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AD) — the D3DPOOL_DEFAULT images nobody was releasing,
//                        and the recovery chain that failed silently.
//  Full analysis in kiwi_devicereset.h / RADIANT_UX_DESIGN §59.
// ═════════════════════════════════════════════════════════════════════════════

// ── ported entry points (each verified against its definition) ───────────────
// Image_Release (r_image.cpp:175) and Image_IsProg (r_image_load_obj.cpp:258) are declared
// in r_image.h:147 / :280, which qe3.h:22 already pulls in.  Image_Rebuild has no
// header declaration, so it is declared here against its definition:
extern void __cdecl Image_Rebuild( GfxImage *image );        // r_image.cpp:1075
// Kiwi_RescueSave — copied verbatim from the existing declaration at
// engine_stubs.cpp:282 (definition map.cpp:836).
extern bool Kiwi_RescueSave(char *outPath, int outPathSize);   // radiant/map.cpp

namespace
{
    // ── the one-line reporter ────────────────────────────────────────────────
    // OutputDebugStringA ALWAYS (it is the only channel that survives a black
    // window and an undrained stdout — the exact situation this exists for), the
    // console only when a console can be reached.  Callers own the throttling.
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

    // Latest facts from the recovery chain.  Written by the two Note* hooks in
    // r_init.cpp, read by the health watch.
    long s_lastCoopHr    = 0;     // TestCooperativeLevel, from R_TestDevice
    long s_lastResetHr   = 0;     // IDirect3DDevice9::Reset, from R_ResetDevice
    int  s_lastResetPass = -1;    // which release pass ran for that attempt
    int  s_resetAttempts = 0;

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

    // WHY THE FRAME IS BLACK, named rather than guessed.  These are, in order,
    // every way R_SetupRendertarget_CheckDevice (r_init.cpp:4793-4839) can answer
    // FALSE.  The frame WM_PAINT's own two pre-conditions come first.
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

// Release every UNMANAGED image.  Reproduces R_FreeLostImage (r_image.cpp:1024)
// verbatim — `category >= IMG_CATEGORY_FIRST_UNMANAGED → Image_Release` — over the
// enumeration the editor actually has (imageGlobals.imageHashTable), because
// R_ReleaseLostImages' DB_EnumXAssets is a stub here.
//
// IDEMPOTENT.  Image_Release (r_image.cpp:175) NULLs image->texture.basemap and
// zeroes cardMemory inside the same `if`, so a second call subtracts 0 from
// imageGlobals.totalMemory and releases nothing.  That matters: the round-AD
// second-chance arm in R_ResetDevice runs this again on a retry.
void KiwiDevice_ReleaseUnmanagedImages()
{
    int released = 0;
    for ( int i = 0; i < IMAGE_HASH_TABLE_SIZE; ++i )
    {
        GfxImage *img = imageGlobals.imageHashTable[i];
        if ( !img || img->category < IMG_CATEGORY_FIRST_UNMANAGED )
            continue;
        if ( !img->texture.basemap )
            continue;                       // already released — keep the count honest
        Image_Release( img );
        ++released;
    }
    if ( released )
        Sys_Printf( "Device reset: released %i unmanaged (default-pool) image(s).\n", released );
}

// The counterpart, run after a Reset() that SUCCEEDED.  Reproduces only
// R_RebuildLostImage's unmanaged arm (r_image.cpp:1108-1125):
//     !basemap && category >= 5 && !Image_IsProg → Image_Rebuild.
// The `category < 5` arm is deliberately NOT reproduced: it ends in
// Com_Error(ERR_DROP) for an image that cannot be reloaded, and in this editor a
// managed image with a NULL basemap is an ordinary missing asset (it kept its
// texture across the Reset if it had one at all) — killing the process over it
// would be a worse bug than the one this round is fixing.  Progs are excluded for
// the engine's own reason: they are render targets, and R_CreateForInitOrReset →
// R_InitRenderTargets rebuilds those.
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

void KiwiDevice_NoteResetResult( long hr, int releasePass )
{
    s_lastResetHr   = hr;
    s_lastResetPass = releasePass;
    ++s_resetAttempts;
}

// ── A + C: the frame WM_PAINT's health watch ────────────────────────────────
// Called from radiant_main.cpp's WM_PAINT AFTER ::EndPaint, so: the paint region
// is already validated, no scene bracket is open, no D3D render target is bound,
// and the pump is alive.  That is what makes the message box at the bottom safe.
void KiwiDevice_FrameHealthWatch( HWND__ *frame, bool authorized, bool painted )
{
    static unsigned s_unhealthySince = 0;   // GetTickCount of the first black tick (0 = healthy)
    static unsigned s_lastLog        = 0;
    static bool     s_escaped        = false;

    // A paint the pump did not authorize is NOT evidence.  Round U made every such
    // paint draw nothing on purpose (imgui_shell.cpp:1272), and they arrive
    // constantly — OS repaints, and the nested message loop of every menu and
    // modal dialog.  Judging them would let a file dialog left open for 15 s pop
    // the "device lost" box.  Ignore them entirely: neither evidence of health nor
    // of failure, so the clock is neither started nor cleared.
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
        }
        return;
    }

    if ( !s_unhealthySince )
    {
        s_unhealthySince = now ? now : 1u;
        s_lastLog        = 0;
    }

    // ONE line per ~2 s while black.  Not per tick: this build's Com_Printf is a
    // bare vprintf that can stall the main thread on an undrained stdout (the
    // hazard R_ResetDevice documents at r_init.cpp:4460), and the whole point of
    // this line is that it is readable.
    if ( !s_lastLog || ( now - s_lastLog ) >= 2000 )
    {
        s_lastLog = now ? now : 1u;
        KiwiDevice_Report(
            "NOTHING RENDERED for %u ms. Reason: %s. TestCooperativeLevel=0x%08x %s; "
            "Reset=0x%08x %s after %i attempt(s), release pass: %s",
            now - s_unhealthySince,
            BlackFrameReason( frame ),
            (unsigned)s_lastCoopHr,  HrName( s_lastCoopHr ),
            (unsigned)s_lastResetHr, HrName( s_lastResetHr ),
            s_resetAttempts,
            ReleasePassName( s_lastResetPass ) );
    }

    // ── C: THE PERMANENT-LOSS ESCAPE HATCH ──────────────────────────────────
    // 15 s of unbroken black is not a transient loss any more.  Nothing here
    // stops the retries — the device may still come back — but the operator must
    // not be left staring at a black window with an unsaved map behind it.
    //
    // Kiwi_RescueSave (map.cpp:836) is safe from this healthy context: it is
    // one-shot, SEH-wrapped, pure fopen/fprintf, and deliberately posts no window
    // messages and touches no editor state (map.cpp:790-802).  It was written for
    // dying contexts; a live one is strictly easier.
    //
    // The message box is owned by the frame — the pump is alive, the paint region
    // is validated (we are past ::EndPaint), and an unauthorized WM_PAINT from the
    // box's nested loop draws nothing (ImGuiShell_FrameAuthorized, round U).  It
    // differs from Kiwi_FatalRescue's NULL owner (engine_stubs.cpp:309) on
    // purpose: that one runs while the process is dying and re-entering our own
    // WndProc as an owner is a risk with no upside, whereas here the editor keeps
    // running and the box belongs over its window.
    if ( !s_escaped && ( now - s_unhealthySince ) >= 15000 )
    {
        s_escaped = true;

        char rescuePath[MAX_PATH] = "";
        const bool saved = Kiwi_RescueSave( rescuePath, sizeof( rescuePath ) ) && rescuePath[0] != '\0';

        char box[1536];
        _snprintf( box, sizeof( box ),
                   "The graphics device was LOST and could not be recovered.\n\n"
                   "The editor is still running, but nothing can be drawn.\n\n"
                   "Last TestCooperativeLevel: 0x%08x (%s)\n"
                   "Last Reset: 0x%08x (%s) after %i attempt(s)\n\n"
                   "%s%s\n\n"
                   "Please RESTART the editor.  Your original .map file was NOT touched.",
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
