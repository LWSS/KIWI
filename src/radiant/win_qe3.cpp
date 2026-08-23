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
#include "kiwi_shadowcache.h"   // KiwiShadowCache_Invalidate
#include "kiwi_lightcache.h"    // KiwiLightCache_MapModified

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

    // The binary runs TranslateString (\n → \r\n) BEFORE the SendMessage so the edit
    // control shows proper line breaks (a bare \n renders as a box in a Win32 edit).
    // stdout above gets the raw \n (native), so gate output is unchanged.
    char *guiText = TranslateString( buf );

    if ( SendMessageA( edit, EM_GETLINECOUNT, 0, 0 ) > 400 )
    {
        DWORD selStart = 0, selEnd = 0;
        SendMessageA( edit, WM_SETREDRAW, FALSE, 0 );
        SendMessageA( edit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd );
        SendMessageA( edit, EM_SETSEL, 0, 500 );             // select first 500 chars
        SendMessageA( edit, EM_REPLACESEL, FALSE, (LPARAM)"" );// delete them (trim top)
        SendMessageA( edit, EM_SETSEL, selStart, selEnd );    // restore caret
        SendMessageA( edit, WM_SETREDRAW, TRUE, 0 );
    }

    SendMessageA( edit, EM_REPLACESEL, FALSE, (LPARAM)guiText );// append at the caret
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
// 0x499eb0  Sys_DoubleTime - `(double)clock() / 1000.0` in the binary (the CRT clock(),
// NOT timeGetTime/GetTickCount; CLOCKS_PER_SEC == 1000 on MSVC).
// ─────────────────────────────────────────────────────────────────────────────
double Sys_DoubleTime( void )
{
    return (double)clock() / 1000.0;
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
extern int g_nUpdateBits;      // engine_stubs.cpp (0x25D5A74)
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
extern int  modified;          // map.cpp (0x23f179c)
extern char currentmap[];      // map.cpp (0x23f18d8)
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
// 0x499c40  Sys_SetTitle - SetWindowTextA(d_hwndMain, text).
// ─────────────────────────────────────────────────────────────────────────────
void Sys_SetTitle( const char *text )
{
    SetWindowTextA( g_qeglobals.d_hwndMain, text );
}

// hCursor in the binary (0x240A114) — the cursor Sys_BeginWait must restore.
static HCURSOR g_hWaitCursor = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// 0x499c50  Sys_BeginWait - SetCursor(IDC_WAIT), saving the PREVIOUS cursor (SetCursor
// returns the one it replaced) into hCursor for Sys_EndWait.
// ─────────────────────────────────────────────────────────────────────────────
void Sys_BeginWait( void )
{
    g_hWaitCursor = SetCursor( LoadCursorA( NULL, IDC_WAIT ) );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499c70  Sys_EndWait - restore the cursor Sys_BeginWait saved, and clear the slot.
// ─────────────────────────────────────────────────────────────────────────────
void Sys_EndWait( void )
{
    if ( g_hWaitCursor )
    {
        SetCursor( g_hWaitCursor );
        g_hWaitCursor = NULL;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499cc0  Sys_SetCursorPos - SetCursorPos(x, y); inverse of Sys_GetCursorPos.
// ─────────────────────────────────────────────────────────────────────────────
void Sys_SetCursorPos( int x, int y )
{
    SetCursorPos( x, y );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499cd0  Sys_Beep - MessageBeep(MB_ICONASTERISK).
// ─────────────────────────────────────────────────────────────────────────────
void Sys_Beep( void )
{
    MessageBeep( MB_ICONASTERISK );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499d50  Sys_ClearPrintf - SendMessageA(d_hwndEdit, WM_SETTEXT, 0, "") empties the
// console edit control (no-op while d_hwndEdit is NULL, i.e. headless).
// ─────────────────────────────────────────────────────────────────────────────
void Sys_ClearPrintf( void )
{
    extern void ImGuiConsole_Clear();      // imgui_shell.cpp — clear the visible ImGui console
    ImGuiConsole_Clear();
    SendMessageA( g_qeglobals.d_hwndEdit, WM_SETTEXT, 0, (LPARAM)"" );
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
// 0x499a40  SaveWindowState - persist a child window's placement.  GetWindowRect; for any
// window other than the main frame, reparent it under the frame (if it is not already) and
// MapWindowPoints the rect into frame client coords, so it is stored RELATIVE to the
// frame; then SaveRegistryInfo(pszName, &rect, 16).  __usercall hWnd@<esi> -> __cdecl.
// Binary caller: CMainFrame::OnDestroy (saves every pane rect on shutdown); the port's
// OnDestroy is a subset that only saves the MRU.
// ─────────────────────────────────────────────────────────────────────────────
BOOL SaveWindowState( HWND hWnd, const char *pszName )
{
    RECT rect;
    GetWindowRect( hWnd, &rect );                 // 0x499a4b

    if ( hWnd != g_qeglobals.d_hwndMain )         // 0x499a57
    {
        if ( GetParent( hWnd ) != g_qeglobals.d_hwndMain )   // 0x499a68
            SetParent( hWnd, g_qeglobals.d_hwndMain );       // 0x499a6c
        // map the screen RECT (2 points) into main-frame client coords  0x499a81
        MapWindowPoints( 0, g_qeglobals.d_hwndMain, (LPPOINT)&rect, 2 );
    }

    return SaveRegistryInfo( pszName, &rect, 16 );           // 0x499a99
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499aa0  LoadWindowState - restore a placement saved by SaveWindowState: seed size=16,
// LoadRegistryInfo into [left,top,right,bottom]; on failure return 0.  Clamp left/top to
// >= 0 and right/bottom to leave at least a 16px window, then MoveWindow(..., FALSE).
// __usercall pszName@<ecx> normalised to __cdecl.  Its binary caller
// CMainFrame::OnCreateClient restores each pane rect; the port lays panes out manually.
// ─────────────────────────────────────────────────────────────────────────────
BOOL LoadWindowState( HWND hWnd, const char *pszName )
{
    int   rect[4];             // [left, top, right, bottom]  ([ebp+X], +Y, +var_10, +var_C)
    DWORD lSize = 16;          // [ebp+var_8] = 0x10  (in/out size)

    if ( !LoadRegistryInfo( pszName, rect, (long *)&lSize ) )   // 0x499ab7
        return FALSE;                                          // 0x499b19

    int left = rect[0];        // X
    if ( left < 0 ) left = rect[0] = 0;                        // 0x499ac8

    int top = rect[1];         // Y
    if ( top < 0 ) top = rect[1] = 0;                          // 0x499ad4

    int right = rect[2];       // var_10
    if ( right < left + 16 ) right = rect[2] = left + 16;      // 0x499ae3

    int bottom = rect[3];      // var_C
    if ( bottom < top + 16 ) bottom = rect[3] = top + 16;      // 0x499af2

    MoveWindow( hWnd, left, top, right - left, bottom - top, FALSE );   // 0x499b07
    return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x499600  MFCCreate - the name is the enthusiast IDB's; it creates NO windows.  It is
// the saved-preferences loader / default-seeder called by CMainFrame::OnCreate:
//   1. LoadRegistryInfo("SavedInfo", &g_qeglobals.d_savedinfo, size=0x2c4);
//   2. if the loaded iSize != 0x2c4 (no/old saved data) seed the FULL default SavedInfo_t
//      (colour palette + iTextMenu=32993 + d_gridsize + d_picmip=2), then - if the old
//      iSize was SMALLER - re-load with the old size on top, preserving what an older
//      build had saved;
//   3. CheckMenuItem the six View->Show toggles whose d_xyShowFlags bits are set
//      (menu ids 0x84b6 Connections / 0x84b3 Names / 0x84b5 Blocks / 0x84b7 Coordinates
//      / 0x84b4 Angles / 0x8d1f ReverseFilter).  Returns the last CheckMenuItem result.
// KISAK: NOT auto-called - the port's OnCreate seeds the same palette itself
// (Radiant_SetDefaultGridState) and its View->Show handlers do their own menu checks.
// Wiring this in to restore the registry-persisted SavedInfo is a follow-up.
// ─────────────────────────────────────────────────────────────────────────────
HMENU MFCCreate( void )
{
    SavedInfo_t &si = g_qeglobals.d_savedinfo;

    DWORD size = 0x2C4;                                              // 0x499619
    LoadRegistryInfo( "SavedInfo", &si, (long *)&size );             // 0x49961c

    int  oldSize = si.iSize;                                         // 0x499621
    bool wasSmaller = (unsigned)si.iSize < 0x2C4u;                   // 0x499629 (CF of cmp)
    if ( si.iSize != 0x2C4 )                                         // 0x49962b
    {
        // ── default SavedInfo_t (verbatim 0x499637..0x49987d) ──
        si.iSize = 0x2C4;                // 0x499637
        si.iTextMenu = 32993;            // 0x499643
        // 0x499653 is `mov dword [d_gridsize], 1` — an INTEGER store of the dword 1 into
        // the float field, NOT 1.0f.  Bit pattern becomes 0x00000001 (a denormal, ~1.4e-45),
        // matching the IDB's `LODWORD(d_gridsize) = 1`.  This SavedInfo_t copy of d_gridsize
        // is never used as the live grid (Radiant_SetDefaultGridState sets the live one to 5);
        // kept bit-exact for faithful registry round-trips.
        *(int *)&si.d_gridsize = 1;      // 0x499653 (bit-exact int store)
        si.d_picmip = 2;                 // 0x4997f5

        si.colors[0][0] = 0.25f;  si.colors[0][1] = 0.25f;  si.colors[0][2] = 0.25f;  si.colors[0][3] = 1.0f;
        si.colors[1][0] = 1.0f;   si.colors[1][1] = 1.0f;   si.colors[1][2] = 1.0f;   si.colors[1][3] = 1.0f;
        si.colors[2][0] = 0.75f;  si.colors[2][1] = 0.75f;  si.colors[2][2] = 0.75f;  si.colors[2][3] = 1.0f;
        si.colors[3][0] = 0.5f;   si.colors[3][1] = 0.5f;   si.colors[3][2] = 0.5f;   si.colors[3][3] = 1.0f;
        si.colors[4][0] = 0.25f;  si.colors[4][1] = 0.25f;  si.colors[4][2] = 0.25f;  si.colors[4][3] = 1.0f;
        // colors[5] / colors[6] left untouched by the binary
        si.colors[7][0] = 0.0f;   si.colors[7][1] = 0.0f;   si.colors[7][2] = 1.0f;   si.colors[7][3] = 1.0f;
        si.colors[8][0] = 0.0f;   si.colors[8][1] = 0.0f;   si.colors[8][2] = 0.0f;   si.colors[8][3] = 1.0f;
        si.colors[9][0] = 0.0f;   si.colors[9][1] = 0.0f;   si.colors[9][2] = 0.0f;   si.colors[9][3] = 1.0f;
        si.colors[10][0] = 1.0f;  si.colors[10][1] = 0.0f;  si.colors[10][2] = 0.0f;  si.colors[10][3] = 1.0f;
        si.colors[11][0] = 1.0f;  si.colors[11][1] = 0.25f; si.colors[11][2] = 0.25f; si.colors[11][3] = 0.25f;
        si.colors[12][0] = 0.0f;  si.colors[12][1] = 0.0f;  si.colors[12][2] = 1.0f;  si.colors[12][3] = 1.0f;
        si.colors[13][0] = 0.5f;  si.colors[13][1] = 0.0f;  si.colors[13][2] = 0.75f; si.colors[13][3] = 1.0f;
        si.colors[14][0] = 0.0f;  si.colors[14][1] = 0.60000002f; si.colors[14][2] = 0.0f; si.colors[14][3] = 1.0f;
        // colors[15] left untouched
        si.colors[16][0] = 1.0f;  si.colors[16][1] = 0.25f; si.colors[16][2] = 0.25f; si.colors[16][3] = 0.25f;
        si.colors[17][0] = 0.75f; si.colors[17][1] = 0.75f; si.colors[17][2] = 0.75f; si.colors[17][3] = 1.0f;
        si.colors[18][0] = 0.75f; si.colors[18][1] = 0.75f; si.colors[18][2] = 0.75f; si.colors[18][3] = 1.0f;
        si.colors[19][0] = 0.5f;  si.colors[19][1] = 0.60000002f; si.colors[19][2] = 0.0f; si.colors[19][3] = 1.0f;
        si.colors[20][0] = 0.64999998f; si.colors[20][1] = 0.0f; si.colors[20][2] = 0.0f; si.colors[20][3] = 1.0f;
        si.colors[21][0] = 0.85000002f; si.colors[21][1] = 0.0f; si.colors[21][2] = 0.85000002f; si.colors[21][3] = 1.0f;
        si.colors[22][0] = 0.80000001f; si.colors[22][1] = 0.60000002f; si.colors[22][2] = 0.0f; si.colors[22][3] = 1.0f;

        if ( wasSmaller )                                            // 0x499883
        {
            DWORD v4 = (DWORD)oldSize;                               // 0x499885
            LoadRegistryInfo( "SavedInfo", &si, (long *)&v4 );       // 0x499895
        }
    }

    HMENU hMenu = GetMenu( g_qeglobals.d_hwndMain );                 // 0x4998a3
    if ( hMenu )                                                     // 0x4998ad
    {
        HMENU v3 = hMenu;
        if ( si.d_xyShowFlags & 0x4 )    hMenu = (HMENU)(UINT_PTR)CheckMenuItem( v3, 0x84B6, 0 ); // Connections
        if ( si.d_xyShowFlags & 0x8 )    hMenu = (HMENU)(UINT_PTR)CheckMenuItem( v3, 0x84B3, 0 ); // Names
        if ( si.d_xyShowFlags & 0x10 )   hMenu = (HMENU)(UINT_PTR)CheckMenuItem( v3, 0x84B5, 0 ); // Blocks
        if ( si.d_xyShowFlags & 0x20 )   hMenu = (HMENU)(UINT_PTR)CheckMenuItem( v3, 0x84B7, 0 ); // Coordinates
        if ( si.d_xyShowFlags & 0x2 )    hMenu = (HMENU)(UINT_PTR)CheckMenuItem( v3, 0x84B4, 0 ); // Angles
        if ( si.d_xyShowFlags & 0x40 )   hMenu = (HMENU)(UINT_PTR)CheckMenuItem( v3, 0x8D1F, 0 ); // ReverseFilter
    }
    return hMenu;
}
