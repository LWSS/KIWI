#include <universal/q_shared.h>
#include <stddef.h>
#include "bg_local.h"
#include "bg_public.h"
#include "bg_weapon_fields.h"
#include <qcommon/mem_track.h>
#include <database64/database.h>
#include <universal/q_parse.h>
#include <universal/com_memory.h>
#include <universal/com_files.h>
#include <universal/com_sndalias.h>
#include <universal/surfaceflags.h>

//int surfaceTypeSoundListCount 828010f0     bg_weapons_load_obj.obj
//struct SurfaceTypeSoundList *surfaceTypeSoundLists 828011f8     bg_weapons_load_obj.obj

uint g_playerAnimTypeNamesCount;

SurfaceTypeSoundList surfaceTypeSoundLists[16];

char *g_playerAnimTypeNames[64];

WeaponDef bg_defaultWeaponDefs;

char *__cdecl BG_GetPlayerAnimTypeName(int index)
{
    return g_playerAnimTypeNames[index];
}

void __cdecl TRACK_bg_weapons_load_obj()
{
    track_static_alloc_internal(szWeapOverlayReticleNames, sizeof(szWeapOverlayReticleNames), "szWeapOverlayReticleNames", 9);
    track_static_alloc_internal(szWeapStanceNames, sizeof(szWeapStanceNames), "szWeapStanceNames", 9);
    track_static_alloc_internal(weaponDefFields, sizeof(weaponDefFields), "weaponDefFields", 9);
    track_static_alloc_internal(&bg_defaultWeaponDefs, sizeof(bg_defaultWeaponDefs), "bg_defaultWeaponDefs", 9);
    track_static_alloc_internal(penetrateTypeNames, sizeof(penetrateTypeNames), "penetrateTypeNames", 9);
    track_static_alloc_internal(szWeapTypeNames, sizeof(szWeapTypeNames), "szWeapTypeNames", 9);
    track_static_alloc_internal(szWeapClassNames, sizeof(szWeapClassNames), "szWeapClassNames", 9);
    track_static_alloc_internal(g_playerAnimTypeNames, sizeof(g_playerAnimTypeNames), "g_playerAnimTypeNames", 9);
    track_static_alloc_internal(szWeapInventoryTypeNames, sizeof(szWeapInventoryTypeNames), "szWeapInventoryTypeNames", 9);
}

const char *__cdecl BG_GetWeaponTypeName(weapType_t type)
{
    bcassert(type < WEAPTYPE_NUM, ARRAY_COUNT(szWeapTypeNames));

    return szWeapTypeNames[type];
}

const char *__cdecl BG_GetWeaponClassName(weapClass_t type)
{
    bcassert(type < WEAPCLASS_NUM, ARRAY_COUNT(szWeapClassNames));

    return szWeapClassNames[type];
}

const char *__cdecl BG_GetWeaponInventoryTypeName(weapInventoryType_t type)
{
    bcassert(type < WEAPINVENTORYCOUNT, ARRAY_COUNT(szWeapInventoryTypeNames));

    return szWeapInventoryTypeNames[type];
}

#ifdef KISAK_MP
void __cdecl BG_LoadWeaponStrings()
{
    uint i; // [esp+0h] [ebp-4h]

    for (i = 0; i < g_playerAnimTypeNamesCount; ++i)
        BG_InitWeaponString(i, g_playerAnimTypeNames[i]);
}
#endif

void __cdecl BG_LoadPlayerAnimTypes()
{
#ifdef KISAK_MP
    char v0; // [esp+3h] [ebp-29h]
    char *v1; // [esp+8h] [ebp-24h]
    const char *v2; // [esp+Ch] [ebp-20h]
    char *buf; // [esp+20h] [ebp-Ch]
    const char *text_p; // [esp+24h] [ebp-8h] BYREF
    const char *token; // [esp+28h] [ebp-4h]

    g_playerAnimTypeNamesCount = 0;
    buf = Com_LoadRawTextFile("mp/playeranimtypes.txt");
    if (!buf)
        Com_Error(ERR_DROP, "Couldn',27h,'t load file %s", "mp/playeranimtypes.txt");
    text_p = buf;
    Com_BeginParseSession("BG_AnimParseAnimScript");
    while (1)
    {
        token = (const char *)Com_Parse(&text_p);
        if (!token || !*token)
            break;
        if (g_playerAnimTypeNamesCount >= 0x40)
            Com_Error(ERR_DROP, "Player anim type array size exceeded");
        g_playerAnimTypeNames[g_playerAnimTypeNamesCount] = (char *)Hunk_Alloc(
            strlen(token) + 1,
            "BG_LoadPlayerAnimTypes",
            9);
        v2 = token;
        v1 = g_playerAnimTypeNames[g_playerAnimTypeNamesCount];
        do
        {
            v0 = *v2;
            *v1++ = *v2++;
        } while (v0);
        ++g_playerAnimTypeNamesCount;
    }
    Com_EndParseSession();
    Com_UnloadRawTextFile(buf);
#elif KISAK_SP
    g_playerAnimTypeNamesCount = 1;
    g_playerAnimTypeNames[0] = (char*)"none";
#endif
}

void __cdecl InitWeaponDef(WeaponDef *weapDef)
{
    const cspField_t *pField; // [esp+4h] [ebp-8h]
    int iField; // [esp+8h] [ebp-4h]

    weapDef->szInternalName = "";
    iField = 0;
    pField = weaponDefFields;
    while (iField < 502)
    {
        if (pField->iFieldType == CSPFT_STRING)
            *(const char **)((char *)weapDef + pField->iOffset) = "";
        ++iField;
        ++pField;
    }
}

char __cdecl G_ParseAIWeaponAccurayGraphFile(
    const char *buffer,
    const char *fileName,
    float (*knots)[2],
    int *knotCount)
{
    int v4; // eax
    long double v5; // st7
    long double v6; // st7
    int knotCountIndex; // [esp+0h] [ebp-8h]
    parseInfo_t *tokenb; // [esp+4h] [ebp-4h]
    parseInfo_t *token; // [esp+4h] [ebp-4h]
    parseInfo_t *tokena; // [esp+4h] [ebp-4h]

    iassert(buffer);
    iassert(fileName);
    iassert(knots);
    iassert(knotCount);

    Com_BeginParseSession(fileName);
    tokenb = Com_Parse(&buffer);
    v4 = atoi(tokenb->token);
    *knotCount = v4;
    knotCountIndex = 0;
    while (1)
    {
        token = Com_Parse(&buffer);
        if (!token->token[0])
            break;
        if (token->token[0] == 125)
            break;
        v5 = atof(token->token);
        (*knots)[2 * knotCountIndex] = v5;
        tokena = Com_Parse(&buffer);
        if (!tokena->token[0] || tokena->token[0] == 125)
            break;
        v6 = atof(tokena->token);
        (*knots)[2 * knotCountIndex++ + 1] = v6;
        if (knotCountIndex >= 16)
        {
            Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" has too many graph knots\n", fileName);
            Com_EndParseSession();
            return 0;
        }
    }
    Com_EndParseSession();
    if (knotCountIndex == *knotCount)
    {
        if ((*knots)[2 * knotCountIndex - 2] == 1.0)
        {
            return 1;
        }
        else
        {
            Com_PrintError(CON_CHANNEL_SERVER, "ERROR: \"%s\" Range must be 0.0 to 1.0\n", fileName);
            return 0;
        }
    }
    else
    {
        Com_PrintError(CON_CHANNEL_SERVER, "ERROR: \"%s\" Error in parsing an ai weapon accuracy file\n", fileName);
        return 0;
    }
}

char __cdecl G_ParseWeaponAccurayGraphInternal(
    WeaponDef *weaponDef,
    const char *dirName,
    const char *graphName,
    float (*knots)[2],
    int *knotCount)
{
    signed int v6; // [esp+10h] [ebp-205Ch]
    char string[64]; // [esp+14h] [ebp-2058h] BYREF
    char buffer[8196]; // [esp+54h] [ebp-2018h] BYREF
    const char *last; // [esp+205Ch] [ebp-10h]
    int knotCounta; // [esp+2060h] [ebp-Ch] BYREF
    int f; // [esp+2064h] [ebp-8h] BYREF
    int len; // [esp+2068h] [ebp-4h]

    last = "WEAPONACCUFILE";
    len = strlen("WEAPONACCUFILE");
    iassert(weaponDef);
    iassert(graphName);
    iassert(knots);
    iassert(knotCount);
    iassert(dirName);

    if (weaponDef->weapType && weaponDef->weapType != WEAPTYPE_PROJECTILE)
        return 1;

    if (!*graphName)
        return 1;

    snprintf(string, ARRAYSIZE(string), "accuracy/%s/%s", dirName, graphName);
    v6 = FS_FOpenFileByMode(string, &f, FS_READ);
    if (v6 >= 0)
    {
        FS_Read((uint8_t *)buffer, len, f);
        buffer[len] = 0;
        if (!strncmp(buffer, last, len))
        {
            if (v6 - len < 0x2000)
            {
                memset((uint8_t *)buffer, 0, 0x2000u);
                FS_Read((uint8_t *)buffer, v6 - len, f);
                buffer[v6 - len] = 0;
                FS_FCloseFile(f);
                knotCounta = 0;
                if (G_ParseAIWeaponAccurayGraphFile(buffer, string, knots, &knotCounta))
                {
                    *knotCount = knotCounta;
                    return 1;
                }
                else
                {
                    return 0;
                }
            }
            else
            {
                Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" Is too long of an ai weapon accuracy file to parse\n", string);
                FS_FCloseFile(f);
                return 0;
            }
        }
        else
        {
            Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" does not appear to be an ai weapon accuracy file\n", string);
            FS_FCloseFile(f);
            return 0;
        }
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: Could not load ai weapon accuracy file '%s'\n", string);
        return 0;
    }
}

char __cdecl G_ParseWeaponAccurayGraphs(WeaponDef *weaponDef)
{
    uint size; // [esp+4h] [ebp-8Ch]
    int weaponType; // [esp+8h] [ebp-88h]
    int accuracyGraphKnotCount; // [esp+Ch] [ebp-84h] BYREF
    float accuracyGraphKnots[16][2]; // [esp+10h] [ebp-80h] BYREF

    for (weaponType = 0; weaponType < 2; ++weaponType)
    {
        memset((uint8_t *)accuracyGraphKnots, 0, sizeof(accuracyGraphKnots));
        accuracyGraphKnotCount = 0;
        if (!G_ParseWeaponAccurayGraphInternal(
            weaponDef,
            accuracyDirName[weaponType],
            weaponDef->accuracyGraphName[weaponType],
            accuracyGraphKnots,
            &accuracyGraphKnotCount))
            return 0;
        if (accuracyGraphKnotCount > 0)
        {
            size = 8 * accuracyGraphKnotCount;
            weaponDef->accuracyGraphKnots[weaponType] = (float (*)[2])Hunk_AllocLowAlign(
                8 * accuracyGraphKnotCount,
                4,
                "G_ParseWeaponAccurayGraphs",
                9);
            weaponDef->originalAccuracyGraphKnots[weaponType] = weaponDef->accuracyGraphKnots[weaponType];
            memcpy((uint8_t *)weaponDef->accuracyGraphKnots[weaponType], (uint8_t *)accuracyGraphKnots, size);
            weaponDef->accuracyGraphKnotCount[weaponType] = accuracyGraphKnotCount;
            weaponDef->originalAccuracyGraphKnotCount[weaponType] = weaponDef->accuracyGraphKnotCount[weaponType];
        }
    }
    return 1;
}

WeaponDef *__cdecl BG_LoadDefaultWeaponDef_LoadObj()
{
    InitWeaponDef(&bg_defaultWeaponDefs);
    bg_defaultWeaponDefs.szInternalName = "none";
    bg_defaultWeaponDefs.accuracyGraphName[0] = "noweapon.accu";
    bg_defaultWeaponDefs.accuracyGraphName[1] = "noweapon.accu";
    bg_defaultWeaponDefs.sprintDurationScale = 1.75;
    G_ParseWeaponAccurayGraphs(&bg_defaultWeaponDefs);
    return &bg_defaultWeaponDefs;
}

WeaponDef *__cdecl BG_LoadDefaultWeaponDef()
{
    if (IsFastFileLoad())
        return BG_LoadDefaultWeaponDef_FastFile();
    else
        return BG_LoadDefaultWeaponDef_LoadObj();
}

WeaponDef *__cdecl BG_LoadDefaultWeaponDef_FastFile()
{
    return DB_FindXAssetHeader(ASSET_TYPE_WEAPON, "none").weapon;
}

int __cdecl Weapon_GetStringArrayIndex(const char *value, char **stringArray, int arraySize)
{
    int arrayIndex; // [esp+0h] [ebp-4h]

    iassert(value);
    iassert(stringArray);

    for (arrayIndex = 0; arrayIndex < arraySize; ++arrayIndex)
    {
        if (!I_stricmp(value, stringArray[arrayIndex]))
            return arrayIndex;
    }
    return -1;
}

snd_alias_list_t **__cdecl BG_RegisterSurfaceTypeSounds(const char *surfaceSoundBase)
{
    char *v2; // eax
    snd_alias_list_t *SoundAlias; // eax
    char v4; // [esp+3h] [ebp-131h]
    char *v5; // [esp+8h] [ebp-12Ch]
    const char *v6; // [esp+Ch] [ebp-128h]
    snd_alias_list_t **result; // [esp+20h] [ebp-114h]
    char aliasName[260]; // [esp+24h] [ebp-110h] BYREF
    snd_alias_list_t *defaultAliasList; // [esp+12Ch] [ebp-8h]
    int i; // [esp+130h] [ebp-4h]

    iassert(surfaceSoundBase);

    if (!*surfaceSoundBase)
        return 0;

    for (i = 0; i < surfaceTypeSoundListCount; ++i)
    {
        if (!I_strcmp(surfaceTypeSoundLists[i].surfaceSoundBase, surfaceSoundBase))
            return surfaceTypeSoundLists[i].soundAliasList;
    }
    if (surfaceTypeSoundListCount == 16)
        Com_Error(ERR_DROP, "Exceeded MAX_SURFACE_TYPE_SOUND_LISTS (%d)", 16);

    result = (snd_alias_list_t **)Hunk_AllocLow(29 * sizeof(snd_alias_list_t *), "BG_RegisterSurfaceTypeSounds", 15);
    Com_sprintf(aliasName, 0x100u, "%s_default", surfaceSoundBase);
    defaultAliasList = Com_FindSoundAlias(aliasName);
    for (i = 0; i < 29; ++i)
    {
        v2 = (char*)Com_SurfaceTypeToName(i);
        Com_sprintf(aliasName, 0x100u, "%s_%s", surfaceSoundBase, v2);
        SoundAlias = Com_FindSoundAlias(aliasName);
        result[i] = SoundAlias;
        if (!result[i])
            result[i] = defaultAliasList;
    }
    surfaceTypeSoundLists[surfaceTypeSoundListCount].surfaceSoundBase = (char *)Hunk_AllocLow(
        strlen(surfaceSoundBase) + 1,
        "BG_RegisterSurfaceTypeSounds",
        15);
    v6 = surfaceSoundBase;
    v5 = surfaceTypeSoundLists[surfaceTypeSoundListCount].surfaceSoundBase;
    do
    {
        v4 = *v6;
        *v5++ = *v6++;
    } while (v4);
    surfaceTypeSoundLists[surfaceTypeSoundListCount++].soundAliasList = result;
    return result;
}

int __cdecl BG_ParseWeaponDefSpecificFieldType(uint8_t *pStruct, const char *pValue, int iFieldType)
{
    uint16_t LowercaseString_DONE; // ax
    uint16_t v5; // ax
    int result; // eax
    char v7; // [esp+3h] [ebp-91h]
    char *v8; // [esp+8h] [ebp-8Ch]
    const char *v9; // [esp+Ch] [ebp-88h]
    int v10; // [esp+10h] [ebp-84h]
    const char *pos; // [esp+38h] [ebp-5Ch] BYREF
    int numHideTags; // [esp+3Ch] [ebp-58h]
    int numNoteTrackMappings; // [esp+40h] [ebp-54h]
    char keyName[64]; // [esp+44h] [ebp-50h] BYREF
    int arrayIndex; // [esp+88h] [ebp-Ch]
    const char *token; // [esp+8Ch] [ebp-8h]
    WeaponDef *weapDef; // [esp+90h] [ebp-4h]

    iassert(pStruct);
    iassert(pValue);

    weapDef = (WeaponDef *)pStruct;
    switch (iFieldType)
    {
    case WFT_WEAPONTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)szWeapTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon type %s in %s", pValue, weapDef->szInternalName);
        weapDef->weapType = (weapType_t)arrayIndex;
        goto LABEL_86;
    case WFT_WEAPONCLASS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)szWeapClassNames, 10);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon class %s in %s", pValue, weapDef->szInternalName);
        weapDef->weapClass = (weapClass_t)arrayIndex;
        goto LABEL_86;
    case WFT_OVERLAYRETICLE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapOverlayReticleNames, 2);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon reticle %s in %s", pValue, weapDef->szInternalName);
        weapDef->overlayReticle = (weapOverlayReticle_t)arrayIndex;
        goto LABEL_86;
    case WFT_PENETRATE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)penetrateTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon penetrate type %s in %s", pValue, weapDef->szInternalName);
        weapDef->penetrateType = (PenetrateType)arrayIndex;
        goto LABEL_86;
    case WFT_IMPACT_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)impactTypeNames, 9);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon impact type %s in %s", pValue, weapDef->szInternalName);
        weapDef->impactType = (ImpactType)arrayIndex;
        goto LABEL_86;
    case WFT_STANCE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapStanceNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon stance %s in %s", pValue, weapDef->szInternalName);
        weapDef->stance = (weapStance_t)arrayIndex;
        goto LABEL_86;
    case WFT_PROJ_EXPLOSION:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szProjectileExplosionNames, 7);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon projExplosion %s in %s", pValue, weapDef->szInternalName);
        weapDef->projExplosion = (weapProjExposion_t)arrayIndex;
        goto LABEL_86;
    case WFT_OFFHAND_CLASS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)offhandClassNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon offhand class %s in %s", pValue, weapDef->szInternalName);
        weapDef->offhandClass = (OffhandClass)arrayIndex;
        goto LABEL_86;
    case WFT_ANIMTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, g_playerAnimTypeNames, g_playerAnimTypeNamesCount);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon player anim type %s in %s", pValue, weapDef->szInternalName);
        weapDef->playerAnimType = arrayIndex;
        goto LABEL_86;
    case WFT_ACTIVE_RETICLE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)activeReticleNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon active reticle type %s in %s", pValue, weapDef->szInternalName);
        weapDef->activeReticleType = (activeReticleType_t)arrayIndex;
        goto LABEL_86;
    case WFT_GUIDED_MISSILE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)guidedMissileNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon guided missile type %s in %s", pValue, weapDef->szInternalName);
        weapDef->guidedMissileType = (guidedMissileType_t)arrayIndex;
        goto LABEL_86;
    case WFT_BOUNCE_SOUND:
        weapDef->bounceSound = BG_RegisterSurfaceTypeSounds(pValue);
        goto LABEL_86;
    case WFT_STICKINESS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)stickinessNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon stickiness %s in %s", pValue, weapDef->szInternalName);
        weapDef->stickiness = (WeapStickinessType)arrayIndex;
        goto LABEL_86;
    case WFT_OVERLAYINTERFACE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)overlayInterfaceNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon overlay interface %s in %s", pValue, weapDef->szInternalName);
        weapDef->overlayInterface = (WeapOverlayInteface_t)arrayIndex;
        goto LABEL_86;
    case WFT_INVENTORYTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapInventoryTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon inventory type %s in %s", pValue, weapDef->szInternalName);
        weapDef->inventoryType = (weapInventoryType_t)arrayIndex;
        goto LABEL_86;
    case WFT_FIRETYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapFireTypeNames, 5);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon firetype %s in %s", pValue, weapDef->szInternalName);
        weapDef->fireType = (weapFireType_t)arrayIndex;
        goto LABEL_86;
    case WFT_AMMOCOUNTER_CLIPTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)ammoCounterClipNames, 7);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon ammo counter clip %s in %s", pValue, weapDef->szInternalName);
        weapDef->ammoCounterClip = (ammoCounterClipType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_HUD:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon hud icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->hudIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_AMMOCOUNTER:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon ammo counter icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->ammoCounterIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_KILL:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon kill icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->killIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_DPAD:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon dpad icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->dpadIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_HIDETAGS:
        numHideTags = 0;
        pos = pValue;
        while (1)
        {
            token = (const char *)Com_Parse(&pos);
            if (!pos)
                break;
            if (numHideTags >= 8)
                Com_Error(ERR_DROP, "maximum hide tags (%s) exceeded: %i > %i'", token, numHideTags, 8);
            weapDef->hideTags[numHideTags] = SL_GetStringOfSize((char *)token, 0, strlen(token) + 1, MT_TYPE_MODEL_PART);
            weapDef->hideTags[numHideTags] = SL_ConvertToLowercase(weapDef->hideTags[numHideTags], 0, MT_TYPE_MODEL_PART);
            ++numHideTags;
        }
        goto LABEL_86;
    case WFT_NOTETRACKSOUNDMAP:
        numNoteTrackMappings = 0;
        pos = pValue;
        keyName[0] = 0;
        while (1)
        {
            token = (const char *)Com_Parse(&pos);
            if (!pos)
                break;
            if (numNoteTrackMappings >= 16)
                Com_Error(ERR_DROP, "Max notetrack-to-sound mappings (%i) exceeded with entry '%s'", 16, token);
            if (keyName[0])
            {
                LowercaseString_DONE = SL_GetLowercaseString(keyName, 0);
                weapDef->notetrackSoundMapKeys[numNoteTrackMappings] = LowercaseString_DONE;
                v5 = SL_GetLowercaseString(token, 0);
                weapDef->notetrackSoundMapValues[numNoteTrackMappings++] = v5;
                keyName[0] = 0;
            }   
            else
            {
                v10 = strlen(token);
                if (v10 >= 63)
                    Com_Error(ERR_DROP, "Notetrack - to - sound: keyname \"%s\" is too long(length % i / % i).", token, v10, 63);
                v9 = token;
                v8 = keyName;
                do
                {
                    v7 = *v9;
                    *v8++ = *v9++;
                } while (v7);
            }
        }
        if (keyName[0])
            Com_PrintWarning(
                CON_CHANNEL_DONT_FILTER,
                "Notetrack-to-Sound: Weapon '%s' has bad entry; notetrack '%s' doesn't have a corresponding sound.\n",
                weapDef->szInternalName,
                keyName);
    LABEL_86:
        result = 1;
        break;
    default:
        Com_Error(ERR_DROP, "Bad field type %i in %s", iFieldType, weapDef->szInternalName);
        result = 0;
        break;
    }
    return result;
}

void __cdecl BG_SetupTransitionTimes(WeaponDef *weapDef)
{
    double v1; // st7
    double v2; // st7

    if (weapDef->iAdsTransInTime <= 0)
        v1 = 1.0 / (float)300.0;
    else
        v1 = 1.0 / (double)weapDef->iAdsTransInTime;
    weapDef->fOOPosAnimLength[0] = v1;
    if (weapDef->iAdsTransOutTime <= 0)
        v2 = 1.0 / (float)500.0;
    else
        v2 = 1.0 / (double)weapDef->iAdsTransOutTime;
    weapDef->fOOPosAnimLength[1] = v2;
}

void __cdecl BG_CheckWeaponDamageRanges(WeaponDef *weapDef)
{
    if (weapDef->fMaxDamageRange <= 0.0f)
        weapDef->fMaxDamageRange = 999999.0f;
    if (weapDef->fMinDamageRange <= 0.0f)
        weapDef->fMinDamageRange = 999999.12f;
}

void __cdecl BG_CheckProjectileValues(WeaponDef *weaponDef)
{
    iassert(weaponDef->weapType == WEAPTYPE_PROJECTILE);

    if ((double)weaponDef->iProjectileSpeed <= 0.0)
        Com_Error(ERR_DROP, "Projectile speed for WeapType %s must be greater than 0.0", weaponDef->szDisplayName);

    if (weaponDef->destabilizationCurvatureMax >= 1000000000.0f || weaponDef->destabilizationCurvatureMax < 0.0)
        Com_Error(
            ERR_DROP,
            "Destabilization angle for for WeapType %s must be between 0 and 45 degrees",
            weaponDef->szDisplayName);

    if (weaponDef->destabilizationRateTime < 0.0)
        Com_Error(ERR_DROP, "Destabilization rate time for for WeapType %s must be non-negative", weaponDef->szDisplayName);
}

WeaponDef *__cdecl BG_LoadWeaponDefInternal(const char *one, const char *two)
{
    snd_alias_list_t *SoundAlias; // eax
    snd_alias_list_t *v4; // eax
    snd_alias_list_t *v5; // eax
    snd_alias_list_t *v6; // eax
    snd_alias_list_t *v7; // eax
    char buffer[10244]; // [esp+1Ch] [ebp-2858h] BYREF
    int f; // [esp+2820h] [ebp-54h] BYREF
    int len; // [esp+2824h] [ebp-50h]
    signed int v11; // [esp+2828h] [ebp-4Ch]
    char dest[64]; // [esp+282Ch] [ebp-48h] BYREF
    WeaponDef *weapDef; // [esp+2870h] [ebp-4h]

    len = strlen("WEAPONFILE");
    weapDef = (WeaponDef *)Hunk_AllocLow(sizeof(WeaponDef), "BG_LoadWeaponDefInternal", 9);
    InitWeaponDef(weapDef);
    Com_sprintf(dest, 0x40u, "weapons/%s/%s", one, two);
    v11 = FS_FOpenFileByMode(dest, &f, FS_READ);
    if (v11 >= 0)
    {
        FS_Read((uint8_t *)buffer, len, f);
        buffer[len] = 0;
        if (!strncmp(buffer, "WEAPONFILE", len))
        {
            if ((uint)(v11 - len) < 0x2800)
            {
                memset((uint8_t *)buffer, 0, 0x2800u);
                FS_Read((uint8_t *)buffer, v11 - len, f);
                buffer[v11 - len] = 0;
                FS_FCloseFile(f);
                if (Info_Validate(buffer))
                {
                    SetConfigString((char **)weapDef, two);
                    if (ParseConfigStringToStructCustomSize(
                        (uint8_t *)weapDef,
                        weaponDefFields,
                        502,
                        buffer,
                        WFT_NUM_FIELD_TYPES,
                        BG_ParseWeaponDefSpecificFieldType,
                        SetConfigString2))
                    {
                        if (I_stricmp(two, "defaultweapon_mp"))
                        {
                            if (!weapDef->viewLastShotEjectEffect)
                                weapDef->viewLastShotEjectEffect = weapDef->viewShellEjectEffect;
                            if (!weapDef->worldLastShotEjectEffect)
                                weapDef->worldLastShotEjectEffect = weapDef->worldShellEjectEffect;
                            if (!weapDef->raiseSound)
                            {
                                SoundAlias = Com_FindSoundAlias("weap_raise");
                                weapDef->raiseSound = SoundAlias;
                            }
                            if (!weapDef->putawaySound)
                            {
                                v4 = Com_FindSoundAlias("weap_putaway");
                                weapDef->putawaySound = v4;
                            }
                            if (!weapDef->pickupSound)
                            {
                                v5 = Com_FindSoundAlias("weap_pickup");
                                weapDef->pickupSound = v5;
                            }
                            if (!weapDef->ammoPickupSound)
                            {
                                v6 = Com_FindSoundAlias("weap_ammo_pickup");
                                weapDef->ammoPickupSound = v6;
                            }
                            if (!weapDef->emptyFireSound)
                            {
                                v7 = Com_FindSoundAlias("weap_dryfire_smg_npc");
                                weapDef->emptyFireSound = v7;
                            }
                        }
                        BG_SetupTransitionTimes(weapDef);
                        BG_CheckWeaponDamageRanges(weapDef);
                        if (weapDef->enemyCrosshairRange > 15000.0)
                            Com_Error(ERR_DROP, "Enemy crosshair ranges should be less than %f ", 15000.0);
                        if (weapDef->weapType == WEAPTYPE_PROJECTILE)
                            BG_CheckProjectileValues(weapDef);
                        if (G_ParseWeaponAccurayGraphs(weapDef))
                        {
                            I_strlwr((char *)weapDef->szAmmoName);
                            I_strlwr((char *)weapDef->szClipName);
                            return weapDef;
                        }
                        else
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: \"%s\" is not a valid weapon file\n", dest);
                    return 0;
                }
            }
            else
            {
                Com_PrintWarning(
                    CON_CHANNEL_PLAYERWEAP,
                    "WARNING: \"%s\" Is too long of a weapon file to parse (fileLength = %d identifierLength = %d)\n",
                    dest,
                    v11,
                    len);
                FS_FCloseFile(f);
                return 0;
            }
        }
        else
        {
            Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: \"%s\" does not appear to be a weapon file\n", dest);
            FS_FCloseFile(f);
            return 0;
        }
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: Could not load weapon file '%s'\n", dest);
        return 0;
    }
}


WeaponDef *__cdecl BG_LoadWeaponDef_LoadObj(const char *name)
{
    WeaponDef *weapDef; // [esp+0h] [ebp-4h]

    if (!*name)
        return 0;
#ifdef KISAK_MP
    weapDef = BG_LoadWeaponDefInternal("mp", name);
#elif KISAK_SP
    weapDef = BG_LoadWeaponDefInternal("sp", name);
#endif
    if (weapDef)
        return weapDef;

#ifdef KISAK_MP
    weapDef = BG_LoadWeaponDefInternal("mp", "defaultweapon_mp");
#elif KISAK_SP
    weapDef = BG_LoadWeaponDefInternal("sp", "defaultweapon");
#endif

    if (!weapDef)
        Com_Error(ERR_DROP, "BG_LoadWeaponDef: Could not find default weapon");

    SetConfigString((char **)weapDef, name);
    return weapDef;
}
