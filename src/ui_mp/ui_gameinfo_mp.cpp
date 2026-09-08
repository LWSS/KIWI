#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "ui_mp.h"
#include <universal/q_parse.h>
#include <database/database.h>
#include <universal/com_files.h>

int ui_numArenas;
char *ui_arenaInfos[64];

int __cdecl UI_ParseInfos(const char *buf, int max, char **infos)
{
    uint8_t *v3; // eax
    char v5; // [esp+3h] [ebp-865h]
    char *v6; // [esp+8h] [ebp-860h]
    char *v7; // [esp+Ch] [ebp-85Ch]
    uint v8; // [esp+10h] [ebp-858h]
    char info[1024]; // [esp+58h] [ebp-810h] BYREF
    char key[1028]; // [esp+458h] [ebp-410h] BYREF
    const char *token; // [esp+860h] [ebp-8h]
    int count; // [esp+864h] [ebp-4h]

    count = 0;
    while (1)
    {
        token = (const char *)Com_Parse(&buf);
        if (!*token)
            return count;
        if (strcmp(token, "{"))
        {
            Com_Printf(CON_CHANNEL_UI, "Missing { in info file\n");
            return count;
        }
        if (count == max)
        {
            Com_Printf(CON_CHANNEL_UI, "Max infos exceeded\n");
            return count;
        }
        info[0] = 0;
        while (1)
        {
            token = (const char *)Com_Parse(&buf);
            if (!*token)
                break;
            if (!strcmp(token, "}"))
                goto LABEL_14;
            I_strncpyz(key, (char *)token, 1024);
            token = (const char *)Com_ParseOnLine(&buf);
            if (!*token)
                token = "<NULL>";
            Info_SetValueForKey(info, key, token);
        }
        Com_Printf(CON_CHANNEL_UI, "Unexpected end of info file\n");
    LABEL_14:
        v8 = strlen(va("%d", 64));
        v3 = UI_Alloc(strlen(info) + v8 + 6, 1);
        infos[count] = (char *)v3;
        if (infos[count])
        {
            v7 = info;
            v6 = infos[count];
            do
            {
                v5 = *v7;
                *v6++ = *v7++;
            } while (v5);
            ++count;
        }
    }
}

void __cdecl UI_LoadArenas()
{
    sharedUiInfo.mapCount = 0;
    UI_LoadArenasFromFile();
    for (int n = 0; n < ui_numArenas && sharedUiInfo.mapCount < ARRAY_COUNT(sharedUiInfo.mapList); ++n)
    {
        mapInfo *map = &sharedUiInfo.mapList[sharedUiInfo.mapCount];
        map->mapLoadName = String_Alloc(Info_ValueForKey(ui_arenaInfos[n], "map"));
        map->mapName = String_Alloc(Info_ValueForKey(ui_arenaInfos[n], "longname"));
        map->imageName = String_Alloc(va("loadscreen_%s", map->mapLoadName));
        map->levelShot = Material_RegisterHandle(map->imageName, IMAGE_TRACK_UI);
        const char *gameTypes = Info_ValueForKey(ui_arenaInfos[n], "gametype");
        map->typeBits = -1;
        if (gameTypes && *gameTypes)
        {
            map->typeBits = 0;
            Com_BeginParseSession(va(".arena files : %s", map->mapLoadName));
            for (;;)
            {
                const char *token = Com_Parse(&gameTypes)->token;
                if (!token || !*token)
                {
                    break;
                }
                for (int i = 0; i < sharedUiInfo.numGameTypes && i < ARRAY_COUNT(sharedUiInfo.gameTypes); ++i)
                {
                    if (!I_stricmp(token, sharedUiInfo.gameTypes[i].gameType))
                    {
                        map->typeBits |= 1u << i;
                    }
                }
            }
            Com_EndParseSession();
        }
        ++sharedUiInfo.mapCount;
    }
}

void UI_LoadArenasFromFile_LoadObj()
{
    int fileCount;
    char string[132];   // [esp+18h] [ebp-24A0h] BYREF
    char *v3;           // [esp+9Ch] [ebp-241Ch]
    char listbuf[1024]; // [esp+A0h] [ebp-2418h] BYREF
    char buffer[8196];  // [esp+4A0h] [ebp-2018h] BYREF
    int len;            // [esp+24A8h] [ebp-10h]
    int f;              // [esp+24ACh] [ebp-Ch] BYREF
    int v8;             // [esp+24B0h] [ebp-8h]
    uint v9;            // [esp+24B4h] [ebp-4h]

    ui_numArenas = 0;
    fileCount = FS_GetFileList("mp", "arena", FS_LIST_PURE_ONLY, listbuf, sizeof(listbuf));
    v3 = listbuf;
    v8 = 0;
    while (v8 < fileCount)
    {
        v9 = strlen(v3);
        snprintf(string, ARRAYSIZE(string), "%s/%s", "mp", v3);
        len = FS_FOpenFileByMode(string, &f, FS_READ);
        if (f)
        {
            if (len >= 0 && len < 0x2000)
            {
                FS_Read((uint8_t *)buffer, len, f);
                buffer[len] = 0;
                FS_FCloseFile(f);
                ui_numArenas += UI_ParseInfos(buffer, 64 - ui_numArenas, &ui_arenaInfos[ui_numArenas]);
            }
            else
            {
                Com_PrintError(CON_CHANNEL_UI, "file too large: %s is %i, max allowed is %i", string, len, 0x2000);
                FS_FCloseFile(f);
            }
        }
        else
        {
            Com_PrintError(CON_CHANNEL_UI, "file not found: %s\n", string);
        }
        ++v8;
        v3 += v9 + 1;
    }
}

void UI_LoadArenasFromFile()
{
    if (IsFastFileLoad())
        UI_LoadArenasFromFile_FastFile();
    else
        UI_LoadArenasFromFile_LoadObj();
}

void UI_LoadArenasFromFile_FastFile()
{
    RawFile *rawfile; // [esp+8h] [ebp-8h]

    rawfile = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, "mp/cod2maps.arena").rawfile;
    if (rawfile)
        ui_numArenas = UI_ParseInfos(rawfile->buffer, 64 - ui_numArenas, &ui_arenaInfos[ui_numArenas]);
    else
        Com_PrintError(CON_CHANNEL_UI, "file not found: %s\n", "mp/cod2maps.arena");
}

