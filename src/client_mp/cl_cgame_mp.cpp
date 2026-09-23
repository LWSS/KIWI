#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include <universal/profile.h>
#include "client_mp.h"

#include <cgame_mp/cg_local_mp.h>
#include <client/client.h>

#include <qcommon/cmd.h>
#include <qcommon/mem_track.h>

#include <gfx_d3d/r_rendercmds.h>

#include <xanim/xanim.h>
#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <stringed/stringed_hooks.h>
#include <qcommon/com_bsp.h>
#include <universal/com_sndalias.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_bsp.h>
#include <database64/database.h>
#include <universal/com_files.h>
#include <universal/q_parse.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_fog.h>
#include <universal/profile.h>

float color_allies[4];
float color_axis[4];

char bigConfigString[8192];
const float g_color_table[8][4]
{
    { 0.0f, 0.0f, 0.0f, 1.0f },
    { 1.0f, 0.36f, 0.36f, 1.0f },
    { 0.0f, 1.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 1.0f },
    { 0.0f, 0.0f, 1.0f, 1.0f },
    { 0.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 0.36f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f }
};

void __cdecl TRACK_cl_cgame()
{
    track_static_alloc_internal(bigConfigString, sizeof(bigConfigString), "bigConfigString", 9);
    track_static_alloc_internal((void *)g_color_table, sizeof(g_color_table), "g_color_table", 10);
}

void __cdecl CL_GetScreenDimensions(int *width, int *height, float *aspect)
{
    iassert(width);
    iassert(height);
    iassert(aspect);
    *width = cls.vidConfig.displayWidth;
    *height = cls.vidConfig.displayHeight;
    *aspect = cls.vidConfig.aspectRatioWindow;
}

double __cdecl CL_GetScreenAspectRatioDisplayPixel()
{
    return cls.vidConfig.aspectRatioDisplayPixel;
}

/*
====================
CL_GetUserCmd
====================
*/
int __cdecl CL_GetUserCmd(int localClientNum, int cmdNumber, usercmd_s *ucmd)
{
    clientActive_t *LocalClientGlobals; // [esp+8h] [ebp-4h]

    // cmds[cmdNumber] is the last properly generated command

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    // can't return anything that we haven't created yet
    if (cmdNumber > LocalClientGlobals->cmdNumber)
        Com_Error(ERR_DROP, "CL_GetUserCmd: %i >= %i", cmdNumber, LocalClientGlobals->cmdNumber);

    // the usercmd has been overwritten in the wrapping
    // buffer because it is too far out of date
    if (cmdNumber <= LocalClientGlobals->cmdNumber - 128)
        return 0;
    memcpy(ucmd, &LocalClientGlobals->cmds[cmdNumber & 0x7F], sizeof(usercmd_s));
    return 1;
}

int __cdecl CL_GetCurrentCmdNumber(int localClientNum)
{
    return CL_GetLocalClientGlobals(localClientNum)->cmdNumber;
}

/*
====================
CL_GetCurrentSnapshotNumber
====================
*/
void __cdecl CL_GetCurrentSnapshotNumber(int localClientNum, int *snapshotNumber, int *serverTime)
{
    clientActive_t *LocalClientGlobals; // eax

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    *snapshotNumber = LocalClientGlobals->snap.messageNum;
    *serverTime = LocalClientGlobals->snap.serverTime;
}

/*
====================
CL_GetSnapshot
====================
*/
int __cdecl CL_GetSnapshot(int localClientNum, int snapshotNumber, snapshot_s *snapshot)
{
    const char *v4; // eax
    uint number; // [esp+8h] [ebp-418h]
    clientActive_t *LocalClientGlobals; // [esp+Ch] [ebp-414h]
    bool entityFound[1024]; // [esp+10h] [ebp-410h] BYREF
    const clSnapshot_t *clSnap; // [esp+414h] [ebp-Ch]
    int i; // [esp+418h] [ebp-8h]
    int count; // [esp+41Ch] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    if (snapshotNumber > LocalClientGlobals->snap.messageNum)
        Com_Error(ERR_DROP, "CL_GetSnapshot: snapshotNumber > cl->snapshot.messageNum");
    // if the frame has fallen out of the circular buffer, we can't return it
    if (LocalClientGlobals->snap.messageNum - snapshotNumber >= 32)
        return 0;

    // if the frame is not valid, we can't return it
    clSnap = &LocalClientGlobals->snapshots[snapshotNumber & 0x1F];
    if (!clSnap->valid)
        return 0;

    // if the entities in the frame have fallen out of their
    // circular buffer, we can't return it
    if (LocalClientGlobals->parseEntitiesNum - clSnap->parseEntitiesNum >= 2048)
        return 0;
    if (LocalClientGlobals->parseClientsNum - clSnap->parseClientsNum >= 2048)
        return 0;

    // write the snapshot
    snapshot->snapFlags = clSnap->snapFlags;
    snapshot->serverCommandSequence = clSnap->serverCommandNum;
    snapshot->ping = clSnap->ping;
    snapshot->serverTime = clSnap->serverTime;
    memcpy((uint8_t *)&snapshot->ps, (uint8_t *)&clSnap->ps, sizeof(snapshot->ps));
    count = clSnap->numEntities;
    if (count > 512)
    {
        if (com_statmon->current.enabled)
            StatMon_Warning(4, 3000, "code_warning_snapshotents");
        else
            Com_DPrintf(CON_CHANNEL_CLIENT, "CL_GetSnapshot: truncated %i entities to %i\n", count, 512);
        count = 512;
    }
    snapshot->numEntities = count;
    memset((uint8_t *)entityFound, 0, sizeof(entityFound));
    for (i = 0; i < count; ++i)
    {
        memcpy(
            &snapshot->entities[i],
            &LocalClientGlobals->parseEntities[((_WORD)i + (uint16_t)clSnap->parseEntitiesNum) & 0x7FF],
            sizeof(snapshot->entities[i]));
        number = snapshot->entities[i].number;
        if (number >= 0x400)
            MyAssertHandler(
                ".\\client_mp\\cl_cgame_mp.cpp",
                221,
                0,
                "%s\n\t(number) = %i",
                "(( number >= 0 && number < (1<<10) ))",
                number);
        if (entityFound[number])
        {
            v4 = va("EntityNum %i was found twice in this snapshot", number);
            MyAssertHandler(".\\client_mp\\cl_cgame_mp.cpp", 222, 0, "%s\n\t%s", "!entityFound[ number ]", v4);
        }
        entityFound[number] = 1;
    }
    count = clSnap->numClients;
    if (count > 64)
        count = 64;
    memset((uint8_t *)snapshot->clients, 0xAAu, sizeof(snapshot->clients));
    snapshot->numClients = count;
    for (i = 0; i < count; ++i)
        memcpy(
            &snapshot->clients[i],
            &LocalClientGlobals->parseClients[((_WORD)i + (uint16_t)clSnap->parseClientsNum) & 0x7FF],
            sizeof(snapshot->clients[i]));

    // FIXME: configstring changes and server commands!!!

    return 1;
}

void __cdecl CL_SetUserCmdWeapons(int localClientNum, int weapon, int offHandIndex)
{
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    LocalClientGlobals->cgameUserCmdWeapon = weapon;
    LocalClientGlobals->cgameUserCmdOffHandIndex = offHandIndex;
}

void __cdecl CL_SetUserCmdAimValues(int localClientNum, const float *kickAngles)
{
    clientActive_t *LocalClientGlobals; // [esp+4h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    LocalClientGlobals->cgameKickAngles[0] = *kickAngles;
    LocalClientGlobals->cgameKickAngles[1] = kickAngles[1];
    LocalClientGlobals->cgameKickAngles[2] = kickAngles[2];
}

void __cdecl CL_SetUserCmdOrigin(
    int localClientNum,
    const float *origin,
    const float *velocity,
    const float *viewangles,
    int bobCycle,
    int movementDir)
{
    clientActive_t *LocalClientGlobals; // eax

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    LocalClientGlobals->cgamePredictedDataServerTime = LocalClientGlobals->serverTime;
    LocalClientGlobals->cgameOrigin[0] = *origin;
    LocalClientGlobals->cgameOrigin[1] = origin[1];
    LocalClientGlobals->cgameOrigin[2] = origin[2];
    LocalClientGlobals->cgameVelocity[0] = *velocity;
    LocalClientGlobals->cgameVelocity[1] = velocity[1];
    LocalClientGlobals->cgameVelocity[2] = velocity[2];
    LocalClientGlobals->cgameBobCycle = bobCycle;
    LocalClientGlobals->cgameMovementDir = movementDir;
    LocalClientGlobals->cgameViewangles[0] = *viewangles;
    LocalClientGlobals->cgameViewangles[1] = viewangles[1];
    LocalClientGlobals->cgameViewangles[2] = viewangles[2];
}

void __cdecl CL_SetFOVSensitivityScale(int localClientNum, float scale)
{
    CL_GetLocalClientGlobals(localClientNum)->cgameFOVSensitivityScale = scale;
}

void __cdecl CL_SetExtraButtons(int localClientNum, int buttons)
{
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    LocalClientGlobals->cgameExtraButtons |= buttons;
}

void __cdecl CL_DumpReliableCommands(int localClientNum)
{
    clientConnection_t *clc; // [esp+0h] [ebp-8h]
    int i; // [esp+4h] [ebp-4h]

    clc = CL_GetLocalClientConnection(localClientNum);
    for (i = 0; i < 128; ++i)
        Com_PrintError(CON_CHANNEL_ERROR, "cmd %5d: '%s'\n", i, clc->serverCommands[i]);
}

int __cdecl CL_CGameNeedsServerCommand(int localClientNum, int serverCommandNumber)
{
    int result; // eax
    const char *v3; // eax
    char *v4; // eax
    char *v5; // eax
    const char *v6; // eax
    char *v7; // eax
    const char *v8; // eax
    const char *v9; // [esp-4h] [ebp-9Ch]
    const char *v10; // [esp-4h] [ebp-9Ch]
    clientActive_t *LocalClientGlobals; // [esp+84h] [ebp-14h]
    clientConnection_t *clc; // [esp+88h] [ebp-10h]
    char *s; // [esp+8Ch] [ebp-Ch]
    const char *sa; // [esp+8Ch] [ebp-Ch]
    const char *sb; // [esp+8Ch] [ebp-Ch]
    const char *cmd; // [esp+90h] [ebp-8h]
    int argc; // [esp+94h] [ebp-4h]

    clc = CL_GetLocalClientConnection(localClientNum);
    if (serverCommandNumber <= clc->serverCommandSequence - 128)
    {
        if (clc->demoplaying)
            return 0;
        Com_Printf(CON_CHANNEL_CLIENT, "===== CL_CGameNeedsServerCommand =====\n");
        Com_Printf(CON_CHANNEL_CLIENT, "serverCommandNumber: %d\n", serverCommandNumber & 0x7F);
        CL_DumpReliableCommands(localClientNum);
        Com_Error(ERR_DROP, "CL_CGameNeedsServerCommand: EXE_ERR_RELIABLE_CYCLED_OUT");
    }
    if (serverCommandNumber > clc->serverCommandSequence)
        Com_Error(ERR_DROP, "CL_CGameNeedsServerCommand: EXE_ERRO_NOT_RECEIVED");
    s = clc->serverCommands[serverCommandNumber & 0x7F];
    clc->lastExecutedServerCommand = serverCommandNumber;
    if (cl_showServerCommands->current.enabled)
        Com_DPrintf(CON_CHANNEL_CLIENT, "serverCommand: %i : %s\n", serverCommandNumber, s);
    while (2)
    {
        Cmd_TokenizeString(s);
        cmd = Cmd_Argv(0);
        argc = Cmd_Argc();
        switch (*cmd)
        {
        case 'B':
        case 'n':
            Con_ClearNotify(localClientNum);
            LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
            memset((uint8_t *)LocalClientGlobals->cmds, 0, sizeof(LocalClientGlobals->cmds));
            return 1;
        case 'd':
            Cmd_EndTokenizedString();
            Cmd_TokenizeStringWithLimit(s, 3);
            CL_ConfigstringModified(localClientNum);
            return 1;
        case 'w':
            if (argc >= 3 && (v3 = Cmd_Argv(2), !_stricmp(v3, "PB")))
            {
                v9 = Cmd_Argv(1);
                v4 = SEH_SafeTranslateString((char*)"EXE_SERVERDISCONNECTREASON");
                v5 = UI_ReplaceConversionString(v4, v9);
                v6 = va("%s", v5);
                Com_Error(ERR_SERVERDISCONNECT, v6);
            }
            else if (argc >= 2)
            {   
                v7 = (char *)Cmd_Argv(1);
                CL_DisconnectError(v7);
            }
            else
            {
                Com_Error(ERR_SERVERDISCONNECT, "EXE_SERVER_DISCONNECTED");
            }
            goto $LN7_22;
        case 'x':
        $LN7_22:
            Cmd_EndTokenizedString();
            Cmd_TokenizeStringWithLimit(s, 3);
            v10 = Cmd_Argv(2);
            v8 = Cmd_Argv(1);
            Com_sprintf(bigConfigString, 0x2000u, "%c %s %s", 100, v8, v10);
            Cmd_EndTokenizedString();
            result = 0;
            break;
        case 'y':
            Cmd_EndTokenizedString();
            Cmd_TokenizeStringWithLimit(s, 3);
            sa = Cmd_Argv(2);
            if (strlen(sa) + strlen(bigConfigString) >= 0x2000)
                Com_Error(ERR_DROP, "bcs exceeded BIG_INFO_STRING");
            strcat(bigConfigString, sa);
            Cmd_EndTokenizedString();
            result = 0;
            break;
        case 'z':
            Cmd_EndTokenizedString();
            Cmd_TokenizeStringWithLimit(s, 3);
            sb = Cmd_Argv(2);
            if (strlen(bigConfigString) + strlen(sb) + 1 >= 0x2000)
                Com_Error(ERR_DROP, "bcs exceeded BIG_INFO_STRING");
            strcat(bigConfigString, sb);
            s = bigConfigString;
            Cmd_EndTokenizedString();
            continue;
        default:
            result = 1;
            break;
        }
        return result;
    }
}

/*
=====================
CL_ConfigstringModified
=====================
*/
void __cdecl CL_ConfigstringModified(int localClientNum)
{
    const char *v1; // eax
    uint v2; // [esp+0h] [ebp-4Ch]
    clientActive_t *LocalClientGlobals; // [esp+24h] [ebp-28h]
    gameState_t *oldGs; // [esp+28h] [ebp-24h]
    char *dup; // [esp+2Ch] [ebp-20h]
    int index; // [esp+3Ch] [ebp-10h]
    const char *s; // [esp+40h] [ebp-Ch]
    int i; // [esp+44h] [ebp-8h]
    const char *old; // [esp+48h] [ebp-4h]

    LargeLocal oldGs_large_local(sizeof(gameState_t));
    //LargeLocal::LargeLocal(&oldGs_large_local, 140844);
    //oldGs = LargeLocal::GetBuf(&oldGs_large_local);
    oldGs = (gameState_t *)oldGs_large_local.GetBuf();
    v1 = Cmd_Argv(1);
    index = atoi(v1);
    if ((uint)index >= 2442)
        Com_Error(ERR_DROP, "configstring > MAX_CONFIGSTRINGS");
    // get everything after "cs <num>"
    s = Cmd_Argv(2);
    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    old = &LocalClientGlobals->gameState.stringData[LocalClientGlobals->gameState.stringOffsets[index]];
    // unchanged
    if (strcmp(old, s))
    {
        // build the new gameState_t
        memcpy(oldGs, &LocalClientGlobals->gameState, sizeof(gameState_t));
        memset((uint8_t *)&LocalClientGlobals->gameState, 0, sizeof(LocalClientGlobals->gameState));
        // leave the first 0 for uninitialized strings
        LocalClientGlobals->gameState.dataCount = 1;
        for (i = 0; i < 2442; ++i)
        {
            if (i == index)
                dup = (char *)s;
            else
                dup = &oldGs->stringData[oldGs->stringOffsets[i]];
            // leave with the default empty string
            if (*dup)
            {
                v2 = strlen(dup);
                if ((int)(v2 + LocalClientGlobals->gameState.dataCount + 1) > 0x20000)
                    Com_Error(ERR_DROP, "MAX_GAMESTATE_CHARS exceeded");
                // append it to the gameState string buffer
                LocalClientGlobals->gameState.stringOffsets[i] = LocalClientGlobals->gameState.dataCount;
                memcpy(
                    (uint8_t *)&LocalClientGlobals->gameState.stringData[LocalClientGlobals->gameState.dataCount],
                    (uint8_t *)dup,
                    v2 + 1);
                LocalClientGlobals->gameState.dataCount += v2 + 1;
            }
        }
        if (index == 1)
            // parse serverId and other cvars
            CL_SystemInfoChanged(localClientNum);
    }
    //LargeLocal::~LargeLocal(&oldGs_large_local);
}

/*
====================
CL_CM_LoadMap

Just adds default parameters that cgame doesn't need to know about
====================
*/
void __cdecl CL_CM_LoadMap(char *mapname)
{
    int checksum; // [esp+0h] [ebp-4h] BYREF

    if (!IsFastFileLoad())
        Com_LoadBsp(mapname);
    CM_LoadMap(mapname, &checksum);
    if (!com_sv_running->current.enabled)
    {
        CM_LinkWorld();
        Com_LoadWorld(mapname);
    }
}

/*
====================
CL_ShutdownCGame

====================
*/
void __cdecl CL_ShutdownCGame(int localClientNum)
{
    Com_UnloadSoundAliases(SASYS_CGAME);
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (clientUIActives[0].cgameInitCalled)
    {
        CG_Shutdown(localClientNum);
        clientUIActives[0].cgameInitCalled = 0;
        clientUIActives[0].cgameInitialized = 0;
        track_shutdown(1);
    }
    else if (clientUIActives[0].cgameInitialized)
    {
        MyAssertHandler(".\\client_mp\\cl_cgame_mp.cpp", 599, 0, "%s", "!cl->cgameInitialized");
    }
}

int warnCount;
bool __cdecl CL_DObjCreateSkelForBone(DObj_s *obj, int boneIndex)
{
    char *buf; // [esp+0h] [ebp-Ch]
    uint len; // [esp+4h] [ebp-8h]
    int timeStamp; // [esp+8h] [ebp-4h]

    iassert(obj);
    timeStamp = CL_GetSkelTimeStamp();
    if (DObjSkelExists(obj, timeStamp))
        return DObjSkelIsBoneUpToDate(obj, boneIndex);
    len = DObjGetAllocSkelSize(obj);
    buf = CL_AllocSkelMemory(len);
    if (buf)
    {
        DObjCreateSkel(obj, buf, timeStamp);
        return 0;
    }
    else
    {
        if (warnCount != timeStamp)
        {
            warnCount = timeStamp;
            Com_PrintWarning(CON_CHANNEL_CLIENT, "WARNING: CL_SKEL_MEMORY_SIZE exceeded - not calculating skeleton\n");
        }
        return 1;
    }
}

void __cdecl CL_SubtitlePrint(int localClientNum, const char *text, int duration, int lineWidth)
{
    const char *translation; // [esp+0h] [ebp-4h]
    char *translationa; // [esp+0h] [ebp-4h]

    translation = SEH_StringEd_GetString(text);
    if (translation)
        goto LABEL_8;
    if (!loc_warnings->current.enabled)
    {
        translation = text;
    LABEL_8:
        CL_ConsolePrint(localClientNum, CON_CHANNEL_SUBTITLE, translation, duration, lineWidth, 0);
        return;
    }
    if (loc_warningsAsErrors->current.enabled)
        Com_Error(ERR_LOCALIZATION, "Could not translate subtitle text: \"%s\"", text);
    else
        Com_PrintWarning(CON_CHANNEL_CLIENT, "WARNING: Could not translate subtitle text: \"%s\"\n", text);
    translationa = va(S_COLOR_RED "UNLOCALIZED(" S_COLOR_WHITE "%s" S_COLOR_RED ")" S_COLOR_WHITE, text);
    CL_ConsolePrint(localClientNum, CON_CHANNEL_SUBTITLE, translationa, duration, lineWidth, 0);
}

const char *__cdecl CL_GetConfigString(int localClientNum, uint configStringIndex)
{
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-4h]

    bcassert(configStringIndex, 0x98A);
    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    if (LocalClientGlobals->gameState.stringData[0])
        MyAssertHandler(".\\client_mp\\cl_cgame_mp.cpp", 729, 0, "%s", "!cl->gameState.stringData[0]");
    return &LocalClientGlobals->gameState.stringData[LocalClientGlobals->gameState.stringOffsets[configStringIndex]];
}

snd_alias_t *__cdecl CL_PickSoundAlias(const char *aliasname)
{
    return Com_PickSoundAlias(aliasname);
}

void __cdecl CL_RenderScene(const refdef_s *fd)
{
    cls.debugRenderPos[0] = fd->vieworg[0];
    cls.debugRenderPos[1] = fd->vieworg[1];
    cls.debugRenderPos[2] = fd->vieworg[2];
    R_RenderScene(fd);
}

void __cdecl CL_DrawStretchPicPhysical(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material)
{
    R_AddCmdDrawStretchPic(x, y, w, h, s1, t1, s2, t2, color, material);
}

void __cdecl CL_DrawStretchPicPhysicalRotateXY(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    float angle,
    const float *color,
    Material *material)
{
    R_AddCmdDrawStretchPicRotateXY(x, y, w, h, s1, t1, s2, t2, angle, color, material);
}

void __cdecl CL_DrawStretchPicPhysicalFlipST(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material)
{
    R_AddCmdDrawStretchPicFlipST(x, y, w, h, s1, t1, s2, t2, color, material);
}

void __cdecl CL_DrawStretchPic(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material)
{
    ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
    CL_DrawStretchPicPhysical(x, y, w, h, s1, t1, s2, t2, color, material);
}

void __cdecl CL_DrawStretchPicFlipST(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material)
{
    ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
    CL_DrawStretchPicPhysicalFlipST(x, y, w, h, s1, t1, s2, t2, color, material);
}

void __cdecl CL_DrawStretchPicRotatedST(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    float centerS,
    float centerT,
    float radiusST,
    float scaleFinalS,
    float scaleFinalT,
    float angle,
    const float *color,
    Material *material)
{
    ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
    R_AddCmdDrawStretchPicRotateST(
        x,
        y,
        w,
        h,
        centerS,
        centerT,
        radiusST,
        scaleFinalS,
        scaleFinalT,
        angle,
        color,
        material);
}

void __cdecl CL_CapTurnRate(int localClientNum, float maxPitchSpeed, float maxYawSpeed)
{
    clientActive_t *LocalClientGlobals; // eax

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    LocalClientGlobals->cgameMaxPitchSpeed = maxPitchSpeed;
    LocalClientGlobals->cgameMaxYawSpeed = maxYawSpeed;
}

void __cdecl CL_SyncTimes(int localClientNum)
{
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (clientUIActives[0].connectionState == CA_ACTIVE)
        CL_FirstSnapshot(localClientNum);
}

int __cdecl LoadWorld(char *mapname)
{
    int checksum; // [esp+0h] [ebp-4h] BYREF

    R_LoadWorld(mapname, &checksum, 0);
    if (!IsFastFileLoad())
        Com_UnloadBsp();
    return checksum;
}

void __cdecl CL_StartLoading()
{
    if (CL_AnyLocalClientsRunning())
    {
        CL_InitRenderer();
        CL_StartHunkUsers();
        SCR_UpdateScreen();
    }
}

void __cdecl CL_SetExpectedHunkUsage(const char *mapname)
{
    int handle; // [esp+0h] [ebp-18h] BYREF
    char *buf; // [esp+8h] [ebp-10h]
    int len; // [esp+Ch] [ebp-Ch]
    const char *token; // [esp+10h] [ebp-8h]
    const char *buftrav; // [esp+14h] [ebp-4h] BYREF

    len = FS_FOpenFileByMode((char*)"hunkusage.dat", &handle, FS_READ);
    if (len >= 0)
    {
        buf = (char *)Z_Malloc(len + 1, "CL_SetExpectedHunkUsage", 10);
        memset((uint8_t *)buf, 0, len + 1);
        FS_Read((uint8_t *)buf, len, handle);
        FS_FCloseFile(handle);
        buftrav = buf;
        while (1)
        {
            token = (const char *)Com_Parse(&buftrav);
            iassert(token);
            if (!*token)
                break;
            if (!I_stricmp(token, mapname))
            {
                token = (const char *)Com_Parse(&buftrav);
                if (token)
                {
                    if (*token)
                    {
                        com_expectedHunkUsage = atoi(token);
                        Z_Free(buf, 10);
                        return;
                    }
                }
            }
        }
        Z_Free(buf, 10);
    }
    com_expectedHunkUsage = 0;
}

/*
====================
CL_InitCGame

Should only be called by CL_StartHunkUsers
====================
*/
void __cdecl CL_InitCGame(int localClientNum)
{
    PROF_SCOPED("CL_InitCGame");
    const char *v1; // eax
    int v2; // eax
    XZoneInfo zoneInfo; // [esp+10h] [ebp-70h] BYREF
    clientUIActive_t *clientUIActive; // [esp+20h] [ebp-60h]
    clientActive_t *LocalClientGlobals; // [esp+24h] [ebp-5Ch]
    const char *info; // [esp+28h] [ebp-58h]
    int t1; // [esp+2Ch] [ebp-54h]
    clientConnection_t *clc; // [esp+30h] [ebp-50h]
    int t2; // [esp+34h] [ebp-4Ch]
    char mapname[68]; // [esp+38h] [ebp-48h] BYREF

    t1 = Sys_Milliseconds();
    SND_ErrorCleanup();
    // put away the console
    Con_Close(localClientNum);
    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    clientUIActive = clientUIActives;
    // find the current mapname
    info = &LocalClientGlobals->gameState.stringData[LocalClientGlobals->gameState.stringOffsets[0]];
    v1 = Info_ValueForKey((char *)info, "mapname");
    I_strncpyz(mapname, v1, 64);
    Dvar_SetStringByName("mapname", mapname);
    Com_GetBspFilename(LocalClientGlobals->mapname, 0x40u, mapname);
    if (!CL_WasMapAlreadyLoaded())
    {
        Com_InitDObj();
        if (IsFastFileLoad())
        {
            zoneInfo.name = mapname;
            zoneInfo.allocFlags = DB_ZONE_GAME;
            zoneInfo.freeFlags = DB_ZONE_GAME;
            DB_LoadXAssets(&zoneInfo, 1u, 0);
        }
        else
        {
            CL_SetExpectedHunkUsage(LocalClientGlobals->mapname);
        }
    }
    vassert((localClientNum) == 0, "%i not in [0, %i)", localClientNum, 1);
    clientUIActives[localClientNum].connectionState = CA_LOADING;
    Com_Printf(CON_CHANNEL_CLIENT, "Setting state to CA_LOADING in CL_InitCGame\n");
    clientUIActive->cgameInitCalled = 1;
    cl_serverLoadingMap = 0;
    clc = CL_GetLocalClientConnection(localClientNum);
    // init for this gamestate
    // use the lastExecutedServerCommand instead of the serverCommandSequence
    // otherwise server commands sent just before a gamestate are dropped
    CG_Init(localClientNum, clc->serverMessageSequence, clc->lastExecutedServerCommand, clc->clientNum);
    clientUIActive->cgameInitialized = 1;
    R_BeginRemoteScreenUpdate();
    vassert((localClientNum) == 0, "%i not in [0, %i)", localClientNum, 1);
    clientUIActives[localClientNum].connectionState = CA_PRIMED;
    t2 = Sys_Milliseconds();
    Com_Printf(CON_CHANNEL_CLIENT, "CL_InitCGame: %5.2f seconds\n", (double)(t2 - t1) / 1000.0);
    // have the renderer touch all its images, so they are present
    // on the card even if the driver does deferred loading
    R_EndRegistration();

    // make sure everything is paged in
    Com_TouchMemory();

    // clear anything that got printed
    Con_ClearNotify(localClientNum);
    Con_InitMessageBuffer();
    Con_InitGameMsgChannels();
    if (!IsFastFileLoad())
    {
        v2 = CL_ControllerIndexFromClientNum(localClientNum);
        Cmd_ExecuteSingleCommand(localClientNum, v2, (char*)"updatehunkusage");
    }
    R_EndRemoteScreenUpdate();
    if (IsFastFileLoad())
        DB_SyncXAssets();
}

/*
==================
CL_FirstSnapshot
==================
*/
void __cdecl CL_FirstSnapshot(int localClientNum)
{
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-8h]
    clientConnection_t *clc; // [esp+4h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    // ignore snapshots that don't have entities
    if ((LocalClientGlobals->snap.snapFlags & 2) == 0)
    {
        CG_RegisterSounds();
        clc = CL_GetLocalClientConnection(localClientNum);
        vassert((localClientNum) == 0, "%i not in [0, %i)", localClientNum, 1);
        clientUIActives[localClientNum].connectionState = CA_ACTIVE;
        clc->isServerRestarting = 0;
        UI_CloseAll(localClientNum);
        // set the timedelta so we are exactly on this first frame
        LocalClientGlobals->serverTimeDelta = LocalClientGlobals->snap.serverTime - cls.realtime;
        LocalClientGlobals->oldServerTime = LocalClientGlobals->snap.serverTime;
        LocalClientGlobals->serverTime = LocalClientGlobals->snap.serverTime;
        clc->timeDemoBaseTime = LocalClientGlobals->snap.serverTime;
        Con_TimeJumped(localClientNum, LocalClientGlobals->serverTime);
        // if this is the first frame of active play,
        // execute the contents of activeAction now
        // this is to allow scripting a timedemo to start right
        // after loading
        if (*cl_activeAction->current.string)
        {
            Cbuf_AddText(localClientNum, cl_activeAction->current.string);
            Cbuf_AddText(localClientNum, "\n");
            Dvar_SetString((dvar_s *)cl_activeAction, (char *)"");
        }
    }
}

char *__cdecl CL_TimeDemoLogBaseName(const char *mapname)
{
    char *result; // eax
    const char *pos; // [esp+0h] [ebp-10h]
    const char *start; // [esp+4h] [ebp-Ch]
    const char *end; // [esp+8h] [ebp-8h]

    end = 0;
    start = mapname;
    for (pos = mapname; *pos; ++pos)
    {
        if (*pos == 47 || *pos == 92)
        {
            start = pos + 1;
            end = 0;
        }
        else if (*pos == 46)
        {
            end = pos;
        }
    }
    if (!end)
        return (char *)start;
    result = va("%s", start);
    result[end - start] = 0;
    return result;
}

void __cdecl CL_UpdateTimeDemo(int localClientNum)
{
    char *v1; // eax
    char *v2; // eax
    int Int; // [esp-4h] [ebp-10h]
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-Ch]
    clientConnection_t *clc; // [esp+4h] [ebp-8h]
    DWORD currentTime; // [esp+8h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    clc = CL_GetLocalClientConnection(localClientNum);
    currentTime = Sys_Milliseconds();
    if (!clc->timeDemoLog)
    {
        Int = Dvar_GetInt("r_mode");
        v1 = CL_TimeDemoLogBaseName(LocalClientGlobals->mapname);
        v2 = va("demos/timedemo_%s_mode_%i.csv", v1, Int);
        clc->timeDemoLog = FS_FOpenFileWrite(v2);
    }
    if (clc->timeDemoStart)
    {
        if (clc->timeDemoLog)
            FS_Printf(clc->timeDemoLog, "%i,%i\n", clc->timeDemoFrames, currentTime - clc->timeDemoPrev);
    }
    else
    {
        clc->timeDemoStart = currentTime;
    }
    clc->timeDemoPrev = currentTime;
    ++clc->timeDemoFrames;
    LocalClientGlobals->serverTime = clc->timeDemoBaseTime + 50 * clc->timeDemoFrames;
}

/*
==================
CL_NextDemo

Called when a demo or cinematic finishes
If the "nextdemo" cvar is set, that command will be issued
==================
*/
void __cdecl CL_NextDemo(int localClientNum)
{
    char v[1028]; // [esp+0h] [ebp-408h] BYREF

    I_strncpyz(v, nextdemo->current.string, 1024);
    Com_DPrintf(CON_CHANNEL_CLIENT, "CL_NextDemo: %s\n", v);
    if (v[0])
    {
        Dvar_SetString((dvar_s *)nextdemo, (char *)"");
        Cbuf_AddText(localClientNum, v);
        Cbuf_AddText(localClientNum, "\n");
        Cbuf_Execute(localClientNum, 0);
    }
    else
    {
        Com_Error(ERR_DISCONNECT, "Demo is over");
    }
}

/*
=================
CL_DemoCompleted
=================
*/
void __cdecl CL_DemoCompleted(int localClientNum)
{
    int time; // [esp+10h] [ebp-8h]
    clientConnection_t *clc; // [esp+14h] [ebp-4h]

    clc = CL_GetLocalClientConnection(localClientNum);
    if (clc->isTimeDemo)
    {
        time = Sys_Milliseconds() - clc->timeDemoStart;
        if (time > 0)
            Com_Printf(
                CON_CHANNEL_CLIENT,
                "%i frames, %3.1f seconds: %3.1f fps\n",
                clc->timeDemoFrames,
                (double)time / 1000.0,
                (double)clc->timeDemoFrames * 1000.0 / (double)time);
    }
    if (clc->timeDemoLog)
    {
        FS_FCloseFile(clc->timeDemoLog);
        clc->timeDemoLog = 0;
    }
    CL_Disconnect(0);
    CL_NextDemo(0);
}

void __cdecl CL_ReadDemoClientArchive(int localClientNum)
{
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-14h]
    uint8_t *archive; // [esp+8h] [ebp-Ch]
    clientConnection_t *clc; // [esp+Ch] [ebp-8h]
    int index; // [esp+10h] [ebp-4h] BYREF

    clc = CL_GetLocalClientConnection(localClientNum);
    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    if (FS_Read((uint8_t *)&index, 4u, clc->demofile) == 4)
    {
        if ((uint)index < 0x100)
        {
            archive = (uint8_t *)&LocalClientGlobals->clientArchive[index];
            FS_Read((uint8_t *)LocalClientGlobals->clientArchive[index].origin, 0xCu, clc->demofile);
            FS_Read(archive + 16, 0xCu, clc->demofile);
            FS_Read(archive + 32, 4u, clc->demofile);
            FS_Read(archive + 28, 4u, clc->demofile);
            FS_Read(archive, 4u, clc->demofile);
            FS_Read(archive + 36, 0xCu, clc->demofile);
            LocalClientGlobals->clientArchiveIndex = index + 1;
        }
        else
        {
            Com_Printf(CON_CHANNEL_CLIENT, "Demo file was corrupt.\n");
            CL_DemoCompleted(localClientNum);
        }
    }
    else
    {
        CL_DemoCompleted(localClientNum);
    }
}

void __cdecl CL_ReadDemoNetworkPacket(int localClientNum)
{
    uint v1; // edx
    int v2; // eax
    uint8_t *bufData; // [esp+4h] [ebp-3Ch]
    msg_t buf; // [esp+8h] [ebp-38h] BYREF
    clientConnection_t *clc; // [esp+30h] [ebp-10h]
    int s; // [esp+3Ch] [ebp-4h] BYREF

    LargeLocal bufData_large_local(0x20000);
    //LargeLocal::LargeLocal(&bufData_large_local, 0x20000);
    //bufData = LargeLocal::GetBuf(&bufData_large_local);
    bufData = bufData_large_local.GetBuf();
    clc = CL_GetLocalClientConnection(localClientNum);
    // get the sequence number
    if (FS_Read((uint8_t *)&s, 4u, clc->demofile) == 4)
    {
        clc->serverMessageSequence = s;

        // init the message
        MSG_Init(&buf, bufData, 0x20000);

        // get the length
        if (FS_Read((uint8_t *)&buf.cursize, 4u, clc->demofile) != 4 || buf.cursize == -1)
        {
            CL_DemoCompleted(localClientNum);
        }
        else
        {
            if (buf.cursize < 0 || buf.cursize > buf.maxsize)
                Com_Error(ERR_DROP, "CL_ReadDemoMessage: demoMsglen > MAX_MSGLEN");
            v1 = FS_Read(buf.data, buf.cursize, clc->demofile);
            if (v1 == buf.cursize)
            {
                clc->lastPacketTime = cls.realtime;
                buf.readcount = 0;
                v2 = MSG_ReadLong(&buf);
                clc->reliableAcknowledge = v2;
                if (clc->reliableAcknowledge >= clc->reliableSequence - 128)
                    CL_ParseServerMessage((netsrc_t)localClientNum, &buf);
                else
                    clc->reliableAcknowledge = clc->reliableSequence;
            }
            else
            {
                Com_Printf(CON_CHANNEL_CLIENT, "Demo file was truncated.\n");
                CL_DemoCompleted(localClientNum);
            }
        }
    }
    else
    {
        CL_DemoCompleted(localClientNum);
    }
}

/*
=================
CL_ReadDemoMessage
=================
*/
void __cdecl CL_ReadDemoMessage(int localClientNum)
{
    clientConnection_t *clc; // [esp+8h] [ebp-8h]
    uint8_t s; // [esp+Fh] [ebp-1h] BYREF

    clc = CL_GetLocalClientConnection(localClientNum);
    if (clc->demofile)
    {
        if (FS_Read(&s, 1u, clc->demofile) == 1)
        {
            if (s)
            {
                if (s == 1)
                    CL_ReadDemoClientArchive(localClientNum);
            }
            else
            {
                CL_ReadDemoNetworkPacket(localClientNum);
            }
        }
        else
        {
            CL_DemoCompleted(localClientNum);
        }
    }
    else
    {
        CL_DemoCompleted(localClientNum);
    }
}

/*
==================
CL_SetCGameTime
==================
*/
void __cdecl CL_SetCGameTime(int localClientNum)
{
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-Ch]
    clientConnection_t *clc; // [esp+8h] [ebp-4h]

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    // getting a valid frame message ends the connection process
    if (clientUIActives[0].connectionState == CA_ACTIVE)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
        clc = CL_GetLocalClientConnection(localClientNum);
    LABEL_16:
        // if we have gotten to this point, cl.snap is guaranteed to be valid
        if (!LocalClientGlobals->snap.valid)
            Com_Error(ERR_DROP, "CL_SetCGameTime: !cl->snap.valid");

        // allow pause in single player
        // paused
        if (!sv_paused->current.integer || !cl_paused->current.integer || !com_sv_running->current.enabled)
        {
            if (LocalClientGlobals->snap.serverTime < LocalClientGlobals->oldFrameServerTime)
            {
                if (I_stricmp(cls.servername, "localhost"))
                    Com_Error(ERR_DROP, "cl->snap.serverTime < cl->oldFrameServerTime");
                else
                    CL_FirstSnapshot(localClientNum);
            }
            LocalClientGlobals->oldFrameServerTime = LocalClientGlobals->snap.serverTime;
            // cl_freezeDemo is used to lock a demo in place for single frame advances
            if (!clc->demoplaying || !cl_freezeDemo->current.enabled)
            {
                // get our current view of time
                LocalClientGlobals->serverTime = LocalClientGlobals->serverTimeDelta + cls.realtime;

                // guarantee that time will never flow backwards, even if
                // serverTimeDelta made an adjustment or cl_timeNudge was changed
                if (LocalClientGlobals->serverTime < LocalClientGlobals->oldServerTime)
                    LocalClientGlobals->serverTime = LocalClientGlobals->oldServerTime;
                LocalClientGlobals->oldServerTime = LocalClientGlobals->serverTime;

                // note if we are almost past the latest frame (without timeNudge),
                // so we will try and adjust back a bit when the next snapshot arrives
                if (LocalClientGlobals->serverTimeDelta + cls.realtime >= LocalClientGlobals->snap.serverTime - 5)
                {
                    LocalClientGlobals->extrapolatedSnapshot = 1;
                    if (cl_showTimeDelta->current.enabled)
                        Com_Printf(CON_CHANNEL_CLIENT, "Extrapolating snapshot!\n");
                }
            }
            // if we have gotten new snapshots, drift serverTimeDelta
            // don't do this every frame, or a period of packet loss would
            // make a huge adjustment
            if (LocalClientGlobals->newSnapshots)
                CL_AdjustTimeDelta(localClientNum);

            // if we are playing a demo back, we can just keep reading
            // messages from the demo file until the cgame definately
            // has valid snapshots to interpolate between
            if (clc->demoplaying)
            {
                // a timedemo will always use a deterministic set of time samples
                // no matter what speed machine it is run on,
                // while a normal demo may have different time samples
                // each time it is played back
                if (clc->isTimeDemo)
                    CL_UpdateTimeDemo(localClientNum);
                do
                {
                    if (LocalClientGlobals->serverTime < LocalClientGlobals->snap.serverTime)
                        break;
                    // feed another messag, which should change
                    // the contents of cl.snap
                    CL_ReadDemoMessage(localClientNum);
                    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
                } while (clientUIActives[0].connectionState == CA_ACTIVE);
            }
        }
        return;
    }
    if (clientUIActives[0].connectionState != CA_PRIMED)
        return;
    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    clc = CL_GetLocalClientConnection(localClientNum);
    if (clc->demoplaying)
    {
        // we shouldn't get the first snapshot on the same frame
        // as the gamestate, because it causes a bad time skip
        if (!clc->firstDemoFrameSkipped)
        {
            clc->firstDemoFrameSkipped = 1;
            return;
        }
        CL_ReadDemoMessage(localClientNum);
    }
    if (LocalClientGlobals->newSnapshots)
    {
        LocalClientGlobals->newSnapshots = 0;
        CL_FirstSnapshot(localClientNum);
    }
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (clientUIActives[0].connectionState == CA_ACTIVE)
        goto LABEL_16;
}

/*
=================
CL_AdjustTimeDelta

Adjust the clients view of server time.

We attempt to have cl.serverTime exactly equal the server's view
of time plus the timeNudge, but with variable latencies over
the internet it will often need to drift a bit to match conditions.

Our ideal time would be to have the adjusted time approach, but not pass,
the very latest snapshot.

Adjustments are only made when a new snapshot arrives with a rational
latency, which keeps the adjustment process framerate independent and
prevents massive overadjustment during times of significant packet loss
or bursted delayed packets.
=================
*/
void __cdecl CL_AdjustTimeDelta(int localClientNum)
{
    clientActive_t *LocalClientGlobals; // [esp+4h] [ebp-14h]
    int idealDelta; // [esp+8h] [ebp-10h]
    uint snapInterval; // [esp+Ch] [ebp-Ch]
    int deltaCorrectionMagnitude; // [esp+10h] [ebp-8h]
    int oldDelta; // [esp+14h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    LocalClientGlobals->newSnapshots = 0;

    // the delta never drifts when replaying a demo
    if (!CL_GetLocalClientConnection(localClientNum)->demoplaying)
    {
        snapInterval = LocalClientGlobals->snap.serverTime - LocalClientGlobals->oldSnapServerTime;
        oldDelta = LocalClientGlobals->serverTimeDelta;
        idealDelta = LocalClientGlobals->snap.serverTime - cls.realtime - snapInterval - 5;
        deltaCorrectionMagnitude = idealDelta - oldDelta;
        if (idealDelta - oldDelta <= 0)
        {
            deltaCorrectionMagnitude = oldDelta - idealDelta;
        }
        else if (snapInterval <= 0x1F4)
        {
            deltaCorrectionMagnitude -= snapInterval;
            if (deltaCorrectionMagnitude < 0)
                deltaCorrectionMagnitude = 0;
        }
        if (deltaCorrectionMagnitude <= 500)
        {
            if (deltaCorrectionMagnitude <= 100)
            {
                // slow drift adjust, only move 1 or 2 msec

                // if any of the frames between this and the previous snapshot
                // had to be extrapolated, nudge our sense of time back a little
                // the granularity of +1 / -2 is too high for timescale modified frametimes
                if (com_timescaleValue == 1.0)
                {
                    if (LocalClientGlobals->extrapolatedSnapshot)
                    {
                        LocalClientGlobals->extrapolatedSnapshot = 0;
                        LocalClientGlobals->serverTimeDelta -= 2;
                    }
                    else if (idealDelta <= LocalClientGlobals->serverTimeDelta)
                    {
                        if (idealDelta < LocalClientGlobals->serverTimeDelta)
                            --LocalClientGlobals->serverTimeDelta;
                    }
                    else
                    {
                        // otherwise, move our sense of time forward to minimize total latency
                        ++LocalClientGlobals->serverTimeDelta;
                    }
                }
            }
            else
            {
                // fast adjust, cut the difference in half
                if (cl_showTimeDelta->current.enabled)
                    Com_Printf(CON_CHANNEL_CLIENT, "<FAST> ");
                LocalClientGlobals->serverTimeDelta = (idealDelta + LocalClientGlobals->serverTimeDelta) >> 1;
            }
        }
        else
        {
            // if the current time is WAY off, just correct to the current value
            Com_PrintWarning(
                CON_CHANNEL_CLIENT,
                "Cl_AdjustTimeDelta RESET: snap is %i, last snap was %i, walltime is %i, current delta is %i, old server time was"
                " %i, server time is %i\n",
                LocalClientGlobals->snap.serverTime,
                LocalClientGlobals->oldSnapServerTime,
                cls.realtime,
                LocalClientGlobals->serverTimeDelta,
                LocalClientGlobals->oldServerTime,
                LocalClientGlobals->serverTime);
            LocalClientGlobals->serverTimeDelta = LocalClientGlobals->snap.serverTime - cls.realtime - 5;
            LocalClientGlobals->oldServerTime = LocalClientGlobals->snap.serverTime;	// FIXME: is this a problem for cgame?
            LocalClientGlobals->serverTime = LocalClientGlobals->snap.serverTime;
            if (cl_showTimeDelta->current.enabled)
                Com_Printf(CON_CHANNEL_CLIENT, "<RESET> ");
        }
        if (LocalClientGlobals->serverTimeDelta != oldDelta)
            Con_TimeNudged(localClientNum, LocalClientGlobals->serverTimeDelta - oldDelta);
        if (cl_showTimeDelta->current.enabled)
        {
            Com_Printf(
                CON_CHANNEL_CLIENT,
                "client time: %i, server time: %i\n",
                LocalClientGlobals->serverTimeDelta + cls.realtime,
                LocalClientGlobals->snap.serverTime);
            Com_Printf(CON_CHANNEL_CLIENT, "ideal delta: %i, current delta: %i\n", idealDelta, LocalClientGlobals->serverTimeDelta);
        }
    }
}

void __cdecl CL_SetADS(int localClientNum, bool ads)
{
    CL_GetLocalClientGlobals(localClientNum)->usingAds = ads;
}

void __cdecl CL_DrawString(int x, int y, char *pszString, int bShadow, int iCharHeight)
{
    float v5; // [esp+0h] [ebp-20h]
    float v6; // [esp+4h] [ebp-1Ch]
    float charHeight; // [esp+18h] [ebp-8h]

    charHeight = (float)iCharHeight;
    v6 = (float)y;
    v5 = (float)x;
    CG_DrawStringExt(&scrPlaceFull, v5, v6, pszString, 0, 0, bShadow, charHeight);
}

void __cdecl CL_DrawRect(int x, int y, int width, int height, const float *color)
{
    float v5; // [esp+0h] [ebp-30h]
    float v6; // [esp+4h] [ebp-2Ch]
    float w; // [esp+8h] [ebp-28h]
    float h; // [esp+Ch] [ebp-24h]

    h = (float)height;
    w = (float)width;
    v6 = (float)y;
    v5 = (float)x;
    CL_DrawStretchPic(&scrPlaceFull, v5, v6, w, h, 1, 1, 0.0, 0.0, 0.0, 0.0, color, cls.whiteMaterial);
}

void __cdecl CL_ArchiveClientState(int localClientNum, MemoryFile *memFile)
{
    CG_ArchiveState(localClientNum, memFile);
    FX_Archive(localClientNum, memFile);
    R_ArchiveFogState(memFile);
}

void __cdecl CL_LookupColor(int localClientNum, uint8_t c, float *color)
{
    float *v3; // [esp+4h] [ebp-18h]
    float *v4; // [esp+8h] [ebp-14h]
    float *v5; // [esp+Ch] [ebp-10h]
    team_t team; // [esp+10h] [ebp-Ch]
    uint index; // [esp+18h] [ebp-4h]
    cg_s *cgameGlob;

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    index = ColorIndex(c);
    if (index >= 8)
    {
        team = cgameGlob->bgs.clientinfo[cgameGlob->clientNum].team;
        if (team != TEAM_AXIS && team != TEAM_ALLIES)
            team = TEAM_ALLIES;
        if (c == 56)
        {
            if (team == TEAM_ALLIES)
                v4 = color_allies;
            else
                v4 = color_axis;
            *color = *v4;
            color[1] = v4[1];
            color[2] = v4[2];
            color[3] = v4[3];
        }
        else if (c == 57)
        {
            if (team == TEAM_ALLIES)
                v3 = color_axis;
            else
                v3 = color_allies;
            *color = *v3;
            color[1] = v3[1];
            color[2] = v3[2];
            color[3] = v3[3];
        }
        else
        {
            *color = 1.0;
            color[1] = 1.0;
            color[2] = 1.0;
            color[3] = 1.0;
        }
    }
    else
    {
        v5 = (float *)g_color_table[index];
        *color = *v5;
        color[1] = v5[1];
        color[2] = v5[2];
        color[3] = v5[3];
    }
}

void __cdecl CL_UpdateColor(int localClientNum)
{
    team_t team; // [esp+0h] [ebp-8h]
    cg_s *cgameGlob;

    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        cgameGlob = CG_GetLocalClientGlobals(localClientNum);
        if (cgameGlob)
        {
            team = cgameGlob->bgs.clientinfo[cgameGlob->clientNum].team;
            if (team != TEAM_AXIS && team != TEAM_ALLIES)
                team = TEAM_ALLIES;
            CL_UpdateColorInternal("g_TeamColor_Allies", color_allies);
            CL_UpdateColorInternal("g_TeamColor_Axis", color_axis);
            R_UpdateTeamColors(team, color_allies, color_axis);
        }
    }
}

void __cdecl CL_UpdateColorInternal(const char *var_name, float *color)
{
    Dvar_GetUnpackedColorByName(var_name, color);
    color[3] = 1.0;
}

BOOL __cdecl CL_IsCgameInitialized(int localClientNum)
{
    vassert((localClientNum == 0), "(localClientNum) = %i", localClientNum);
    return clientUIActives[0].cgameInitialized;
}

