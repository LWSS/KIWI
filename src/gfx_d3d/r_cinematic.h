#pragma once

#include <stdint.h>

// KIWI: Bink support removed.

enum CinematicEnum : __int32
{                                       // ...
    CINEMATIC_NOT_PAUSED = 0x0,
    CINEMATIC_PAUSED = 0x1,
};

struct CinematicGlob
{
    char currentCinematicName[256];     // read by UI subtitle code (ui_shared.cpp)
    uint32_t timeInMsec;                // read by UI subtitle code (ui_shared.cpp)
};

extern CinematicGlob cinematicGlob;

void __cdecl R_Cinematic_Init();
void __cdecl R_Cinematic_Shutdown();
void __cdecl R_Cinematic_StartPlayback(char *name, uint32_t playbackFlags, float volume);
void __cdecl R_Cinematic_StartNextPlayback();
void __cdecl R_Cinematic_StopPlayback();
void __cdecl R_Cinematic_UpdateFrame();
void __cdecl R_Cinematic_SyncNow();
void __cdecl R_Cinematic_DrawStretchPic_Letterboxed();
bool __cdecl R_Cinematic_IsFinished();
bool __cdecl R_Cinematic_IsStarted();
bool R_Cinematic_IsPending();
bool __cdecl R_Cinematic_IsNextReady();
bool __cdecl R_Cinematic_IsUnderrun();
void __cdecl R_Cinematic_BeginLostDevice();
void __cdecl R_Cinematic_EndLostDevice();
void __cdecl R_Cinematic_SetPaused(CinematicEnum paused);
void R_Cinematic_SetNextPlayback(const char *name, uint32_t playbackFlags);
void R_Cinematic_UnsetNextPlayback();
