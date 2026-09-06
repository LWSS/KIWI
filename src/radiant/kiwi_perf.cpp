#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI perf HUD — see kiwi_perf.h.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include "kiwi_perf.h"
#include "kiwi_hints.h"      // KiwiHud_BandTake - the bottom band

#include <stdio.h>
#include <string.h>

extern int Sys_Printf( const char *fmt, ... );

namespace
{
    const char *const KPERF_NAME[KPERF_COUNT] =
        { "camera", "xy", "z", "texture", "thumbs", "refimages", "terrain", "imgui" };

    bool          s_enabled = false;
    LARGE_INTEGER s_freq    = { 0 };
    LARGE_INTEGER s_start[KPERF_COUNT];
    double        s_lastMs[KPERF_COUNT];      // this tick's raw sample
    double        s_avgMs[KPERF_COUNT];       // ~30-frame exponential average
    bool          s_ran[KPERF_COUNT];         // sampled this tick at all
    bool          s_ranAvg[KPERF_COUNT];      // sampled within the averaging window
    int           s_tickBits = 0;
    double        s_frameAvgMs = 0.0;
    float         s_lastFrameTime = -1.0f;

    double Freq()
    {
        if ( !s_freq.QuadPart )
            QueryPerformanceFrequency( &s_freq );
        return (double)s_freq.QuadPart;
    }
}

bool KiwiPerf_Enabled()
{
    return s_enabled;
}

void KiwiPerf_Toggle()
{
    s_enabled = !s_enabled;
    if ( s_enabled )
    {
        memset( s_avgMs, 0, sizeof( s_avgMs ) );
        memset( s_ranAvg, 0, sizeof( s_ranAvg ) );
        s_frameAvgMs = 0.0;
    }
    Sys_Printf( "KIWI perf HUD: %s\n", s_enabled ? "ON (camera bottom band)" : "off" );
}

void KiwiPerf_Begin( kiwiPerfSlot_t slot )
{
    if ( !s_enabled || slot < 0 || slot >= KPERF_COUNT )
        return;
    QueryPerformanceCounter( &s_start[slot] );
}

void KiwiPerf_End( kiwiPerfSlot_t slot )
{
    if ( !s_enabled || slot < 0 || slot >= KPERF_COUNT || !s_start[slot].QuadPart )
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    const double ms = (double)( now.QuadPart - s_start[slot].QuadPart ) * 1000.0 / Freq();
    s_start[slot].QuadPart = 0;
    s_lastMs[slot] = ms;
    s_ran[slot]    = true;
    s_ranAvg[slot] = true;
    s_avgMs[slot]  = s_avgMs[slot] > 0.0 ? s_avgMs[slot] + ( ms - s_avgMs[slot] ) / 30.0 : ms;
}

void KiwiPerf_TickBegin( int updateBits )
{
    if ( !s_enabled )
        return;
    s_tickBits = updateBits;
    for ( int i = 0; i < KPERF_COUNT; ++i )
        s_ran[i] = false;
}

void KiwiPerf_Draw( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( !s_enabled )
        return;
    // Frame time from ImGui's own clock (the whole pump, not just the renders).
    const float dt = ImGui::GetIO().DeltaTime;
    if ( dt > 0.0f )
        s_frameAvgMs = s_frameAvgMs > 0.0 ? s_frameAvgMs + ( dt * 1000.0 - s_frameAvgMs ) / 30.0 : dt * 1000.0;

    // Sort the slots by average cost, most expensive first.
    int order[KPERF_COUNT];
    for ( int i = 0; i < KPERF_COUNT; ++i ) order[i] = i;
    for ( int i = 0; i < KPERF_COUNT; ++i )
        for ( int j = i + 1; j < KPERF_COUNT; ++j )
            if ( s_avgMs[order[j]] > s_avgMs[order[i]] )
            {
                const int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    char line1[256], line2[256];
    _snprintf( line1, sizeof( line1 ), "PERF  frame %.1f ms (%.0f fps)   update bits 0x%X",
               s_frameAvgMs, s_frameAvgMs > 0.0 ? 1000.0 / s_frameAvgMs : 0.0, (unsigned)s_tickBits );
    line1[sizeof( line1 ) - 1] = 0;
    int o = 0;
    line2[0] = 0;
    for ( int k = 0; k < KPERF_COUNT && o < (int)sizeof( line2 ) - 40; ++k )
    {
        const int i = order[k];
        if ( !s_ranAvg[i] )
            continue;
        o += _snprintf( line2 + o, sizeof( line2 ) - (size_t)o, "%s%s %.2f%s",
                        o ? "   " : "", KPERF_NAME[i], s_avgMs[i], s_ran[i] ? "" : " (skipped)" );
    }
    line2[sizeof( line2 ) - 1] = 0;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const float boxH  = lineH * 2.0f + 8.0f;
    const float top   = KiwiHud_BandTake( boxH, imgMinY + imgH - boxH - 8.0f );
    const ImVec2 s1 = ImGui::CalcTextSize( line1 ), s2 = ImGui::CalcTextSize( line2 );
    const float w = ( s1.x > s2.x ? s1.x : s2.x ) + 12.0f;
    const float x0 = imgMinX + 8.0f;
    dl->AddRectFilled( ImVec2( x0, top ), ImVec2( x0 + w, top + boxH ), IM_COL32( 18, 18, 22, 190 ), 3.0f );
    dl->AddText( ImVec2( x0 + 6.0f, top + 4.0f ), IM_COL32( 255, 226, 110, 255 ), line1 );
    dl->AddText( ImVec2( x0 + 6.0f, top + 4.0f + lineH ), IM_COL32( 220, 224, 232, 235 ), line2 );
}
