#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Synchronous load feedback uses only the native frame title; it does not pump or touch the renderer.

#include "stdafx.h"
#include "qe3.h"                   // g_qeglobals — d_hwndMain qe3.h:909

#include "kiwi_loadprogress.h"

#include <string.h>
#include <stdio.h>

// Nested loads leave the caption owned by the outermost bracket.
static int  s_depth = 0;

static char s_what[260];

// Title restored by the outermost End(); SetTitle updates it during a load.
static char s_baseTitle[512];

// Caption-update throttle: every update is a WM_SETTEXT plus a non-client repaint.
static DWORD s_lastPaintMs = 0;
enum { KLP_THROTTLE_MS = 100 };

// Copy one trimmed line, converting tabs to spaces.
static void KiwiLoadProgress_OneLine( char *dst, int dstSize, const char *src )
{
    if ( dstSize <= 0 )
        return;
    // Publish an empty string first in case another thread reads the destination mid-copy.
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
    while ( o > 0 && dst[o - 1] == ' ' )
        --o;
    dst[o] = '\0';
}

// The blocked pump cannot run its per-frame legacy-pane hider during a load.
static void KiwiLoadProgress_HideLegacyPanes()
{
    HWND panes[] = { g_qeglobals.d_hwndEdit,     // legacy console pane
                     g_qeglobals.d_hwndCamera,   // ImGui-backed render panes
                     g_qeglobals.d_hwndXY,       // native HWND backing
                     g_qeglobals.d_hwndZ,        // native HWND backing
                     g_qeglobals.d_hwndTexture };
    for ( int i = 0; i < (int)( sizeof( panes ) / sizeof( panes[0] ) ); ++i )
        if ( panes[i] && ::IsWindowVisible( panes[i] ) )
            ::ShowWindow( panes[i], SW_HIDE );
}

void KiwiLoadProgress_Begin( const char *what )
{
    if ( s_depth++ != 0 )
        return;                                   // the outer bracket owns the caption

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

    // Same-thread SetWindowTextA dispatches WM_SETTEXT directly; no message pump is needed.
    ::SetWindowTextA( frame, caption );
}

void KiwiLoadProgress_SetTitle( const char *title )
{
    if ( !title )
        return;

    // Preserve direct SetWindowTextA behavior outside a load.
    if ( s_depth <= 0 )
    {
        if ( g_qeglobals.d_hwndMain )
            ::SetWindowTextA( g_qeglobals.d_hwndMain, title );
        return;
    }

    // End() restores the most recent requested title.
    _snprintf( s_baseTitle, sizeof( s_baseTitle ), "%s", title );
    s_baseTitle[sizeof( s_baseTitle ) - 1] = '\0';
}

void KiwiLoadProgress_End()
{
    if ( s_depth <= 0 )
    {
        s_depth = 0;                              // unbalanced End stays latched off
        return;
    }
    if ( --s_depth != 0 )
        return;                                   // still inside an outer bracket

    if ( g_qeglobals.d_hwndMain && s_baseTitle[0] )
        ::SetWindowTextA( g_qeglobals.d_hwndMain, s_baseTitle );
    s_lastPaintMs = 0;
}
