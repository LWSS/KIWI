#include <universal/q_shared.h>
#include "r_cinematic.h"

#include "r_init.h"
#include "rb_state.h"

// KIWI: Bink support removed (see r_cinematic.h). Stubs behave as "nothing is playing and whatever was requested already finished"

CinematicGlob cinematicGlob;

void __cdecl R_Cinematic_Init() {}
void __cdecl R_Cinematic_Shutdown() {}
void __cdecl R_Cinematic_StartPlayback(char *name, uint32_t playbackFlags, float volume) {}
void __cdecl R_Cinematic_StartNextPlayback() {}
void __cdecl R_Cinematic_StopPlayback() {}

// Keep the cinematic code images bound to something valid every frame - materials with
// cinematic samplers otherwise fatal in R_TextureFromCodeError ("Tried to use 'cinematicY'
// when it isn't valid"). Same black/gray/gray/black set the no-frame path used originally.
void __cdecl R_Cinematic_UpdateFrame()
{
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = rgp.blackImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = rgp.grayImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = rgp.grayImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.blackImage;
}

void __cdecl R_Cinematic_SyncNow() {}
void __cdecl R_Cinematic_DrawStretchPic_Letterboxed() {}
bool __cdecl R_Cinematic_IsFinished() { return true; }
bool __cdecl R_Cinematic_IsStarted() { return false; }
bool R_Cinematic_IsPending() { return false; }
bool __cdecl R_Cinematic_IsNextReady() { return false; }
bool __cdecl R_Cinematic_IsUnderrun() { return false; }
void __cdecl R_Cinematic_BeginLostDevice() {}
void __cdecl R_Cinematic_EndLostDevice() {}
void __cdecl R_Cinematic_SetPaused(CinematicEnum paused) {}
void R_Cinematic_SetNextPlayback(const char *name, uint32_t playbackFlags) {}
void R_Cinematic_UnsetNextPlayback() {}
