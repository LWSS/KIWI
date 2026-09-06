#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI perf HUD — where does a frame go?
//
// A fixed table of named timers (QueryPerformanceCounter, milliseconds, smoothed over
// ~30 frames) around the editor's per-frame stages: the four RTT renders, the
// reference-image draw, the ImGui overlay, and whatever else a caller wraps.  The
// "KiwiPerf" command (palette / console) toggles a line in the camera's bottom band
// that lists the slots sorted by cost, the frame time, and the g_nUpdateBits value the
// tick started with (which tells whether the 2D views are being re-rendered every
// frame).  Off by default; costs nothing but the two counter reads per slot when on.
//
// Usage:  { KiwiPerfScope scope( KPERF_CAMERA ); CamWnd_RenderToRT( ... ); }
enum kiwiPerfSlot_t
{
    KPERF_CAMERA = 0,     // CamWnd_RenderToRT (every tick)
    KPERF_XY,             // XYWnd_RenderToRT (dirty ticks only)
    KPERF_Z,              // ZWnd_RenderToRT
    KPERF_TEXTURE,        // TexWnd_RenderToRT
    KPERF_THUMBS,         // entity-browser model thumbnails
    KPERF_REFIMAGES,      // KiwiRefImage_DrawWorld inside the camera
    KPERF_TERRAIN_DRAW,   // KiwiTerrain_DrawWorld (wireframe + weight overlay)
    KPERF_OVERLAY,        // ImGuiShell_DrawOverlay (the whole ImGui frame)
    KPERF_COUNT
};

bool KiwiPerf_Enabled();
void KiwiPerf_Toggle();
// Manual bracket; the scope below is the usual form.
void KiwiPerf_Begin( kiwiPerfSlot_t slot );
void KiwiPerf_End( kiwiPerfSlot_t slot );
// Once per tick, before the renders: remember the update bits the tick drains.
void KiwiPerf_TickBegin( int updateBits );
// The camera overlay line (kiwi_viewport.cpp KiwiVP_DrawCameraOverlay, bottom band).
void KiwiPerf_Draw( float imgMinX, float imgMinY, float imgW, float imgH );

struct KiwiPerfScope
{
    kiwiPerfSlot_t slot;
    explicit KiwiPerfScope( kiwiPerfSlot_t s ) : slot( s ) { KiwiPerf_Begin( s ); }
    ~KiwiPerfScope() { KiwiPerf_End( slot ); }
};
