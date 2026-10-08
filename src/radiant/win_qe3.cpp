#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\win_qe3.cpp
// Windows-specific system layer: console window, print routing, cursor helpers,
// registry save/load, window state, map-modified flag.

#include "stdafx.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "kiwi_test.h"          // KIWI-TEST: console tee (no-op outside test mode)
#include "kiwi_shadowcache.h"   // KiwiShadowCache_Invalidate
#include "kiwi_lightcache.h"    // KiwiLightCache_MapModified
#include "kiwi_ported.h"        // KIWI: shared engine/editor externs

// ────────────────────────────────────────────────────────────────────────────
// Forward declarations from engine / other radiant TUs
// ────────────────────────────────────────────────────────────────────────────
void __cdecl Com_Error(int code, const char *fmt, ...);

// ─────────────────────────────────────────────────────────────────────────────
// 0x499ce0  TranslateString - convert '\n' to '\r\n' in a static buffer (byte_240a590, a
// 0x10000+1-byte BSS array) so text displays correctly in Win32 edit controls.  The
// binary fatals via Com_Error when the output index reaches 0x10000 - reproduced.
// ─────────────────────────────────────────────────────────────────────────────
char *TranslateString( char *string )
{
    iassert( string );   // win_qe3.cpp:88
    // 0x10000+1 valid indices; a few extra bytes absorb the binary's benign
    // off-by-edge when the final char is a '\n' near the limit (matches the
    // binary's generous BSS slack — the 0x10000 guard is the intended cap).
    static char buf2[0x10008];
    int         o = 0;                 // output index (v1 in the binary)

    for ( const char *p = string; *p; ++p )
    {
        if ( o == 0x10000 )
            Com_Error( ERR_FATAL, "TranslateString buffer exceeded" );

        if ( *p == '\n' )
        {
            buf2[o++] = '\r';
            buf2[o]   = '\n';
        }
        else
        {
            buf2[o] = *p;
        }
        ++o;
    }
    buf2[o] = '\0';

    return buf2;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499d80  console_print( fmt, va_list ) - the CENTRAL console sink.  vsprintf into a
// 32 KB buffer -> TranslateString -> append to the console edit control
// (g_qeglobals.d_hwndEdit), trimming the oldest lines once the control holds > 400.  The
// binary's OnCreate installs it as the global `console_stuff` callback (0x420a54), so
// every Com_PrintMessage / Com_PrintError / R_Warn editor line funnels through it
// (cmdlib.cpp owns console_stuff + SetConsoleHandler here); Sys_Printf tail-calls it.
// The SendMessageA sequence is the binary's verbatim: EM_GETLINECOUNT(0xba),
// WM_SETREDRAW(0x0b), EM_GETSEL(0xb0), EM_SETSEL(0xb1), EM_REPLACESEL(0xc2) - over 400
// lines it saves the caret, selects+deletes the first 500 chars, restores the caret, then
// appends.  EM_REPLACESEL appends at the caret, which keeps the console on the newest line.
// KISAK: the stdout write is kept (the headless selftest never creates the frame, so
// d_hwndEdit stays 0 and the GUI block is skipped - gate output is byte-identical).
// ─────────────────────────────────────────────────────────────────────────────
void console_print( const char *fmt, va_list args )
{
    // The binary uses an unbounded vsprintf into char[32772]; keep the same size but
    // bound it (_vsnprintf) for safety — overlong lines are rare editor diagnostics.
    char buf[32772];
    _vsnprintf( buf, sizeof( buf ), fmt, args );
    buf[sizeof( buf ) - 1] = '\0';

    KiwiTest_ConsoleTap( buf );  // KIWI-TEST: capture the central formatted console stream
    fputs( buf, stdout );

    // Tee to the ImGui console panel (imgui_shell.cpp). It renders the raw \n directly, so it
    // needs no \r\n translation. This is the console the user actually sees now; the native
    // EDIT below stays fed but hidden (kept only so the legacy SendMessage paths still no-op
    // cleanly). Headless/selftest has no shell inited — the append is a cheap string op.
    extern void ImGuiConsole_Append( const char *s );
    ImGuiConsole_Append( buf );

    // The legacy EDIT pane below is hidden now, so the last log line is the only load
    // readout.  No-op unless a load bracket is open; throttled internally to ~10 Hz.
    extern void KiwiLoadProgress_Note( const char *line );   // kiwi_loadprogress.h
    KiwiLoadProgress_Note( buf );

    HWND edit = g_qeglobals.d_hwndEdit;
    if ( !edit )
        return;                       // headless / pre-OnCreate — stdout only

    // KIWI (2026-09-05): the legacy EDIT is no longer FED.  It was kept as a hidden mirror of
    // the ImGui console, but during a synchronous print storm (the lazy model loads of the
    // first camera frame) it was seen painting itself white over the dark console tab, and
    // every EM_REPLACESEL on a 400-line control re-lays the whole text out (real CPU per
    // line).  The ImGui console above is the only readout now; the control stays alive
    // only because legacy HWND users still expect it to exist.  If anything shows it,
    // hide it again right here - this runs on every line, so it cannot linger.
    if ( ::IsWindowVisible( edit ) )
    {
        ::ShowWindow( edit, SW_HIDE );
        static bool s_reported = false;
        if ( !s_reported )
        {
            s_reported = true;
            ImGuiConsole_Append( "KIWI: the legacy console pane was visible and has been hidden (report this if it keeps happening).\n" );
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499e90  Sys_Printf - varargs prologue + a tail call into console_print.  The return
// value is the binary's CString length; every caller ignores it.
// ─────────────────────────────────────────────────────────────────────────────
int Sys_Printf( const char *fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    console_print( fmt, ap );
    va_end( ap );
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sys_GetCursorPos  (0x499C90, 36 bytes)
// Thin Win32 GetCursorPos() wrapper.  36 bytes on x86 = call + POINT local +
// two field reads.  Fully portable.
// ─────────────────────────────────────────────────────────────────────────────
void Sys_GetCursorPos( int *x, int *y )
{
    POINT pt;
    GetCursorPos( &pt );
    if ( x ) *x = (int)pt.x;
    if ( y ) *y = (int)pt.y;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sys_UpdateWindows  (0x427130, 12 bytes)  [P5.5 — real port]
// The editor's invalidation request: OR the window-update bits into g_nUpdateBits.
// The actual RedrawWindow broadcast is deferred to CMainFrame::UpdateWindows, flushed
// each idle by RoutineProcessing (CRadiantApp::OnIdle). Faithful to the binary.
// ─────────────────────────────────────────────────────────────────────────────
void Sys_UpdateWindows( int bits )
{
    g_nUpdateBits |= bits;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sys_UpdateStatusBar  (0x499B20, 107 bytes)  [P5.5 — real port]
// Build "Brushes: N Entities: M" from the live counts and route it to status-bar
// pane 2 (the binary writes m_strStatus[2] then CMainFrame::UpdateStatusText). Our
// MainFrm_SetStatusText sets the pane directly. g_numbrushes/g_numentities are filled
// by QE_CountBrushesAndUpdateStatusBar (qe3.cpp).
// ─────────────────────────────────────────────────────────────────────────────
extern int  g_numbrushes;      // qe3.cpp
extern int  g_numentities;     // qe3.cpp
extern void MainFrm_SetStatusText( int pane, const char *text );   // mainfrm.cpp
void Sys_UpdateStatusBar( void )
{
    char buf[100];
    sprintf( buf, "Brushes: %d Entities: %d", g_numbrushes, g_numentities );
    MainFrm_SetStatusText( 2, buf );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499b90  Sys_Status - SendMessageA(d_hwndStatus, SB_SETTEXT, 0, psz).  GtkRadiant's
// takes (psz, part); the CoD binary hardcodes pane 0, as its caller ConnectEntities
// (0x48eb30) does inline.  No-op while d_hwndStatus is NULL (headless).
// ─────────────────────────────────────────────────────────────────────────────
void Sys_Status( const char *psz )
{
    SendMessageA( g_qeglobals.d_hwndStatus, WM_USER + 1 /* SB_SETTEXT, pane 0 */,
                  0, (LPARAM)psz );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499bb0  MarkMapModified - set `modified` and put "<currentmap> *" (backslashes to
// forward slashes) in the frame title.  Brush_Move calls it after every edit.
// ─────────────────────────────────────────────────────────────────────────────
void MarkMapModified( void )
{
    // The editor's "geometry changed" funnel: the recorded caster walk stores composed
    // orientations, so it goes stale here.
    KiwiShadowCache_Invalidate();
    KiwiLightCache_MapModified(); // KIWI-UX: Preserve unrelated per-light lists after a light-only epair edit.
    if ( modified != 1 )
    {
        modified = 1;
        char title[1024];
        _snprintf( title, sizeof( title ), "%s *", currentmap );
        for ( char *p = title; *p; ++p )
            if ( *p == '\\' ) *p = '/';
        SetWindowTextA( g_qeglobals.d_hwndMain, title );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499930  SaveRegistryInfo / 0x4999c0  LoadRegistryInfo — the binary wrote these blobs
// as REG_BINARY values under HKCU\Software\iw\CoD4Radiant\CoD4Radiant (or the
// use_ini_registry key).  KISAK: ALL registry usage removed on request — the blobs
// (window placements, SavedInfo, dialog rects) are hex-encoded into the [Blobs] section
// of kiwi_radiant.ini beside the exe (radiant_registry.cpp Radiant_IniPath), so the
// editor is fully portable and nothing machine-global redirects or persists state.
// The largest caller blob is SavedInfo (0x2c4 = 708 bytes → 1416 hex chars), well inside
// private-profile line limits.  Call-site semantics preserved: SaveRegistryInfo returns
// TRUE iff written; LoadRegistryInfo fills pvBuf, returns TRUE iff the value exists, and
// plSize is in/out (seed = buffer capacity, out = stored byte count; NULL discards it —
// the binary's 1-byte scratch-cell case, only hit by callers that know the exact size).
// ─────────────────────────────────────────────────────────────────────────────
#include "radiant_registry.h"   // Radiant_IniPath()

BOOL SaveRegistryInfo( const char *pszName, void *pvBuf, int lSize )
{
    if ( !pszName || !pvBuf || lSize < 0 || lSize > 8192 )
        return FALSE;
    char hex[8192 * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    const unsigned char *src = (const unsigned char *)pvBuf;
    for ( int i = 0; i < lSize; ++i )
    {
        hex[i * 2]     = digits[src[i] >> 4];
        hex[i * 2 + 1] = digits[src[i] & 0xF];
    }
    hex[lSize * 2] = 0;
    return ::WritePrivateProfileStringA( "Blobs", pszName, hex, Radiant_IniPath() ) != 0;
}

BOOL LoadRegistryInfo( const char *pszName, void *pvBuf, long *plSize )
{
    if ( !pszName || !pvBuf )
        return FALSE;
    char hex[8192 * 2 + 1];
    DWORD n = ::GetPrivateProfileStringA( "Blobs", pszName, "", hex, sizeof( hex ),
                                          Radiant_IniPath() );
    if ( n == 0 || ( n & 1 ) )
        return FALSE;                       // absent or malformed
    long stored = (long)( n / 2 );
    long cap    = plSize ? *plSize : stored;   // NULL plSize: caller knows the exact size
    long count  = stored < cap ? stored : cap;
    unsigned char *dst = (unsigned char *)pvBuf;
    for ( long i = 0; i < count; ++i )
    {
        int hi, lo;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        hi = ( a >= '0' && a <= '9' ) ? a - '0' : ( a >= 'a' && a <= 'f' ) ? a - 'a' + 10
           : ( a >= 'A' && a <= 'F' ) ? a - 'A' + 10 : -1;
        lo = ( b >= '0' && b <= '9' ) ? b - '0' : ( b >= 'a' && b <= 'f' ) ? b - 'a' + 10
           : ( b >= 'A' && b <= 'F' ) ? b - 'A' + 10 : -1;
        if ( hi < 0 || lo < 0 )
            return FALSE;                   // not our hex — treat as absent
        dst[i] = (unsigned char)( ( hi << 4 ) | lo );
    }
    if ( plSize )
        *plSize = stored;                   // registry semantics: report the stored size
    return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// KIWI: MFCCreate (0x499600, OnCreate's "SavedInfo" registry-blob loader + default seeder) is
// gone - Radiant_SetDefaultGridState seeds the same defaults, and nothing writes the blob.
