#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_loadprogress.cpp — see kiwi_loadprogress.h.  New code: no thread, no COM,
// nothing renderer-touching.

#include "stdafx.h"
#include "qe3.h"                   // g_qeglobals — d_hwndMain qe3.h:909

#include "kiwi_loadprogress.h"

#include <string.h>
#include <stdio.h>

// Bracket depth: nested loads (a map whose prefabs load further prefabs) must not
// let an inner End() restore the caption while the outer load is still running.
static int  s_depth = 0;

// The subject of the OUTERMOST bracket ("mp_backlot.map").
static char s_what[260];

// The caption to put back at the outermost End().  Seeded from the frame's title at
// the outermost Begin(), and overwritten by KiwiLoadProgress_SetTitle so a loader that
// renames the window mid-bracket is not reverted.
static char s_baseTitle[512];

// Caption-update throttle: every update is a WM_SETTEXT plus a non-client repaint.
static DWORD s_lastPaintMs = 0;
enum { KLP_THROTTLE_MS = 100 };

// Copy `src` into `dst`, stopping at the first newline and squashing the rest of the
// whitespace, so a multi-line Com_PrintError never smears the title bar.
static void KiwiLoadProgress_OneLine( char *dst, int dstSize, const char *src )
{
    if ( dstSize <= 0 )
        return;
    // Terminate FIRST: a line can arrive from a non-main thread mid-copy, so the worst a
    // race can do is show a truncated status line rather than an unterminated buffer.
    dst[0] = '\0';
    int o = 0;
    while ( *src == ' ' || *src == '\t' || *src == '\r' || *src == '\n' )
        ++src;
    for ( ; *src && o < dstSize - 1; ++src )
    {
        if ( *src == '\r' || *src == '\n' )
            break;
        dst[o++] = ( *src == '\t' ) ? ' ' : *src;
    }
    // Trim the trailing run of spaces the squash above can leave behind.
    while ( o > 0 && dst[o - 1] == ' ' )
        --o;
    dst[o] = '\0';
}

// Insurance, run ONCE per outermost load: a long synchronous load is the one window in
// which the pump (and so ImGuiShell_ApplyViewportDocks' per-frame sweep) cannot hide these.
static void KiwiLoadProgress_HideLegacyPanes()
{
    HWND panes[] = { g_qeglobals.d_hwndEdit,     // the QE3 console EDIT — the white strip
                     g_qeglobals.d_hwndCamera,   // the four RTT'd render panes: every one of
                     g_qeglobals.d_hwndXY,       // them is an ImGui image now and the native
                     g_qeglobals.d_hwndZ,        // child is kept only for its HWND-shaped uses
                     g_qeglobals.d_hwndTexture };
    for ( int i = 0; i < (int)( sizeof( panes ) / sizeof( panes[0] ) ); ++i )
        if ( panes[i] && ::IsWindowVisible( panes[i] ) )
            ::ShowWindow( panes[i], SW_HIDE );
}

void KiwiLoadProgress_Begin( const char *what )
{
    if ( s_depth++ != 0 )
        return;                                   // nested — the outer bracket owns the caption

    KiwiLoadProgress_HideLegacyPanes();

    s_what[0] = '\0';
    if ( what )
        KiwiLoadProgress_OneLine( s_what, (int)sizeof( s_what ), what );

    s_baseTitle[0] = '\0';
    HWND frame = g_qeglobals.d_hwndMain;
    if ( frame )
        ::GetWindowTextA( frame, s_baseTitle, (int)sizeof( s_baseTitle ) );

    // Paint the first caption immediately (no throttle).
    s_lastPaintMs = 0;
    KiwiLoadProgress_Note( "" );
}

void KiwiLoadProgress_Note( const char *line )
{
    if ( s_depth <= 0 )
        return;
    HWND frame = g_qeglobals.d_hwndMain;
    if ( !frame )
        return;

    const DWORD now = ::GetTickCount();
    if ( s_lastPaintMs && ( now - s_lastPaintMs ) < (DWORD)KLP_THROTTLE_MS )
        return;
    s_lastPaintMs = now ? now : 1;                // never latch 0 back into "unthrottled"

    char tail[200];
    tail[0] = '\0';
    if ( line )
        KiwiLoadProgress_OneLine( tail, (int)sizeof( tail ), line );

    char caption[512];
    if ( tail[0] )
        _snprintf( caption, sizeof( caption ), "Loading %s ... %s", s_what, tail );
    else
        _snprintf( caption, sizeof( caption ), "Loading %s ...", s_what );
    caption[sizeof( caption ) - 1] = '\0';

    // SetWindowTextA on our OWN window is a direct WM_SETTEXT to DefWindowProc, so the
    // caption updates even though the message pump is blocked inside the load.
    ::SetWindowTextA( frame, caption );
}

void KiwiLoadProgress_SetTitle( const char *title )
{
    if ( !title )
        return;

    // Outside a bracket this is just SetWindowTextA — the caller's original behaviour.
    if ( s_depth <= 0 )
    {
        if ( g_qeglobals.d_hwndMain )
            ::SetWindowTextA( g_qeglobals.d_hwndMain, title );
        return;
    }

    // Inside one, remember it as the caption to settle on at End() and keep showing
    // progress until then.
    _snprintf( s_baseTitle, sizeof( s_baseTitle ), "%s", title );
    s_baseTitle[sizeof( s_baseTitle ) - 1] = '\0';
}

void KiwiLoadProgress_End()
{
    if ( s_depth <= 0 )
    {
        s_depth = 0;                              // unbalanced End — stay latched off
        return;
    }
    if ( --s_depth != 0 )
        return;                                   // still inside an outer bracket

    if ( g_qeglobals.d_hwndMain && s_baseTitle[0] )
        ::SetWindowTextA( g_qeglobals.d_hwndMain, s_baseTitle );
    s_lastPaintMs = 0;
}
