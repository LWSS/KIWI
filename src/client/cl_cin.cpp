#include <universal/q_shared.h>
#include "client.h"
#include <sound/snd_public.h>
#include <gfx_d3d/r_cinematic.h>
#include <qcommon/cmd.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#endif

bool cin_skippable;

int __cdecl CIN_PlayCinematic(int localClientNum, char *arg)
{
    float volume; // [esp+4h] [ebp-4h]

    iassert(arg);
    volume = SND_GetVolumeNormalized() * snd_cinematicVolumeScale->current.value;
    R_Cinematic_StartPlayback(arg, 5u, volume);
    if (cls.uiStarted)
        UI_SetActiveMenu(localClientNum, UIMENU_NONE);
    Con_Close(localClientNum);
    vassert((localClientNum) == 0, "%i not in [0, %i)", localClientNum, 1);
    clientUIActives[localClientNum].connectionState = CA_CINEMATIC;
    return 1;
}

void __cdecl CL_PlayCinematic_f()
{
    char *arg1; // [esp+0h] [ebp-Ch]

    cin_skippable = 1;
    arg1 = (char *)Cmd_Argv(1);
    Cmd_Argv(2);
    if (!arg1 || *arg1)
        CIN_PlayCinematic(0, arg1);
    else
        Com_Printf(0, "No cinematic file specified: cinematic filename [1/2/3]\n");
}

void __cdecl CL_PlayUnskippableCinematic_f()
{
    CL_PlayCinematic_f();
    cin_skippable = 0;
}

void __cdecl SCR_DrawCinematic(int localClientNum)
{
    if (R_Cinematic_IsNextReady())
        R_Cinematic_StartNextPlayback();
    if (R_Cinematic_IsFinished())
        SCR_StopCinematic(localClientNum);
    if (R_Cinematic_IsStarted())
        R_Cinematic_DrawStretchPic_Letterboxed();
}

void __cdecl SCR_StopCinematic(int localClientNum)
{
    const char *v1; // eax

    if (cin_skippable || R_Cinematic_IsFinished())
    {
        R_Cinematic_StopPlayback();
        vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
        if (clientUIActives[0].connectionState == CA_CINEMATIC)
        {
            vassert((localClientNum) == 0, "%i not in [0, %i)", localClientNum, 1);
            clientUIActives[localClientNum].connectionState = CA_DISCONNECTED;
            if (nextmap->current.integer)
            {
                v1 = va("%s\n", nextmap->current.string);
                Cbuf_AddText(0, v1);
                Dvar_SetString(nextmap, (char*)"");
            }
        }
    }
}

